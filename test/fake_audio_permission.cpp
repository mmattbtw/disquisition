#include "desktop/audio_permission.h"
#include <QTimer>

// The UI suite uses fake audio devices and must not display an OS permission
// prompt. Keep the completion asynchronous, as with the native implementation.
void requestMicrophoneAccess(QObject* context, std::function<void(bool)> completion) {
    QTimer::singleShot(0, context, [completion = std::move(completion)] { completion(true); });
}
// Camera denial exercises the recovery dialog without requesting OS access.
void requestCameraAccess(QObject* context, std::function<void(bool)> completion) {
    QTimer::singleShot(0, context, [completion = std::move(completion)] { completion(false); });
}
QStringList availableAudioInputs() { return {"Test microphone"}; }
QStringList availableAudioOutputs() { return {"Test speakers"}; }
