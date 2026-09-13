#include "AsciiConverter.h"

#include "Timer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}
namespace {

constexpr std::string_view kClassicCharacters =
    R"( .'`^",:;Il!i~+_-?][}{1)(|\/*tfjrxnuvczXYUJCLQ0OZmwqpdbkhao*#MW&8%B@$)";
constexpr std::string_view kDenseCharacters = "@%#*+=-:. ";
constexpr double kCharacterWidthToHeight = 0.5;

struct Rgb {
    int red;
    int green;
    int blue;
};

constexpr std::array<Rgb, 16> kConsolePalette{{
    {0, 0, 0},
    {0, 0, 128},
    {0, 128, 0},
    {0, 128, 128},
    {128, 0, 0},
    {128, 0, 128},
    {128, 128, 0},
    {192, 192, 192},
    {128, 128, 128},
    {0, 0, 255},
    {0, 255, 0},
    {0, 255, 255},
    {255, 0, 0},
    {255, 0, 255},
    {255, 255, 0},
    {255, 255, 255}
}};

std::uint16_t nearestConsoleColor(int red, int green, int blue, int luminance) {
    if (luminance < 24) {
        return 8;
    }

    int bestDistance = std::numeric_limits<int>::max();
    std::uint16_t bestIndex = 7;
    for (std::uint16_t index = 1; index < kConsolePalette.size(); ++index) {
        const auto& color = kConsolePalette[index];
        const int deltaRed = red - color.red;
        const int deltaGreen = green - color.green;
        const int deltaBlue = blue - color.blue;
        const int distance =
            deltaRed * deltaRed + deltaGreen * deltaGreen + deltaBlue * deltaBlue;
        if (distance < bestDistance) {
            bestDistance = distance;
            bestIndex = index;
        }
    }
    return bestIndex;
}

} // namespace

struct AsciiConverter::Impl {
    Impl(
        DisplaySize& size,
        CharacterSet characterSet,
        bool useColor,
        bool reserveLine)
        : displaySize(size),
          color(useColor),
          reserveStatusLine(reserveLine) {
        const std::string_view characters =
            characterSet == CharacterSet::Classic
                ? kClassicCharacters
                : kDenseCharacters;
        for (std::size_t luminance = 0; luminance < lookup.size(); ++luminance) {
            std::size_t index{};
            if (characterSet == CharacterSet::Classic) {
                index = (255 - luminance) * (characters.size() - 1) / 255;
            } else {
                index = luminance * (characters.size() - 1) / 255;
            }
            lookup[luminance] = characters[index];
        }
    }

    ~Impl() {
        sws_freeContext(scaleContext);
    }

    std::pair<int, int> outputDimensions(int sourceWidth, int sourceHeight) const {
        const int maxWidth =
            std::max(1, displaySize.columns.load(std::memory_order_relaxed));
        const int consoleRows =
            std::max(1, displaySize.rows.load(std::memory_order_relaxed));
        const int maxHeight = std::max(
            1, consoleRows - (reserveStatusLine && consoleRows > 1 ? 1 : 0));

        const double sourceAspect =
            static_cast<double>(sourceWidth) / std::max(1, sourceHeight);
        int width = maxWidth;
        int height = std::max(
            1,
            static_cast<int>(std::lround(
                static_cast<double>(width) * kCharacterWidthToHeight /
                sourceAspect)));

        if (height > maxHeight) {
            height = maxHeight;
            width = std::max(
                1,
                static_cast<int>(std::lround(
                    static_cast<double>(height) * sourceAspect /
                    kCharacterWidthToHeight)));
        }
        return {std::min(width, maxWidth), std::min(height, maxHeight)};
    }

    AsciiFrame convert(const DecodedVideoFrame& source) {
        if (!source.frame) {
            throw std::runtime_error("ASCII converter received an empty frame");
        }

        const auto [targetWidth, targetHeight] =
            outputDimensions(source.frame->width, source.frame->height);
        const AVPixelFormat outputFormat = color ? AV_PIX_FMT_BGR24 : AV_PIX_FMT_GRAY8;
        scaleContext = sws_getCachedContext(
            scaleContext,
            source.frame->width,
            source.frame->height,
            static_cast<AVPixelFormat>(source.frame->format),
            targetWidth,
            targetHeight,
            outputFormat,
            SWS_FAST_BILINEAR,
            nullptr,
            nullptr,
            nullptr);
        if (!scaleContext) {
            throw std::runtime_error("Could not create FFmpeg scaling context");
        }

        const int bytesPerPixel = color ? 3 : 1;
        const int stride = targetWidth * bytesPerPixel;
        std::vector<std::uint8_t> pixels(
            static_cast<std::size_t>(stride) * targetHeight);
        std::uint8_t* destinationData[4]{pixels.data(), nullptr, nullptr, nullptr};
        int destinationStride[4]{stride, 0, 0, 0};

        const int scaledRows = sws_scale(
            scaleContext,
            source.frame->data,
            source.frame->linesize,
            0,
            source.frame->height,
            destinationData,
            destinationStride);
        if (scaledRows != targetHeight) {
            throw std::runtime_error("FFmpeg returned an incomplete scaled frame");
        }

        AsciiFrame result;
        result.width = targetWidth;
        result.height = targetHeight;
        result.presentationTime = source.presentationTime;
        const std::size_t characterCount =
            static_cast<std::size_t>(targetWidth) * targetHeight;
        result.characters.resize(characterCount);
        if (color) {
            result.attributes.resize(characterCount);
        }

        for (std::size_t index = 0; index < characterCount; ++index) {
            int luminance{};
            if (color) {
                const std::size_t pixelOffset = index * 3;
                const int blue = pixels[pixelOffset];
                const int green = pixels[pixelOffset + 1];
                const int red = pixels[pixelOffset + 2];
                luminance = (54 * red + 183 * green + 19 * blue) >> 8;
                result.attributes[index] =
                    nearestConsoleColor(red, green, blue, luminance);
            } else {
                luminance = pixels[index];
            }
            result.characters[index] =
                lookup[static_cast<std::size_t>(std::clamp(luminance, 0, 255))];
        }

        return result;
    }

    DisplaySize& displaySize;
    std::array<char, 256> lookup{};
    SwsContext* scaleContext{};
    bool color{};
    bool reserveStatusLine{};
};

AsciiConverter::AsciiConverter(
    DisplaySize& displaySize,
    CharacterSet characterSet,
    bool color,
    bool reserveStatusLine)
    : impl_(std::make_unique<Impl>(
          displaySize, characterSet, color, reserveStatusLine)) {}

AsciiConverter::~AsciiConverter() = default;

void AsciiConverter::run(
    BlockingQueue<DecodedVideoFrame>& input,
    BlockingQueue<AsciiFrame>& output,
    PlaybackClock& clock,
    PlaybackStatistics& statistics) {
    while (!clock.stopped()) {
        auto frame = input.pop();
        if (!frame) {
            break;
        }
        AsciiFrame converted = impl_->convert(*frame);
        if (!output.push(std::move(converted))) {
            break;
        }
        statistics.convertedFrames.fetch_add(1, std::memory_order_relaxed);
    }
    output.close();
}
