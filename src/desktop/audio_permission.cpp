#include "desktop/audio_permission.h"

#include <QApplication>
#include <QAudioDevice>
#include <QMediaDevices>
#include <QPermissions>
#include <QTimer>
#include <QVector>

#include <utility>

#if defined(Q_OS_MACOS)
#import <AVFoundation/AVFoundation.h>
#import <CoreAudio/CoreAudio.h>
#endif

namespace {

#if defined(Q_OS_MACOS)
QStringList coreAudioDevices(bool input) {
    AudioObjectPropertyAddress address = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, nullptr, &size) !=
        noErr) {
        return {};
    }
    QVector<AudioDeviceID> devices(static_cast<qsizetype>(size / sizeof(AudioDeviceID)));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size,
                                   devices.data()) != noErr) {
        return {};
    }
    QStringList names;
    for (AudioDeviceID device : devices) {
        address.mSelector = kAudioDevicePropertyStreams;
        address.mScope = input ? kAudioDevicePropertyScopeInput : kAudioDevicePropertyScopeOutput;
        UInt32 streamSize = 0;
        if (AudioObjectGetPropertyDataSize(device, &address, 0, nullptr, &streamSize) != noErr ||
            streamSize == 0) {
            continue;
        }
        CFStringRef name = nullptr;
        size = sizeof(name);
        address.mSelector = kAudioDevicePropertyDeviceNameCFString;
        address.mScope = kAudioObjectPropertyScopeGlobal;
        if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &name) != noErr ||
            !name) {
            continue;
        }
        char value[256] = {};
        if (CFStringGetCString(name, value, sizeof(value), kCFStringEncodingUTF8)) {
            names.append(QString::fromUtf8(value));
        }
        CFRelease(name);
    }
    names.removeDuplicates();
    names.sort(Qt::CaseInsensitive);
    return names;
}
#endif

}  // namespace

void requestMicrophoneAccess(QObject* context, std::function<void(bool)> completion) {
#if defined(Q_OS_MACOS)
    const AVAuthorizationStatus status =
        [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
    if (status == AVAuthorizationStatusAuthorized) {
        QTimer::singleShot(0, context, [completion = std::move(completion)] { completion(true); });
    } else if (status == AVAuthorizationStatusNotDetermined) {
        [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                                completionHandler:^(BOOL granted) {
          QTimer::singleShot(0, context, [completion, granted] { completion(granted); });
        }];
    } else {
        QTimer::singleShot(0, context, [completion = std::move(completion)] { completion(false); });
    }
#else
    QMicrophonePermission permission;
    if (qApp->checkPermission(permission) == Qt::PermissionStatus::Granted) {
        QTimer::singleShot(0, context, [completion = std::move(completion)] { completion(true); });
    } else if (qApp->checkPermission(permission) == Qt::PermissionStatus::Undetermined) {
        qApp->requestPermission(permission, context,
                                [completion = std::move(completion)](const QPermission& result) {
            completion(result.status() == Qt::PermissionStatus::Granted);
        });
    } else {
        QTimer::singleShot(0, context, [completion = std::move(completion)] { completion(false); });
    }
#endif
}

QStringList availableAudioInputs() {
#if defined(Q_OS_MACOS)
    return coreAudioDevices(true);
#else
    QStringList names;
    for (const QAudioDevice& device : QMediaDevices::audioInputs()) {
        names.append(device.description());
    }
    return names;
#endif
}

QStringList availableAudioOutputs() {
#if defined(Q_OS_MACOS)
    return coreAudioDevices(false);
#else
    QStringList names;
    for (const QAudioDevice& device : QMediaDevices::audioOutputs()) {
        names.append(device.description());
    }
    return names;
#endif
}
