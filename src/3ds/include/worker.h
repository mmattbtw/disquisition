#pragma once

#include "session.h"
#include "settings.h"
#include <3ds.h>

namespace handheld {

class Worker {
public:
    Worker();
    ~Worker();
    bool start();
    void join(const Settings& settings);
    void leave();
    void chat(const std::string& body);
    void voice(bool join);
    void controls(bool muted, bool deafened, bool pushToTalk, bool held);
    void suspend();
    View snapshot();
private:
    struct Request {
        enum Kind { Join, Leave, Chat, Voice, Controls, Suspend, Quit } kind;
        Settings settings;
        std::string body;
        bool a = false, b = false, c = false, d = false;
    };
    LightLock lock_;
    Thread thread_ = nullptr;
    std::deque<Request> requests_;
    View published_;
    void post(Request request);
    void run();
    static void entry(void* self);
};

} // namespace handheld
