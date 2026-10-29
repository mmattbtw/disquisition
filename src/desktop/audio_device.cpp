#include "desktop/audio_device.h"

QAudioDevice findAudioDevice(const QList<QAudioDevice>& devices, const QString& id,
                             const QAudioDevice& fallback) {
    for (const QAudioDevice& device : devices) {
        if (device.id() == id.toUtf8()) return device;
    }
    return fallback;
}

QAudioFormat supportedAudioFormat(const QAudioDevice& device) {
    if (device.isNull()) return {};
    const QAudioFormat preferred = device.preferredFormat();
    if (device.isFormatSupported(preferred)) return preferred;

    for (int rate : {48000, 44100}) {
        for (int channels : {1, 2}) {
            for (QAudioFormat::SampleFormat sampleFormat :
                 {QAudioFormat::Int16, QAudioFormat::Float}) {
                QAudioFormat format;
                format.setSampleRate(rate);
                format.setChannelCount(channels);
                format.setSampleFormat(sampleFormat);
                if (device.isFormatSupported(format)) return format;
            }
        }
    }
    return {};
}
