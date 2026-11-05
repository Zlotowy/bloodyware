#pragma once

#include <atomic>
#include <chrono>
#include <limits>

namespace diagnostics
{
// Lets a journal note through at most once per interval, for notes raised from loops that run many times a second.
// Thread-safe: concurrent callers race for the same slot and only one of them wins it.
class rate_gate
{
public:
    explicit constexpr rate_gate(const std::chrono::milliseconds interval) noexcept
        : m_interval(interval)
    {
    }

    [[nodiscard]] bool
    allow(const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) noexcept
    {
        const auto stamp = now.time_since_epoch().count();
        auto last = m_last.load(std::memory_order_relaxed);
        if (last != k_never &&
            stamp - last < std::chrono::duration_cast<std::chrono::steady_clock::duration>(m_interval).count())
            return false;
        return m_last.compare_exchange_strong(last, stamp, std::memory_order_relaxed);
    }

private:
    static constexpr auto k_never = std::numeric_limits<std::chrono::steady_clock::rep>::min();

    std::chrono::milliseconds m_interval;
    std::atomic<std::chrono::steady_clock::rep> m_last{k_never};
};
} // namespace diagnostics
