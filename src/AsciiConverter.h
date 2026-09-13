#pragma once

#include "BlockingQueue.h"
#include "MediaTypes.h"

#include <array>
#include <memory>

class PlaybackClock;

class AsciiConverter {
public:
    AsciiConverter(
        DisplaySize& displaySize,
        CharacterSet characterSet,
        bool color,
        bool reserveStatusLine);
    ~AsciiConverter();

    AsciiConverter(const AsciiConverter&) = delete;
    AsciiConverter& operator=(const AsciiConverter&) = delete;

    void run(
        BlockingQueue<DecodedVideoFrame>& input,
        BlockingQueue<AsciiFrame>& output,
        PlaybackClock& clock,
        PlaybackStatistics& statistics);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
