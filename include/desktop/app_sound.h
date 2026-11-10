#pragma once

#include <QString>

class QObject;
class QSoundEffect;

// A sound bundled under :/sounds/<name>.wav, or null when this build lacks it.
QSoundEffect* bundledSound(const QString& name, QObject* parent);

// Plays on the output device chosen for voice. Does nothing for a null sound.
void playSound(QSoundEffect* sound);
