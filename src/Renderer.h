#pragma once

#include "BlockingQueue.h"
#include "MediaTypes.h"

#include <memory>

class PlaybackClock;

class Renderer {
public:
    Renderer(
        DisplaySize& displaySize,
        const VideoInfo& videoInfo,
        bool statusLine,
        bool benchmark);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    void run(
        BlockingQueue<AsciiFrame>& frames,
        PlaybackClock& clock,
        PlaybackStatistics& statistics);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
