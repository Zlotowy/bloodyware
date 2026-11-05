#pragma once

#include <chrono>
#include <cstdint>

namespace features::misc
{

// CT only. Finds the planted bomb (read-only), checks that a defuse can still finish, optionally nudges the view onto
// the bomb with injected mouse movement and holds the use key while the crosshair is on it. Stops as soon as the bomb
// is gone, someone else is defusing, or time runs out.
class auto_defuse_t
{
public:
    void tick();

private:
    void release();

    std::uintptr_t m_bomb{};
    std::chrono::steady_clock::time_point m_next_scan{};
    std::chrono::steady_clock::time_point m_last_aim{};
    std::uint16_t m_key{};
    bool m_use_down{};
    float m_last_yaw{};
    float m_last_pitch{};
    float m_carry_x{};
    float m_carry_y{};
};

inline auto_defuse_t& auto_defuse()
{
    static auto_defuse_t value{};
    return value;
}

} // namespace features::misc
