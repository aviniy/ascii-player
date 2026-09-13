#include "Renderer.h"

#include "Timer.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr WORD kDefaultAttribute =
    FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;

std::string windowsError(const char* operation) {
    const DWORD error = GetLastError();
    char* message = nullptr;
    const DWORD length = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
            FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        0,
        reinterpret_cast<char*>(&message),
        0,
        nullptr);
    std::string result(operation);
    result += " failed";
    if (length != 0 && message) {
        result += ": ";
        result.append(message, length);
    }
    if (message) {
        LocalFree(message);
    }
    return result;
}
std::string formatTime(double seconds) {
    const int totalSeconds = std::max(0, static_cast<int>(seconds));
    const int hours = totalSeconds / 3600;
    const int minutes = (totalSeconds / 60) % 60;
    const int remainingSeconds = totalSeconds % 60;
    char buffer[32]{};
    if (hours > 0) {
        std::snprintf(
            buffer,
            sizeof(buffer),
            "%d:%02d:%02d",
            hours,
            minutes,
            remainingSeconds);
    } else {
        std::snprintf(
            buffer,
            sizeof(buffer),
            "%02d:%02d",
            minutes,
            remainingSeconds);
    }
    return buffer;
}

} // namespace

struct Renderer::Impl {
    Impl(
        DisplaySize& size,
        VideoInfo info,
        bool showStatusLine,
        bool headless)
        : displaySize(size),
          videoInfo(std::move(info)),
          statusLine(showStatusLine),
          benchmark(headless) {
        if (benchmark) {
            return;
        }

        output = GetStdHandle(STD_OUTPUT_HANDLE);
        if (!output || output == INVALID_HANDLE_VALUE) {
            throw std::runtime_error(windowsError("GetStdHandle"));
        }
        if (!GetConsoleMode(output, &originalMode)) {
            throw std::runtime_error(
                "stdout is not a Windows console; use --benchmark for headless decoding");
        }
        originalCodePage = GetConsoleOutputCP();
        SetConsoleOutputCP(437);

        if (!GetConsoleCursorInfo(output, &originalCursorInfo)) {
            throw std::runtime_error(windowsError("GetConsoleCursorInfo"));
        }
        CONSOLE_CURSOR_INFO hiddenCursor = originalCursorInfo;
        hiddenCursor.bVisible = FALSE;
        if (!SetConsoleCursorInfo(output, &hiddenCursor)) {
            throw std::runtime_error(windowsError("SetConsoleCursorInfo"));
        }
        cursorHidden = true;
        refreshGeometry();
    }

    ~Impl() {
        if (!benchmark && output && output != INVALID_HANDLE_VALUE) {
            if (cursorHidden) {
                SetConsoleCursorInfo(output, &originalCursorInfo);
            }
            if (originalCodePage != 0) {
                SetConsoleOutputCP(originalCodePage);
            }
        }
    }

    void refreshGeometry() {
        if (benchmark) {
            return;
        }
        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (!GetConsoleScreenBufferInfo(output, &info)) {
            throw std::runtime_error(windowsError("GetConsoleScreenBufferInfo"));
        }
        originX = info.srWindow.Left;
        originY = info.srWindow.Top;
        const int newColumns =
            info.srWindow.Right - info.srWindow.Left + 1;
        const int newRows =
            info.srWindow.Bottom - info.srWindow.Top + 1;
        if (newColumns != columns || newRows != rows) {
            columns = std::max(1, newColumns);
            rows = std::max(1, newRows);
            displaySize.columns.store(columns, std::memory_order_relaxed);
            displaySize.rows.store(rows, std::memory_order_relaxed);
            previousCharacters.clear();
            previousAttributes.clear();
        }
    }

    std::string makeStatus(
        const AsciiFrame& frame,
        const PlaybackClock& clock,
        double fps) const {
        const double current =
            std::max(0.0, frame.presentationTime - videoInfo.startTime);
        std::string text = " ASCII Player | ";
        text += formatTime(current);
        if (videoInfo.durationSeconds > 0.0) {
            text += " / ";
            text += formatTime(videoInfo.durationSeconds);
        }

        char fpsBuffer[32]{};
        std::snprintf(fpsBuffer, sizeof(fpsBuffer), " | %5.1f FPS", fps);
        text += fpsBuffer;
        text += clock.paused()
            ? " | PAUSED | Space: resume  Q: quit"
            : " | Space: pause  Q: quit";
        return text;
    }

    void buildCanvas(
        const AsciiFrame& frame,
        const PlaybackClock& clock,
        double fps) {
        currentCharacters.assign(
            static_cast<std::size_t>(columns) * rows, ' ');
        currentAttributes.assign(
            static_cast<std::size_t>(columns) * rows, kDefaultAttribute);

        const int usableRows =
            std::max(1, rows - (statusLine && rows > 1 ? 1 : 0));
        const int copyWidth = std::min(columns, frame.width);
        const int copyHeight = std::min(usableRows, frame.height);
        const int destinationX = std::max(0, (columns - copyWidth) / 2);
        const int destinationY = std::max(0, (usableRows - copyHeight) / 2);
        const int sourceX = std::max(0, (frame.width - copyWidth) / 2);
        const int sourceY = std::max(0, (frame.height - copyHeight) / 2);

        for (int row = 0; row < copyHeight; ++row) {
            const std::size_t sourceOffset =
                static_cast<std::size_t>(sourceY + row) * frame.width + sourceX;
            const std::size_t destinationOffset =
                static_cast<std::size_t>(destinationY + row) * columns +
                destinationX;
            std::copy_n(
                frame.characters.begin() +
                    static_cast<std::ptrdiff_t>(sourceOffset),
                copyWidth,
                currentCharacters.begin() +
                    static_cast<std::ptrdiff_t>(destinationOffset));
            if (!frame.attributes.empty()) {
                std::copy_n(
                    frame.attributes.begin() +
                        static_cast<std::ptrdiff_t>(sourceOffset),
                    copyWidth,
                    currentAttributes.begin() +
                        static_cast<std::ptrdiff_t>(destinationOffset));
            }
        }

        if (statusLine && rows > 1) {
            std::string text = makeStatus(frame, clock, fps);
            if (static_cast<int>(text.size()) > columns) {
                text.resize(static_cast<std::size_t>(columns));
            }
            const std::size_t statusOffset =
                static_cast<std::size_t>(rows - 1) * columns;
            std::copy(
                text.begin(),
                text.end(),
                currentCharacters.begin() +
                    static_cast<std::ptrdiff_t>(statusOffset));
            std::fill_n(
                currentAttributes.begin() +
                    static_cast<std::ptrdiff_t>(statusOffset),
                columns,
                static_cast<std::uint16_t>(
                    BACKGROUND_BLUE |
                    FOREGROUND_RED |
                    FOREGROUND_GREEN |
                    FOREGROUND_BLUE |
                    FOREGROUND_INTENSITY));
        }
    }

    void writeRun(int row, int startColumn, int length) {
        const std::size_t offset =
            static_cast<std::size_t>(row) * columns + startColumn;
        const COORD position{
            static_cast<SHORT>(originX + startColumn),
            static_cast<SHORT>(originY + row)
        };
        DWORD written{};
        if (!WriteConsoleOutputCharacterA(
                output,
                currentCharacters.data() +
                    static_cast<std::ptrdiff_t>(offset),
                static_cast<DWORD>(length),
                position,
                &written)) {
            throw std::runtime_error(
                windowsError("WriteConsoleOutputCharacterA"));
        }
        if (!WriteConsoleOutputAttribute(
                output,
                reinterpret_cast<const WORD*>(
                    currentAttributes.data() +
                    static_cast<std::ptrdiff_t>(offset)),
                static_cast<DWORD>(length),
                position,
                &written)) {
            throw std::runtime_error(
                windowsError("WriteConsoleOutputAttribute"));
        }
    }

    void dirtyRender() {
        const std::size_t cellCount = currentCharacters.size();
        if (previousCharacters.size() != cellCount) {
            for (int row = 0; row < rows; ++row) {
                writeRun(row, 0, columns);
            }
            previousCharacters = currentCharacters;
            previousAttributes = currentAttributes;
            return;
        }

        std::size_t changedCells{};
        for (std::size_t index = 0; index < cellCount; ++index) {
            if (
                currentCharacters[index] != previousCharacters[index] ||
                currentAttributes[index] != previousAttributes[index]) {
                ++changedCells;
            }
        }

        if (changedCells * 10 >= cellCount * 4) {
            for (int row = 0; row < rows; ++row) {
                writeRun(row, 0, columns);
            }
        } else {
            for (int row = 0; row < rows; ++row) {
                int column = 0;
                while (column < columns) {
                    const std::size_t index =
                        static_cast<std::size_t>(row) * columns + column;
                    const bool changed =
                        currentCharacters[index] != previousCharacters[index] ||
                        currentAttributes[index] != previousAttributes[index];
                    if (!changed) {
                        ++column;
                        continue;
                    }

                    const int runStart = column;
                    ++column;
                    while (column < columns) {
                        const std::size_t runIndex =
                            static_cast<std::size_t>(row) * columns + column;
                        if (
                            currentCharacters[runIndex] ==
                                previousCharacters[runIndex] &&
                            currentAttributes[runIndex] ==
                                previousAttributes[runIndex]) {
                            break;
                        }
                        ++column;
                    }
                    writeRun(row, runStart, column - runStart);
                }
            }
        }

        previousCharacters = currentCharacters;
        previousAttributes = currentAttributes;
    }

    void render(
        const AsciiFrame& frame,
        const PlaybackClock& clock,
        double fps) {
        if (benchmark) {
            return;
        }
        refreshGeometry();
        buildCanvas(frame, clock, fps);
        dirtyRender();
    }

    DisplaySize& displaySize;
    VideoInfo videoInfo;
    HANDLE output{INVALID_HANDLE_VALUE};
    DWORD originalMode{};
    UINT originalCodePage{};
    CONSOLE_CURSOR_INFO originalCursorInfo{};
    bool cursorHidden{};
    bool statusLine{};
    bool benchmark{};
    int originX{};
    int originY{};
    int columns{120};
    int rows{40};
    std::vector<char> previousCharacters;
    std::vector<char> currentCharacters;
    std::vector<std::uint16_t> previousAttributes;
    std::vector<std::uint16_t> currentAttributes;
};

Renderer::Renderer(
    DisplaySize& displaySize,
    const VideoInfo& videoInfo,
    bool statusLine,
    bool benchmark)
    : impl_(std::make_unique<Impl>(
          displaySize, videoInfo, statusLine, benchmark)) {}

Renderer::~Renderer() = default;

void Renderer::run(
    BlockingQueue<AsciiFrame>& frames,
    PlaybackClock& clock,
    PlaybackStatistics& statistics) {
    FrameRateMeter rateMeter;

    while (!clock.stopped()) {
        auto selected = frames.pop();
        if (!selected) {
            break;
        }

        if (!impl_->benchmark) {
            while (
                clock.mediaTime() - selected->presentationTime > 0.075) {
                auto newer = frames.tryPop();
                if (!newer) {
                    break;
                }
                selected = std::move(newer);
                statistics.droppedFrames.fetch_add(
                    1, std::memory_order_relaxed);
            }
            if (!clock.waitUntil(selected->presentationTime)) {
                break;
            }
        }

        const double fps = rateMeter.tick();
        statistics.renderFps.store(fps, std::memory_order_relaxed);
        impl_->render(*selected, clock, fps);
        statistics.renderedFrames.fetch_add(1, std::memory_order_relaxed);
    }
}
