#include "desktop/relay_audio.h"

#include <QIODevice>
#include <QMediaDevices>

#include <cmath>

RelayAudio::RelayAudio(QObject* parent) : QObject(parent) {
    format_.setSampleRate(16000);
    format_.setChannelCount(1);
    format_.setSampleFormat(QAudioFormat::Int16);
}

bool RelayAudio::start(const QString& inputId, const QString& outputId) {
    stop();
    outputId_ = outputId;
    QAudioDevice device = QMediaDevices::defaultAudioInput();
    for (const QAudioDevice& candidate : QMediaDevices::audioInputs()) {
        if (candidate.id() == inputId.toUtf8()) {
            device = candidate;
            break;
        }
    }
    if (!device.isFormatSupported(format_)) {
        return false;
    }
    source_ = new QAudioSource(device, format_, this);
    source_->setBufferSize(640 * 8);
    capture_ = source_->start();
    if (!capture_) {
        stop();
        return false;
    }
    connect(capture_, &QIODevice::readyRead, this, [this] {
        pending_ += capture_->readAll();
        while (pending_.size() >= 640) {
            const QByteArray frame = pending_.left(640);
            pending_.remove(0, 640);
            const auto* samples = reinterpret_cast<const unsigned char*>(frame.constData());
            double sum = 0;
            for (int i = 0; i < 640; i += 2) {
                const auto sample = static_cast<qint16>(samples[i] | (samples[i + 1] << 8));
                sum += static_cast<double>(sample) * sample;
            }
            const bool speaking = !muted_ && std::sqrt(sum / 320) > 450;
            if (speaking != speaking_) {
                speaking_ = speaking;
                emit speakingChanged(speaking);
            }
            if (!muted_) {
                emit frameReady(frame);
            }
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
    pending_.clear();
    muted_ = false;
    deafened_ = false;
    if (speaking_) {
        speaking_ = false;
        emit speakingChanged(false);
    }
}

void RelayAudio::receive(const QString& sender, const QByteArray& pcm) {
    if (!running() || deafened_ || pcm.size() != 640) {
        return;
    }
    if (!playback_.contains(sender)) {
        QAudioDevice device = QMediaDevices::defaultAudioOutput();
        for (const QAudioDevice& candidate : QMediaDevices::audioOutputs()) {
            if (candidate.id() == outputId_.toUtf8()) {
                device = candidate;
                break;
            }
        }
        if (!device.isFormatSupported(format_)) {
            return;
        }
        auto* sink = new QAudioSink(device, format_, this);
        sink->setBufferSize(640 * 10);
        playback_.insert(sender, {sink, sink->start()});
    }
    if (QIODevice* output = playback_[sender].device) {
        output->write(pcm);
    }
}

void RelayAudio::remove(const QString& sender) {
    if (playback_.contains(sender)) {
        Playback playback = playback_.take(sender);
        playback.sink->stop();
        delete playback.sink;
    }
}
