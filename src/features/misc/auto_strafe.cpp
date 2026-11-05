#include <stdafx.hpp>
#include <foundation/identity.hpp>
#include <config/settings.hpp>
#include <config/misc.hpp>
#include <platform/windows/input.hpp>
#include <game/state/services.hpp>
#include <game/memory/bound_services.hpp>
#include <game/input/bindings.hpp>
#include <render/presentation.hpp>
#include <features/misc/auto_strafe.hpp>
#include <features/misc/misc.hpp>
#include <array>
#include <cmath>
#include <span>

namespace features::misc
{
namespace
{
// A camera sample only changes once per rendered frame, so a decided direction is kept for a few frames instead of
// flickering the key between samples.
constexpr auto k_direction_hold = std::chrono::milliseconds(25);

// Steer mode tuning.
constexpr auto k_steer_min_speed = 20.0f;     // below this there is no meaningful heading
constexpr auto k_steer_latch_speed = 100.0f;  // heading is taken from velocity above this, view below
constexpr auto k_steer_flick_degrees = 20.0f; // view this far off velocity = player re-aimed
constexpr auto k_steer_gain = 0.6f;           // fraction of the yaw error corrected per camera frame
constexpr auto k_steer_max_step = 15.0f;      // max degrees injected in one step
constexpr auto k_steer_retry = std::chrono::milliseconds(12);      // re-inject if the view did not move
constexpr auto k_heading_keep = std::chrono::milliseconds(250);    // heading survives a bhop landing
constexpr auto k_rad_to_deg = 57.29577951308232f;

[[nodiscard]] bool game_has_input_focus()
{
    const auto foreground = ::GetForegroundWindow();
    const auto root = foreground ? ::GetAncestor(foreground, GA_ROOT) : nullptr;
    DWORD process_id{};
    if (root)
        ::GetWindowThreadProcessId(root, &process_id);
    return process_id != 0 && process_id == game::bound_memory().process.process_id();
}

[[nodiscard]] bool keyboard_key_down(const game::input_binding& binding)
{
    return binding.device == game::input_device::keyboard && binding.virtual_key &&
           platform::windows::bound_input().physical_key_down(binding.virtual_key);
}
} // namespace

bool auto_strafe_t::set_control(owned_control& control, const bool pressed)
{
    return input_lifecycle::transition(control, pressed,
                                       [](const game::input_binding& binding, const bool down)
                                       {
                                           return binding.device == game::input_device::keyboard &&
                                                  binding.virtual_key &&
                                                  platform::windows::bound_input().key(binding.virtual_key, down);
                                       });
}

void auto_strafe_t::release()
{
    static_cast<void>(set_control(m_left, false));
    static_cast<void>(set_control(m_right, false));
    const std::array<owned_control*, 2> controls{&m_left, &m_right};
    static_cast<void>(input_lifecycle::discard_released(controls));
    m_direction = 0;
}

void auto_strafe_t::apply_keys(const bool want_left, const bool want_right, const game::input_binding& left_binding,
                               const game::input_binding& right_binding)
{
    // Release first so both side keys are never down together.
    if (!want_left)
        static_cast<void>(set_control(m_left, false));
    if (!want_right)
        static_cast<void>(set_control(m_right, false));
    if (want_left && !m_left.pressed)
    {
        m_left.binding = left_binding;
        static_cast<void>(set_control(m_left, true));
    }
    if (want_right && !m_right.pressed)
    {
        m_right.binding = right_binding;
        static_cast<void>(set_control(m_right, true));
    }
}

void auto_strafe_t::steer(const std::uintptr_t pawn, const float camera_yaw,
                          const std::chrono::steady_clock::time_point now, const game::input_binding& left_binding,
                          const game::input_binding& right_binding)
{
    m_has_yaw = false; // follow mode must re-prime if the mode is switched back

    const auto& process = game::bound_memory().process;
    foundation::vec3 velocity{};
    if (!process.copy(pawn + SCHEMA("C_BaseEntity", "m_vecAbsVelocity"_id), &velocity, sizeof(velocity)) ||
        !std::isfinite(velocity.x) || !std::isfinite(velocity.y))
    {
        release();
        return;
    }

    const auto speed = std::hypot(velocity.x, velocity.y);
    if (speed < k_steer_min_speed)
    {
        release();
        m_has_heading = false;
        return;
    }
    const auto velocity_yaw = std::atan2(velocity.y, velocity.x) * k_rad_to_deg;

    // Heading = where the player wants to go. Latched once, then only a hard flick of the view (further off the
    // velocity than strafing itself ever pulls it) re-aims it.
    if (!m_has_heading)
    {
        m_heading = speed >= k_steer_latch_speed ? velocity_yaw : camera_yaw;
        m_has_heading = true;
    }
    else if (std::abs(std::remainder(camera_yaw - velocity_yaw, 360.0f)) > k_steer_flick_degrees)
    {
        m_heading = camera_yaw;
    }

    // Side-only wish direction is exactly view yaw -/+ 90, so the best view yaw is the velocity yaw itself and the
    // key just picks which way velocity gets pulled back toward the heading.
    const auto error = std::remainder(velocity_yaw - m_heading, 360.0f); // >0: velocity is left of heading
    const auto want_right = error > 0.0f;
    apply_keys(!want_right, want_right, left_binding, right_binding);

    // Mouse: one correction per camera frame (the camera value only changes per frame, so correcting every 1 ms tick
    // would inject the same error many times).
    const auto new_frame = camera_yaw != m_last_camera_yaw;
    m_last_camera_yaw = camera_yaw;
    if (!new_frame && now - m_last_steer < k_steer_retry)
        return;

    constexpr auto m_yaw = 0.022f;
    const auto sensitivity = game::variables().get<float>(CONVAR("sensitivity"_id));
    const auto fov_adjust = process.load<float>(pawn + SCHEMA("C_BasePlayerPawn", "m_flFOVSensitivityAdjust"_id));
    const auto degrees_per_pixel = sensitivity * m_yaw * fov_adjust;
    if (!std::isfinite(degrees_per_pixel) || degrees_per_pixel <= 0.0f)
        return;

    const auto delta = std::clamp(std::remainder(velocity_yaw - camera_yaw, 360.0f) * k_steer_gain,
                                  -k_steer_max_step, k_steer_max_step);
    // Mouse right lowers yaw. The sub-pixel remainder is carried so small corrections still land.
    m_mouse_carry += -delta / degrees_per_pixel;
    const auto dx = static_cast<int>(m_mouse_carry);
    m_mouse_carry -= static_cast<float>(dx);
    m_last_steer = now;
    if (dx != 0)
        platform::windows::bound_input().pointer(dx, 0, platform::windows::pointer_action::relative_move);
}

void auto_strafe_t::tick()
{
    const auto snapshot = config::get_runtime_snapshot();
    const auto now = std::chrono::steady_clock::now();
    const auto pawn = game::local_player().pawn();

    const auto stop = [this]()
    {
        release();
        m_has_yaw = false;
    };

    // The lineup throw drives the movement keys itself.
    if (!snapshot || !snapshot->general.m_auto_strafe.enabled || !pawn || !game::local_player().alive() ||
        !game_has_input_focus() || render::menu_open() || game::input_bindings().text_entry_active() ||
        nade_helper().movement_taken())
    {
        stop();
        m_has_heading = false;
        return;
    }

    // Only while the player is actually bunny hopping: jump key or the bhop key held.
    const auto jump_binding = game::input_bindings().resolve(game::input_action::jump);
    const auto bhop_key =
        static_cast<std::uint16_t>(std::clamp(snapshot->general.m_bunny_hop.activation_key, 0, 255));
    const auto hopping =
        keyboard_key_down(jump_binding) ||
        (bhop_key != 0 && platform::windows::bound_input().physical_key_down(bhop_key));
    if (!hopping)
    {
        stop();
        m_has_heading = false;
        return;
    }

    const auto& process = game::bound_memory().process;
    std::uint32_t flags{}, ground_entity{};
    if (!process.copy(pawn + SCHEMA("C_BaseEntity", "m_fFlags"_id), &flags, sizeof(flags)) ||
        !process.copy(pawn + SCHEMA("C_BaseEntity", "m_hGroundEntity"_id), &ground_entity, sizeof(ground_entity)))
    {
        stop();
        return;
    }
    const auto on_ground = (flags & 1u) != 0u || ground_entity != 0xffffffffu;
    if (on_ground)
    {
        stop();
        return;
    }

    // A new hop chain starts a new heading.
    if (now - m_last_air_tick > k_heading_keep)
        m_has_heading = false;
    m_last_air_tick = now;

    // The player strafes by hand: leave both keys alone.
    const auto left_binding = game::input_bindings().resolve(game::input_action::left);
    const auto right_binding = game::input_bindings().resolve(game::input_action::right);
    if ((!m_left.pressed && keyboard_key_down(left_binding)) || (!m_right.pressed && keyboard_key_down(right_binding)))
    {
        stop();
        return;
    }

    foundation::vec3 origin{}, angles{};
    if (!game::camera().sample(origin, angles) || !std::isfinite(angles.y))
    {
        stop();
        return;
    }

    // Steer mode assumes side-only input, so it stands aside while forward is held.
    const auto forward_held = keyboard_key_down(game::input_bindings().resolve(game::input_action::forward));
    if (snapshot->general.m_auto_strafe.steer && !forward_held)
    {
        steer(pawn, angles.y, now, left_binding, right_binding);
        return;
    }
    m_has_heading = false;

    if (!m_has_yaw || pawn != m_pawn)
    {
        m_pawn = pawn;
        m_last_yaw = angles.y;
        m_has_yaw = true;
        return;
    }

    // Source yaw grows counter-clockwise: positive delta = view turning left.
    const auto delta = std::remainder(angles.y - m_last_yaw, 360.0f);
    m_last_yaw = angles.y;
    const auto threshold = std::clamp(snapshot->general.m_auto_strafe.turn_threshold, 0.01f, 5.0f);
    if (std::abs(delta) >= threshold)
    {
        m_direction = delta > 0.0f ? 1 : -1;
        m_direction_until = now + k_direction_hold;
    }
    else if (now >= m_direction_until)
    {
        m_direction = 0;
    }

    apply_keys(m_direction > 0, m_direction < 0, left_binding, right_binding);
}

} // namespace features::misc
