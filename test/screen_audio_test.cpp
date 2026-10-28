#include <QByteArray>
#include <QList>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <vector>

#include "check.h"
#include "common/media_limits.h"
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

QByteArray readFrom(PlaybackBuffer& buffer, qint64 size) {
    QByteArray data(size, '\x7f');
    CHECK(buffer.read(data.data(), size) == size);
    return data;
}

// Bytes of one sample frame; values are distinct so order can be checked.
QByteArray frames(int count, char first) {
    QByteArray pcm;
    for (int i = 0; i < count; ++i) {
        pcm += QByteArray(4, static_cast<char>(first + i));
    }
    return pcm;
}

// Long enough that the cushion never shrinks during a test.
constexpr qsizetype kNeverSettles = 1 << 20;

void playbackWaitsForItsCushion() {
    PlaybackBuffer buffer(8 * 4, 16 * 4, kNeverSettles, 4);
    buffer.push(frames(4, 1));
    // Short of the cushion: silence, and nothing is used up.
    CHECK(readFrom(buffer, 8) == QByteArray(8, 0));
    buffer.push(frames(4, 5));
    CHECK(readFrom(buffer, 8 * 4) == frames(8, 1));
}

void runningDryGrowsTheCushionByTheGap() {
    PlaybackBuffer buffer(4 * 4, 16 * 4, kNeverSettles, 4);
    buffer.push(frames(4, 1));
    CHECK(readFrom(buffer, 6 * 4) == frames(4, 1) + QByteArray(2 * 4, 0));
    CHECK(readFrom(buffer, 4) == QByteArray(4, 0));
    // Sound was missing for 3 frames, so the cushion grows by those and a
    // step: 4 + 3 + 1 = 8 frames.
    buffer.push(frames(7, 10));
    CHECK(readFrom(buffer, 4) == QByteArray(4, 0));
    buffer.push(frames(1, 17));
    CHECK(readFrom(buffer, 8 * 4) == frames(8, 10));
}

void theCushionStopsGrowingAtItsMaximum() {
    PlaybackBuffer buffer(4 * 4, 6 * 4, kNeverSettles, 4);
    for (int i = 0; i < 5; ++i) {
        buffer.push(frames(8, 1));
        readFrom(buffer, 9 * 4);
    }
    buffer.push(frames(6, 1));
    CHECK(readFrom(buffer, 6 * 4) == frames(6, 1));
}

void aBurstIsCutBackToTheCushion() {
    PlaybackBuffer buffer(4 * 4, 8 * 4, kNeverSettles, 4);
    buffer.push(frames(10, 1));
    // More than twice the cushion keeps only a cushion's worth, in whole frames.
    CHECK(readFrom(buffer, 4 * 4) == frames(4, 7));
}

// Runs a buffer with a 4-frame cushion dry for one frame; the next push grows
// the cushion to 6.
void runDryForOneFrame(PlaybackBuffer& buffer) {
    buffer.push(frames(4, 1));
    readFrom(buffer, 5 * 4);
}

void theCushionShrinksWhenItHasRoomToSpare() {
    PlaybackBuffer buffer(4 * 4, 16 * 4, 8 * 4, 4);
    runDryForOneFrame(buffer);
    buffer.push(frames(12, 1));
    // 8 frames of steady playback never went below 4 buffered, so the cushion
    // shrinks to 5.
    readFrom(buffer, 4 * 4);
    readFrom(buffer, 4 * 4);
    // 11 frames is more than twice 5 but not twice 6, so it is cut back to 5.
    buffer.push(frames(7, 30));
    CHECK(readFrom(buffer, 5 * 4) == frames(5, 32));
}

void theCushionKeepsItsSizeWhenItIsNeeded() {
    PlaybackBuffer buffer(4 * 4, 16 * 4, 8 * 4, 4);
    runDryForOneFrame(buffer);
    buffer.push(frames(8, 1));
    // Steady playback that used up everything buffered leaves it at 6.
    readFrom(buffer, 8 * 4);
    buffer.push(frames(13, 30));
    CHECK(readFrom(buffer, 6 * 4) == frames(6, 37));
}

} // namespace

int main() {
    packetsFitTheProtocol();
    decodesToTheDeviceFormat();
    ignoresDamagedPackets();
    playbackWaitsForItsCushion();
    runningDryGrowsTheCushionByTheGap();
    theCushionStopsGrowingAtItsMaximum();
    aBurstIsCutBackToTheCushion();
    theCushionShrinksWhenItHasRoomToSpare();
    theCushionKeepsItsSizeWhenItIsNeeded();
    return 0;
}
