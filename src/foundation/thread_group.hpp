#pragma once

#include <atomic>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace foundation
{

// start()/join() belong to the owner thread; request_stop() may run on any thread.
// A common stop source stops ALL workers before the owner joins even the first.
class thread_group final
{
public:
    using worker_id = std::uint32_t;
    static constexpr worker_id no_failure = std::numeric_limits<worker_id>::max();
    struct worker_error
    {
        bool system{};
        int code{};
    };
    using failure_handler = void (*)(void*, worker_id, worker_error) noexcept;

    explicit thread_group(failure_handler failure = nullptr, void* owner = nullptr,
                          std::stop_source source = std::stop_source{})
        : m_stop(std::move(source))
        , m_failure(failure)
        , m_owner(owner)
    {
        m_threads.reserve(8);
    }
    ~thread_group() { stop_and_join(); }
    thread_group(const thread_group&) = delete;
    thread_group& operator=(const thread_group&) = delete;

    template<class Function>
    void start(worker_id id, Function function)
    {
        if (m_stop.stop_requested())
            throw std::logic_error("Cannot start a stopped thread group");
        m_threads.emplace_back(
            [this, id, function = std::move(function)]() mutable
            {
                try
                {
                    function(m_stop.get_token());
                }
                catch (const std::system_error& error)
                {
                    notify_failure(id, {true, error.code().value()});
                }
                catch (...)
                {
                    notify_failure(id, {});
                }
            });
    }

    void request_stop() noexcept { m_stop.request_stop(); }
    void stop_and_join() noexcept
    {
        request_stop();
        for (auto& thread : m_threads)
            if (thread.joinable())
                thread.join();
        m_threads.clear();
    }
    [[nodiscard]] std::stop_token token() const noexcept { return m_stop.get_token(); }
    [[nodiscard]] worker_id failed_worker() const noexcept { return m_failed.load(std::memory_order_relaxed); }

private:
    void notify_failure(worker_id id, worker_error error) noexcept
    {
        auto expected = no_failure;
        m_failed.compare_exchange_strong(expected, id, std::memory_order_relaxed);
        request_stop();
        if (m_failure)
            m_failure(m_owner, id, error);
    }
    std::stop_source m_stop;
    std::vector<std::thread> m_threads;
    std::atomic<worker_id> m_failed{no_failure};
    failure_handler m_failure{};
    void* m_owner{};
};

} // namespace foundation
