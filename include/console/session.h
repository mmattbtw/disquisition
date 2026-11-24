#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "console/client.h"

// Platform-independent console state. A platform worker thread owns one
// Session; its UI thread reads copies of View. Voice audio uses the
// desktop's 20 ms, 16 kHz, 16-bit little-endian mono PCM frames.
namespace console {

constexpr std::size_t kSamples = 320;
constexpr std::size_t kPcmBytes = kSamples * 2;
constexpr std::size_t kMessageLimit = 128;
constexpr std::size_t kMemberLimit = 128;
using Samples = std::array<std::int16_t, kSamples>;

struct ChatLine {
    std::string peer;
    std::string sender;
    std::int64_t timestamp = 0; // milliseconds since the Unix epoch
    std::string body;
    std::string color;
    bool local = false;
    bool undelivered = false;
};

struct ChannelView {
    std::string id;
    std::string name;
    std::string type; // text or voice
    std::string topic;
    bool unread = false;
};

struct MemberView {
    std::string id;
    std::string name;
    bool self = false;
    bool online = false;
    bool admin = false;
    std::string voiceChannel;
    bool muted = false;
    bool deafened = false;
    bool speaking = false;
};

struct CommunityView {
    std::string id;
    std::string name;
    bool pending = false; // joining with an invite
    bool unread = false;
    std::vector<ChannelView> channels; // text channels, then voice; by name
    std::vector<MemberView> members;   // online first, then by name
};

struct View {
    bool started = false;
    bool relayConnected = false;
    std::string status = "Choose a relay and name to connect.";
    std::string name;
    std::string color = "mint";
    std::string peerId;
    std::vector<CommunityView> communities;
    std::string community; // selected community
    std::string channel;   // selected text channel
    std::vector<ChatLine> messages;
    std::uint64_t revision = 0;     // increases on every change
    std::uint64_t sentRevision = 0; // increases when this device sends chat
    std::string voiceCommunity;
    std::string voiceChannel;
    bool muted = false;
    bool deafened = false;
    bool pushToTalk = true;
    bool transmitting = false;
};

class Session {
public:
    Session(Storage& storage, Clock& clock);
    View view;
    std::function<void(const std::string& peer, const std::string& pcm)> audioReceived;
    std::function<void(const std::string& peer)> speakerLeft;

    // Loads saved communities and connects through the relay.
    bool start(const std::string& name, const std::string& relay, const std::string& color);
    void stop(const std::string& status);
    void join(const std::string& link);
    void leave(const std::string& community);
    void select(const std::string& community, const std::string& channel = {});
    bool sendChat(const std::string& body);
    // The worker starts the microphone first.
    bool joinVoice(const std::string& community, const std::string& channel);
    void leaveVoice();
    void controls(bool muted, bool deafened, bool pushToTalk, bool held);
    void capture(const std::string& pcm);
    void poll();
    Client& client() { return client_; }

private:
    Clock& clock_;
    Client client_;
    bool held_ = false;
    bool dirty_ = true;
    std::map<std::string, std::deque<ChatLine>> history_; // community/channel
    std::set<std::string> unread_;
    std::map<std::string, std::uint64_t> lastAudio_;
    std::set<std::string> speaking_;
    std::set<std::string> speakers_;
    std::uint64_t activityCheck_ = 0;
    void choose(const std::string& community, const std::string& channel = {});
    void handle(const Event& event);
    void append(const std::string& key, ChatLine line);
    void publishVoice();
    void rebuild();
    void status(const std::string& text);
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

// Linear interpolation from a microphone rate (the 3DS uses 16364.479 Hz) to
// 16000 Hz. Phase survives capture chunks and ring-buffer wraparounds.
class Resampler {
public:
    explicit Resampler(double inputRate = 16364.479) : step_(inputRate / 16000.0) {}
    void reset();
    bool push(std::int16_t sample, Samples& frame);

private:
    double step_;
    std::int16_t previous_ = 0;
    bool first_ = true;
    double phase_ = 0;
    Samples pending_{};
    std::size_t count_ = 0;
};

} // namespace console
