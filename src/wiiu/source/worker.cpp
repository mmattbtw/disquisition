#include "worker.h"
#include "storage.h"

#include <coreinit/systeminfo.h>
#include <coreinit/time.h>
#include <cstdlib>
#include <malloc.h>
#include <utility>

namespace wiiu {

std::uint64_t nowMilliseconds() { return OSTicksToMilliseconds(OSGetSystemTime()); }

namespace {
class SystemClock final : public console::Clock {
public:
    std::uint64_t monotonicMs() override { return nowMilliseconds(); }
    std::int64_t wallMs() override {
        // OSGetTime counts from 2000-01-01. OSTicksToMilliseconds would
        // overflow multiplying that many ticks by 1000, so split the division.
        const auto ticks = static_cast<std::uint64_t>(OSGetTime());
        const std::uint64_t rate = OSTimerClockSpeed;
        return static_cast<std::int64_t>(ticks / rate * 1000 + ticks % rate * 1000 / rate) +
            946684800000LL;
    }
};

void failure(console::Session& session, const std::string& text) {
    if (session.view.status == text) return;
    session.view.status = text;
    ++session.view.revision;
}
}

Worker::Worker(std::string directory) : directory_(std::move(directory)) { OSInitMutex(&mutex_); }
Worker::~Worker() {
    if (thread_) {
        post({Request::Quit});
        OSJoinThread(thread_, nullptr);
    }
    std::free(thread_);
    std::free(stack_);
}
bool Worker::start() {
    // TLS handshakes and Ed25519 verification run on this stack.
    constexpr unsigned size = 512 * 1024;
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
void Worker::connect(const Settings& settings) {
    Request request{Request::Connect};
    request.settings = settings;
    post(std::move(request));
}
void Worker::disconnect() { post({Request::Disconnect}); }
void Worker::join(const std::string& link) { post({Request::Join, link}); }
void Worker::leave(const std::string& community) { post({Request::Leave, community}); }
void Worker::select(const std::string& community, const std::string& channel) {
    post({Request::Select, community, channel});
}
void Worker::chat(const std::string& body) { post({Request::Chat, body}); }
void Worker::joinVoice(const std::string& community, const std::string& channel) {
    post({Request::JoinVoice, community, channel});
}
void Worker::leaveVoice() { post({Request::LeaveVoice}); }
void Worker::controls(bool muted, bool deafened, bool pushToTalk, bool held) {
    Request request{Request::Controls};
    request.muted = muted; request.deafened = deafened;
    request.pushToTalk = pushToTalk; request.held = held;
    post(std::move(request));
}
void Worker::capture(const std::string& pcm) { post({Request::Capture, pcm}); }
bool Worker::snapshot(console::View& view) {
    OSLockMutex(&mutex_);
    const bool newer = published_.revision != view.revision;
    if (newer) view = published_;
    OSUnlockMutex(&mutex_);
    return newer;
}
std::deque<ReceivedAudio> Worker::takeAudio() {
    OSLockMutex(&mutex_);
    std::deque<ReceivedAudio> result;
    result.swap(audio_);
    OSUnlockMutex(&mutex_);
    return result;
}
void Worker::received(const std::string& sender, const std::string& pcm) {
    OSLockMutex(&mutex_);
    // The mixer imposes a second, per-speaker bound on the main thread.
    if (audio_.size() >= 48) audio_.pop_front();
    audio_.push_back({sender, pcm});
    OSUnlockMutex(&mutex_);
}
int Worker::entry(int, const char** self) { reinterpret_cast<Worker*>(self)->run(); return 0; }

void Worker::run() {
    SdStorage storage(directory_ + "/community");
    SystemClock clock;
    console::Session session(storage, clock);
    session.audioReceived = [this](const std::string& peer, const std::string& pcm) { received(peer, pcm); };
    session.speakerLeft = [this](const std::string& peer) { received(peer, {}); };
    std::uint64_t publishedRevision = 0, publishedAt = 0;
    bool quit = false;
    while (!quit) {
        std::deque<Request> requests;
        OSLockMutex(&mutex_);
        requests.swap(requests_);
        OSUnlockMutex(&mutex_);
        for (const auto& request : requests) {
            try {
                switch (request.kind) {
                case Request::Quit: quit = true; break;
                case Request::Connect:
                    session.start(request.settings.name, request.settings.relay, request.settings.color);
                    break;
                case Request::Disconnect: session.stop("Disconnected."); break;
                case Request::Join: session.join(request.first); break;
                case Request::Leave: session.leave(request.first); break;
                case Request::Select: session.select(request.first, request.second); break;
                case Request::Chat: session.sendChat(request.first); break;
                case Request::JoinVoice: session.joinVoice(request.first, request.second); break;
                case Request::LeaveVoice: session.leaveVoice(); break;
                case Request::Controls:
                    session.controls(request.muted, request.deafened, request.pushToTalk, request.held);
                    break;
                case Request::Capture: session.capture(request.first); break;
                }
            } catch (const std::exception& error) {
                failure(session, error.what());
            }
        }
        if (quit) break;
        try {
            session.poll();
        } catch (const std::exception& error) {
            failure(session, error.what());
        }
        const auto now = nowMilliseconds();
        OSLockMutex(&mutex_);
        if (session.view.voiceChannel.empty()) audio_.clear();
        // Copying the view is not free; the UI needs it at most 30 times a second.
        if (session.view.revision != publishedRevision && now - publishedAt >= 33) {
            published_ = session.view;
            publishedRevision = session.view.revision;
            publishedAt = now;
        }
        OSUnlockMutex(&mutex_);
        OSSleepTicks(OSMillisecondsToTicks(5));
    }
    session.stop("Closed.");
}

} // namespace wiiu
