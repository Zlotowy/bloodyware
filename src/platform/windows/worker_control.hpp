#pragma once

#include <foundation/unique_handle.hpp>

#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <stop_token>
#include <system_error>
#include <utility>

namespace platform::windows
{

class stop_signal final
{
    struct signal
    {
        HANDLE handle;
        void operator()() const noexcept { ::SetEvent(handle); }
    };
    static HANDLE create()
    {
        const auto result = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!result)
            throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(), "CreateEventW");
        return result;
    }

public:
    explicit stop_signal(std::stop_token stop)
        : m_handle(create())
        , m_callback(stop, signal{m_handle.get()})
    {
    }
    [[nodiscard]] HANDLE get() const noexcept { return m_handle.get(); }

private:
    foundation::unique_handle m_handle;
    std::stop_callback<signal> m_callback;
};

struct worker_control
{
    std::stop_token stop;
    HANDLE wake{};

    [[nodiscard]] bool stopping() const noexcept { return stop.stop_requested(); }
    void wait_handle(HANDLE object) const
    {
        const HANDLE handles[]{wake, object};
        const auto result = ::WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        if (result != WAIT_OBJECT_0 && result != WAIT_OBJECT_0 + 1)
            throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(),
                                    "WaitForMultipleObjects");
    }
    template<class Rep, class Period>
    void sleep_for(std::chrono::duration<Rep, Period> duration) const
    {
        if (stopping())
            return;
        // Match sleep_for's millisecond ceiling and its expired-deadline no-op.
        const auto millis = std::chrono::ceil<std::chrono::milliseconds>(duration).count();
        if (millis <= 0)
            return;
        const auto timeout = static_cast<DWORD>(std::min<std::int64_t>(millis, INFINITE - 1));
        const auto result = ::WaitForSingleObject(wake, timeout);
        if (result != WAIT_OBJECT_0 && result != WAIT_TIMEOUT)
            throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(), "WaitForSingleObject");
    }
    void sleep_until(std::chrono::steady_clock::time_point deadline) const
    {
        sleep_for(deadline - std::chrono::steady_clock::now());
    }
};

// Same timer/fallback scheduling as the existing precise poll, but both the
// timer wait and its owning HANDLE now participate in managed shutdown.
class precise_worker_wait final
{
    static HANDLE create() noexcept
    {
        const auto precise =
            ::CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        return precise ? precise : ::CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }

public:
    explicit precise_worker_wait(worker_control control)
        : m_control(control)
        , m_timer(create())
    {
    }
    void wait(std::chrono::microseconds duration) const
    {
        if (m_control.stopping())
            return;
        if (duration <= std::chrono::microseconds::zero())
        {
            ::SwitchToThread();
            return;
        }
        if (m_timer.get())
        {
            LARGE_INTEGER due{};
            due.QuadPart = -std::max<LONGLONG>(1, static_cast<LONGLONG>(duration.count()) * 10);
            if (::SetWaitableTimer(m_timer.get(), &due, 0, nullptr, nullptr, FALSE))
            {
                m_control.wait_handle(m_timer.get());
                return;
            }
        }
        // Preserve the original sub-millisecond Sleep(0) fallback, not a new
        // 1ms latency penalty for systems without a working waitable timer.
        if (duration >= std::chrono::milliseconds(1))
            m_control.sleep_for(std::chrono::milliseconds(duration.count() / 1000));
        else if (!m_control.stopping())
            ::Sleep(0);
    }

private:
    worker_control m_control;
    foundation::unique_handle m_timer;
};

} // namespace platform::windows
