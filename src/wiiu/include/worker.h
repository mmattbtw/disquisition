#pragma once

#include "session.h"
#include "settings.h"
#include <coreinit/mutex.h>
#include <coreinit/thread.h>

namespace wiiu {

struct ReceivedAudio { std::string sender, pcm; };

// DNS and sockets stay on this thread. Audio hardware and UI stay on the main
// thread, so foreground release can stop the microphone synchronously.
class Worker {
public:
    Worker();
    ~Worker();
    bool start();
    void join(const Settings& settings);
    void leave();
    void chat(const std::string& body);
    void voice(bool enabled);
    void controls(bool muted, bool deafened, bool pushToTalk, bool held);
    void capture(const std::string& pcm);
    void foreground(bool active);
    handheld::View snapshot();
    std::deque<ReceivedAudio> takeAudio();
private:
    struct Request {
        enum Kind { Join, Leave, Chat, Voice, Controls, Capture, Foreground, Quit } kind;
        Settings settings{};
        std::string body;
        bool a = false, b = false, c = false, d = false;
    };
    OSMutex mutex_{};
    OSThread* thread_ = nullptr;
    void* stack_ = nullptr;
    std::deque<Request> requests_;
    std::deque<ReceivedAudio> audio_;
    handheld::View published_;
    void post(Request request);
    static int entry(int, const char** self);
    void run();
};

std::uint64_t nowMilliseconds();

} // namespace wiiu
