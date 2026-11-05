#pragma once

#include <chrono>
#include <cstdint>
#include <game/input/bindings.hpp>
#include <features/misc/input_lifecycle.hpp>

namespace features::misc
{

// Air strafe without memory writes. While airborne and the jump / bhop key is held:
//  - follow mode: holds the left/right key that matches the direction the view is turning.
//  - steer mode (needs forward released): injects mouse movement so the view tracks the velocity heading and
//    alternates left/right to keep that heading, i.e. straight-line strafing.
class auto_strafe_t
{
public:
    void tick();

private:
    using owned_control = input_lifecycle::owned_control;

    [[nodiscard]] bool set_control(owned_control& control, bool pressed);
    void release();
    void apply_keys(bool want_left, bool want_right, const game::input_binding& left_binding,
                    const game::input_binding& right_binding);
    void steer(std::uintptr_t pawn, float camera_yaw, std::chrono::steady_clock::time_point now,
               const game::input_binding& left_binding, const game::input_binding& right_binding);

    owned_control m_left{};
    owned_control m_right{};
    std::uintptr_t m_pawn{};
    float m_last_yaw{};
    bool m_has_yaw{};
    int m_direction{}; // +1 turning left (A), -1 turning right (D), 0 none
    std::chrono::steady_clock::time_point m_direction_until{};

    // steer mode
    float m_heading{};
    float m_last_camera_yaw{};
    float m_mouse_carry{};
    bool m_has_heading{};
    std::chrono::steady_clock::time_point m_last_air_tick{};
    std::chrono::steady_clock::time_point m_last_steer{};
};

inline auto_strafe_t& auto_strafe()
{
    static auto_strafe_t value{};
    return value;
}

} // namespace features::misc
