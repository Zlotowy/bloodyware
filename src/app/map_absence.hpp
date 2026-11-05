#pragma once

#include <chrono>
#include <optional>

namespace app
{

class map_absence_guard
{
public:
    bool observe(bool local_valid, std::chrono::steady_clock::time_point now) noexcept
    {
        if (local_valid)
        {
            m_first_missing.reset();
            return false;
        }
        if (!m_first_missing)
        {
            m_first_missing = now;
            return false;
        }
        return now - *m_first_missing >= std::chrono::milliseconds(1500);
    }

private:
    std::optional<std::chrono::steady_clock::time_point> m_first_missing{};
};

} // namespace app