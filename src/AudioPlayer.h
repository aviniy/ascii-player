#pragma once

#include "BlockingQueue.h"
#include "MediaTypes.h"

#include <memory>

class PlaybackClock;

class AudioPlayer {
public:
    explicit AudioPlayer(AudioStreamDescription description);
    ~AudioPlayer();

    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    void run(
        BlockingQueue<EncodedAudioPacket>& packets,
        PlaybackClock& clock);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
