#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace simulation
{
class seed_receipt
{
public:
    void reset() noexcept { *this = {}; }

    [[nodiscard]] bool observe(std::uintptr_t weapon, float time, int prediction_tick) noexcept
    {
        if (!weapon || !std::isfinite(time) || time < 0 || prediction_tick <= 0 || prediction_tick < m_confirmed_tick)
            return false;
        select_weapon(weapon);
        // Prediction can retract last-shot time without retracting this input.
        return true;
    }

    void arm(std::uintptr_t weapon, float before, int prediction_tick) noexcept
    {
        select_weapon(weapon);
        m_before = before;
        m_submitted_tick = prediction_tick;
        m_pending = true;
    }

    [[nodiscard]] bool consumed(std::uintptr_t weapon, float time) const noexcept
    {
        return m_pending && weapon == m_weapon && std::isfinite(time) && time > m_before && time > m_confirmed_time;
    }

    void acknowledge(float time) noexcept
    {
        if (!consumed(m_weapon, time))
            return;
        m_confirmed_time = time;
        m_confirmed_tick = std::max(m_confirmed_tick, m_submitted_tick);
        m_pending = false;
    }

private:
    void select_weapon(std::uintptr_t weapon) noexcept
    {
        if (weapon == m_weapon)
            return;
        m_weapon = weapon;
        m_confirmed_time = -1.0f;
        m_pending = false;
    }
    std::uintptr_t m_weapon{};
    float m_before{}, m_confirmed_time{-1.0f};
    int m_submitted_tick{}, m_confirmed_tick{};
    bool m_pending{};
};
} // namespace simulation
