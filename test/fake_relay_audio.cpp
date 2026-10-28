#include <deque>

#include "desktop/relay_audio.h"
#include "fake_relay_audio.h"

namespace {
std::deque<bool> startResults;
}

void testAudio::setStartResults(std::initializer_list<bool> results) {
    startResults.assign(results);
}

RelayAudio::RelayAudio(QObject* parent) : QObject(parent) {}

bool RelayAudio::start(const QString&, const QString&) {
    stop();
    const bool success = startResults.empty() || startResults.front();
    if (!startResults.empty())
        startResults.pop_front();
    if (!success)
        return false;
    // The source exists for running(), but start() is never called on it:
    // no microphone is opened and no audio is captured by the UI test.
    source_ = new QAudioSource(QAudioFormat{}, this);
    return true;
}

void RelayAudio::stop() {
    delete source_;
    source_ = nullptr;
    capture_ = nullptr;
    muted_ = false;
    deafened_ = false;
    if (speaking_) {
        speaking_ = false;
        emit speakingChanged(false);
    }
}

// Packet transport and speaking detection stay real; only hardware playback is absent.
void RelayAudio::receive(const QString&, const QByteArray&) {}
void RelayAudio::remove(const QString&) {}
