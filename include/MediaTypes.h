#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct AVCodecParameters;
struct AVFrame;
struct AVPacket;

struct VideoInfo {
    int width{};
    int height{};
    double fps{};
    double durationSeconds{};
    double startTime{};
    bool hasAudio{};
    std::string videoCodec;
    std::string audioCodec;
};

struct AudioStreamDescription {
    std::shared_ptr<AVCodecParameters> codecParameters;
    int timeBaseNumerator{};
    int timeBaseDenominator{1};
};

struct DecodedVideoFrame {
    std::shared_ptr<AVFrame> frame;
    double presentationTime{};
    double duration{};
};

struct EncodedAudioPacket {
    std::shared_ptr<AVPacket> packet;
};

struct AsciiFrame {
    int width{};
    int height{};
    double presentationTime{};
    std::vector<char> characters;
    std::vector<std::uint16_t> attributes;
};

struct DisplaySize {
    std::atomic<int> columns{120};
    std::atomic<int> rows{40};
};

struct PlaybackStatistics {
    std::atomic<std::uint64_t> decodedFrames{};
    std::atomic<std::uint64_t> convertedFrames{};
    std::atomic<std::uint64_t> renderedFrames{};
    std::atomic<std::uint64_t> droppedFrames{};
    std::atomic<double> renderFps{};
};

enum class CharacterSet {
    Classic,
    Dense
};

struct PlayerOptions {
    std::string inputPath;
    CharacterSet characterSet{CharacterSet::Classic};
    bool color{};
    bool audio{true};
    bool statusLine{true};
    bool benchmark{};
    std::uint64_t maxFrames{};
};
