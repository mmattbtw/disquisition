#pragma once

#include "console/session.h"
#include "settings.h"
#include <coreinit/mutex.h>
#include <coreinit/thread.h>
#include <deque>

namespace wiiu {

// An empty pcm means the speaker left voice.
struct ReceivedAudio { std::string sender, pcm; };

// The community client, its DNS, TLS and SD storage stay on this thread;
// mbedTLS is built without threading. Audio hardware and UI stay on the main
// thread, so foreground release can stop the microphone synchronously.
class Worker {
public:
    explicit Worker(std::string directory);
    ~Worker();
    bool start();
    void connect(const Settings& settings);
    void disconnect();
    void join(const std::string& link);
    void leave(const std::string& community);
    void select(const std::string& community, const std::string& channel);
    void chat(const std::string& body);
    // The main thread starts the microphone first.
    void joinVoice(const std::string& community, const std::string& channel);
    void leaveVoice();
    void controls(bool muted, bool deafened, bool pushToTalk, bool held);
    void capture(const std::string& pcm);
    // Copies the published view when it is newer than view.
    bool snapshot(console::View& view);
    std::deque<ReceivedAudio> takeAudio();
private:
    struct Request {
        enum Kind { Connect, Disconnect, Join, Leave, Select, Chat, JoinVoice, LeaveVoice,
            Controls, Capture, Quit } kind;
        std::string first{}, second{};
        Settings settings{};
        bool muted = false, deafened = false, pushToTalk = false, held = false;
    };
    std::string directory_;
    OSMutex mutex_{};
    OSThread* thread_ = nullptr;
    void* stack_ = nullptr;
    std::deque<Request> requests_;
    std::deque<ReceivedAudio> audio_;
    console::View published_;
    void post(Request request);
    void received(const std::string& sender, const std::string& pcm);
    static int entry(int, const char** self);
    void run();
};

std::uint64_t nowMilliseconds();

} // namespace wiiu
