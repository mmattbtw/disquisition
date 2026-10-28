#include "desktop/screen_audio.h"

#include <QAudioSink>
#include <QMediaDevices>
#include <QMutexLocker>

#include <algorithm>
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
// Enough to ride out ordinary jitter, little enough to stay in step with the
// picture.
constexpr qint64 kPrefillUs = 60'000;
constexpr qint64 kMaxCushionUs = 200'000;
constexpr qint64 kSettleUs = 5'000'000;
constexpr qint64 kSinkBufferUs = 40'000;

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

PlaybackBuffer::PlaybackBuffer(qsizetype prefill, qsizetype maxCushion, qsizetype settle,
                               int bytesPerFrame)
    : prefill_(prefill), cushion_(prefill), growth_(prefill / 4 / bytesPerFrame * bytesPerFrame),
      maxCushion_(maxCushion), settle_(settle), bytesPerFrame_(bytesPerFrame) {
    open(QIODevice::ReadOnly | QIODevice::Unbuffered);
}

void PlaybackBuffer::push(const QByteArray& pcm) {
    QMutexLocker lock(&mutex_);
    if (starving_) {
        cushion_ = std::min(cushion_ + starved_ + growth_, maxCushion_);
        starving_ = false;
    }
    pending_ += pcm;
    if (pending_.size() > 2 * cushion_) {
        const qsizetype excess = pending_.size() - cushion_;
        pending_.remove(0, excess - excess % bytesPerFrame_);
    }
}

qint64 PlaybackBuffer::readData(char* data, qint64 size) {
    QMutexLocker lock(&mutex_);
    if (!playing_ && pending_.size() >= cushion_) {
        playing_ = true;
    }
    qsizetype copied = 0;
    if (playing_) {
        copied = std::min(static_cast<qsizetype>(size), pending_.size());
        std::memcpy(data, pending_.constData(), static_cast<size_t>(copied));
        pending_.remove(0, copied);
        if (copied < size) {
            playing_ = false;
            starving_ = true;
            starved_ = 0;
            steady_ = 0;
            lowest_ = kUnmeasured;
        }
        else {
            lowest_ = std::min(lowest_, pending_.size());
            if ((steady_ += copied) >= settle_) {
                if (lowest_ > growth_) {
                    cushion_ = std::max(cushion_ - growth_, prefill_);
                }
                steady_ = 0;
                lowest_ = kUnmeasured;
            }
        }
    }
    if (starving_) {
        starved_ += size - copied;
    }
    std::memset(data + copied, 0, static_cast<size_t>(size - copied));
    return size;
}

ScreenAudioPlayer::ScreenAudioPlayer(QObject* parent) : QObject(parent) {
    thread_.setObjectName("Share playback");
    worker_ = new QObject;
    worker_->moveToThread(&thread_);
    thread_.start(QThread::TimeCriticalPriority);
    setOutputDevice({});
}

ScreenAudioPlayer::~ScreenAudioPlayer() {
    // Sinks belong to the playback thread and must stop there.
    QMetaObject::invokeMethod(worker_, [this] { streams_.clear(); },
                              Qt::BlockingQueuedConnection);
    thread_.quit();
    thread_.wait();
    delete worker_;
}

void ScreenAudioPlayer::post(std::function<void()> work) {
    QMetaObject::invokeMethod(worker_, std::move(work), Qt::QueuedConnection);
}

void ScreenAudioPlayer::setOutputDevice(const QString& id) {
    const QAudioDevice device =
        findAudioDevice(QMediaDevices::audioOutputs(), id, QMediaDevices::defaultAudioOutput());
    const QAudioFormat format = supportedAudioFormat(device);
    post([this, device, format] {
        device_ = device;
        format_ = format;
        streams_.clear();
    });
}

void ScreenAudioPlayer::setVolume(const QString& sharer, qreal volume) {
    post([this, sharer, volume] {
        volumes_[sharer] = volume;
        if (const auto stream = streams_.find(sharer); stream != streams_.end()) {
            stream->second.sink->setVolume(volume);
        }
    });
}

void ScreenAudioPlayer::play(const QString& sharer, const QByteArray& packet) {
    post([this, sharer, packet] {
        if (!streams_.contains(sharer)) {
            start(sharer);
        }
        if (const auto stream = streams_.find(sharer); stream != streams_.end()) {
            stream->second.buffer->push(stream->second.decoder->decode(packet));
        }
    });
}

void ScreenAudioPlayer::start(const QString& sharer) {
    if (!format_.isValid()) {
        return;
    }
    Stream started;
    started.decoder = std::make_unique<ScreenAudioDecoder>(format_);
    started.buffer = std::make_unique<PlaybackBuffer>(
        format_.bytesForDuration(kPrefillUs), format_.bytesForDuration(kMaxCushionUs),
        format_.bytesForDuration(kSettleUs), format_.bytesPerFrame());
    started.sink = std::make_unique<QAudioSink>(device_, format_);
    started.sink->setBufferSize(format_.bytesForDuration(kSinkBufferUs));
    if (const auto volume = volumes_.find(sharer); volume != volumes_.end()) {
        started.sink->setVolume(volume->second);
    }
    started.sink->start(started.buffer.get());
    if (started.sink->error() != QAudio::NoError) {
        return;
    }
    streams_.emplace(sharer, std::move(started));
}

void ScreenAudioPlayer::remove(const QString& sharer) {
    post([this, sharer] { streams_.erase(sharer); });
}
