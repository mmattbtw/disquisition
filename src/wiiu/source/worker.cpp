#include "worker.h"
#include "transport.h"

#include <coreinit/time.h>
#include <cstdlib>
#include <ctime>
#include <malloc.h>
#include <utility>

namespace wiiu {

std::uint64_t nowMilliseconds() { return OSTicksToMilliseconds(OSGetSystemTime()); }
Worker::Worker() { OSInitMutex(&mutex_); }
Worker::~Worker() {
    if (thread_) {
        post({Request::Quit, {}, {}});
        OSJoinThread(thread_, nullptr);
    }
    std::free(thread_);
    std::free(stack_);
}
bool Worker::start() {
    constexpr unsigned size = 256 * 1024;
    stack_ = memalign(16, size);
    thread_ = static_cast<OSThread*>(memalign(8, sizeof(OSThread)));
    if (!stack_ || !thread_ || !OSCreateThread(thread_, entry, 0, reinterpret_cast<char*>(this),
        static_cast<char*>(stack_) + size, size, 16, OS_THREAD_ATTRIB_AFFINITY_CPU2)) {
        std::free(thread_); std::free(stack_);
        thread_ = nullptr; stack_ = nullptr;
        return false;
    }
    OSResumeThread(thread_);
    return true;
}
void Worker::post(Request request) {
    OSLockMutex(&mutex_);
    if (request.kind == Request::Controls && !requests_.empty() &&
        requests_.back().kind == Request::Controls) requests_.back() = std::move(request);
    else if (request.kind != Request::Capture || requests_.size() < 16)
        requests_.push_back(std::move(request));
    OSUnlockMutex(&mutex_);
}
void Worker::join(const Settings& settings) { post({Request::Join, settings, {}}); }
void Worker::leave() { post({Request::Leave, {}, {}}); }
void Worker::chat(const std::string& body) { post({Request::Chat, {}, body}); }
void Worker::voice(bool enabled) { post({Request::Voice, {}, {}, enabled}); }
void Worker::controls(bool muted, bool deafened, bool ptt, bool held) {
    post({Request::Controls, {}, {}, muted, deafened, ptt, held});
}
void Worker::capture(const std::string& pcm) { post({Request::Capture, {}, pcm}); }
void Worker::foreground(bool active) { post({Request::Foreground, {}, {}, active}); }
handheld::View Worker::snapshot() {
    OSLockMutex(&mutex_);
    auto view = published_;
    OSUnlockMutex(&mutex_);
    return view;
}
std::deque<ReceivedAudio> Worker::takeAudio() {
    OSLockMutex(&mutex_);
    std::deque<ReceivedAudio> result;
    result.swap(audio_);
    OSUnlockMutex(&mutex_);
    return result;
}
int Worker::entry(int, const char** self) { reinterpret_cast<Worker*>(self)->run(); return 0; }

void Worker::run() {
    handheld::Session session;
    handheld::Transport network;
    session.audioReceived = [&](const std::string& sender, const std::string& pcm) {
        OSLockMutex(&mutex_);
        // The mixer imposes a second, per-speaker bound on the main thread.
        if (audio_.size() >= 48) audio_.pop_front();
        audio_.push_back({sender, pcm});
        OSUnlockMutex(&mutex_);
    };
    Settings settings;
    bool wanted = false, active = true, quit = false, loggedIn = false;
    std::uint64_t retry = 0, signInDeadline = 0, historyDeadline = 0, nextHistory = 0;
    while (!quit) {
        const auto now = nowMilliseconds();
        std::deque<Request> requests;
        OSLockMutex(&mutex_);
        requests.swap(requests_);
        OSUnlockMutex(&mutex_);
        for (const auto& request : requests) {
            switch (request.kind) {
            case Request::Quit: quit = true; wanted = false; break;
            case Request::Join:
                settings = request.settings;
                network.close(); session.disconnected("Connecting to relay...");
                session.view.color = settings.color;
                session.controls(false, false, settings.pushToTalk, false);
                wanted = true; loggedIn = false; retry = 0; historyDeadline = 0;
                break;
            case Request::Leave:
                wanted = false; historyDeadline = 0;
                network.close(); session.disconnected("Disconnected. Choose Join to return.");
                break;
            case Request::Foreground:
                active = request.a;
                network.close(); session.disconnected("Paused. Voice is off.");
                loggedIn = false; retry = 0; historyDeadline = 0;
                break;
            case Request::Chat:
                if (!session.sendChat(request.body, std::time(nullptr)))
                    session.view.status = "Join the relay before sending.";
                break;
            case Request::Voice: session.setVoice(request.a); break;
            case Request::Controls: session.controls(request.a, request.b, request.c, request.d); break;
            case Request::Capture: session.capture(request.body); break;
            }
        }
        if (quit) break;
        if (wanted && active && network.state() == handheld::Transport::State::Closed && now >= retry) {
            session.view.status = "Connecting to " + settings.host + ":" + std::to_string(settings.port);
            OSLockMutex(&mutex_); published_ = session.view; OSUnlockMutex(&mutex_);
            network.open(settings.host, settings.port, now);
            loggedIn = false; signInDeadline = nowMilliseconds() + 15000;
        }
        std::vector<chat::Message> messages;
        network.poll(now, messages);
        if (network.state() == handheld::Transport::State::Connected && !loggedIn) {
            session.begin(settings.name); loggedIn = true;
            historyDeadline = 0; nextHistory = now + 10000;
        }
        for (const auto& message : messages) {
            session.receive(message, now);
            if (message.type == chat::MsgType::LoginOk) historyDeadline = now + 15000;
            if (message.type == chat::MsgType::HistoryEnd) {
                historyDeadline = 0; nextHistory = now + 10000;
            }
        }
        if (session.view.connected && now >= nextHistory && historyDeadline == 0) {
            network.queue({chat::MsgType::FetchHistory, {}}); historyDeadline = now + 15000;
        }
        for (const auto& message : session.takeOutgoing()) if (!network.queue(message)) break;
        std::string failure;
        if (wanted && network.state() == handheld::Transport::State::Failed) failure = network.error();
        else if (wanted && loggedIn && !session.view.connected && now >= signInDeadline)
            failure = "Relay could not sign in to the server.";
        else if (wanted && historyDeadline && now >= historyDeadline) failure = "Server stopped responding.";
        if (!failure.empty()) {
            network.close(); session.disconnected(failure + " Retrying in 3 seconds.");
            loggedIn = false; historyDeadline = 0; retry = now + 3000;
        }
        OSLockMutex(&mutex_);
        published_ = session.view;
        if (!session.view.voice) audio_.clear();
        OSUnlockMutex(&mutex_);
        OSSleepTicks(OSMillisecondsToTicks(5));
    }
    network.close();
}

} // namespace wiiu
