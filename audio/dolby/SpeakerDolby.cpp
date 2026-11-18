// SPDX-License-Identifier: Apache-2.0
#define LOG_TAG "Gts9uSpeakerDolby"

#include <gts9u/SpeakerDolby.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <dlfcn.h>
#include <iterator>

#include <audio_utils/format.h>
#include <log/log.h>

namespace gts9u::audio {
namespace {
constexpr int kDisabled = -1;
constexpr int kStockOffProfile = 5;
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
        command(mEffect, EFFECT_CMD_SET_DEVICE, sizeof(speaker), &speaker) != 0 ||
        setParameter(mEffect, 22, 11212950) != 0) {
        ALOGE("Cannot configure Dolby speaker effect for %u Hz", sampleRate);
        return false;
    }
    mFormat = format;
    mSampleRate = sampleRate;
    mAudioChannels = audio_channel_count_from_out_mask(audibleMask);
    mFrameSize = audio_bytes_per_frame(audio_channel_count_from_out_mask(channels), format);
    return true;
}

bool SpeakerDolby::applyProfile(int profile) {
    const bool enabled = profile != kDisabled;
    // Follow SoundAlive: change the profile on the existing effect instance.
    if (setParameter(mEffect, kProfileEnabled, enabled) != 0 ||
        setParameter(mEffect, kProfile, enabled ? profile : kStockOffProfile) != 0 ||
        (enabled && !mEffectEnabled && command(mEffect, EFFECT_CMD_ENABLE) != 0)) {
        ALOGE("Cannot set Dolby speaker profile %d", profile);
        if (mEffectEnabled) command(mEffect, EFFECT_CMD_DISABLE);
        mEffectEnabled = false;
        mProcessing = false;
        return false;
    }
    mNotifiedVolume = -1.0f;
    if (enabled) {
        mEffectEnabled = true;
        mProcessing = true;
        ALOGI("Using Dolby speaker profile %d at %u Hz", profile, mSampleRate);
    }
    return true;
}

void SpeakerDolby::process(void* data, size_t bytes, bool routedToSpeaker, float mixVolume) {
    if (mEffect == nullptr || mFrameSize == 0 || bytes == 0 || bytes % mFrameSize != 0) return;

    if (!std::isfinite(mixVolume)) return;
    mixVolume = std::clamp(mixVolume, 0.0f, 1.0f);
    // The mixer can report zero before its queued PCM drains. Keep the last
    // valid attenuation during that gap; AudioFlinger already applies mute.
    if (mixVolume > 1.0e-6f) {
        if (!mVolumeReady || mixVolume >= mMixVolume) {
            // A louder track can already be in the current buffer.
            mMixVolume = mixVolume;
            mTargetVolume = mixVolume;
            mVolumeRampFrames = 0;
            mVolumeReady = true;
        } else if (mixVolume != mTargetVolume) {
            mTargetVolume = mixVolume;
            mVolumeRampFrames = std::max(1u, mSampleRate / 100);
            mVolumeStep = (mTargetVolume - mMixVolume) / mVolumeRampFrames;
        }
    }

    const int profile = routedToSpeaker ? gProfile.load(std::memory_order_relaxed) : kDisabled;
    if (profile != mRequestedProfile) {
        mRequestedProfile = profile;
        if (!applyProfile(profile)) return;
    }
    if (!mProcessing) {
        mMixVolume = mTargetVolume;
        mVolumeRampFrames = 0;
    }
    if (mProcessing && mVolumeReady && mNotifiedVolume != mTargetVolume) {
        uint32_t volume[2] = {
                static_cast<uint32_t>(std::lround(mTargetVolume * (1u << 24))),
                static_cast<uint32_t>(std::lround(mTargetVolume * (1u << 24)))};
        uint32_t returnedVolume[2]{};
        uint32_t replySize = sizeof(returnedVolume);
        if ((*mEffect)->command(mEffect, EFFECT_CMD_SET_VOLUME, sizeof(volume), volume,
                                &replySize, returnedVolume) == 0) {
            mNotifiedVolume = mTargetVolume;
        }
    }

    auto* samples = static_cast<uint8_t*>(data);
    size_t frames = bytes / mFrameSize;
    while (mProcessing && frames > 0) {
        const size_t block = std::min(frames, kBlockFrames);
        processBlock(samples, block, routedToSpeaker);
        samples += block * mFrameSize;
        frames -= block;
    }
}

void SpeakerDolby::processBlock(uint8_t* data, size_t frames, bool routedToSpeaker) {
    const size_t sampleBytes = audio_bytes_per_sample(mFormat);
    const bool packedStereo = mAudioChannels == 2 && mFrameSize == 2 * sampleBytes;
    if (!routedToSpeaker) {
        // Let the native fade finish without processing another device's audio.
        std::fill_n(mDry.begin(), frames * 2, 0.0f);
    } else if (packedStereo) {
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

    // The reference describes the loudest track, while this block can contain
    // several tracks or older PCM still queued in FastMixer. Bound the input to
    // the effect's float range and restore the same scale after processing.
    float inputPeak = 0.0f;
    for (size_t sample = 0; sample < frames * 2; ++sample) {
        inputPeak = std::max(inputPeak, std::abs(mDry[sample]));
    }
    for (size_t frame = 0; frame < frames; ++frame) {
        if (mVolumeRampFrames != 0) {
            mMixVolume += mVolumeStep;
            if (--mVolumeRampFrames == 0) mMixVolume = mTargetVolume;
        }
        const float volume = std::max(std::clamp(mMixVolume, 0.0f, 1.0f), inputPeak);
        mFrameVolume[frame] = volume;
        const float inverse = volume > 1.0e-6f ? 1.0f / volume : 0.0f;
        mDry[frame * 2] *= inverse;
        mDry[frame * 2 + 1] *= inverse;
    }

    audio_buffer_t input{.frameCount = frames, .f32 = mDry.data()};
    audio_buffer_t output{.frameCount = frames, .f32 = mWet.data()};
    const int status = (*mEffect)->process(mEffect, &input, &output);
    if (status != 0) {
        // The stock effect returns ENOSYS once the transition to bypass is done.
        if (mRequestedProfile == kDisabled && (status == -ENOSYS || status == -ENODATA)) {
            mProcessing = false;
        }
        return;
    }
    if (!routedToSpeaker ||
        !std::all_of(mWet.begin(), mWet.begin() + frames * 2,
                     [](float sample) { return std::isfinite(sample); })) {
        return;
    }

    for (size_t frame = 0; frame < frames; ++frame) {
        mWet[frame * 2] *= mFrameVolume[frame];
        mWet[frame * 2 + 1] *= mFrameVolume[frame];
    }

    if (packedStereo) {
        memcpy_by_audio_format(data, mFormat, mWet.data(), AUDIO_FORMAT_PCM_FLOAT, frames * 2);
    } else {
        for (size_t frame = 0; frame < frames; ++frame) {
            const size_t left = frame * 2;
            if (mAudioChannels == 1) mWet[left] = 0.5f * (mWet[left] + mWet[left + 1]);
            memcpy_by_audio_format(data + frame * mFrameSize, mFormat, mWet.data() + left,
                                   AUDIO_FORMAT_PCM_FLOAT, mAudioChannels);
        }
    }
}

}  // namespace gts9u::audio
