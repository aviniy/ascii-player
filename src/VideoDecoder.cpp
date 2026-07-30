#include "VideoDecoder.h"

#include "Timer.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

namespace {

std::string ffmpegError(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

void requireFfmpeg(int code, const char* operation) {
    if (code < 0) {
        throw std::runtime_error(std::string(operation) + ": " + ffmpegError(code));
    }
}

std::shared_ptr<AVFrame> cloneFrame(const AVFrame* source) {
    AVFrame* cloned = av_frame_clone(source);
    if (!cloned) {
        throw std::bad_alloc();
    }
    return std::shared_ptr<AVFrame>(cloned, [](AVFrame* frame) {
        av_frame_free(&frame);
    });
}

std::shared_ptr<AVPacket> clonePacket(const AVPacket* source) {
    AVPacket* cloned = av_packet_clone(source);
    if (!cloned) {
        throw std::bad_alloc();
    }
    return std::shared_ptr<AVPacket>(cloned, [](AVPacket* packet) {
        av_packet_free(&packet);
    });
}

struct PacketDeleter {
    void operator()(AVPacket* packet) const {
        av_packet_free(&packet);
    }
};

struct FrameDeleter {
    void operator()(AVFrame* frame) const {
        av_frame_free(&frame);
    }
};

} // namespace

struct VideoDecoder::Impl {
    explicit Impl(std::string path) : inputPath(std::move(path)) {}

    ~Impl() {
        if (videoCodecContext) {
            avcodec_free_context(&videoCodecContext);
        }
        if (formatContext) {
            avformat_close_input(&formatContext);
        }
    }

    std::string inputPath;
    AVFormatContext* formatContext{};
    AVCodecContext* videoCodecContext{};
    int videoStreamIndex{-1};
    int audioStreamIndex{-1};
    VideoInfo videoInfo;
    AudioStreamDescription audioStream;
    bool opened{};
};

VideoDecoder::VideoDecoder(std::string inputPath)
    : impl_(std::make_unique<Impl>(std::move(inputPath))) {}

VideoDecoder::~VideoDecoder() = default;

void VideoDecoder::open() {
    if (impl_->opened) {
        return;
    }

    avformat_network_init();
    requireFfmpeg(
        avformat_open_input(&impl_->formatContext, impl_->inputPath.c_str(), nullptr, nullptr),
        "Could not open input");
    requireFfmpeg(
        avformat_find_stream_info(impl_->formatContext, nullptr),
        "Could not read stream information");

    const AVCodec* videoCodec = nullptr;
    impl_->videoStreamIndex = av_find_best_stream(
        impl_->formatContext, AVMEDIA_TYPE_VIDEO, -1, -1, &videoCodec, 0);
    requireFfmpeg(impl_->videoStreamIndex, "No decodable video stream");

    const AVStream* videoStream = impl_->formatContext->streams[impl_->videoStreamIndex];
    impl_->videoCodecContext = avcodec_alloc_context3(videoCodec);
    if (!impl_->videoCodecContext) {
        throw std::bad_alloc();
    }
    requireFfmpeg(
        avcodec_parameters_to_context(
            impl_->videoCodecContext, videoStream->codecpar),
        "Could not copy video codec parameters");
    requireFfmpeg(
        avcodec_open2(impl_->videoCodecContext, videoCodec, nullptr),
        "Could not open video decoder");

    AVRational guessedRate = av_guess_frame_rate(
        impl_->formatContext,
        impl_->formatContext->streams[impl_->videoStreamIndex],
        nullptr);
    double fps = av_q2d(guessedRate);
    if (!std::isfinite(fps) || fps <= 0.0) {
        fps = 30.0;
    }

    impl_->videoInfo.width = impl_->videoCodecContext->width;
    impl_->videoInfo.height = impl_->videoCodecContext->height;
    impl_->videoInfo.fps = fps;
    impl_->videoInfo.durationSeconds =
        impl_->formatContext->duration == AV_NOPTS_VALUE
            ? 0.0
            : static_cast<double>(impl_->formatContext->duration) / AV_TIME_BASE;
    impl_->videoInfo.startTime =
        impl_->formatContext->start_time == AV_NOPTS_VALUE
            ? 0.0
            : static_cast<double>(impl_->formatContext->start_time) / AV_TIME_BASE;
    impl_->videoInfo.videoCodec = videoCodec->name ? videoCodec->name : "unknown";

    const AVCodec* audioCodec = nullptr;
    impl_->audioStreamIndex = av_find_best_stream(
        impl_->formatContext, AVMEDIA_TYPE_AUDIO, -1, -1, &audioCodec, 0);
    if (impl_->audioStreamIndex >= 0) {
        const AVStream* audioStream =
            impl_->formatContext->streams[impl_->audioStreamIndex];
        AVCodecParameters* parameters = avcodec_parameters_alloc();
        if (!parameters) {
            throw std::bad_alloc();
        }
        impl_->audioStream.codecParameters =
            std::shared_ptr<AVCodecParameters>(parameters, [](AVCodecParameters* value) {
                avcodec_parameters_free(&value);
            });
        requireFfmpeg(
            avcodec_parameters_copy(parameters, audioStream->codecpar),
            "Could not copy audio codec parameters");
        impl_->audioStream.timeBaseNumerator = audioStream->time_base.num;
        impl_->audioStream.timeBaseDenominator = audioStream->time_base.den;
        impl_->videoInfo.hasAudio = true;
        impl_->videoInfo.audioCodec = audioCodec && audioCodec->name
            ? audioCodec->name
            : "unknown";
    }

    impl_->opened = true;
}

void VideoDecoder::decode(
    BlockingQueue<DecodedVideoFrame>& videoFrames,
    BlockingQueue<EncodedAudioPacket>* audioPackets,
    PlaybackClock& clock,
    PlaybackStatistics& statistics,
    std::uint64_t maxFrames) {
    if (!impl_->opened) {
        throw std::logic_error("VideoDecoder::open must be called before decode");
    }

    AVPacket* rawPacket = av_packet_alloc();
    AVFrame* rawFrame = av_frame_alloc();
    if (!rawPacket || !rawFrame) {
        av_packet_free(&rawPacket);
        av_frame_free(&rawFrame);
        throw std::bad_alloc();
    }
    const std::unique_ptr<AVPacket, PacketDeleter> packet(rawPacket);
    const std::unique_ptr<AVFrame, FrameDeleter> frame(rawFrame);

    const AVStream* videoStream =
        impl_->formatContext->streams[impl_->videoStreamIndex];
    const double timeBase = av_q2d(videoStream->time_base);
    const double fallbackDuration = 1.0 / impl_->videoInfo.fps;
    double nextPresentationTime = impl_->videoInfo.startTime;
    std::uint64_t emittedFrames = 0;

    auto receiveVideoFrames = [&]() -> bool {
        while (!clock.stopped()) {
            const int result = avcodec_receive_frame(impl_->videoCodecContext, frame.get());
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
                return true;
            }
            requireFfmpeg(result, "Video decode failed");

            double presentationTime = nextPresentationTime;
            if (frame->best_effort_timestamp != AV_NOPTS_VALUE) {
                presentationTime =
                    static_cast<double>(frame->best_effort_timestamp) * timeBase;
            }

            double frameDuration = fallbackDuration;
            if (frame->duration > 0) {
                frameDuration = static_cast<double>(frame->duration) * timeBase;
            }
            nextPresentationTime = presentationTime + frameDuration;

            if (!clock.started()) {
                clock.start(presentationTime);
            }

            DecodedVideoFrame output{
                cloneFrame(frame.get()),
                presentationTime,
                frameDuration
            };
            av_frame_unref(frame.get());

            if (!videoFrames.push(std::move(output))) {
                return false;
            }
            statistics.decodedFrames.fetch_add(1, std::memory_order_relaxed);
            ++emittedFrames;
            if (maxFrames != 0 && emittedFrames >= maxFrames) {
                return false;
            }
        }
        return false;
    };

    bool reachedLimit = false;
    while (!clock.stopped()) {
        const int readResult = av_read_frame(impl_->formatContext, packet.get());
        if (readResult == AVERROR_EOF) {
            break;
        }
        requireFfmpeg(readResult, "Demux failed");

        if (packet->stream_index == impl_->videoStreamIndex) {
            const int sendResult =
                avcodec_send_packet(impl_->videoCodecContext, packet.get());
            if (sendResult != AVERROR(EAGAIN)) {
                requireFfmpeg(sendResult, "Could not submit video packet");
            }
            if (!receiveVideoFrames()) {
                reachedLimit = maxFrames != 0 && emittedFrames >= maxFrames;
                av_packet_unref(packet.get());
                break;
            }
        } else if (
            audioPackets &&
            packet->stream_index == impl_->audioStreamIndex) {
            if (!audioPackets->push(EncodedAudioPacket{clonePacket(packet.get())})) {
                av_packet_unref(packet.get());
                break;
            }
        }
        av_packet_unref(packet.get());
    }

    if (!reachedLimit && !clock.stopped()) {
        requireFfmpeg(
            avcodec_send_packet(impl_->videoCodecContext, nullptr),
            "Could not flush video decoder");
        receiveVideoFrames();
    }

    if (!clock.started() && !clock.stopped()) {
        clock.start(impl_->videoInfo.startTime);
    }
    videoFrames.close();
    if (audioPackets) {
        audioPackets->close();
    }
}

const VideoInfo& VideoDecoder::info() const noexcept {
    return impl_->videoInfo;
}

const AudioStreamDescription& VideoDecoder::audioDescription() const noexcept {
    return impl_->audioStream;
}
