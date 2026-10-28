#include "desktop/screen_audio.h"

#include <QAudioSink>
#include <QMediaDevices>

#include <cstring>
#include <vector>

#include "desktop/audio_device.h"
#include "desktop/system_audio_capture.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
}

namespace {

// libopus first; FFmpeg's own encoder is experimental but always built in.
constexpr const char* kEncoders[] = {"libopus", "opus"};
// Keeps music clear while staying small next to the video.
constexpr qint64 kBitrate = 128'000;
// Long enough to ride out packets that video holds up on the connection,
// short enough to stay in step with the picture.
constexpr qint64 kPlaybackBufferUs = 200'000;

// Capture delivers interleaved floats, which libopus takes as they are.
AVSampleFormat sampleFormatFor(const AVCodecContext* context, const AVCodec* codec) {
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
    const void* configs = nullptr;
    int count = 0;
    if (avcodec_get_supported_config(context, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &configs,
                                     &count) < 0 ||
        !configs) {
        return AV_SAMPLE_FMT_FLT;
    }
    const auto* formats = static_cast<const AVSampleFormat*>(configs);
#else
    // FFmpeg before 7.1 (e.g. Ubuntu 24.04) lists the formats on the codec.
    (void)context;
    const AVSampleFormat* formats = codec->sample_fmts;
    if (!formats) {
        return AV_SAMPLE_FMT_FLT;
    }
    int count = 0;
    while (formats[count] != AV_SAMPLE_FMT_NONE) {
        ++count;
    }
#endif
    for (int i = 0; i < count; ++i) {
        if (formats[i] == AV_SAMPLE_FMT_FLT) {
            return AV_SAMPLE_FMT_FLT;
        }
    }
    return count > 0 ? formats[0] : AV_SAMPLE_FMT_NONE;
}

AVSampleFormat sampleFormatOf(QAudioFormat::SampleFormat format) {
    switch (format) {
        case QAudioFormat::UInt8:
            return AV_SAMPLE_FMT_U8;
        case QAudioFormat::Int16:
            return AV_SAMPLE_FMT_S16;
        case QAudioFormat::Int32:
            return AV_SAMPLE_FMT_S32;
        case QAudioFormat::Float:
            return AV_SAMPLE_FMT_FLT;
        default:
            return AV_SAMPLE_FMT_NONE;
    }
}

AVChannelLayout shareLayout() {
    AVChannelLayout layout;
    av_channel_layout_default(&layout, kShareAudioChannels);
    return layout;
}

} // namespace

struct ScreenAudioEncoder::State {
    AVCodecContext* context = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    SwrContext* converter = nullptr;
    std::vector<float> pending; // interleaved samples not yet in a packet
    qint64 pts = 0;

    ~State() {
        swr_free(&converter);
        av_packet_free(&packet);
        av_frame_free(&frame);
        avcodec_free_context(&context);
    }
};

ScreenAudioEncoder::ScreenAudioEncoder() : state_(std::make_unique<State>()) {}

ScreenAudioEncoder::~ScreenAudioEncoder() = default;

bool ScreenAudioEncoder::open() {
    State& s = *state_;
    const AVChannelLayout layout = shareLayout();
    for (const char* name : kEncoders) {
        const AVCodec* codec = avcodec_find_encoder_by_name(name);
        if (!codec) {
            continue;
        }
        s.context = avcodec_alloc_context3(codec);
        s.context->sample_rate = kShareAudioRate;
        s.context->ch_layout = layout;
        s.context->sample_fmt = sampleFormatFor(s.context, codec);
        s.context->bit_rate = kBitrate;
        s.context->time_base = {1, kShareAudioRate};
        s.context->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
        if (avcodec_open2(s.context, codec, nullptr) == 0) {
            break;
        }
        avcodec_free_context(&s.context);
    }
    // Both encoders cut fixed 20 ms frames; packets are built to that size.
    if (!s.context || s.context->frame_size <= 0) {
        return false;
    }

    s.frame = av_frame_alloc();
    s.frame->nb_samples = s.context->frame_size;
    s.frame->format = s.context->sample_fmt;
    s.frame->sample_rate = kShareAudioRate;
    s.frame->ch_layout = layout;
    s.packet = av_packet_alloc();
    return av_frame_get_buffer(s.frame, 0) == 0 &&
           swr_alloc_set_opts2(&s.converter, &layout, s.context->sample_fmt, kShareAudioRate,
                               &layout, AV_SAMPLE_FMT_FLT, kShareAudioRate, 0, nullptr) == 0 &&
           swr_init(s.converter) == 0;
}

QList<QByteArray> ScreenAudioEncoder::encode(const float* interleaved, int frames) {
    State& s = *state_;
    QList<QByteArray> packets;
    s.pending.insert(s.pending.end(), interleaved, interleaved + frames * kShareAudioChannels);
    const int frameSize = s.context->frame_size;
    const size_t packetSamples = static_cast<size_t>(frameSize) * kShareAudioChannels;
    size_t consumed = 0;
    while (s.pending.size() - consumed >= packetSamples) {
        if (av_frame_make_writable(s.frame) < 0) {
            break;
        }
        const auto* input = reinterpret_cast<const uint8_t*>(s.pending.data() + consumed);
        swr_convert(s.converter, s.frame->data, frameSize, &input, frameSize);
        consumed += packetSamples;
        s.frame->pts = s.pts;
        s.pts += frameSize;
        if (avcodec_send_frame(s.context, s.frame) < 0) {
            continue;
        }
        while (avcodec_receive_packet(s.context, s.packet) == 0) {
            packets.append(QByteArray(reinterpret_cast<const char*>(s.packet->data), s.packet->size));
            av_packet_unref(s.packet);
        }
    }
    s.pending.erase(s.pending.begin(), s.pending.begin() + static_cast<std::ptrdiff_t>(consumed));
    return packets;
}

struct ScreenAudioDecoder::State {
    AVCodecContext* context = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    // Made for the first decoded frame, which says what the decoder outputs.
    SwrContext* converter = nullptr;
    QAudioFormat format;

    ~State() {
        swr_free(&converter);
        av_packet_free(&packet);
        av_frame_free(&frame);
        avcodec_free_context(&context);
    }

    bool convertFrom(const AVFrame& decoded) {
        AVChannelLayout output;
        av_channel_layout_default(&output, format.channelCount());
        return swr_alloc_set_opts2(&converter, &output, sampleFormatOf(format.sampleFormat()),
                                   format.sampleRate(), &decoded.ch_layout,
                                   static_cast<AVSampleFormat>(decoded.format),
                                   decoded.sample_rate, 0, nullptr) == 0 &&
               swr_init(converter) == 0;
    }
};

ScreenAudioDecoder::ScreenAudioDecoder(const QAudioFormat& format)
    : state_(std::make_unique<State>()) {
    State& s = *state_;
    s.format = format;
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_OPUS);
    if (!codec || sampleFormatOf(format.sampleFormat()) == AV_SAMPLE_FMT_NONE) {
        return;
    }
    s.context = avcodec_alloc_context3(codec);
    s.context->sample_rate = kShareAudioRate;
    s.context->ch_layout = shareLayout();
    if (avcodec_open2(s.context, codec, nullptr) < 0) {
        avcodec_free_context(&s.context);
        return;
    }
    s.frame = av_frame_alloc();
    s.packet = av_packet_alloc();
}

ScreenAudioDecoder::~ScreenAudioDecoder() = default;

QByteArray ScreenAudioDecoder::decode(const QByteArray& packet) {
    State& s = *state_;
    if (!s.context || av_new_packet(s.packet, static_cast<int>(packet.size())) < 0) {
        return {};
    }
    std::memcpy(s.packet->data, packet.constData(), static_cast<size_t>(packet.size()));
    const int sent = avcodec_send_packet(s.context, s.packet);
    av_packet_unref(s.packet);
    if (sent < 0) {
        return {};
    }

    QByteArray pcm;
    const int bytesPerFrame = s.format.bytesPerFrame();
    while (avcodec_receive_frame(s.context, s.frame) == 0) {
        if (!s.converter && !s.convertFrom(*s.frame)) {
            av_frame_unref(s.frame);
            return {};
        }
        const int capacity = swr_get_out_samples(s.converter, s.frame->nb_samples);
        const qsizetype offset = pcm.size();
        pcm.resize(offset + static_cast<qsizetype>(capacity) * bytesPerFrame);
        auto* output = reinterpret_cast<uint8_t*>(pcm.data() + offset);
        const int converted = swr_convert(s.converter, &output, capacity,
                                          const_cast<const uint8_t**>(s.frame->extended_data),
                                          s.frame->nb_samples);
        pcm.resize(offset + static_cast<qsizetype>(qMax(converted, 0)) * bytesPerFrame);
        av_frame_unref(s.frame);
    }
    return pcm;
}

ScreenAudioPlayer::ScreenAudioPlayer(QObject* parent) : QObject(parent) {
    setOutputDevice({});
}

ScreenAudioPlayer::~ScreenAudioPlayer() = default;

void ScreenAudioPlayer::setOutputDevice(const QString& id) {
    device_ = findAudioDevice(QMediaDevices::audioOutputs(), id, QMediaDevices::defaultAudioOutput());
    format_ = supportedAudioFormat(device_);
}

void ScreenAudioPlayer::play(const QString& sharer, const QByteArray& packet) {
    auto stream = streams_.find(sharer);
    if (stream == streams_.end()) {
        if (!format_.isValid()) {
            return;
        }
        Stream started;
        started.sink = std::make_unique<QAudioSink>(device_, format_);
        started.sink->setBufferSize(format_.bytesForDuration(kPlaybackBufferUs));
        started.device = started.sink->start();
        if (!started.device) {
            return;
        }
        started.decoder = std::make_unique<ScreenAudioDecoder>(format_);
        stream = streams_.emplace(sharer, std::move(started)).first;
    }
    stream->second.device->write(stream->second.decoder->decode(packet));
}

void ScreenAudioPlayer::remove(const QString& sharer) {
    streams_.erase(sharer);
}
