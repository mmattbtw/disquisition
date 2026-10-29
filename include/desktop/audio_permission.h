#pragma once

#include <functional>
#include <QStringList>

class QObject;

void requestCameraAccess(QObject* context, std::function<void(bool)> completion);
void requestMicrophoneAccess(QObject* context, std::function<void(bool)> completion);
QStringList availableAudioInputs();
QStringList availableAudioOutputs();
