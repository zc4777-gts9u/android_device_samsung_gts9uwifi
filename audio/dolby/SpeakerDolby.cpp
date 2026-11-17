// SPDX-License-Identifier: Apache-2.0
#define LOG_TAG "Gts9uSpeakerDolby"

#include <gts9u/SpeakerDolby.h>

#include <algorithm>
#include <cmath>
#include <dlfcn.h>
#include <iterator>

#include <audio_utils/format.h>
#include <log/log.h>

namespace gts9u::audio {
namespace {
constexpr int kDisabled = -1;
constexpr uint32_t kProfile = 0;
constexpr uint32_t kProfileEnabled = 19;
constexpr effect_uuid_t kDolbyUuid = {
        0x6ab06da4, 0xc516, 0x4611, 0x8166, {0x45, 0x27, 0x99, 0x21, 0x85, 0x39}};
std::atomic<int> gProfile{kDisabled};

int command(effect_handle_t effect, uint32_t code, uint32_t length = 0, void* data = nullptr) {
    int32_t result = 0;
    uint32_t replySize = sizeof(result);
    int status = (*effect)->command(effect, code, length, data, &replySize, &result);
    return status == 0 ? result : status;
}

int setParameter(effect_handle_t effect, uint32_t key, int32_t value) {
    struct {
        int32_t status;
        uint32_t parameterSize;
        uint32_t valueSize;
        uint32_t key;
        int32_t value;
    } parameter{0, sizeof(key), sizeof(value), key, value};
    return command(effect, EFFECT_CMD_SET_PARAM, sizeof(parameter), &parameter);
}

audio_effect_library_t* loadLibrary() {
    constexpr const char* path = sizeof(void*) == 8 ? "/vendor/lib64/soundfx/libswdap.so"
                                                    : "/vendor/lib/soundfx/libswdap.so";
    // Keep the library loaded for the lifetime of the audio HAL process.
    static audio_effect_library_t* library = [] {
        void* handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (handle == nullptr) {
            ALOGE("Cannot load Dolby speaker engine: %s", dlerror());
            return static_cast<audio_effect_library_t*>(nullptr);
        }
        auto* api = static_cast<audio_effect_library_t*>(
                dlsym(handle, AUDIO_EFFECT_LIBRARY_INFO_SYM_AS_STR));
        if (api == nullptr || api->tag != AUDIO_EFFECT_LIBRARY_TAG ||
            api->create_effect == nullptr || api->release_effect == nullptr) {
            ALOGE("Invalid Dolby speaker effect library");
            dlclose(handle);
            return static_cast<audio_effect_library_t*>(nullptr);
        }
        return api;
    }();
    return library;
}
}  // namespace

bool setDolbyProfile(std::string_view value) {
    if (value == "-1") {
        gProfile.store(kDisabled, std::memory_order_relaxed);
        return true;
    }
    if (value.size() != 1 || value.front() < '0' || value.front() > '3') return false;
    gProfile.store(value.front() - '0', std::memory_order_relaxed);
    return true;
}

std::shared_ptr<SpeakerRoute> SpeakerRoutes::open(audio_io_handle_t io, audio_devices_t device) {
    auto route = std::make_shared<SpeakerRoute>(device == AUDIO_DEVICE_OUT_SPEAKER);
    std::lock_guard lock(mMutex);
    for (auto it = mOutputs.begin(); it != mOutputs.end();) {
        it = it->second.route.expired() ? mOutputs.erase(it) : std::next(it);
    }
    mOutputs.insert_or_assign(io, Output{route});
    return route;
}

void SpeakerRoutes::updatePatch(audio_patch_handle_t previous, audio_patch_handle_t patch,
                                size_t sourceCount, const audio_port_config* sources,
                                size_t sinkCount, const audio_port_config* sinks) {
    if (sourceCount == 0 || sources[0].type != AUDIO_PORT_TYPE_MIX ||
        sources[0].role != AUDIO_PORT_ROLE_SOURCE) return;
    const auto io = sources[0].ext.mix.handle;
    const bool speaker = sinkCount != 0 &&
            std::all_of(sinks, sinks + sinkCount, [](const auto& sink) {
                return sink.type == AUDIO_PORT_TYPE_DEVICE &&
                        sink.ext.device.type == AUDIO_DEVICE_OUT_SPEAKER;
            });

    std::lock_guard lock(mMutex);
    for (auto& [outputIo, output] : mOutputs) {
        if (outputIo == io) {
            output.patch = patch;
            if (auto route = output.route.lock()) {
                route->isSpeaker.store(speaker, std::memory_order_relaxed);
            }
        } else if (previous != AUDIO_PATCH_HANDLE_NONE && output.patch == previous) {
            output.patch = AUDIO_PATCH_HANDLE_NONE;
            if (auto route = output.route.lock()) {
                route->isSpeaker.store(false, std::memory_order_relaxed);
            }
        }
    }
}

void SpeakerRoutes::releasePatch(audio_patch_handle_t patch) {
    if (patch == AUDIO_PATCH_HANDLE_NONE) return;
    std::lock_guard lock(mMutex);
    for (auto& entry : mOutputs) {
        auto& output = entry.second;
        if (output.patch != patch) continue;
        output.patch = AUDIO_PATCH_HANDLE_NONE;
        if (auto route = output.route.lock()) {
            route->isSpeaker.store(false, std::memory_order_relaxed);
        }
    }
}

SpeakerDolby::~SpeakerDolby() {
    if (mEffect != nullptr) mLibrary->release_effect(mEffect);
}

bool SpeakerDolby::configure(audio_format_t format, audio_channel_mask_t channels,
                             uint32_t sampleRate) {
    switch (format) {
        case AUDIO_FORMAT_PCM_16_BIT:
        case AUDIO_FORMAT_PCM_32_BIT:
        case AUDIO_FORMAT_PCM_8_24_BIT:
        case AUDIO_FORMAT_PCM_24_BIT_PACKED:
        case AUDIO_FORMAT_PCM_FLOAT:
            break;
        default:
            return false;
    }

    const auto audibleMask = static_cast<audio_channel_mask_t>(channels & ~AUDIO_CHANNEL_HAPTIC_ALL);
    if ((audibleMask != AUDIO_CHANNEL_OUT_MONO && audibleMask != AUDIO_CHANNEL_OUT_STEREO) ||
        sampleRate == 0) return false;

    mLibrary = loadLibrary();
    if (mLibrary == nullptr) return false;
    const int status = mLibrary->create_effect(&kDolbyUuid, AUDIO_SESSION_OUTPUT_STAGE,
                                               AUDIO_IO_HANDLE_NONE, &mEffect);
    if (status != 0 || mEffect == nullptr) {
        mEffect = nullptr;
        ALOGE("Cannot create Dolby speaker effect: %d", status);
        return false;
    }

    effect_config_t config{};
    config.inputCfg.samplingRate = sampleRate;
    config.inputCfg.channels = AUDIO_CHANNEL_OUT_STEREO;
    config.inputCfg.format = AUDIO_FORMAT_PCM_FLOAT;
    config.inputCfg.accessMode = EFFECT_BUFFER_ACCESS_READ;
    config.inputCfg.mask = EFFECT_CONFIG_ALL;
    config.outputCfg = config.inputCfg;
    config.outputCfg.accessMode = EFFECT_BUFFER_ACCESS_WRITE;
    uint32_t speaker = AUDIO_DEVICE_OUT_SPEAKER;
    if (command(mEffect, EFFECT_CMD_INIT) != 0 ||
        command(mEffect, EFFECT_CMD_SET_CONFIG, sizeof(config), &config) != 0 ||
        command(mEffect, EFFECT_CMD_SET_DEVICE, sizeof(speaker), &speaker) != 0) {
        ALOGE("Cannot configure Dolby speaker effect for %u Hz", sampleRate);
        return false;
    }
    mFormat = format;
    mSampleRate = sampleRate;
    mAudioChannels = audio_channel_count_from_out_mask(audibleMask);
    mFrameSize = audio_bytes_per_frame(audio_channel_count_from_out_mask(channels), format);
    mFadeFrames = std::max(1u, sampleRate / 20);
    return true;
}

bool SpeakerDolby::applyProfile(int profile) {
    const bool wasEnabled = mActiveProfile != kDisabled;
    if (setParameter(mEffect, kProfile, profile) != 0 ||
        setParameter(mEffect, kProfileEnabled, 1) != 0 ||
        (!wasEnabled && command(mEffect, EFFECT_CMD_ENABLE) != 0)) {
        ALOGE("Cannot activate Dolby speaker profile %d", profile);
        if (wasEnabled) command(mEffect, EFFECT_CMD_DISABLE);
        mActiveProfile = kDisabled;
        mWetFrames = 0;
        return false;
    }
    mActiveProfile = profile;
    ALOGI("Using Dolby speaker profile %d at %u Hz", profile, mSampleRate);
    return true;
}

void SpeakerDolby::process(void* data, size_t bytes, bool routedToSpeaker) {
    if (mEffect == nullptr || mFrameSize == 0 || bytes == 0 || bytes % mFrameSize != 0) return;

    if (!routedToSpeaker) {
        if (mActiveProfile != kDisabled) command(mEffect, EFFECT_CMD_DISABLE);
        mActiveProfile = kDisabled;
        mRequestedProfile = kDisabled;
        mWetFrames = 0;
        return;
    }

    const int profile = gProfile.load(std::memory_order_relaxed);
    if (profile != mRequestedProfile) {
        mRequestedProfile = profile;
        if (profile != kDisabled && !applyProfile(profile)) return;
    }
    if (mActiveProfile == kDisabled) return;
    const bool fadingOut = profile == kDisabled;
    auto* samples = static_cast<uint8_t*>(data);
    size_t frames = bytes / mFrameSize;
    while (frames > 0) {
        const size_t block = std::min(frames, kBlockFrames);
        processBlock(samples, block, fadingOut);
        samples += block * mFrameSize;
        frames -= block;
        if (fadingOut && mWetFrames == 0) {
            command(mEffect, EFFECT_CMD_DISABLE);
            mActiveProfile = kDisabled;
            mGain = 1.0f;
            break;
        }
    }
}

void SpeakerDolby::processBlock(uint8_t* data, size_t frames, bool fadingOut) {
    const size_t sampleBytes = audio_bytes_per_sample(mFormat);
    const bool packedStereo = mAudioChannels == 2 && mFrameSize == 2 * sampleBytes;
    if (packedStereo) {
        memcpy_by_audio_format(mDry.data(), AUDIO_FORMAT_PCM_FLOAT, data, mFormat, frames * 2);
    } else {
        // Only copy audible samples. Haptic channels remain byte-for-byte intact.
        for (size_t frame = 0; frame < frames; ++frame) {
            float* stereo = &mDry[frame * 2];
            memcpy_by_audio_format(stereo, AUDIO_FORMAT_PCM_FLOAT, data + frame * mFrameSize,
                                   mFormat, mAudioChannels);
            if (mAudioChannels == 1) stereo[1] = stereo[0];
        }
    }

    float inputPower = 0.0f;
    for (size_t i = 0; i < frames * 2; ++i) inputPower += mDry[i] * mDry[i];
    audio_buffer_t input{.frameCount = frames, .f32 = mDry.data()};
    audio_buffer_t output{.frameCount = frames, .f32 = mWet.data()};
    if ((*mEffect)->process(mEffect, &input, &output) != 0) {
        mWetFrames = 0;
        mGain = 1.0f;
        return;
    }

    float outputPower = 0.0f;
    for (size_t i = 0; i < frames * 2; ++i) outputPower += mWet[i] * mWet[i];
    if (!std::isfinite(outputPower) || !std::isfinite(inputPower)) {
        mWetFrames = 0;
        mGain = 1.0f;
        return;
    }

    // Drain the engine during silence without writing its buffered tail or
    // resetting the gain used when sound resumes.
    if (inputPower == 0.0f) {
        if (fadingOut) mWetFrames -= std::min<size_t>(mWetFrames, frames);
        return;
    }

    // Volume is already present in this PCM. Bound the wet block's energy to
    // the dry block's energy so DAX's leveler cannot raise a quiet media stream.
    // Recover attenuation gradually when DAX's gain falls.
    const float ceiling = outputPower > inputPower ? std::sqrt(inputPower / outputPower) : 1.0f;
    const float recovery = std::min(1.0f, static_cast<float>(frames) / (mSampleRate * 0.15f));
    mGain = std::min(ceiling, mGain + (1.0f - mGain) * recovery);
    for (size_t frame = 0; frame < frames; ++frame) {
        if (fadingOut) {
            if (mWetFrames > 0) --mWetFrames;
        } else if (mWetFrames < mFadeFrames) {
            ++mWetFrames;
        }
        const float mix = static_cast<float>(mWetFrames) / mFadeFrames;
        const size_t left = frame * 2;
        for (size_t i = left; i < left + 2; ++i) {
            mWet[i] = std::clamp(mDry[i] * (1.0f - mix) + mWet[i] * (mix * mGain), -1.0f, 1.0f);
        }
        if (!packedStereo) {
            if (mAudioChannels == 1) mWet[left] = 0.5f * (mWet[left] + mWet[left + 1]);
            memcpy_by_audio_format(data + frame * mFrameSize, mFormat, mWet.data() + left,
                                   AUDIO_FORMAT_PCM_FLOAT, mAudioChannels);
        }
    }
    if (packedStereo) {
        memcpy_by_audio_format(data, mFormat, mWet.data(), AUDIO_FORMAT_PCM_FLOAT, frames * 2);
    }
}

}  // namespace gts9u::audio
