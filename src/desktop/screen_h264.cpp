#include "desktop/screen_h264.h"

#include <cstring>
#include <string_view>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace {

// Encoders in order of preference. Hardware ones fail to open on machines
// without that GPU, and the next is tried.
constexpr const char* kEncoders[] = {"h264_nvenc", "h264_videotoolbox", "h264_qsv", "h264_amf",
                                     "h264_mf",    "libx264",           "libopenh264"};

AVPixelFormat pixelFormatOf(QImage::Format format) {
    switch (format) {
        case QImage::Format_RGB32:
        case QImage::Format_ARGB32:
        case QImage::Format_ARGB32_Premultiplied:
            return AV_PIX_FMT_RGB32;
        case QImage::Format_RGBX8888:
        case QImage::Format_RGBA8888:
        case QImage::Format_RGBA8888_Premultiplied:
            return AV_PIX_FMT_RGBA;
        default:
            return AV_PIX_FMT_NONE;
    }
}

// Hardware encoders such as NVENC take the captured RGB frame as is and convert
// it on the GPU, which saves 10-20 ms of CPU work per 1080p frame. Other
// encoders get NV12 or YUV 4:2:0, converted on the CPU.
AVPixelFormat inputFormatFor(const AVCodecContext* context, const AVCodec* codec,
                             AVPixelFormat sourceFormat) {
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
    const void* configs = nullptr;
    int count = 0;
    if (avcodec_get_supported_config(context, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &configs,
                                     &count) < 0 ||
        !configs) {
        return AV_PIX_FMT_YUV420P;
    }
    const auto* formats = static_cast<const AVPixelFormat*>(configs);
#else
    // FFmpeg before 7.1 (e.g. Ubuntu 24.04) lists the formats on the codec.
    (void)context;
    const AVPixelFormat* formats = codec->pix_fmts;
    if (!formats) {
        return AV_PIX_FMT_YUV420P;
    }
    int count = 0;
    while (formats[count] != AV_PIX_FMT_NONE) {
        ++count;
    }
#endif
    for (const AVPixelFormat wanted : {sourceFormat, AV_PIX_FMT_NV12, AV_PIX_FMT_YUV420P}) {
        for (int i = 0; i < count; ++i) {
            if (formats[i] == wanted) {
                return wanted;
            }
        }
    }
    return AV_PIX_FMT_NONE;
}

// Low-latency settings: no lookahead or frame reordering, so each frame comes
// out as soon as it goes in.
void tune(std::string_view name, void* options) {
    const auto set = [options](const char* key, const char* value) {
        av_opt_set(options, key, value, 0);
    };
    if (name == "h264_nvenc") {
        set("preset", "p4");
        set("tune", "ull");
        set("zerolatency", "1");
        set("delay", "0");
        set("forced-idr", "1");
    }
    else if (name == "h264_qsv") {
        set("preset", "veryfast");
        set("async_depth", "1");
    }
    else if (name == "h264_amf") {
        set("usage", "ultralowlatency");
        set("quality", "speed");
    }
    else if (name == "h264_mf") {
        set("scenario", "display_remoting");
    }
    else if (name == "h264_videotoolbox") {
        set("realtime", "1");
        set("prio_speed", "1");
    }
    else if (name == "libx264") {
        set("preset", "veryfast");
        set("tune", "zerolatency");
    }
}

// About 0.08 bits per pixel: 1080p30 gets 5 Mbps and 1080p60 10 Mbps.
qint64 bitrateFor(const QSize& size, int fps) {
    const qint64 bits = static_cast<qint64>(size.width()) * size.height() * fps * 8 / 100;
    return qBound(qint64 {1'000'000}, bits, qint64 {20'000'000});
}

} // namespace

struct H264Encoder::State {
    AVCodecContext* context = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    SwsContext* scaler = nullptr;
    QSize size;
    int fps = 0;
    AVPixelFormat sourceFormat = AV_PIX_FMT_NONE;
    qint64 lastPts = -1;
    QString name;

    ~State() {
        sws_freeContext(scaler);
        av_packet_free(&packet);
        av_frame_free(&frame);
        avcodec_free_context(&context);
    }

    bool open(const QSize& target, int framesPerSecond, AVPixelFormat source) {
        size = target;
        fps = framesPerSecond;
        sourceFormat = source;
        // A missing GPU makes the hardware encoders log errors while failing.
        const int logLevel = av_log_get_level();
        av_log_set_level(AV_LOG_QUIET);
        for (const char* encoderName : kEncoders) {
            const AVCodec* codec = avcodec_find_encoder_by_name(encoderName);
            if (!codec) {
                continue;
            }
            AVCodecContext* candidate = avcodec_alloc_context3(codec);
            candidate->width = size.width();
            candidate->height = size.height();
            candidate->pix_fmt = inputFormatFor(candidate, codec, sourceFormat);
            candidate->time_base = {1, 1000};
            candidate->framerate = {fps, 1};
            // A keyframe every two seconds lets a new viewer start quickly.
            candidate->gop_size = fps * 2;
            candidate->max_b_frames = 0;
            candidate->bit_rate = bitrateFor(size, fps);
            candidate->rc_max_rate = candidate->bit_rate * 3 / 2;
            candidate->rc_buffer_size = static_cast<int>(candidate->bit_rate);
            candidate->flags |= AV_CODEC_FLAG_LOW_DELAY;
            tune(encoderName, candidate->priv_data);
            if (candidate->pix_fmt == AV_PIX_FMT_NONE || avcodec_open2(candidate, codec, nullptr) < 0) {
                avcodec_free_context(&candidate);
                continue;
            }
            context = candidate;
            name = QString::fromLatin1(encoderName);
            break;
        }
        av_log_set_level(logLevel);
        if (!context) {
            return false;
        }
        frame = av_frame_alloc();
        packet = av_packet_alloc();
        frame->format = context->pix_fmt;
        frame->width = size.width();
        frame->height = size.height();
        return frame && packet && av_frame_get_buffer(frame, 0) >= 0;
    }
};

H264Encoder::H264Encoder() = default;
H264Encoder::~H264Encoder() = default;

QString H264Encoder::name() const {
    return state_ ? state_->name : QString();
}

bool H264Encoder::encode(const QImage& image, const QSize& maxSize, int fps, qint64 ptsMs,
                         QByteArray& out, bool& keyframe) {
    QSize target = image.size();
    if (target.width() > maxSize.width() || target.height() > maxSize.height()) {
        target.scale(maxSize, Qt::KeepAspectRatio);
    }
    // 4:2:0 chroma needs even dimensions.
    target = QSize(target.width() & ~1, target.height() & ~1);
    if (target.isEmpty()) {
        return true;
    }
    QImage source = image;
    AVPixelFormat sourceFormat = pixelFormatOf(source.format());
    if (sourceFormat == AV_PIX_FMT_NONE) {
        source = source.convertToFormat(QImage::Format_RGB32);
        sourceFormat = AV_PIX_FMT_RGB32;
    }
    bool forceKeyframe = false;
    if (!state_ || state_->size != target || state_->fps != fps ||
        state_->sourceFormat != sourceFormat) {
        state_ = std::make_unique<State>();
        if (!state_->open(target, fps, sourceFormat)) {
            state_.reset();
            return false;
        }
        forceKeyframe = true;
    }
    State& state = *state_;
    if (av_frame_make_writable(state.frame) < 0) {
        return true;
    }

    const uint8_t* const planes[] = {source.constBits()};
    const int strides[] = {static_cast<int>(source.bytesPerLine())};
    if (state.context->pix_fmt == sourceFormat && source.size() == target) {
        // The encoder takes the frame as is: a plain copy, no conversion.
        av_image_copy_plane(state.frame->data[0], state.frame->linesize[0], planes[0], strides[0],
                            static_cast<int>(source.width() * 4), source.height());
    }
    else {
        state.scaler = sws_getCachedContext(state.scaler, source.width(), source.height(),
                                            sourceFormat, target.width(), target.height(),
                                            state.context->pix_fmt, SWS_BILINEAR, nullptr, nullptr,
                                            nullptr);
        if (!state.scaler) {
            return true;
        }
        sws_scale(state.scaler, planes, strides, 0, source.height(), state.frame->data,
                  state.frame->linesize);
    }

    state.lastPts = qMax(ptsMs, state.lastPts + 1);
    state.frame->pts = state.lastPts;
    state.frame->pict_type = forceKeyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    if (avcodec_send_frame(state.context, state.frame) < 0) {
        return true;
    }
    while (avcodec_receive_packet(state.context, state.packet) == 0) {
        out.append(reinterpret_cast<const char*>(state.packet->data), state.packet->size);
        keyframe = keyframe || (state.packet->flags & AV_PKT_FLAG_KEY);
        av_packet_unref(state.packet);
    }
    return true;
}

struct ScreenDecoder::State {
    AVCodecContext* context = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    SwsContext* scaler = nullptr;

    ~State() {
        sws_freeContext(scaler);
        av_packet_free(&packet);
        av_frame_free(&frame);
        avcodec_free_context(&context);
    }
};

ScreenDecoder::ScreenDecoder() {
    const AVCodec* decoder = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!decoder) {
        return;
    }
    auto state = std::make_unique<State>();
    state->context = avcodec_alloc_context3(decoder);
    state->context->flags |= AV_CODEC_FLAG_LOW_DELAY;
    // Frame threading holds pictures back to decode them in parallel.
    state->context->thread_type = FF_THREAD_SLICE;
    state->frame = av_frame_alloc();
    state->packet = av_packet_alloc();
    if (state->frame && state->packet && avcodec_open2(state->context, decoder, nullptr) >= 0) {
        state_ = std::move(state);
    }
}

ScreenDecoder::~ScreenDecoder() = default;

QImage ScreenDecoder::decode(const QByteArray& data) {
    if (!state_ || data.isEmpty() || av_new_packet(state_->packet, static_cast<int>(data.size())) < 0) {
        return {};
    }
    State& state = *state_;
    std::memcpy(state.packet->data, data.constData(), static_cast<size_t>(data.size()));
    const int sent = avcodec_send_packet(state.context, state.packet);
    av_packet_unref(state.packet);
    if (sent < 0) {
        return {};
    }
    QImage image;
    while (avcodec_receive_frame(state.context, state.frame) == 0) {
        const int width = state.frame->width;
        const int height = state.frame->height;
        state.scaler = sws_getCachedContext(state.scaler, width, height,
                                            static_cast<AVPixelFormat>(state.frame->format), width,
                                            height, AV_PIX_FMT_RGB32, SWS_BILINEAR, nullptr,
                                            nullptr, nullptr);
        if (state.scaler) {
            image = QImage(width, height, QImage::Format_RGB32);
            uint8_t* const planes[] = {image.bits()};
            const int strides[] = {static_cast<int>(image.bytesPerLine())};
            sws_scale(state.scaler, state.frame->data, state.frame->linesize, 0, height, planes,
                      strides);
        }
        av_frame_unref(state.frame);
    }
    return image;
}

