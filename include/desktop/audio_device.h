#pragma once

#include <QAudioDevice>
#include <QAudioFormat>
#include <QList>
#include <QString>

QAudioDevice findAudioDevice(const QList<QAudioDevice>& devices, const QString& id,
                             const QAudioDevice& fallback);

// The device's preferred format when it really takes it, otherwise a common
// one it does. Invalid when the device is null or takes none of them.
QAudioFormat supportedAudioFormat(const QAudioDevice& device);
