#include "worker.h"
#include "audio.h"
#include "transport.h"

#include <ctime>
#include <utility>

namespace handheld {

Worker::Worker() { LightLock_Init(&lock_); }
Worker::~Worker() {
    if (thread_) {
        post({Request::Quit, {}, {}});
        threadJoin(thread_, U64_MAX);
        threadFree(thread_);
    }
}

bool Worker::start() {
    thread_ = threadCreate(entry, this, 64 * 1024, 0x30, -2, false);
    return thread_ != nullptr;
}

void Worker::post(Request request) {
    LightLock_Lock(&lock_);
    // Input can produce controls every frame. Keep only the newest controls
    // without displacing reliable button/chat requests or reordering joins.
    if (request.kind == Request::Controls && !requests_.empty() &&
        requests_.back().kind == Request::Controls) requests_.back() = std::move(request);
    else requests_.push_back(std::move(request));
    LightLock_Unlock(&lock_);
}
void Worker::join(const Settings& settings) { post({Request::Join, settings, {}}); }
void Worker::leave() { post({Request::Leave, {}, {}}); }
void Worker::chat(const std::string& body) { post({Request::Chat, {}, body}); }
void Worker::voice(bool join) { post({Request::Voice, {}, {}, join}); }
void Worker::suspend() { post({Request::Suspend, {}, {}}); }
void Worker::controls(bool muted, bool deafened, bool pushToTalk, bool held) {
    post({Request::Controls, {}, {}, muted, deafened, pushToTalk, held});
}
View Worker::snapshot() {
    LightLock_Lock(&lock_);
    View copy = published_;
    LightLock_Unlock(&lock_);
    return copy;
}
void Worker::entry(void* self) { static_cast<Worker*>(self)->run(); }

void Worker::run() {
    Session session;
    Transport network;
    Audio audio;
    session.audioReceived = [&](const std::string& sender, const std::string& pcm) {
        audio.mixer.receive(sender, pcm);
    };
    Settings settings;
    bool wanted = false, quit = false, loggedIn = false;
    std::uint64_t retry = 0, signInDeadline = 0, publish = 0;
    std::uint64_t historyDeadline = 0, nextHistory = 0;
    while (!quit) {
        const auto now = static_cast<std::uint64_t>(osGetTime());
        std::deque<Request> requests;
        LightLock_Lock(&lock_);
        requests.swap(requests_);
        LightLock_Unlock(&lock_);
        for (const auto& request : requests) {
            switch (request.kind) {
            case Request::Quit: quit = true; wanted = false; break;
            case Request::Join:
                settings = request.settings;
                network.close(); audio.stop();
                session.disconnected("Connecting to relay...");
                session.view.color = settings.color;
                session.controls(false, false, settings.pushToTalk, false);
                wanted = true; loggedIn = false; retry = 0;
                historyDeadline = 0;
                break;
            case Request::Leave:
                wanted = false;
                historyDeadline = 0;
                audio.stop(); network.close();
                session.disconnected("Disconnected. Tap Join to return.");
                break;
            case Request::Suspend:
                // Closing the lid/app suspension always stops recording. Chat
                // reconnects, but the user must explicitly join voice again.
                audio.stop(); network.close();
                session.disconnected("Connection paused. Reconnecting...");
                loggedIn = false; retry = now + 3000;
                historyDeadline = 0;
                break;
            case Request::Chat:
                if (!session.sendChat(request.body, std::time(nullptr)))
                    session.view.status = "Wait until you have joined to send.";
                break;
            case Request::Voice:
                if (request.a && session.view.connected && !session.view.voice) {
                    std::string error;
                    if (audio.start(error)) session.setVoice(true);
                    else session.view.status = error;
                } else if (!request.a) {
                    session.setVoice(false); audio.stop();
                }
                break;
            case Request::Controls:
                session.controls(request.a, request.b, request.c, request.d);
                break;
            }
        }
        if (quit) break;
        if (wanted && (network.state() == Transport::State::Closed ||
                       network.state() == Transport::State::Failed) && now >= retry) {
            session.view.status = "Connecting to " + settings.host + "...";
            network.open(settings.host, settings.port, now);
            loggedIn = false;
            signInDeadline = now + 15000;
        }
        std::vector<chat::Message> messages;
        network.poll(now, messages);
        if (network.state() == Transport::State::Connected && !loggedIn) {
            session.begin(settings.name);
            loggedIn = true;
            historyDeadline = 0;
            nextHistory = now + 10000;
        }
        for (const auto& message : messages) {
            session.receive(message, now);
            if (message.type == chat::MsgType::LoginOk) historyDeadline = now + 15000;
            if (message.type == chat::MsgType::HistoryEnd) {
                historyDeadline = 0;
                nextHistory = now + 10000;
            }
            if (message.type == chat::MsgType::PeerLeft && !message.fields.empty())
                audio.mixer.remove(message.fields[0]);
        }
        if (!session.view.voice && audio.running()) audio.stop();
        if (session.view.connected && now >= nextHistory && historyDeadline == 0) {
            network.queue({chat::MsgType::FetchHistory, {}});
            historyDeadline = now + 15000;
        }
        audio.tick(session, now);
        for (const auto& message : session.takeOutgoing()) {
            if (!network.queue(message)) break;
        }
        std::string failure;
        if (wanted && network.state() == Transport::State::Failed) failure = network.error();
        else if (wanted && loggedIn && !session.view.connected && now >= signInDeadline)
            failure = "Relay cannot sign in to the server.";
        else if (wanted && historyDeadline != 0 && now >= historyDeadline)
            failure = "Server stopped responding.";
        if (!failure.empty()) {
            audio.stop(); network.close();
            session.disconnected(failure + " Retrying in 3 seconds.");
            retry = now + 3000;
            historyDeadline = 0;
            loggedIn = false;
        }
        if (now >= publish) {
            LightLock_Lock(&lock_);
            published_ = session.view;
            LightLock_Unlock(&lock_);
            publish = now + 33;
        }
        svcSleepThread(5 * 1000 * 1000LL);
    }
    audio.stop();
    network.close();
}

} // namespace handheld
