#include "AsciiConverter.h"
#include "AudioPlayer.h"
#include "BlockingQueue.h"
#include "MediaTypes.h"
#include "Renderer.h"
#include "Timer.h"
#include "VideoDecoder.h"

#include <conio.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

std::string wideToUtf8(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (size <= 0) {
        throw std::runtime_error("Could not convert the input path to UTF-8");
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        size,
        nullptr,
        nullptr);
    return result;
}

std::filesystem::path utf8Path(const std::string& value) {
    const auto* begin = reinterpret_cast<const char8_t*>(value.data());
    return std::filesystem::path(
        std::u8string(begin, begin + value.size()));
}

void printUsage() {
    std::cout
        << "ascii-player - real-time FFmpeg terminal video player\n\n"
        << "Usage:\n"
        << "  ascii-player.exe [options] <video>\n\n"
        << "Options:\n"
        << "  --charset classic|dense  Select the luminance character set\n"
        << "  --color                  Use the Windows 16-color console palette\n"
        << "  --no-audio               Disable audio playback\n"
        << "  --no-status              Hide the playback status line\n"
        << "  --benchmark              Decode/convert without console or audio timing\n"
        << "  --max-frames N           Stop after N decoded video frames\n"
        << "  -h, --help               Show this help\n\n"
        << "Keys during playback: Space pause/resume, Q or Esc quit\n";
}

std::uint64_t parseUnsigned(const std::wstring& value, const char* option) {
    std::size_t consumed{};
    unsigned long long parsed{};
    try {
        parsed = std::stoull(value, &consumed, 10);
    } catch (const std::exception&) {
        throw std::runtime_error(std::string(option) + " expects an integer");
    }
    if (consumed != value.size()) {
        throw std::runtime_error(std::string(option) + " expects an integer");
    }
    return static_cast<std::uint64_t>(parsed);
}

std::optional<PlayerOptions> parseArguments(int argc, wchar_t** argv) {
    if (argc <= 1) {
        printUsage();
        return std::nullopt;
    }

    PlayerOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::wstring argument = argv[index];
        if (argument == L"-h" || argument == L"--help") {
            printUsage();
            return std::nullopt;
        }
        if (argument == L"--color") {
            options.color = true;
            continue;
        }
        if (argument == L"--no-audio") {
            options.audio = false;
            continue;
        }
        if (argument == L"--no-status") {
            options.statusLine = false;
            continue;
        }
        if (argument == L"--benchmark") {
            options.benchmark = true;
            options.audio = false;
            continue;
        }
        if (argument == L"--charset") {
            if (++index >= argc) {
                throw std::runtime_error("--charset requires classic or dense");
            }
            const std::wstring value = argv[index];
            if (value == L"classic") {
                options.characterSet = CharacterSet::Classic;
            } else if (value == L"dense") {
                options.characterSet = CharacterSet::Dense;
            } else {
                throw std::runtime_error("--charset requires classic or dense");
            }
            continue;
        }
        if (argument == L"--max-frames") {
            if (++index >= argc) {
                throw std::runtime_error("--max-frames requires a value");
            }
            options.maxFrames = parseUnsigned(argv[index], "--max-frames");
            continue;
        }
        if (argument.starts_with(L"--")) {
            throw std::runtime_error(
                "Unknown option: " + wideToUtf8(argument));
        }
        if (!options.inputPath.empty()) {
            throw std::runtime_error("Only one input video can be played");
        }
        options.inputPath = wideToUtf8(argument);
    }

    if (options.inputPath.empty()) {
        throw std::runtime_error("A video file path is required");
    }
    return options;
}

class SharedError {
public:
    void capture() {
        std::lock_guard lock(mutex_);
        if (!error_) {
            error_ = std::current_exception();
        }
    }

    void rethrowIfPresent() const {
        std::lock_guard lock(mutex_);
        if (error_) {
            std::rethrow_exception(error_);
        }
    }

private:
    mutable std::mutex mutex_;
    std::exception_ptr error_;
};

void printMediaInfo(const VideoInfo& info, bool audioEnabled) {
    std::cout
        << "Input: " << info.width << 'x' << info.height
        << " @ " << info.fps << " FPS"
        << " | video: " << info.videoCodec;
    if (info.hasAudio && audioEnabled) {
        std::cout << " | audio: " << info.audioCodec;
    } else if (info.hasAudio) {
        std::cout << " | audio: disabled";
    } else {
        std::cout << " | audio: none";
    }
    std::cout << '\n';
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        const auto parsedOptions = parseArguments(argc, argv);
        if (!parsedOptions) {
            return argc <= 1 ? 1 : 0;
        }
        const PlayerOptions options = *parsedOptions;
        if (!std::filesystem::exists(utf8Path(options.inputPath))) {
            throw std::runtime_error("Input file does not exist: " + options.inputPath);
        }

        VideoDecoder decoder(options.inputPath);
        decoder.open();
        const VideoInfo videoInfo = decoder.info();
        const bool useAudio =
            options.audio && videoInfo.hasAudio && !options.benchmark;
        printMediaInfo(videoInfo, useAudio);

        DisplaySize displaySize;
        PlaybackStatistics statistics;
        PlaybackClock clock;
        BlockingQueue<DecodedVideoFrame> decodedFrames(6);
        BlockingQueue<EncodedAudioPacket> audioPackets(128);
        BlockingQueue<AsciiFrame> asciiFrames(3);
        AsciiConverter converter(
            displaySize,
            options.characterSet,
            options.color,
            options.statusLine);
        auto renderer = std::make_unique<Renderer>(
            displaySize,
            videoInfo,
            options.statusLine,
            options.benchmark);
        std::unique_ptr<AudioPlayer> audioPlayer;
        if (useAudio) {
            audioPlayer =
                std::make_unique<AudioPlayer>(decoder.audioDescription());
        }

        SharedError sharedError;
        std::atomic<bool> rendererFinished{false};
        std::atomic<bool> audioFinished{!useAudio};

        auto abortPipeline = [&] {
            clock.stop();
            decodedFrames.close();
            audioPackets.close();
            asciiFrames.close();
        };

        std::thread decodeThread([&] {
            try {
                decoder.decode(
                    decodedFrames,
                    useAudio ? &audioPackets : nullptr,
                    clock,
                    statistics,
                    options.maxFrames);
            } catch (...) {
                sharedError.capture();
                abortPipeline();
            }
        });

        std::thread convertThread([&] {
            try {
                converter.run(
                    decodedFrames,
                    asciiFrames,
                    clock,
                    statistics);
            } catch (...) {
                sharedError.capture();
                abortPipeline();
            }
        });

        std::thread renderThread([&] {
            try {
                renderer->run(asciiFrames, clock, statistics);
            } catch (...) {
                sharedError.capture();
                abortPipeline();
            }
            rendererFinished.store(true, std::memory_order_release);
        });

        std::thread audioThread;
        if (useAudio) {
            audioThread = std::thread([&] {
                try {
                    audioPlayer->run(audioPackets, clock);
                } catch (...) {
                    sharedError.capture();
                    abortPipeline();
                }
                audioFinished.store(true, std::memory_order_release);
            });
        }

        while (
            !rendererFinished.load(std::memory_order_acquire) ||
            !audioFinished.load(std::memory_order_acquire)) {
            if (!options.benchmark && _kbhit()) {
                const int key = _getch();
                if (key == ' ' && clock.started()) {
                    clock.setPaused(!clock.paused());
                } else if (key == 'q' || key == 'Q' || key == 27) {
                    abortPipeline();
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        clock.stop();
        decodedFrames.close();
        audioPackets.close();
        asciiFrames.close();

        decodeThread.join();
        convertThread.join();
        renderThread.join();
        if (audioThread.joinable()) {
            audioThread.join();
        }

        renderer.reset();
        sharedError.rethrowIfPresent();

        if (options.benchmark) {
            std::cout
                << "Benchmark complete: "
                << statistics.decodedFrames.load(std::memory_order_relaxed)
                << " decoded, "
                << statistics.convertedFrames.load(std::memory_order_relaxed)
                << " converted, "
                << statistics.renderedFrames.load(std::memory_order_relaxed)
                << " consumed\n";
        } else {
            std::cout
                << "\nPlayback finished. Rendered "
                << statistics.renderedFrames.load(std::memory_order_relaxed)
                << " frames, dropped "
                << statistics.droppedFrames.load(std::memory_order_relaxed)
                << ".\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ascii-player: " << error.what() << '\n';
        return 1;
    }
}
