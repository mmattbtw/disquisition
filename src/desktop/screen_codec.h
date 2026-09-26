#pragma once

#include <QByteArray>
#include <QImage>
#include <QSize>
#include <QString>

#include <memory>

enum class ScreenCodec { Jpeg, H264 };

// H.264 through FFmpeg, on the GPU when an encoder for it opens and in
// software otherwise. Each keyframe carries its parameter sets, so a viewer can
// start decoding at any keyframe. Not thread-safe: one thread uses it at a time.
class H264Encoder {
public:
    H264Encoder();
    ~H264Encoder();
    H264Encoder(const H264Encoder&) = delete;
    H264Encoder& operator=(const H264Encoder&) = delete;

    // False when the app was built without FFmpeg.
    static bool available();

    // Encodes `image`, scaled down to fit `maxSize`. `ptsMs` must increase.
    // `out` may stay empty while the encoder buffers. Returns false when no
    // encoder can be opened.
    bool encode(const QImage& image, const QSize& maxSize, int fps, qint64 ptsMs, QByteArray& out,
                bool& keyframe);
    // The FFmpeg encoder in use, such as "h264_nvenc".
    QString name() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

// Turns received frames back into images. H.264 frames depend on earlier ones,
// so each sender needs its own decoder, fed every frame in order.
class ScreenDecoder {
public:
    explicit ScreenDecoder(ScreenCodec codec);
    ~ScreenDecoder();
    ScreenDecoder(const ScreenDecoder&) = delete;
    ScreenDecoder& operator=(const ScreenDecoder&) = delete;

    // Returns a null image until a whole picture has been decoded.
    QImage decode(const QByteArray& data);

private:
    ScreenCodec codec_;
    struct State;
    std::unique_ptr<State> state_;
};
