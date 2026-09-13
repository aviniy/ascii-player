#include "AudioPlayer.h"

#include "Timer.h"

#include <windows.h>
#include <mmsystem.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libswresample/swresample.h>
}
namespace {

constexpr int kOutputSampleRate = 48'000;
constexpr int kOutputChannels = 2;
constexpr int kWaveBufferCount = 8;

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

void requireWave(MMRESULT result, const char* operation) {
    if (result == MMSYSERR_NOERROR) {
        return;
    }
    char buffer[MAXERRORLENGTH]{};
    waveOutGetErrorTextA(result, buffer, MAXERRORLENGTH);
    throw std::runtime_error(std::string(operation) + ": " + buffer);
}

struct CodecContextDeleter {
    void operator()(AVCodecContext* context) const {
        avcodec_free_context(&context);
    }
};

struct FrameDeleter {
    void operator()(AVFrame* frame) const {
        av_frame_free(&frame);
    }
};

class WaveOutDevice {
public:
    WaveOutDevice() {
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = kOutputChannels;
        format.nSamplesPerSec = kOutputSampleRate;
        format.wBitsPerSample = 16;
        format.nBlockAlign =
            static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8);
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

        requireWave(
            waveOutOpen(
                &handle_,
                WAVE_MAPPER,
                &format,
                0,
                0,
                CALLBACK_NULL),
            "Could not open Windows audio output");
    }

    ~WaveOutDevice() {
        if (!handle_) {
            return;
        }
        waveOutReset(handle_);
        for (auto& block : blocks_) {
            if (block.header.dwFlags & WHDR_PREPARED) {
                waveOutUnprepareHeader(handle_, &block.header, sizeof(WAVEHDR));
            }
        }
        waveOutClose(handle_);
    }

    WaveOutDevice(const WaveOutDevice&) = delete;
    WaveOutDevice& operator=(const WaveOutDevice&) = delete;

    bool submit(
        const std::int16_t* samples,
        std::size_t sampleCount,
        PlaybackClock& clock) {
        Block& block = blocks_[nextBlock_];
        while (
            (block.header.dwFlags & WHDR_PREPARED) &&
            !(block.header.dwFlags & WHDR_DONE)) {
            syncPause(clock);
            if (clock.stopped()) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }

        if (block.header.dwFlags & WHDR_PREPARED) {
            requireWave(
                waveOutUnprepareHeader(handle_, &block.header, sizeof(WAVEHDR)),
                "Could not recycle audio buffer");
        }

        block.samples.assign(samples, samples + sampleCount);
        block.header = {};
        block.header.lpData =
            reinterpret_cast<LPSTR>(block.samples.data());
        block.header.dwBufferLength = static_cast<DWORD>(
            block.samples.size() * sizeof(std::int16_t));
        requireWave(
            waveOutPrepareHeader(handle_, &block.header, sizeof(WAVEHDR)),
            "Could not prepare audio buffer");
        requireWave(
            waveOutWrite(handle_, &block.header, sizeof(WAVEHDR)),
            "Could not submit audio buffer");

        nextBlock_ = (nextBlock_ + 1) % blocks_.size();
        syncPause(clock);
        return true;
    }

    void syncPause(PlaybackClock& clock) {
        const bool shouldPause = clock.paused();
        if (shouldPause == paused_) {
            return;
        }
        requireWave(
            shouldPause ? waveOutPause(handle_) : waveOutRestart(handle_),
            shouldPause ? "Could not pause audio" : "Could not resume audio");
        paused_ = shouldPause;
    }

    void drain(PlaybackClock& clock) {
        bool pending = true;
        while (pending && !clock.stopped()) {
            pending = false;
            syncPause(clock);
            for (const auto& block : blocks_) {
                if (
                    (block.header.dwFlags & WHDR_PREPARED) &&
                    !(block.header.dwFlags & WHDR_DONE)) {
                    pending = true;
                    break;
                }
            }
            if (pending) {
                std::this_thread::sleep_for(std::chrono::milliseconds(3));
            }
        }
    }

private:
    struct Block {
        WAVEHDR header{};
        std::vector<std::int16_t> samples;
    };

    HWAVEOUT handle_{};
    std::array<Block, kWaveBufferCount> blocks_;
    std::size_t nextBlock_{};
    bool paused_{};
};

} // namespace

struct AudioPlayer::Impl {
    explicit Impl(AudioStreamDescription streamDescription)
        : description(std::move(streamDescription)) {}

    void open() {
        if (!description.codecParameters) {
            throw std::runtime_error("Audio stream description is empty");
        }

        const AVCodec* codec =
            avcodec_find_decoder(description.codecParameters->codec_id);
        if (!codec) {
            throw std::runtime_error("No decoder is available for the audio stream");
        }

        AVCodecContext* rawContext = avcodec_alloc_context3(codec);
        if (!rawContext) {
            throw std::bad_alloc();
        }
        codecContext.reset(rawContext);
        requireFfmpeg(
            avcodec_parameters_to_context(
                codecContext.get(), description.codecParameters.get()),
            "Could not copy audio codec parameters");
        requireFfmpeg(
            avcodec_open2(codecContext.get(), codec, nullptr),
            "Could not open audio decoder");

        AVChannelLayout inputLayout = codecContext->ch_layout;
        AVChannelLayout fallbackLayout{};
        if (inputLayout.nb_channels == 0) {
            av_channel_layout_default(&fallbackLayout, 2);
            inputLayout = fallbackLayout;
        }
        AVChannelLayout outputLayout = AV_CHANNEL_LAYOUT_STEREO;

        requireFfmpeg(
            swr_alloc_set_opts2(
                &resampleContext,
                &outputLayout,
                AV_SAMPLE_FMT_S16,
                kOutputSampleRate,
                &inputLayout,
                codecContext->sample_fmt,
                codecContext->sample_rate,
                0,
                nullptr),
            "Could not configure audio resampler");
        if (fallbackLayout.nb_channels != 0) {
            av_channel_layout_uninit(&fallbackLayout);
        }
        requireFfmpeg(
            swr_init(resampleContext),
            "Could not initialize audio resampler");
    }

    ~Impl() {
        swr_free(&resampleContext);
    }

    bool outputFrame(
        AVFrame* frame,
        WaveOutDevice& device,
        PlaybackClock& clock) {
        const int inputRate = std::max(1, codecContext->sample_rate);
        const std::int64_t delayed =
            swr_get_delay(resampleContext, inputRate);
        const int maximumSamples = static_cast<int>(av_rescale_rnd(
            delayed + frame->nb_samples,
            kOutputSampleRate,
            inputRate,
            AV_ROUND_UP));

        convertedSamples.resize(
            static_cast<std::size_t>(maximumSamples) * kOutputChannels);
        std::uint8_t* outputData =
            reinterpret_cast<std::uint8_t*>(convertedSamples.data());
        const int converted = swr_convert(
            resampleContext,
            &outputData,
            maximumSamples,
            const_cast<const std::uint8_t**>(frame->extended_data),
            frame->nb_samples);
        requireFfmpeg(converted, "Audio resampling failed");

        if (!audioStarted) {
            double presentationTime = 0.0;
            if (frame->best_effort_timestamp != AV_NOPTS_VALUE) {
                const AVRational timeBase{
                    description.timeBaseNumerator,
                    description.timeBaseDenominator
                };
                presentationTime =
                    static_cast<double>(frame->best_effort_timestamp) *
                    av_q2d(timeBase);
            }
            if (!clock.waitUntil(presentationTime)) {
                return false;
            }
            audioStarted = true;
        }

        return device.submit(
            convertedSamples.data(),
            static_cast<std::size_t>(converted) * kOutputChannels,
            clock);
    }

    bool receiveFrames(WaveOutDevice& device, PlaybackClock& clock) {
        while (!clock.stopped()) {
            const int result =
                avcodec_receive_frame(codecContext.get(), decodedFrame.get());
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
                return true;
            }
            requireFfmpeg(result, "Audio decode failed");
            if (!outputFrame(decodedFrame.get(), device, clock)) {
                return false;
            }
            av_frame_unref(decodedFrame.get());
        }
        return false;
    }

    AudioStreamDescription description;
    std::unique_ptr<AVCodecContext, CodecContextDeleter> codecContext;
    std::unique_ptr<AVFrame, FrameDeleter> decodedFrame{
        av_frame_alloc()
    };
    SwrContext* resampleContext{};
    std::vector<std::int16_t> convertedSamples;
    bool audioStarted{};
};

AudioPlayer::AudioPlayer(AudioStreamDescription description)
    : impl_(std::make_unique<Impl>(std::move(description))) {
    if (!impl_->decodedFrame) {
        throw std::bad_alloc();
    }
}

AudioPlayer::~AudioPlayer() = default;

void AudioPlayer::run(
    BlockingQueue<EncodedAudioPacket>& packets,
    PlaybackClock& clock) {
    impl_->open();
    WaveOutDevice device;

    while (!clock.stopped()) {
        auto encoded = packets.pop();
        if (!encoded) {
            break;
        }
        const int result =
            avcodec_send_packet(impl_->codecContext.get(), encoded->packet.get());
        if (result != AVERROR(EAGAIN)) {
            requireFfmpeg(result, "Could not submit audio packet");
        }
        if (!impl_->receiveFrames(device, clock)) {
            return;
        }
        device.syncPause(clock);
    }

    if (!clock.stopped()) {
        requireFfmpeg(
            avcodec_send_packet(impl_->codecContext.get(), nullptr),
            "Could not flush audio decoder");
        impl_->receiveFrames(device, clock);
        device.drain(clock);
    }
}
