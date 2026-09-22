#pragma once

#include <functional>
#include <QStringList>

class QObject;

void requestMicrophoneAccess(QObject* context, std::function<void(bool)> completion);
QStringList availableAudioInputs();
QStringList availableAudioOutputs();
