#include "desktop/system_audio_capture.h"

#if defined(Q_OS_WIN)

// clang-format off: COM declarations require Windows types first.
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
// clang-format on
#include <wrl/client.h>
#if __has_include(<audioclientactivationparams.h>)
#include <audioclientactivationparams.h>
#else
// From the Windows SDK, which MinGW-w64 does not ship yet.
enum AUDIOCLIENT_ACTIVATION_TYPE {
    AUDIOCLIENT_ACTIVATION_TYPE_DEFAULT = 0,
    AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK = 1,
};
enum PROCESS_LOOPBACK_MODE {
    PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE = 0,
    PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE = 1,
};
struct AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS {
    DWORD TargetProcessId;
    PROCESS_LOOPBACK_MODE ProcessLoopbackMode;
};
struct AUDIOCLIENT_ACTIVATION_PARAMS {
    AUDIOCLIENT_ACTIVATION_TYPE ActivationType;
    union {
        AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS ProcessLoopbackParams;
    };
};
#endif

#include <atomic>
#include <future>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kProcessLoopbackDevice[] = L"VAD\\Process_Loopback";
constexpr REFERENCE_TIME kBufferDuration = 200000; // 20 ms, in 100 ns units

// Signals when ActivateAudioInterfaceAsync finishes. Windows calls it on a
// worker thread, so it must be agile.
class ActivationWaiter final : public IActivateAudioInterfaceCompletionHandler,
                               public IAgileObject {
public:
    ActivationWaiter() : done_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~ActivationWaiter() { CloseHandle(done_); }
    void wait() const { WaitForSingleObject(done_, INFINITE); }

    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation*) override {
        SetEvent(done_);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override {
        if (id == __uuidof(IUnknown) || id == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
            *object = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        }
        else if (id == __uuidof(IAgileObject)) {
            *object = static_cast<IAgileObject*>(this);
        }
        else {
            *object = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --references_;
        if (left == 0) delete this;
        return left;
    }

private:
    std::atomic<ULONG> references_ {1};
    HANDLE done_;
};

// Process loopback captures one process tree's sound, or everything but it.
ComPtr<IAudioClient> activateLoopback(DWORD processId, PROCESS_LOOPBACK_MODE mode) {
    AUDIOCLIENT_ACTIVATION_PARAMS params {};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = processId;
    params.ProcessLoopbackParams.ProcessLoopbackMode = mode;
    PROPVARIANT activation {};
    activation.vt = VT_BLOB;
    activation.blob.cbSize = sizeof(params);
    activation.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

    ComPtr<ActivationWaiter> waiter;
    waiter.Attach(new ActivationWaiter);
    ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
    if (FAILED(ActivateAudioInterfaceAsync(kProcessLoopbackDevice, __uuidof(IAudioClient),
                                           &activation, waiter.Get(), &operation))) {
        return nullptr;
    }
    waiter->wait();
    HRESULT result = E_FAIL;
    ComPtr<IUnknown> client;
    if (FAILED(operation->GetActivateResult(&result, &client)) || FAILED(result)) {
        return nullptr;
    }
    ComPtr<IAudioClient> audio;
    client.As(&audio);
    return audio;
}

} // namespace

struct SystemAudioCapture::State {
    std::thread thread;
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ~State() { CloseHandle(stop); }

    // Runs on the capture thread. Reports whether capture started through
    // `started`, then delivers sound until `stop` is set.
    void run(DWORD processId, PROCESS_LOOPBACK_MODE mode, const ShareAudioCallback& samples,
             std::promise<QString>& started) const {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const QString error = capture(processId, mode, samples, started);
        if (!error.isEmpty()) started.set_value(error);
        CoUninitialize();
    }

    // Returns an error only when capture never started.
    QString capture(DWORD processId, PROCESS_LOOPBACK_MODE mode, const ShareAudioCallback& samples,
                    std::promise<QString>& started) const {
        const ComPtr<IAudioClient> client = activateLoopback(processId, mode);
        if (!client) {
            return "Windows 10 version 2004 or newer is needed to share sound";
        }
        WAVEFORMATEX format {};
        format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        format.nChannels = kShareAudioChannels;
        format.nSamplesPerSec = kShareAudioRate;
        format.wBitsPerSample = 32;
        format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
        ComPtr<IAudioCaptureClient> reader;
        const HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        const bool opened =
            SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                         AUDCLNT_STREAMFLAGS_LOOPBACK |
                                             AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                             AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
                                         kBufferDuration, 0, &format, nullptr)) &&
            SUCCEEDED(client->SetEventHandle(ready)) &&
            SUCCEEDED(client->GetService(IID_PPV_ARGS(&reader))) && SUCCEEDED(client->Start());
        if (!opened) {
            CloseHandle(ready);
            return "the sound could not be captured";
        }
        started.set_value({});

        // Windows marks silence instead of filling the buffer with it.
        std::vector<float> silence;
        const HANDLE events[] = {stop, ready};
        while (WaitForMultipleObjects(2, events, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
            UINT32 frames = 0;
            while (SUCCEEDED(reader->GetNextPacketSize(&frames)) && frames > 0) {
                BYTE* data = nullptr;
                DWORD flags = 0;
                if (FAILED(reader->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    silence.assign(static_cast<size_t>(frames) * kShareAudioChannels, 0.0f);
                    samples(silence.data(), static_cast<int>(frames));
                }
                else {
                    samples(reinterpret_cast<const float*>(data), static_cast<int>(frames));
                }
                reader->ReleaseBuffer(frames);
            }
        }
        client->Stop();
        CloseHandle(ready);
        return {};
    }
};

SystemAudioCapture::SystemAudioCapture() = default;

SystemAudioCapture::~SystemAudioCapture() { stop(); }

bool SystemAudioCapture::start(const ShareSource& source, ShareAudioCallback samples,
                               QString& error) {
    stop();
    DWORD processId = GetCurrentProcessId();
    PROCESS_LOOPBACK_MODE mode = PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;
    if (source.kind == ShareSource::Kind::Window) {
        processId = static_cast<DWORD>(shareSourceProcessId(source));
        if (processId == 0) {
            error = "this window's application could not be identified";
            return false;
        }
        mode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    }

    auto state = std::make_unique<State>();
    std::promise<QString> started;
    std::future<QString> result = started.get_future();
    state->thread = std::thread([&state = *state, processId, mode, samples = std::move(samples),
                                 started = std::move(started)]() mutable {
        state.run(processId, mode, samples, started);
    });
    error = result.get();
    if (!error.isEmpty()) {
        state->thread.join();
        return false;
    }
    state_ = std::move(state);
    return true;
}

void SystemAudioCapture::stop() {
    if (!state_) return;
    SetEvent(state_->stop);
    state_->thread.join();
    state_.reset();
}

#elif defined(DISQUISITION_HAVE_PULSE)

// PulseAudio, or PipeWire through its PulseAudio server. A monitor of the
// whole output would include voice chat, so each application's stream is
// recorded on its own and the chosen ones are mixed here.

#include <pulse/pulseaudio.h>
#include <pulse/rtclock.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr int kMixFrames = kShareAudioRate / 50; // 20 ms
constexpr pa_usec_t kMixInterval = 20 * PA_USEC_PER_MSEC;
// A stream is mixed once this much is buffered, so late deliveries do not
// leave gaps, and is dropped back to waiting when it runs dry.
constexpr size_t kPrimeSamples = 2 * kMixFrames * kShareAudioChannels;
constexpr size_t kMaxBufferedSamples = 10 * kMixFrames * kShareAudioChannels;

pid_t parentOf(pid_t process) {
    std::ifstream file("/proc/" + std::to_string(process) + "/stat");
    std::string line;
    std::getline(file, line);
    // The command name in parentheses may hold spaces; the state and parent
    // follow the last closing one.
    const size_t close = line.rfind(')');
    if (close == std::string::npos) return 0;
    std::istringstream rest(line.substr(close + 1));
    char state = 0;
    pid_t parent = 0;
    rest >> state >> parent;
    return parent;
}

bool isInTree(pid_t process, pid_t root) {
    for (int depth = 0; process > 1 && depth < 64; ++depth) {
        if (process == root) return true;
        process = parentOf(process);
    }
    return false;
}

} // namespace

struct SystemAudioCapture::State {
    struct Input {
        State* state = nullptr;
        pa_stream* stream = nullptr;
        std::deque<float> buffered;
        bool primed = false;
    };

    pa_threaded_mainloop* loop = pa_threaded_mainloop_new();
    pa_context* context = nullptr;
    pa_time_event* tick = nullptr;
    pa_usec_t nextTick = 0;
    pid_t target = 0;
    bool includeTarget = false; // otherwise everything but the target
    ShareAudioCallback samples;
    // Sink input index to its recording. Map nodes stay put, so streams keep
    // a pointer to theirs.
    std::map<uint32_t, Input> inputs;
    std::vector<float> mix;

    ~State() {
        if (context) {
            pa_threaded_mainloop_lock(loop);
            for (auto& [index, input] : inputs) {
                pa_stream_set_read_callback(input.stream, nullptr, nullptr);
                pa_stream_set_state_callback(input.stream, nullptr, nullptr);
                pa_stream_disconnect(input.stream);
                pa_stream_unref(input.stream);
            }
            inputs.clear();
            if (tick) pa_threaded_mainloop_get_api(loop)->time_free(tick);
            pa_context_set_subscribe_callback(context, nullptr, nullptr);
            pa_context_set_state_callback(context, nullptr, nullptr);
            pa_context_disconnect(context);
            pa_context_unref(context);
            pa_threaded_mainloop_unlock(loop);
        }
        pa_threaded_mainloop_stop(loop);
        pa_threaded_mainloop_free(loop);
    }

    bool wanted(const pa_sink_input_info& info) const {
        const char* text = pa_proplist_gets(info.proplist, PA_PROP_APPLICATION_PROCESS_ID);
        const pid_t process = text ? static_cast<pid_t>(std::atol(text)) : 0;
        return includeTarget ? process > 0 && isInTree(process, target)
                             : process <= 0 || !isInTree(process, target);
    }

    // Recording one sink input means recording its sink's monitor, narrowed
    // to that input, so the sink is looked up first.
    static void onInputInfo(pa_context* context, const pa_sink_input_info* info, int end,
                            void* data) {
        auto* state = static_cast<State*>(data);
        if (end || !info || state->inputs.count(info->index) || !state->wanted(*info)) return;
        auto* request = new std::pair<State*, uint32_t>(state, info->index);
        pa_operation_unref(pa_context_get_sink_info_by_index(
            context, info->sink,
            [](pa_context*, const pa_sink_info* sink, int last, void* data) {
                auto* request = static_cast<std::pair<State*, uint32_t>*>(data);
                if (last) {
                    delete request;
                    return;
                }
                if (sink) request->first->record(request->second, sink->monitor_source);
            },
            request));
    }

    void record(uint32_t index, uint32_t monitor) {
        if (inputs.count(index)) return;
        const pa_sample_spec spec {PA_SAMPLE_FLOAT32NE, kShareAudioRate, kShareAudioChannels};
        pa_stream* stream = pa_stream_new(context, "screen share", &spec, nullptr);
        if (!stream) return;
        Input& input = inputs[index];
        input.state = this;
        input.stream = stream;
        pa_stream_set_monitor_stream(stream, index);
        pa_stream_set_read_callback(stream, onRead, &input);
        // A stream that ends, or moves to another sink, is dropped; a move is
        // reported as a change, which records it again.
        pa_stream_set_state_callback(
            stream,
            [](pa_stream* stream, void* data) {
                const pa_stream_state_t now = pa_stream_get_state(stream);
                if (now == PA_STREAM_FAILED || now == PA_STREAM_TERMINATED) {
                    static_cast<Input*>(data)->state->forget(stream);
                }
            },
            &input);
        pa_buffer_attr attributes;
        attributes.maxlength = static_cast<uint32_t>(-1);
        attributes.fragsize = static_cast<uint32_t>(pa_usec_to_bytes(10 * PA_USEC_PER_MSEC, &spec));
        const std::string source = std::to_string(monitor);
        if (pa_stream_connect_record(stream, source.c_str(), &attributes,
                                     static_cast<pa_stream_flags_t>(PA_STREAM_ADJUST_LATENCY |
                                                                    PA_STREAM_DONT_MOVE)) < 0) {
            forget(stream);
        }
    }

    void forget(pa_stream* stream) {
        for (auto input = inputs.begin(); input != inputs.end(); ++input) {
            if (input->second.stream != stream) continue;
            pa_stream_set_read_callback(stream, nullptr, nullptr);
            pa_stream_set_state_callback(stream, nullptr, nullptr);
            pa_stream_disconnect(stream);
            pa_stream_unref(stream);
            inputs.erase(input);
            return;
        }
    }

    static void onRead(pa_stream* stream, size_t, void* data) {
        auto& input = *static_cast<Input*>(data);
        const void* bytes = nullptr;
        size_t size = 0;
        while (pa_stream_readable_size(stream) > 0 && pa_stream_peek(stream, &bytes, &size) == 0 &&
               size > 0) {
            // A hole has no data; the mix fills it with silence.
            if (bytes) {
                const auto* floats = static_cast<const float*>(bytes);
                input.buffered.insert(input.buffered.end(), floats, floats + size / sizeof(float));
            }
            pa_stream_drop(stream);
        }
        if (input.buffered.size() > kMaxBufferedSamples) {
            input.buffered.erase(input.buffered.begin(),
                                 input.buffered.end() - static_cast<std::ptrdiff_t>(kPrimeSamples));
        }
    }

    static void onEvent(pa_context* context, pa_subscription_event_type_t event, uint32_t index,
                        void* data) {
        auto* state = static_cast<State*>(data);
        const auto type = event & PA_SUBSCRIPTION_EVENT_TYPE_MASK;
        if (type == PA_SUBSCRIPTION_EVENT_REMOVE) {
            if (const auto input = state->inputs.find(index); input != state->inputs.end()) {
                state->forget(input->second.stream);
            }
            return;
        }
        pa_operation_unref(pa_context_get_sink_input_info(context, index, onInputInfo, state));
    }

    // Runs every 20 ms whether or not anything plays, so the share's sound
    // keeps time with the clock, as it does on other systems.
    static void onTick(pa_mainloop_api*, pa_time_event* event, const timeval*, void* data) {
        auto* state = static_cast<State*>(data);
        state->mix.assign(static_cast<size_t>(kMixFrames) * kShareAudioChannels, 0.0f);
        for (auto& [index, input] : state->inputs) {
            if (!input.primed && input.buffered.size() >= kPrimeSamples) input.primed = true;
            if (!input.primed) continue;
            const size_t count = std::min(input.buffered.size(), state->mix.size());
            for (size_t i = 0; i < count; ++i) state->mix[i] += input.buffered[i];
            input.buffered.erase(input.buffered.begin(),
                                 input.buffered.begin() + static_cast<std::ptrdiff_t>(count));
            if (count < state->mix.size()) input.primed = false;
        }
        state->samples(state->mix.data(), kMixFrames);

        // Scheduled from the last deadline so the rate holds; after a stall
        // it restarts from now instead of catching up in a burst.
        const pa_usec_t now = pa_rtclock_now();
        state->nextTick += kMixInterval;
        if (state->nextTick <= now) state->nextTick = now + kMixInterval;
        pa_context_rttime_restart(state->context, event, state->nextTick);
    }

    QString open() {
        pa_mainloop_api* api = pa_threaded_mainloop_get_api(loop);
        context = pa_context_new(api, "Disquisition");
        if (!context) return "no sound server is running";
        pa_context_set_state_callback(
            context, [](pa_context*, void* loop) {
                pa_threaded_mainloop_signal(static_cast<pa_threaded_mainloop*>(loop), 0);
            },
            loop);
        if (pa_context_connect(context, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0 ||
            pa_threaded_mainloop_start(loop) < 0) {
            return "no PulseAudio or PipeWire sound server is running";
        }

        pa_threaded_mainloop_lock(loop);
        pa_context_state_t now;
        while ((now = pa_context_get_state(context)) != PA_CONTEXT_READY &&
               PA_CONTEXT_IS_GOOD(now)) {
            pa_threaded_mainloop_wait(loop);
        }
        if (now != PA_CONTEXT_READY) {
            pa_threaded_mainloop_unlock(loop);
            return "no PulseAudio or PipeWire sound server is running";
        }
        pa_context_set_subscribe_callback(context, onEvent, this);
        pa_operation_unref(pa_context_subscribe(context, PA_SUBSCRIPTION_MASK_SINK_INPUT, nullptr,
                                                nullptr));
        pa_operation_unref(pa_context_get_sink_input_info_list(context, onInputInfo, this));
        nextTick = pa_rtclock_now() + kMixInterval;
        tick = pa_context_rttime_new(context, nextTick, onTick, this);
        pa_threaded_mainloop_unlock(loop);
        return {};
    }
};

SystemAudioCapture::SystemAudioCapture() = default;

SystemAudioCapture::~SystemAudioCapture() { stop(); }

bool SystemAudioCapture::start(const ShareSource& source, ShareAudioCallback samples,
                               QString& error) {
    stop();
    auto state = std::make_unique<State>();
    state->samples = std::move(samples);
    state->target = getpid();
    if (source.kind == ShareSource::Kind::Window) {
        state->target = static_cast<pid_t>(shareSourceProcessId(source));
        state->includeTarget = true;
        if (state->target == 0) {
            error = "this window's application could not be identified";
            return false;
        }
    }
    error = state->open();
    if (!error.isEmpty()) return false;
    state_ = std::move(state);
    return true;
}

void SystemAudioCapture::stop() { state_.reset(); }

#else

struct SystemAudioCapture::State {};

SystemAudioCapture::SystemAudioCapture() = default;

SystemAudioCapture::~SystemAudioCapture() = default;

bool SystemAudioCapture::start(const ShareSource&, ShareAudioCallback, QString& error) {
    error = "sharing sound is not supported on this system yet";
    return false;
}

void SystemAudioCapture::stop() {}

#endif
