#pragma once

#include "console/session.h"
#include "settings.h"
#include <3ds.h>

#include <cstdint>
#include <deque>
#include <string>

namespace handheld {

// Owns the community session on one thread: mbedTLS is built without
// threading, and DNS, TLS handshakes and signature checks block.
class Worker {
public:
    Worker();
    ~Worker();
    bool start();
    void connect(const Settings& settings);
    void disconnect();
    void join(const std::string& link);
    void leave(const std::string& community);
    void select(const std::string& community, const std::string& channel = {});
    void chat(const std::string& body);
    // An empty channel leaves voice.
    void voice(const std::string& community, const std::string& channel);
    void controls(bool muted, bool deafened, bool pushToTalk, bool held);
    void suspend();
    // Copies the newest published view; false if nothing changed since the last copy.
    bool snapshot(console::View& view);

private:
    struct Request {
        enum Kind { Connect, Disconnect, Join, Leave, Select, Chat, Voice, Controls, Suspend, Quit } kind;
        Settings settings;
        std::string a, b;
        bool muted = false, deafened = false, pushToTalk = false, held = false;
    };
    LightLock lock_;
    Thread thread_ = nullptr;
    std::deque<Request> requests_;
    console::View published_;
    std::uint64_t publishedRevision_ = 0;
    std::uint64_t copiedRevision_ = ~std::uint64_t(0);
    void post(Request request);
    void publish(console::View view);
    void run();
    static void entry(void* self);
};

} // namespace handheld
