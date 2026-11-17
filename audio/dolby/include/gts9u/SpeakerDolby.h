// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>

#include <hardware/audio_effect.h>
#include <system/audio.h>

namespace gts9u::audio {

inline constexpr char kDolbyProfileParameter[] = "gts9u_speaker_dolby_profile";
bool setDolbyProfile(std::string_view value);

struct SpeakerRoute {
    explicit SpeakerRoute(bool speaker) : isSpeaker(speaker) {}
    std::atomic<bool> isSpeaker;
};

// Routing is maintained by the HAL's audio patches. The writer only reads its
// own atomic route, so sample processing never takes the patch-tracking mutex.
class SpeakerRoutes {
  public:
    std::shared_ptr<SpeakerRoute> open(audio_io_handle_t io, audio_devices_t device);
    void updatePatch(audio_patch_handle_t previous, audio_patch_handle_t patch,
                     size_t sourceCount, const audio_port_config* sources,
                     size_t sinkCount, const audio_port_config* sinks);
    void releasePatch(audio_patch_handle_t patch);

  private:
    struct Output {
        std::weak_ptr<SpeakerRoute> route;
        audio_patch_handle_t patch = AUDIO_PATCH_HANDLE_NONE;
    };

    std::mutex mMutex;
    std::unordered_map<audio_io_handle_t, Output> mOutputs;
};

class SpeakerDolby {
  public:
    SpeakerDolby() = default;
    ~SpeakerDolby();
    SpeakerDolby(const SpeakerDolby&) = delete;
    SpeakerDolby& operator=(const SpeakerDolby&) = delete;

    bool configure(audio_format_t format, audio_channel_mask_t channels, uint32_t sampleRate);
    void process(void* data, size_t bytes, bool routedToSpeaker);

  private:
    static constexpr size_t kBlockFrames = 240;
    bool applyProfile(int profile);
    void processBlock(uint8_t* data, size_t frames, bool fadingOut);

    audio_effect_library_t* mLibrary = nullptr;
    effect_handle_t mEffect = nullptr;
    audio_format_t mFormat = AUDIO_FORMAT_INVALID;
    uint32_t mSampleRate = 0;
    uint32_t mAudioChannels = 0;
    size_t mFrameSize = 0;
    uint32_t mFadeFrames = 0;
    uint32_t mWetFrames = 0;
    float mGain = 1.0f;
    int mActiveProfile = -1;
    int mRequestedProfile = -1;
    std::array<float, 2 * kBlockFrames> mDry{};
    std::array<float, 2 * kBlockFrames> mWet{};
};

}  // namespace gts9u::audio
