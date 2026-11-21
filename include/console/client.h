#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "console/model.h"
#include "console/net.h"

// A console member of desktop communities: the subset of community::Node
// (src/community/node.cpp) a relay-only client needs. It is driven by poll()
// on one worker thread and opens no listening port.
namespace console {

// Persistent files are small and flat. Consoles supply SD card access.
class Storage {
public:
    virtual ~Storage() = default;
    // false if missing; throws if unreadable or larger than limit.
    virtual bool read(const std::string& name, std::string& bytes, std::size_t limit) = 0;
    virtual void write(const std::string& name, const std::string& bytes) = 0; // atomic; throws
    virtual void remove(const std::string& name) = 0;
    virtual std::vector<std::string> list(const std::string& suffix) = 0;
};

class Clock {
public:
    virtual ~Clock() = default;
    virtual std::uint64_t monotonicMs() = 0;
    virtual std::int64_t wallMs() = 0; // milliseconds since the Unix epoch
};

struct Presence {
    std::string voiceChannel;
    bool muted = false;
    bool deafened = false;
    bool sharing = false;
};

struct Event {
    enum class Kind { Changed, Joined, JoinRejected, Error, Packet, VoiceMoved };
    Kind kind = Kind::Changed;
    std::string community;
    std::string channel;
    std::string peer;
    std::string type; // packet kind
    std::string message;
    json::Value data;
};

class Client {
public:
    Client(Storage& storage, Clock& clock);
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // Loads the identity and communities. Throws for unreadable data.
    void load();
    bool start(const std::string& displayName, const std::string& relay);
    void stop();
    bool running() const { return running_; }
    void poll();
    std::vector<Event> takeEvents();

    std::string peerId() const { return identity_ ? identity_->id() : std::string{}; }
    const std::string& name() const { return name_; }
    const std::string& endpoint() const { return endpoint_; }
    const std::map<std::string, Model>& communities() const { return models_; }
    std::set<std::string> pendingCommunities() const;
    bool relayConnected() const { return relayRegistered_; }
    const std::string& relayError() const { return relayError_; }

    void joinCommunity(const std::string& link); // throws for an invalid link
    void leaveCommunity(const std::string& community);
    int sendPacket(const std::string& community, const std::string& channel,
                   const std::string& type, json::Object data);
    void setVoice(const std::string& community, const std::string& channel, bool muted = false,
                  bool deafened = false);
    std::set<std::string> online(const std::string& community) const;
    Presence presence(const std::string& community, const std::string& peer) const;
    std::size_t connectionCount() const { return peers_.size(); }
    bool synchronizing(const std::string& community) const;

private:
    struct Link;
    struct Retry {
        std::uint64_t due = 0;
        int failures = 0;
    };
    std::uint64_t now() const { return clock_.monotonicMs(); }
    Link* makeLink(bool outgoing, const std::string& expected = {});
    void dropLink(Link* link, const std::string& reason = {});
    void pollLink(Link* link);
    void encrypted(Link* link);
    void handle(Link* link, const json::Value& message);
    void mergeSnapshot(Link* link, const std::string& community, const json::Value& snapshot);
    void send(Link* link, const json::Object& message);
    std::vector<Link*> connections() const;
    void sendEach(Link* link, const std::function<void()>& action);
    void pump(Link* link);
    void sendState(Link* link, const std::string& community);
    void offer(Link* link);
    void authorize(Link* link, const std::string& community);
    void broadcastState(const std::string& community);
    void dial(const std::string& peer, const std::string& endpoint,
              const std::string& discover = {});
    void tick();
    void pollRelay();
    void registerRelay();
    void save(Model model);
    void processAdmissions();
    void publishProfile(const std::string& community);
    void persistPending();
    void notify(Event::Kind kind, const std::string& community = {}, const std::string& message = {});
    bool writable(const Link* link, const std::string& community) const;
    bool validPacket(const std::string& community, const std::string& channel,
                     const std::string& type, const json::Value& data,
                     const std::string& peer) const;
    json::Object presenceObject(const std::string& community, const Presence& presence) const;

    Storage& storage_;
    Clock& clock_;
    std::optional<Identity> identity_;
    std::unique_ptr<Credentials> credentials_;
    std::map<std::string, Model> models_;
    std::map<std::string, Invite> pending_;
    std::map<std::string, Link*> joining_;
    std::map<std::string, std::uint64_t> joinStarted_;
    std::vector<std::unique_ptr<Link>> links_;
    std::map<std::string, Link*> peers_;
    std::map<std::string, Retry> retries_;
    std::map<std::string, std::map<std::string, Presence>> presence_;
    std::map<std::string, Presence> mine_;
    std::unique_ptr<Stream> relay_;
    std::string relayEndpoint_;
    std::string relayNonce_;
    std::string relayError_;
    bool relayRegistered_ = false;
    std::uint64_t relayDue_ = 0;
    std::uint64_t relayLastRead_ = 0;
    std::uint64_t relayPinged_ = 0;
    std::uint64_t lastTick_ = 0;
    std::string endpoint_;
    std::string name_;
    bool running_ = false;
    std::uint64_t recoveringUntil_ = 0;
    std::set<std::string> seenMessages_;
    std::deque<std::string> messageOrder_;
    std::vector<Event> events_;
};

} // namespace console
