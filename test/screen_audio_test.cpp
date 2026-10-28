#include <QByteArray>
#include <QList>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <vector>

#include "check.h"
#include "common/protocol.h"
#include "desktop/screen_audio.h"
#include "desktop/system_audio_capture.h"

namespace {

constexpr double kAmplitude = 0.5;

// One second of a 440 Hz tone, the same in both channels.
std::vector<float> tone() {
    std::vector<float> samples;
    for (int frame = 0; frame < kShareAudioRate; ++frame) {
        const auto value = static_cast<float>(
            kAmplitude * std::sin(2 * std::numbers::pi * 440 * frame / kShareAudioRate));
        samples.insert(samples.end(), kShareAudioChannels, value);
    }
    return samples;
}

// Capture hands over whatever the system had ready, rarely a whole packet.
QList<QByteArray> encodeInUnevenPieces(ScreenAudioEncoder& encoder, const std::vector<float>& samples) {
    constexpr int kPieceFrames = 441;
    QList<QByteArray> packets;
    const int frames = static_cast<int>(samples.size()) / kShareAudioChannels;
    for (int start = 0; start < frames; start += kPieceFrames) {
        const int count = std::min(kPieceFrames, frames - start);
        packets += encoder.encode(samples.data() + start * kShareAudioChannels, count);
    }
    return packets;
}

void packetsFitTheProtocol() {
    ScreenAudioEncoder encoder;
    CHECK(encoder.open());
    const QList<QByteArray> packets = encodeInUnevenPieces(encoder, tone());
    // 50 packets of 20 ms, less the few the encoder still holds.
    CHECK(packets.size() >= 45 && packets.size() <= 50);
    for (const QByteArray& packet : packets) {
        CHECK(!packet.isEmpty());
        CHECK(static_cast<std::size_t>(packet.size()) <= chat::kMaxScreenAudioBytes);
    }
}

// Decoding converts to whatever the output device takes; mono 16-bit at
// 44.1 kHz exercises the channel, sample format and rate conversions at once.
void decodesToTheDeviceFormat() {
    ScreenAudioEncoder encoder;
    CHECK(encoder.open());
    const QList<QByteArray> packets = encodeInUnevenPieces(encoder, tone());

    QAudioFormat format;
    format.setSampleRate(44100);
    format.setChannelCount(1);
    format.setSampleFormat(QAudioFormat::Int16);
    ScreenAudioDecoder decoder(format);
    QByteArray pcm;
    for (const QByteArray& packet : packets) {
        pcm += decoder.decode(packet);
    }

    const qsizetype frames = pcm.size() / format.bytesPerFrame();
    const qsizetype expected = packets.size() * 44100 / 50;
    CHECK(std::abs(frames - expected) < 44100 / 50);

    // Past the encoder's start-up delay, the tone comes through at its level.
    std::vector<qint16> samples(static_cast<size_t>(frames));
    std::memcpy(samples.data(), pcm.constData(), samples.size() * sizeof(qint16));
    double sum = 0;
    for (size_t i = samples.size() / 2; i < samples.size(); ++i) {
        sum += static_cast<double>(samples[i]) * samples[i];
    }
    const double rms = std::sqrt(sum / static_cast<double>(samples.size() - samples.size() / 2));
    const double expectedRms = kAmplitude / std::numbers::sqrt2 * 32768;
    CHECK(std::abs(rms - expectedRms) < expectedRms * 0.1);
}

void ignoresDamagedPackets() {
    QAudioFormat format;
    format.setSampleRate(48000);
    format.setChannelCount(2);
    format.setSampleFormat(QAudioFormat::Float);
    ScreenAudioDecoder decoder(format);
    decoder.decode(QByteArray());
    decoder.decode(QByteArray("\xff\xff\xff\xff not opus", 13));

    // The stream still decodes afterwards.
    ScreenAudioEncoder encoder;
    CHECK(encoder.open());
    QByteArray pcm;
    for (const QByteArray& packet : encodeInUnevenPieces(encoder, tone())) {
        pcm += decoder.decode(packet);
    }
    CHECK(!pcm.isEmpty());
}

} // namespace

int main() {
    packetsFitTheProtocol();
    decodesToTheDeviceFormat();
    ignoresDamagedPackets();
    return 0;
}
