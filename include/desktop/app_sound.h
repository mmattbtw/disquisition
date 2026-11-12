#pragma once

#include <QString>

class QObject;
class QSoundEffect;

// A sound bundled under :/sounds/<name>.wav, or null when this build lacks it.
QSoundEffect* bundledSound(const QString& name, QObject* parent);

// Startup, notification, and voice join/leave sounds are opt-in.
bool soundEffectsEnabled();
void setSoundEffectsEnabled(bool enabled);

// Plays a sound effect on the output device chosen for voice, when sound
// effects are on. Does nothing for a null sound.
void playSound(QSoundEffect* sound);

// playSound regardless of the setting, for alerts someone asked to be heard.
void playAlert(QSoundEffect* sound);
