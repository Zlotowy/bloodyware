#pragma once

#include <windows.h>
#include <timeapi.h>

namespace app
{

enum class instance_state
{
    acquired,
    already_running,
    failed,
};

// Existence, not mutex ownership, is the single-instance contract.
class single_instance_guard final
{
public:
    explicit single_instance_guard(const wchar_t* name = L"Local\\vesta.overlay.single-instance") noexcept
        : m_handle(::CreateMutexW(nullptr, FALSE, name))
    {
        m_error = ::GetLastError();
        m_state = !m_handle
                      ? instance_state::failed
                      : (m_error == ERROR_ALREADY_EXISTS ? instance_state::already_running : instance_state::acquired);
    }

    ~single_instance_guard()
    {
        if (m_handle)
            ::CloseHandle(m_handle);
    }

    single_instance_guard(const single_instance_guard&) = delete;
    single_instance_guard& operator=(const single_instance_guard&) = delete;

    [[nodiscard]] instance_state state() const noexcept { return m_state; }
    [[nodiscard]] DWORD error() const noexcept { return m_error; }

private:
    HANDLE m_handle{};
    DWORD m_error{};
    instance_state m_state{instance_state::failed};
};

class timer_period_guard final
{
public:
    explicit timer_period_guard(const UINT period = 1) noexcept
        : m_period(period)
        , m_active(::timeBeginPeriod(period) == TIMERR_NOERROR)
    {
    }

    ~timer_period_guard()
    {
        if (m_active)
            ::timeEndPeriod(m_period);
    }

    timer_period_guard(const timer_period_guard&) = delete;
    timer_period_guard& operator=(const timer_period_guard&) = delete;

    [[nodiscard]] bool active() const noexcept { return m_active; }

private:
    UINT m_period{};
    bool m_active{};
};

} // namespace app
