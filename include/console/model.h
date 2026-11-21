#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "console/crypto.h"
#include "console/json.h"

// A Qt-free port of community::Model (src/community/model.cpp). Peers merge
// signed operation logs and must derive identical membership, so the rules
// here follow the desktop implementation line for line.
namespace console {

constexpr std::size_t kMaxMetadataBytes = 8 * 1024 * 1024;
constexpr int kMaxOperations = 8192;
constexpr std::size_t kMaxIconBytes = 128 * 1024;
constexpr std::size_t kMaxMembers = 1024;
constexpr std::size_t kMaxBioLength = 500;
constexpr std::size_t kMaxAvatarBytes = 32 * 1024;

struct Member {
    std::string id;
    std::string name;
    std::string endpoint;
    bool admin = false;
};
struct Channel {
    std::string id;
    std::string name;
    std::string type; // text or voice
    std::string topic;
};
struct Invite {
    std::string community;
    std::string owner;
    std::string code;
    json::Array seeds; // {peer, endpoint}: routing hints, never authority
    static Invite fromLink(const std::string& link); // throws for invalid links
};

class Model {
public:
    static Model fromJson(const json::Value& object, const std::string& expectedId = {},
                          const std::string& expectedOwner = {});
    json::Object toJson() const;
    bool merge(const json::Value& snapshot); // atomic; throws on malformed input
    json::Object change(const Identity& author, const std::string& kind, json::Object data);
    json::Object admit(const Identity& host, const json::Object& intent);
    bool acceptsIntent(const std::string& host, const json::Object& intent) const;
    bool isMember(const std::string& peer) const { return !deleted && members.count(peer); }
    bool canAdmin(const std::string& peer) const;
    std::string revision() const;
    std::string host(const std::set<std::string>& online) const;
    std::size_t operationCount() const { return operations_.size(); }

    std::string id;
    std::string owner;
    bool deleted = false;
    std::string name;
    std::map<std::string, Member> members;
    std::map<std::string, Channel> channels;
    std::vector<std::string> priority;
    std::set<std::string> revokedInvites;
    std::set<std::string> banned;

private:
    static Model load(const json::Value& object, const std::string& expectedId,
                      const std::string& expectedOwner, std::set<std::string> verified);
    void rebuild();
    json::Object append(const Identity& author, const std::string& kind, json::Object data);
    bool ancestor(const std::string& older, const std::string& newer) const;
    bool validIntent(const json::Object& intent) const;
    std::map<std::string, json::Value> operations_;
    std::map<std::string, json::Value> invites_;
    std::map<std::string, std::string> adminGrants_;
    std::set<std::string> heads_;
    int clock_ = 0;
    // Signatures already checked for this exact operation or intent. Snapshots
    // repeat the whole log; consoles verify only what they haven't seen.
    mutable std::set<std::string> verified_;
};

json::Object joinIntent(const Identity& peer, const Invite& invite, const std::string& name,
                        const std::string& endpoint, const std::string& host = {},
                        const std::string& nonce = {}, const std::string& request = {});
bool validEndpoint(const std::string& endpoint);
// A hostname or host:port. Returns "relay://host:port" (default port 3333).
std::string normalizeRelayEndpoint(const std::string& address);
// Parses a validated tcp:// or relay:// endpoint.
bool splitEndpoint(const std::string& endpoint, std::string& scheme, std::string& host,
                   unsigned& port);
bool label(const std::string& text, std::size_t maximum = 80);

} // namespace console
