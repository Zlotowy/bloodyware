#pragma once

#include <chrono>
#include <dxgi.h>

namespace ui
{

class present_recovery
{
public:
    using clock = std::chrono::steady_clock;
    [[nodiscard]] bool retry_surface(HRESULT present, HRESULT device, clock::time_point now)
    {
        if (FAILED(device) || (present != DXGI_ERROR_INVALID_CALL && present != DXGI_ERROR_NOT_CURRENTLY_AVAILABLE &&
                               present != DXGI_ERROR_ACCESS_LOST))
            return false;
        if (m_attempts && now - m_window >= std::chrono::seconds(30))
            m_attempts = 0;
        if (!m_attempts)
            m_window = now;
        if (m_attempts >= 3)
            return false;
        ++m_attempts;
        return true;
    }

private:
    clock::time_point m_window{};
    unsigned m_attempts{};
};

} // namespace ui
