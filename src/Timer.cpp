#include "Timer.h"

#include <algorithm>

void PlaybackClock::start(double mediaTime) {
    std::lock_guard lock(mutex_);
    if (started_ || stopped_) {
        return;
    }
    mediaAnchor_ = mediaTime;
    wallAnchor_ = Clock::now();
    started_ = true;
    changed_.notify_all();
}
void PlaybackClock::setPaused(bool paused) {
    std::lock_guard lock(mutex_);
    if (!started_ || stopped_ || paused_ == paused) {
        return;
    }

    const auto now = Clock::now();
    if (paused) {
        mediaAnchor_ += std::chrono::duration<double>(now - wallAnchor_).count();
        pauseBegan_ = now;
        paused_ = true;
    } else {
        wallAnchor_ = now;
        paused_ = false;
    }
    changed_.notify_all();
}

bool PlaybackClock::paused() const {
    std::lock_guard lock(mutex_);
    return paused_;
}

bool PlaybackClock::started() const {
    std::lock_guard lock(mutex_);
    return started_;
}

double PlaybackClock::mediaTime() const {
    std::lock_guard lock(mutex_);
    if (!started_) {
        return mediaAnchor_;
    }
    if (paused_) {
        return mediaAnchor_;
    }
    return mediaAnchor_ + std::chrono::duration<double>(Clock::now() - wallAnchor_).count();
}

bool PlaybackClock::waitUntil(double targetMediaTime) {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [this] { return started_ || stopped_; });

    while (!stopped_) {
        if (paused_) {
            changed_.wait(lock, [this] { return !paused_ || stopped_; });
            continue;
        }

        const double current = mediaAnchor_ +
            std::chrono::duration<double>(Clock::now() - wallAnchor_).count();
        const double remaining = targetMediaTime - current;
        if (remaining <= 0.0) {
            return true;
        }

        changed_.wait_for(
            lock,
            std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double>(std::min(remaining, 0.050))));
    }
    return false;
}

void PlaybackClock::stop() {
    {
        std::lock_guard lock(mutex_);
        stopped_ = true;
    }
    changed_.notify_all();
}

bool PlaybackClock::stopped() const {
    std::lock_guard lock(mutex_);
    return stopped_;
}

double FrameRateMeter::tick() {
    ++frames_;
    const auto now = Clock::now();
    const double elapsed = std::chrono::duration<double>(now - windowStart_).count();
    if (elapsed >= 0.5) {
        lastRate_ = static_cast<double>(frames_) / elapsed;
        frames_ = 0;
        windowStart_ = now;
    }
    return lastRate_;
}
