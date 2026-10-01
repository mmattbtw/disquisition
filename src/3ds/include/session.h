#pragma once

#include "common/protocol.h"

#include <array>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace handheld {

constexpr std::size_t kSamples = 320;
constexpr std::size_t kPcmBytes = kSamples * 2;
constexpr std::size_t kMessageLimit = 128;
constexpr std::size_t kMemberLimit = 64;
using Samples = std::array<std::int16_t, kSamples>;

struct Member {
    std::string name;
    bool voice = false;
    bool muted = false;
    bool deafened = false;
    std::uint64_t lastAudio = 0;
};

struct View {
    bool connected = false;
    bool historyLoading = false;
    bool voice = false;
    bool muted = false;
    bool deafened = false;
    bool pushToTalk = true;
    bool transmitting = false;
    std::string name;
    std::string status = "Choose a relay and name to join.";
    std::string color = "mint";
    std::uint64_t revision = 0;
    std::vector<chat::ChatPayload> messages;
    std::map<std::string, Member> members;
};

// Platform-independent state machine. The 3DS worker owns this object; the UI
// reads copies of View. Both console builds and host tests use the real protocol.
class Session {
public:
    View view;
    std::function<void(const std::string&, const std::string&)> audioReceived;
    void begin(const std::string& name);
    void disconnected(const std::string& reason);
    void receive(const chat::Message& message, std::uint64_t now);
    bool sendChat(const std::string& body, std::int64_t timestamp);
    void setVoice(bool enabled);
    void controls(bool muted, bool deafened, bool pushToTalk, bool held);
    void capture(const std::string& pcm);
    std::vector<chat::Message> takeOutgoing();

private:
    std::vector<chat::Message> outgoing_;
    bool held_ = false;
    bool wireMuted_ = true;
    bool wireDeafened_ = false;
    Member* member(const std::string& name);
    void append(chat::ChatPayload message);
    void voiceState(bool force = false);
};

std::string encodePcm(const Samples& samples);
bool decodePcm(const std::string& pcm, Samples& samples);
bool speaking(const Samples& samples);

// Bounded per-user jitter queues. Each render mixes one 20 ms frame from every
// speaker rather than playing concurrent users sequentially. Old audio is dropped.
class Mixer {
public:
    void receive(const std::string& sender, const std::string& pcm);
    Samples render();
    void clear() { streams_.clear(); }
    void remove(const std::string& sender) { streams_.erase(sender); }
private:
    std::map<std::string, std::deque<Samples>> streams_;
};

// Linear interpolation from MICU_SAMPLE_RATE_16360 (16364.479 Hz) to 16000 Hz.
// Phase survives capture chunks and ring-buffer wraparounds.
class Resampler {
public:
    void reset();
    bool push(std::int16_t sample, Samples& frame);
private:
    std::int16_t previous_ = 0;
    bool first_ = true;
    double phase_ = 0;
    Samples pending_{};
    std::size_t count_ = 0;
};

} // namespace handheld
