#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>

namespace foundation
{

// One pending job, one active job. A replaced job can never publish its result.
class latest_job_queue
{
public:
    struct job
    {
        std::string key;
        std::stop_token stop;
        // Monotonic per queue; newer requests always have larger serials.
        std::uint64_t serial{};
    };

    // invalidate(serial) runs under the queue lock before a worker can see the job,
    // so state published for this serial cannot race with the job's own result.
    template<class Invalidate>
    std::uint64_t request(std::string key, Invalidate&& invalidate)
    {
        std::lock_guard lock(m_mutex);
        m_active.request_stop();
        m_pending = std::move(key);
        m_pending_serial = ++m_serial;
        invalidate(m_pending_serial);
        m_changed.notify_one();
        return m_pending_serial;
    }

    [[nodiscard]] std::optional<job> next(std::stop_token stop)
    {
        std::unique_lock lock(m_mutex);
        if (!m_changed.wait(lock, stop, [&] { return m_pending.has_value(); }))
            return {};
        m_active = std::stop_source{};
        job result{std::move(*m_pending), m_active.get_token(), m_pending_serial};
        m_pending.reset();
        return result;
    }

    template<class Publish>
    bool commit(std::stop_token token, Publish&& publish)
    {
        std::lock_guard lock(m_mutex);
        if (token.stop_requested())
            return false;
        publish();
        return true;
    }

    void cancel()
    {
        std::lock_guard lock(m_mutex);
        m_active.request_stop();
    }

private:
    std::mutex m_mutex;
    std::condition_variable_any m_changed;
    std::optional<std::string> m_pending;
    std::uint64_t m_pending_serial{};
    std::uint64_t m_serial{};
    std::stop_source m_active;
};

} // namespace foundation
