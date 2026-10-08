#include "audio.h"
#include "worker.h"

#include <coreinit/cache.h>
#include <sndcore2/core.h>
#include <cstdlib>
#include <cstring>
#include <malloc.h>

namespace wiiu {

Audio* Audio::current_ = nullptr;

void Audio::audioFrame() {
    // Called by AX every 3 ms. Never let a stalled UI replay a ring of old speech.
    // No allocation, network, or locks on the audio service callback.
    if (!current_) return;
    const auto now = static_cast<std::uint32_t>(nowMilliseconds());
    if (now - current_->heartbeat_.load(std::memory_order_relaxed) > 70) {
        AXSetVoiceState(current_->voice_, AX_VOICE_STATE_STOPPED);
        current_->underrun_.store(true, std::memory_order_relaxed);
    }
}

bool Audio::initialize(std::string& error) {
    if (!AXIsInit()) {
        AXInitParams parameters{};
        parameters.renderer = AX_INIT_RENDERER_48KHZ;
        AXInitWithParams(&parameters);
        ownsAx_ = true;
    }
    if (!AXIsInit()) { error = "Could not initialize Wii U audio."; shutdown(); return false; }
    return true;
}

bool Audio::start(std::string& error) {
    stop();
    if (!initialize(error)) return false;
    microphone_ = static_cast<std::int16_t*>(memalign(64, kMicSamples * 2));
    playback_ = static_cast<std::int16_t*>(memalign(64, handheld::kPcmBytes * kSegments));
    if (!microphone_ || !playback_) { error = "Not enough memory for voice."; stop(); return false; }
    std::memset(microphone_, 0, kMicSamples * 2);
    DCFlushRange(microphone_, kMicSamples * 2);
    memory_ = {kMicSamples, microphone_};
    MICError result = MIC_ERROR_OK;
    handle_ = MICInit(MIC_INSTANCE_0, 0, &memory_, &result);
    if (handle_ < 0 || result != MIC_ERROR_OK) {
        error = "GamePad microphone init: " + std::to_string(result); stop(); return false;
    }
    // State 0 is the capture sample rate. Never send the wrong rate as 16 kHz.
    std::uint32_t rate = 0;
    if (MICGetState(handle_, 0, &rate) != MIC_ERROR_OK || rate != 32000) {
        error = "Unsupported GamePad microphone rate: " + std::to_string(rate); stop(); return false;
    }
    result = MICOpen(handle_);
    if (result != MIC_ERROR_OK) { error = "GamePad microphone open: " + std::to_string(result); stop(); return false; }
    voice_ = AXAcquireVoice(31, nullptr, nullptr);
    if (!voice_) { error = "Could not acquire Wii U playback voice."; stop(); return false; }
    AXVoiceDeviceMixData mix[6]{};
    mix[0].bus[0].volume = mix[1].bus[0].volume = 0x6000;
    AXSetVoiceDeviceMix(voice_, AX_DEVICE_TYPE_DRC, 0, mix);
    // GamePad output also feeds its headphone jack; TV audio is optional later.
    AXVoiceVeData volume{0x8000, 0};
    AXSetVoiceVe(voice_, &volume);
    AXSetVoiceSrcType(voice_, AX_VOICE_SRC_TYPE_LINEAR);
    if (AXSetVoiceSrcRatio(voice_, 16000.0f / AXGetInputSamplesPerSec()) !=
        AX_VOICE_RATIO_RESULT_SUCCESS) {
        error = "Could not configure 16 kHz playback."; stop(); return false;
    }
    frames_.reset(); mixer_.clear();
    transmitting_ = deafened_ = false;
    lastTick_ = 0;
    resetPlayback(true);
    heartbeat_.store(static_cast<std::uint32_t>(nowMilliseconds()), std::memory_order_relaxed);
    underrun_.store(false, std::memory_order_relaxed);
    current_ = this;
    if (AXRegisterAppFrameCallback(audioFrame) != AX_RESULT_SUCCESS) {
        current_ = nullptr;
        error = "Could not register Wii U audio callback."; stop(); return false;
    }
    callbackRegistered_ = true;
    return true;
}

void Audio::resetPlayback(bool playing) {
    AXSetVoiceState(voice_, AX_VOICE_STATE_STOPPED);
    std::memset(playback_, 0, handheld::kPcmBytes * kSegments);
    DCFlushRange(playback_, handheld::kPcmBytes * kSegments);
    AXVoiceOffsets offsets{};
    offsets.dataType = AX_VOICE_FORMAT_LPCM16;
    offsets.loopingEnabled = AX_VOICE_LOOP_ENABLED;
    offsets.endOffset = handheld::kSamples * kSegments - 1;
    offsets.data = playback_;
    AXSetVoiceOffsets(voice_, &offsets);
    segment_ = 0;
    if (playing) AXSetVoiceState(voice_, AX_VOICE_STATE_PLAYING);
}

void Audio::stop() {
    if (callbackRegistered_) {
        AXDeregisterAppFrameCallback(audioFrame);
        callbackRegistered_ = false;
    }
    if (current_ == this) current_ = nullptr;
    if (handle_ >= 0) { MICClose(handle_); MICUninit(handle_); handle_ = -1; }
    if (voice_) { AXSetVoiceState(voice_, AX_VOICE_STATE_STOPPED); AXFreeVoice(voice_); voice_ = nullptr; }
    std::free(microphone_); std::free(playback_);
    microphone_ = playback_ = nullptr;
    frames_.reset(); mixer_.clear();
}

void Audio::shutdown() {
    stop();
    if (ownsAx_) { AXQuit(); ownsAx_ = false; }
}

bool Audio::tick(const handheld::View& view, Worker& worker, bool transmit,
                 std::uint64_t now, std::string& error) {
    if (!running()) return true;
    MICStatus status{};
    const MICError result = MICGetStatus(handle_, &status);
    if (result != MIC_ERROR_OK || !(status.state & 2) || !(status.state & 4) || status.availableData < 0 ||
        status.availableData >= int(kMicSamples) || status.bufferPos < 0 || status.bufferPos >= int(kMicSamples)) {
        error = "GamePad microphone disconnected or unavailable: " + std::to_string(result);
        stop(); return false;
    }
    const bool stalled = (lastTick_ != 0 && now - lastTick_ > 70) ||
        underrun_.exchange(false, std::memory_order_relaxed);
    heartbeat_.store(static_cast<std::uint32_t>(now), std::memory_order_relaxed);
    if (!transmit || transmit != transmitting_ || stalled) frames_.reset();
    else {
        DCInvalidateRange(microphone_, kMicSamples * 2);
        handheld::Samples frame;
        for (int i = 0; i < status.availableData; ++i) {
            const auto sample = microphone_[(unsigned(status.bufferPos) + unsigned(i)) % kMicSamples];
            if (frames_.push(sample, frame)) worker.capture(handheld::encodePcm(frame));
        }
    }
    if (MICSetDataConsumed(handle_, status.availableData) != MIC_ERROR_OK) {
        error = "Could not consume GamePad microphone samples."; stop(); return false;
    }
    transmitting_ = transmit;
    for (const auto& received : worker.takeAudio()) {
        const auto member = view.members.find(received.sender);
        if (!view.deafened && member != view.members.end() && member->second.voice)
            mixer_.receive(received.sender, received.pcm);
    }
    if (view.deafened != deafened_ || stalled) {
        mixer_.clear(); resetPlayback(!view.deafened); deafened_ = view.deafened;
    }
    if (!view.deafened) {
        const unsigned current = AXGetVoiceCurrentOffsetEx(voice_, playback_) / handheld::kSamples;
        if (current < kSegments) {
            // Refill only segments the hardware has already finished. A segment
            // is always replaced, including silence, so it cannot repeat old speech.
            while (segment_ != current) {
                const auto samples = mixer_.render();
                auto* destination = playback_ + segment_ * handheld::kSamples;
                std::memcpy(destination, samples.data(), handheld::kPcmBytes);
                DCFlushRange(destination, handheld::kPcmBytes);
                segment_ = (segment_ + 1) % kSegments;
            }
        }
    }
    lastTick_ = now;
    return true;
}

} // namespace wiiu
