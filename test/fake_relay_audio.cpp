#include <deque>

#include "desktop/relay_audio.h"
#include "fake_relay_audio.h"

namespace {
std::deque<bool> startResults;
QHash<QString, qreal> peerVolumes;
QSet<QString> mutedPeers;
}

void testAudio::setStartResults(std::initializer_list<bool> results) {
    startResults.assign(results);
}

qreal testAudio::peerVolume(const QString& sender) {
    return peerVolumes.value(sender, 1.0);
}

bool testAudio::peerMuted(const QString& sender) {
    return mutedPeers.contains(sender);
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
void RelayAudio::setPeerVolume(const QString& sender, qreal volume) {
    peerVolumes[sender] = volume;
}
void RelayAudio::setPeerMuted(const QString& sender, bool muted) {
    if (muted)
        mutedPeers.insert(sender);
    else
        mutedPeers.remove(sender);
}
