#pragma once

#include "BlockingQueue.h"
#include "MediaTypes.h"

#include <cstdint>
#include <memory>
#include <string>

class PlaybackClock;

class VideoDecoder {
public:
    explicit VideoDecoder(std::string inputPath);
    ~VideoDecoder();

    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    void open();
    void decode(
        BlockingQueue<DecodedVideoFrame>& videoFrames,
        BlockingQueue<EncodedAudioPacket>* audioPackets,
        PlaybackClock& clock,
        PlaybackStatistics& statistics,
        std::uint64_t maxFrames = 0);

    [[nodiscard]] const VideoInfo& info() const noexcept;
    [[nodiscard]] const AudioStreamDescription& audioDescription() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
