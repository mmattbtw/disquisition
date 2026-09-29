#include "desktop/relay_audio.h"

#include <QIODevice>
#include <QMediaDevices>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

constexpr int relayRate = 16000;
constexpr int relayFrameBytes = 640; // 20 ms of mono signed 16-bit PCM.

QAudioDevice selectedDevice(const QList<QAudioDevice>& devices, const QString& id,
                            const QAudioDevice& fallback) {
    for (const QAudioDevice& device : devices) {
        if (device.id() == id.toUtf8()) return device;
    }
    return fallback;
}

QAudioFormat deviceFormat(const QAudioDevice& device) {
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

float readSample(const char* data, QAudioFormat::SampleFormat format) {
    switch (format) {
    case QAudioFormat::UInt8:
        return (static_cast<unsigned char>(*data) - 128) / 128.0f;
    case QAudioFormat::Int16: {
        qint16 sample;
        std::memcpy(&sample, data, sizeof(sample));
        return sample / 32768.0f;
    }
    case QAudioFormat::Int32: {
        qint32 sample;
        std::memcpy(&sample, data, sizeof(sample));
        return static_cast<float>(sample / 2147483648.0);
    }
    case QAudioFormat::Float: {
        float sample;
        std::memcpy(&sample, data, sizeof(sample));
        return std::isfinite(sample) ? sample : 0.0f;
    }
    default:
        return 0;
    }
}

void writeSample(QByteArray& bytes, float value, QAudioFormat::SampleFormat format) {
    const float sample = std::clamp(value, -1.0f, 1.0f);
    switch (format) {
    case QAudioFormat::UInt8:
        bytes.append(static_cast<char>(std::clamp(std::lround(sample * 128 + 128), 0L, 255L)));
        break;
    case QAudioFormat::Int16: {
        const qint16 pcm = static_cast<qint16>(std::clamp(std::lround(sample * 32768),
                                                          -32768L, 32767L));
        bytes.append(reinterpret_cast<const char*>(&pcm), sizeof(pcm));
        break;
    }
    case QAudioFormat::Int32: {
        const qint32 pcm = static_cast<qint32>(std::clamp(std::llround(sample * 2147483648.0),
                                                          -2147483648LL, 2147483647LL));
        bytes.append(reinterpret_cast<const char*>(&pcm), sizeof(pcm));
        break;
    }
    case QAudioFormat::Float:
        bytes.append(reinterpret_cast<const char*>(&sample), sizeof(sample));
        break;
    default:
        break;
    }
}

QVector<float> resample(QVector<float>& samples, double& position,
                        int sourceRate, int targetRate) {
    QVector<float> output;
    const double step = static_cast<double>(sourceRate) / targetRate;
    while (position + 1 < samples.size()) {
        const int index = static_cast<int>(position);
        const float fraction = static_cast<float>(position - index);
        output.append(samples[index] + (samples[index + 1] - samples[index]) * fraction);
        position += step;
    }
    const int consumed = std::min(static_cast<int>(position), static_cast<int>(samples.size()));
    samples.remove(0, consumed);
    position -= consumed;
    return output;
}

} // namespace

RelayAudio::RelayAudio(QObject* parent) : QObject(parent) {}

bool RelayAudio::start(const QString& inputId, const QString& outputId) {
    stop();
    const QAudioDevice input = selectedDevice(QMediaDevices::audioInputs(), inputId,
                                              QMediaDevices::defaultAudioInput());
    outputDevice_ = selectedDevice(QMediaDevices::audioOutputs(), outputId,
                                   QMediaDevices::defaultAudioOutput());
    inputFormat_ = deviceFormat(input);
    outputFormat_ = deviceFormat(outputDevice_);
    if (!inputFormat_.isValid() || !outputFormat_.isValid()) return false;

    source_ = new QAudioSource(input, inputFormat_, this);
    source_->setBufferSize(inputFormat_.bytesForDuration(160000));
    capture_ = source_->start();
    if (!capture_) {
        stop();
        return false;
    }
    connect(capture_, &QIODevice::readyRead, this, [this] {
        captureBytes_ += capture_->readAll();
        const int frameBytes = inputFormat_.bytesPerFrame();
        const int completeBytes = captureBytes_.size() / frameBytes * frameBytes;
        const int sampleBytes = inputFormat_.bytesPerSample();
        for (int offset = 0; offset < completeBytes; offset += frameBytes) {
            float mono = 0;
            for (int channel = 0; channel < inputFormat_.channelCount(); ++channel) {
                mono += readSample(captureBytes_.constData() + offset + channel * sampleBytes,
                                   inputFormat_.sampleFormat());
            }
            captureSamples_.append(mono / inputFormat_.channelCount());
        }
        captureBytes_.remove(0, completeBytes);
        for (float sample : resample(captureSamples_, capturePosition_,
                                     inputFormat_.sampleRate(), relayRate)) {
            writeSample(pending_, sample, QAudioFormat::Int16);
        }
        while (pending_.size() >= relayFrameBytes) {
            const QByteArray frame = pending_.left(relayFrameBytes);
            pending_.remove(0, relayFrameBytes);
            const auto* samples = reinterpret_cast<const unsigned char*>(frame.constData());
            double sum = 0;
            for (int i = 0; i < relayFrameBytes; i += 2) {
                const auto sample = static_cast<qint16>(samples[i] | (samples[i + 1] << 8));
                sum += static_cast<double>(sample) * sample;
            }
            const bool speaking = !muted_ && std::sqrt(sum / 320) > 450;
            if (speaking != speaking_) {
                speaking_ = speaking;
                emit speakingChanged(speaking);
            }
            if (!muted_) emit frameReady(frame);
        }
    });
    return true;
}

void RelayAudio::stop() {
    if (source_) {
        source_->stop();
        delete source_;
        source_ = nullptr;
    }
    capture_ = nullptr;
    for (const Playback& playback : std::as_const(playback_)) {
        playback.sink->stop();
        delete playback.sink;
    }
    playback_.clear();
    captureBytes_.clear();
    captureSamples_.clear();
    capturePosition_ = 0;
    pending_.clear();
    muted_ = false;
    deafened_ = false;
    if (speaking_) {
        speaking_ = false;
        emit speakingChanged(false);
    }
}

void RelayAudio::receive(const QString& sender, const QByteArray& pcm) {
    if (!running() || deafened_ || pcm.size() != relayFrameBytes) return;
    if (!playback_.contains(sender)) {
        auto* sink = new QAudioSink(outputDevice_, outputFormat_, this);
        sink->setBufferSize(outputFormat_.bytesForDuration(200000));
        QIODevice* device = sink->start();
        if (!device) {
            delete sink;
            return;
        }
        playback_.insert(sender, {sink, device, {}, 0});
    }

    Playback& playback = playback_[sender];
    const auto* bytes = reinterpret_cast<const unsigned char*>(pcm.constData());
    for (int i = 0; i < relayFrameBytes; i += 2) {
        const auto sample = static_cast<qint16>(bytes[i] | (bytes[i + 1] << 8));
        playback.samples.append(sample / 32768.0f);
    }
    const QVector<float> converted = resample(playback.samples, playback.position,
                                               relayRate, outputFormat_.sampleRate());
    QByteArray output;
    output.reserve(converted.size() * outputFormat_.bytesPerFrame());
    for (float sample : converted) {
        for (int channel = 0; channel < outputFormat_.channelCount(); ++channel) {
            writeSample(output, sample, outputFormat_.sampleFormat());
        }
    }
    playback.device->write(output);
}

void RelayAudio::remove(const QString& sender) {
    if (playback_.contains(sender)) {
        Playback playback = playback_.take(sender);
        playback.sink->stop();
        delete playback.sink;
    }
}
