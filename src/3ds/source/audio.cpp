#include "audio.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>

namespace handheld {
namespace {
std::string serviceError(const char* text, Result result) {
    char code[24];
    std::snprintf(code, sizeof(code), " (0x%08lX)", static_cast<unsigned long>(result));
    return std::string(text) + code;
}
}

bool Audio::start(std::string& error) {
    stop();
    Result result = ndspInit();
    if (R_FAILED(result)) {
        error = serviceError("Audio unavailable. Dump DSP firmware in Rosalina.", result);
        return false;
    }
    dspReady_ = true;
    microphone_ = static_cast<u8*>(memalign(0x1000, kMicSize));
    playback_ = static_cast<s16*>(linearAlloc(kPcmBytes * 3));
    if (!microphone_ || !playback_) {
        error = "Not enough memory for voice.";
        stop();
        return false;
    }
    std::memset(microphone_, 0, kMicSize);
    result = micInit(microphone_, kMicSize);
    if (R_FAILED(result)) {
        error = serviceError("Microphone unavailable.", result);
        stop();
        return false;
    }
    micReady_ = true;
    MICU_SetAllowShellClosed(false);
    result = MICU_StartSampling(MICU_ENCODING_PCM16_SIGNED, MICU_SAMPLE_RATE_16360,
                               0, micGetSampleDataSize(), true);
    if (R_FAILED(result)) {
        error = serviceError("Cannot start microphone.", result);
        stop();
        return false;
    }
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    ndspChnSetRate(0, 16000.0f);
    ndspChnSetFormat(0, NDSP_FORMAT_MONO_PCM16);
    float mix[12]{};
    mix[0] = mix[1] = 0.8f;
    ndspChnSetMix(0, mix);
    std::memset(waves_, 0, sizeof(waves_));
    for (unsigned i = 0; i < 3; ++i) {
        waves_[i].data_pcm16 = playback_ + i * kSamples;
        waves_[i].nsamples = kSamples;
    }
    cursor_ = 0;
    lastCapture_ = nextPlayback_ = 0;
    wasTransmitting_ = wasDeafened_ = false;
    resampler_.reset();
    return true;
}

void Audio::stop() {
    if (micReady_) {
        MICU_StopSampling();
        micExit();
        micReady_ = false;
    }
    if (dspReady_) {
        ndspChnWaveBufClear(0);
        ndspExit();
        dspReady_ = false;
    }
    if (playback_) linearFree(playback_);
    std::free(microphone_);
    microphone_ = nullptr;
    playback_ = nullptr;
    mixer.clear();
    resampler_.reset();
}

void Audio::tick(Session& session, std::uint64_t now) {
    if (!running()) return;
    const unsigned dataSize = micGetSampleDataSize();
    const unsigned write = micGetLastSampleOffset() & ~1u;
    if (write >= dataSize) return;
    // Discard stale microphone samples after an applet/sleep or a mute/PTT
    // change, so opening the keyboard never transmits a recording afterwards.
    if (!session.view.transmitting || session.view.transmitting != wasTransmitting_ ||
        (lastCapture_ != 0 && now - lastCapture_ > 150)) {
        cursor_ = write;
        resampler_.reset();
    } else {
        Samples frame;
        while (cursor_ != write) {
            const unsigned value = microphone_[cursor_] | (microphone_[cursor_ + 1] << 8);
            const auto sample = static_cast<std::int16_t>(value < 32768 ? int(value) : int(value) - 65536);
            cursor_ = (cursor_ + 2) % dataSize;
            if (resampler_.push(sample, frame)) session.capture(encodePcm(frame));
        }
    }
    wasTransmitting_ = session.view.transmitting;
    lastCapture_ = now;

    if (session.view.deafened) {
        mixer.clear();
        if (!wasDeafened_) ndspChnWaveBufClear(0);
    } else if (now >= nextPlayback_) {
        for (auto& wave : waves_) {
            if (wave.status != NDSP_WBUF_FREE && wave.status != NDSP_WBUF_DONE) continue;
            const Samples samples = mixer.render();
            std::memcpy(wave.data_pcm16, samples.data(), kPcmBytes);
            DSP_FlushDataCache(wave.data_pcm16, kPcmBytes);
            ndspChnWaveBufAdd(0, &wave);
            break;
        }
        nextPlayback_ = (now > nextPlayback_ + 60) ? now + 20 : nextPlayback_ + 20;
    }
    wasDeafened_ = session.view.deafened;
}

} // namespace handheld
