#include "worker.h"
#include "audio.h"
#include "storage.h"

#include <algorithm>
#include <exception>
#include <utility>

namespace handheld {
namespace {
class SystemClock final : public console::Clock {
public:
    std::uint64_t monotonicMs() override {
        const std::uint64_t ticks = svcGetSystemTick();
        return ticks / SYSCLOCK_ARM11 * 1000 + ticks % SYSCLOCK_ARM11 * 1000 / SYSCLOCK_ARM11;
    }
    // osGetTime counts milliseconds since 1900-01-01.
    std::int64_t wallMs() override { return static_cast<std::int64_t>(osGetTime()) - 2208988800000LL; }
};
} // namespace

Worker::Worker() { LightLock_Init(&lock_); }
Worker::~Worker() {
    if (thread_) {
        post({Request::Quit});
        threadJoin(thread_, U64_MAX);
        threadFree(thread_);
    }
}

bool Worker::start() {
    // Run just below the UI thread, which sleeps on every vertical sync, so
    // handshakes and signature checks never stall drawing or the keyboard.
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    thread_ = threadCreate(entry, this, 256 * 1024, std::min<s32>(priority + 1, 0x3F), -2, false);
    return thread_ != nullptr;
}

void Worker::post(Request request) {
    LightLock_Lock(&lock_);
    // Input can produce controls every frame. Keep only the newest controls
    // without displacing reliable button/chat requests or reordering them.
    if (request.kind == Request::Controls && !requests_.empty() &&
        requests_.back().kind == Request::Controls) requests_.back() = std::move(request);
    else requests_.push_back(std::move(request));
    LightLock_Unlock(&lock_);
}
void Worker::connect(const Settings& settings) { post({Request::Connect, settings}); }
void Worker::disconnect() { post({Request::Disconnect}); }
void Worker::join(const std::string& link) { post({Request::Join, {}, link}); }
void Worker::leave(const std::string& community) { post({Request::Leave, {}, community}); }
void Worker::select(const std::string& community, const std::string& channel) {
    post({Request::Select, {}, community, channel});
}
void Worker::chat(const std::string& body) { post({Request::Chat, {}, body}); }
void Worker::voice(const std::string& community, const std::string& channel) {
    post({Request::Voice, {}, community, channel});
}
void Worker::suspend() { post({Request::Suspend}); }
void Worker::controls(bool muted, bool deafened, bool pushToTalk, bool held) {
    Request request{Request::Controls};
    request.muted = muted;
    request.deafened = deafened;
    request.pushToTalk = pushToTalk;
    request.held = held;
    post(std::move(request));
}

bool Worker::snapshot(console::View& view) {
    LightLock_Lock(&lock_);
    const bool changed = publishedRevision_ != copiedRevision_;
    if (changed) {
        view = published_;
        copiedRevision_ = publishedRevision_;
    }
    LightLock_Unlock(&lock_);
    return changed;
}

void Worker::publish(console::View view) {
    LightLock_Lock(&lock_);
    published_ = std::move(view);
    ++publishedRevision_;
    LightLock_Unlock(&lock_);
}

void Worker::entry(void* self) { static_cast<Worker*>(self)->run(); }

void Worker::run() {
    SdStorage storage(kCommunityDirectory);
    SystemClock clock;
    console::Session session(storage, clock);
    Audio audio;
    session.audioReceived = [&](const std::string& peer, const std::string& pcm) {
        audio.mixer.receive(peer, pcm);
    };
    session.speakerLeft = [&](const std::string& peer) { audio.mixer.remove(peer); };
    const bool wifiService = R_SUCCEEDED(acInit());
    bool wifi = true, shownWifi = true, quit = false;
    std::uint64_t shownRevision = ~std::uint64_t(0), nextPublish = 0, nextWifi = 0;
    const auto status = [&](const std::string& text) {
        session.view.status = text;
        ++session.view.revision;
    };
    const auto report = [&] {
        console::View view = session.view;
        if (!wifi) view.status = "Wi-Fi disconnected.";
        publish(std::move(view));
        shownRevision = session.view.revision;
        shownWifi = wifi;
    };
    while (!quit) {
        std::deque<Request> requests;
        LightLock_Lock(&lock_);
        requests.swap(requests_);
        LightLock_Unlock(&lock_);
        for (const auto& request : requests) {
            try {
                switch (request.kind) {
                case Request::Quit: quit = true; break;
                case Request::Connect:
                    audio.stop();
                    // Loading saved communities and DNS block this thread.
                    // Publish progress first so Connect never looks ignored.
                    status("Connecting to " + request.settings.relay + "...");
                    report();
                    session.start(request.settings.name, request.settings.relay, request.settings.color);
                    break;
                case Request::Disconnect:
                    audio.stop();
                    session.stop("Disconnected.");
                    break;
                case Request::Join: session.join(request.a); break;
                case Request::Leave: session.leave(request.a); break;
                case Request::Select: session.select(request.a, request.b); break;
                case Request::Chat: session.sendChat(request.a); break;
                case Request::Voice:
                    if (request.b.empty()) session.leaveVoice();
                    else {
                        std::string error;
                        if (audio.running() || audio.start(error)) session.joinVoice(request.a, request.b);
                        else status(error);
                    }
                    break;
                case Request::Controls:
                    session.controls(request.muted, request.deafened, request.pushToTalk, request.held);
                    break;
                case Request::Suspend:
                    // Closing the lid or leaving the application always stops
                    // recording. Chat stays connected, or the session reconnects
                    // on its own, but voice must be joined again.
                    session.leaveVoice();
                    audio.stop();
                    break;
                }
            } catch (const std::exception& error) {
                status(error.what());
            }
        }
        if (quit) break;
        const auto now = clock.monotonicMs();
        try {
            session.poll();
            if (session.view.voiceChannel.empty() && audio.running()) audio.stop();
            audio.tick(session, now);
        } catch (const std::exception& error) {
            status(error.what());
        }
        if (wifiService && now >= nextWifi) {
            u32 connected = 1;
            wifi = R_FAILED(ACU_GetWifiStatus(&connected)) || connected != 0;
            nextWifi = now + 1000;
        }
        if ((session.view.revision != shownRevision || wifi != shownWifi) && now >= nextPublish) {
            report();
            nextPublish = now + 33;
        }
        svcSleepThread(5 * 1000 * 1000LL);
    }
    session.stop("Disconnected.");
    audio.stop();
    if (wifiService) acExit();
}

} // namespace handheld
