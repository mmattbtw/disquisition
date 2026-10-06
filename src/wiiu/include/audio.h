#pragma once

#include "session.h"
#include "microphone_frames.h"
#include <mic/mic.h>
#include <sndcore2/voice.h>
#include <atomic>

namespace wiiu {

class Worker;

class Audio {
public:
    ~Audio() { stop(); }
    bool start(std::string& error);
    void stop();
    bool running() const { return microphone_ != nullptr && voice_ != nullptr; }
    // transmit is the current UI gate, independent of an older worker snapshot.
    bool tick(const handheld::View& view, Worker& worker, bool transmit, std::uint64_t now,
              std::string& error);
private:
    static constexpr unsigned kMicSamples = 0x8000;
    static constexpr unsigned kSegments = 4;
    MICHandle handle_ = -1;
    MICWorkMemory memory_{};
    std::int16_t* microphone_ = nullptr;
    std::int16_t* playback_ = nullptr;
    AXVoice* voice_ = nullptr;
    bool ownsAx_ = false;
    bool transmitting_ = false;
    bool deafened_ = false;
    unsigned segment_ = 0;
    std::uint64_t lastTick_ = 0;
    std::atomic<std::uint32_t> heartbeat_{0};
    std::atomic<bool> underrun_{false};
    bool callbackRegistered_ = false;
    static Audio* current_;
    static void audioFrame();
    MicrophoneFrames frames_;
    handheld::Mixer mixer_;
    void resetPlayback(bool playing);
};

} // namespace wiiu
