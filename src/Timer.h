#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>

class PlaybackClock {
public:
    void start(double mediaTime);
    void setPaused(bool paused);
    [[nodiscard]] bool paused() const;
    [[nodiscard]] bool started() const;
    [[nodiscard]] double mediaTime() const;
    bool waitUntil(double targetMediaTime);
    void stop();
    [[nodiscard]] bool stopped() const;

private:
    using Clock = std::chrono::steady_clock;

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    Clock::time_point wallAnchor_{};
    Clock::time_point pauseBegan_{};
    double mediaAnchor_{};
    bool started_{};
    bool paused_{};
    bool stopped_{};
};
class FrameRateMeter {
public:
    double tick();

private:
    using Clock = std::chrono::steady_clock;

    Clock::time_point windowStart_{Clock::now()};
    std::uint64_t frames_{};
    double lastRate_{};
};
