/*
 * metrics.h - the two numbers we care about.
 *
 * lat is how long one engine call took, in milliseconds. fps is how many
 * frames went through in the last second. lat_max is the worst call of that
 * second, not of the whole run, so one old hiccup does not hide what the
 * pipeline is doing now.
 */
#pragma once

#include <chrono>
#include <cstdint>

namespace pl {

class Metrics {
public:
    void frame(double latency_ms)
    {
        ++frames_;
        lat_sum_ += latency_ms;
        lat_sum_all_ += latency_ms;
        ++lat_n_;
        ++lat_n_all_;
        if (latency_ms > lat_max_) {
            lat_max_ = latency_ms;
        }
        if (latency_ms > lat_max_all_) {
            lat_max_all_ = latency_ms;
        }
    }

    /* Closes the one-second window.  Returns true when the window ended, so
       the caller can redraw or print. */
    bool tick()
    {
        const auto now = std::chrono::steady_clock::now();
        const double since = std::chrono::duration<double>(now - last_).count();
        if (since < 1.0) {
            return false;
        }
        fps_ = static_cast<double>(frames_ - frames_at_last_) / since;
        if (fps_ > peak_) {
            peak_ = fps_;
        }
        lat_ = lat_n_ != 0u ? lat_sum_ / static_cast<double>(lat_n_) : 0.0;
        lat_max_window_ = lat_max_;
        last_ = now;
        frames_at_last_ = frames_;
        lat_sum_ = 0.0;
        lat_max_ = 0.0;
        lat_n_ = 0;
        return true;
    }

    double fps() const { return fps_; }
    double peak() const { return peak_; }
    double lat() const { return lat_; }
    double lat_max() const { return lat_max_window_; }
    double lat_mean_all() const
    {
        return lat_n_all_ != 0u ? lat_sum_all_ / static_cast<double>(lat_n_all_)
                                : 0.0;
    }
    double lat_max_all() const { return lat_max_all_; }
    std::uint64_t frames() const { return frames_; }

private:
    std::chrono::steady_clock::time_point last_ =
        std::chrono::steady_clock::now();
    std::uint64_t frames_ = 0;
    std::uint64_t frames_at_last_ = 0;
    std::uint64_t lat_n_ = 0;
    std::uint64_t lat_n_all_ = 0;
    double lat_sum_ = 0.0;
    double lat_sum_all_ = 0.0;
    double lat_max_ = 0.0;
    double lat_max_all_ = 0.0;
    double fps_ = 0.0;
    double peak_ = 0.0;
    double lat_ = 0.0;
    double lat_max_window_ = 0.0;
};

} // namespace pl
