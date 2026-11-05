#pragma once

#include <windows.h>

#include <utility>

namespace foundation
{
// Owns a Win32 kernel handle and closes it once. Both null and INVALID_HANDLE_VALUE mean "no handle", since
// Win32 APIs report failure with either.
class unique_handle final
{
public:
    explicit unique_handle(const HANDLE handle = nullptr) noexcept
        : m_handle(handle)
    {
    }
    ~unique_handle() { reset(); }

    unique_handle(const unique_handle&) = delete;
    unique_handle& operator=(const unique_handle&) = delete;
    unique_handle(unique_handle&& other) noexcept
        : m_handle(std::exchange(other.m_handle, nullptr))
    {
    }
    unique_handle& operator=(unique_handle&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            m_handle = std::exchange(other.m_handle, nullptr);
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept { return m_handle; }
    [[nodiscard]] bool valid() const noexcept { return m_handle && m_handle != INVALID_HANDLE_VALUE; }
    [[nodiscard]] explicit operator bool() const noexcept { return valid(); }

    void reset(const HANDLE handle = nullptr) noexcept
    {
        if (valid())
            ::CloseHandle(m_handle);
        m_handle = handle;
    }

private:
    HANDLE m_handle{};
};
} // namespace foundation
