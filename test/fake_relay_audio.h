#pragma once

#include <QString>
#include <QtGlobal>

#include <initializer_list>

namespace testAudio {
// Each startup replaces the current audio, just like the hardware backend.
void setStartResults(std::initializer_list<bool> results);
// What the panel last set for a sender's playback.
qreal peerVolume(const QString& sender);
bool peerMuted(const QString& sender);
}
