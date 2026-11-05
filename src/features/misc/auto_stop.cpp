#include <stdafx.hpp>
#include <foundation/identity.hpp>
#include <game/state/constants.hpp>
#include <render/presentation.hpp>
#include <features/visuals/visuals.hpp>
#include <platform/windows/input.hpp>
#include <config/settings.hpp>
#include <game/state/pose.hpp>
#include <game/memory/bound_services.hpp>
#include <game/state/services.hpp>
#include <config/misc.hpp>
#include <game/input/bindings.hpp>
#include <features/misc/auto_stop.hpp>
#include <diagnostics/journal.hpp>

namespace
{
[[nodiscard]] bool finite_velocity(const foundation::vec3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
} // namespace

namespace features::misc
{

void auto_stop_t::request_stop(const auto_stop_source source, const std::chrono::steady_clock::time_point shot_time,
                               const float required_shoot_speed)
{
    const auto runtime_settings = config::get_runtime_snapshot();
    if (!runtime_settings->general.m_auto_stop.enabled ||
        source == auto_stop_source::seed_trigger) // the seeded trigger fires on its own seed, not on a stop
    {
        cancel_request(source);
        return;
    }

    std::scoped_lock lock(m_request_mutex);
    auto& request = m_requests[static_cast<std::size_t>(source)];
    const auto was_active = request.active;
    request.shot_time = shot_time;
    request.required_shoot_speed =
        std::clamp(std::isfinite(required_shoot_speed) ? required_shoot_speed : 5.0f, 0.0f, 150.0f);
    request.active = true;
    // Refreshing the same combat decision must not restart braking. Revision only
    // changes on a new ownership edge, cancellation or a completed shot.
    if (!was_active)
    {
        ++request.revision;
        request.braking = false;
    }
}

void auto_stop_t::cancel_request(const auto_stop_source source)
{
    std::scoped_lock lock(m_request_mutex);
    auto& request = m_requests[static_cast<std::size_t>(source)];
    if (request.active || request.braking)
        ++request.revision;
    request = {.revision = request.revision};
}

void auto_stop_t::notify_shot(const auto_stop_source source)
{
    {
        std::scoped_lock lock(m_request_mutex);
        auto& request = m_requests[static_cast<std::size_t>(source)];
        ++request.revision;
        request = {.revision = request.revision};
    }

    release();
}

bool auto_stop_t::ready_to_fire(const auto_stop_source source) const
{
    const auto runtime_settings = config::get_runtime_snapshot();
    if (!runtime_settings->general.m_auto_stop.enabled)
        return true;

    float required_speed{};
    {
        std::scoped_lock lock(m_request_mutex);
        const auto& request = m_requests[static_cast<std::size_t>(source)];
        if (!request.active)
            return true;
        required_speed = request.required_shoot_speed;
    }

    // Why a wanted shot is held back is journaled (at most 4 times a second) so a stuck stop can be read afterwards:
    // code = reason << 24 | current speed (u/s) << 8 | speed it waits for.
    const auto note = [this](const std::uint32_t reason, const float speed, const float wanted)
    {
        if (!m_wait_note.allow())
            return;
        const auto clamp8 = [](const float value)
        { return static_cast<std::uint32_t>(std::clamp(value, 0.0f, 255.0f)); };
        diagnostics::emit(
            diagnostics::event::autostop_waiting, diagnostics::severity::information, diagnostics::code_domain::mode,
            (reason << 24) |
                (std::min<std::uint32_t>(static_cast<std::uint32_t>(std::max(speed, 0.0f)), 0xFFFFu) << 8) |
                clamp8(wanted));
    };
    const auto pawn = game::local_player().pawn();
    if (!pawn)
    {
        note(1, 0.0f, 0.0f);
        return false;
    }
    std::uint32_t flags{};
    if (!game::bound_memory().process.copy(pawn + SCHEMA("C_BaseEntity", "m_fFlags"_id), &flags, sizeof(flags)))
    {
        note(2, 0.0f, 0.0f);
        return false;
    }
    // Auto Stop cannot affect airborne movement. Legit checks decide separately
    // whether the shot itself is allowed; never deadlock an otherwise valid shot.
    if ((flags & 1u) == 0u)
        return true;
    foundation::vec3 velocity{};
    if (!game::bound_memory().process.copy(pawn + SCHEMA("C_BaseEntity", "m_vecAbsVelocity"_id), &velocity,
                                           sizeof(velocity)))
    {
        note(3, 0.0f, 0.0f);
        return false;
    }

    const auto completion_speed =
        std::min(required_speed, std::max(runtime_settings->general.m_auto_stop.stop_speed, 0.5f));
    const auto ready = finite_velocity(velocity) && velocity.length_2d() <= completion_speed;
    if (!ready)
        note(4, velocity.length_2d(), completion_speed);
    return ready;
}

auto_stop_t::movement_set auto_stop_t::movement_bindings() const
{
    auto& bindings = game::input_bindings();
    movement_set set{};
    set.forward = bindings.resolve(game::input_action::forward);
    set.back = bindings.resolve(game::input_action::back);
    set.left = bindings.resolve(game::input_action::left);
    set.right = bindings.resolve(game::input_action::right);
    for (const auto* binding : {&set.forward, &set.back, &set.left, &set.right})
    {
        if (*binding && binding->virtual_key && std::ranges::find(set.all, binding->virtual_key) == set.all.end())
            set.all.push_back(binding->virtual_key);
    }
    return set;
}

// The keys that push against the current velocity, relative to the view. A key is chosen only while the
// player is fast enough for one tick of acceleration not to overshoot: the last part is friction's work.
std::vector<std::uint16_t> auto_stop_t::counter_keys(const foundation::vec3& velocity, const movement_set& set) const
{
    const auto speed = velocity.length_2d();
    const auto accelerate = std::max(game::variables().get<float>(CONVAR("sv_accelerate"_id)), 0.1f);
    const auto one_tick_gain = accelerate * std::max(game::rules::simulation_step, 0.001f) * 250.0f;
    if (!std::isfinite(speed) || speed <= one_tick_gain * 0.8f)
        return {};

    foundation::vec3 forward{}, right{};
    game::camera().angles().to_directions(&forward, &right, nullptr);
    forward.z = 0.0f;
    right.z = 0.0f;
    forward.normalize();
    right.normalize();
    const auto along = velocity.x * forward.x + velocity.y * forward.y;
    const auto across = velocity.x * right.x + velocity.y * right.y;
    const auto dead = speed * 0.25f;
    std::vector<std::uint16_t> keys{};
    const auto add = [&](const game::input_binding& binding)
    {
        if (binding && binding.virtual_key && std::ranges::find(keys, binding.virtual_key) == keys.end())
            keys.push_back(binding.virtual_key);
    };
    if (along > dead)
        add(set.back);
    else if (along < -dead)
        add(set.forward);
    if (across > dead)
        add(set.left);
    else if (across < -dead)
        add(set.right);
    return keys;
}

void auto_stop_t::engage(const foundation::vec3& velocity, const float)
{
    std::scoped_lock control_lock(m_control_mutex);
    const auto set = movement_bindings();
    if (set.all.empty())
        return;

    // Take the movement keys away from the player for as long as the stop is wanted: otherwise a held key
    // accelerates again the moment the counter key lets go and the stop becomes a slowdown.
    if (!m_active || m_gated_keys != set.all)
    {
        platform::windows::bound_input().set_movement_gate(platform::windows::input_gateway::gate_owner::auto_stop,
                                                           set.all, true);
        m_gated_keys = set.all;
        m_active = true;
    }
    if (!platform::windows::bound_input().key_gate_ready())
        return;

    const auto desired = counter_keys(velocity, set);
    for (const auto key : m_synthetic_keys)
        if (std::ranges::find(desired, key) == desired.end())
            (void)platform::windows::bound_input().key(key, false);
    std::vector<std::uint16_t> held{};
    for (const auto key : desired)
    {
        const auto already = std::ranges::find(m_synthetic_keys, key) != m_synthetic_keys.end();
        if (already || platform::windows::bound_input().key(key, true))
            held.push_back(key);
    }
    m_synthetic_keys = std::move(held);
}

void auto_stop_t::release()
{
    std::scoped_lock control_lock(m_control_mutex);
    if (!m_active && m_synthetic_keys.empty() && m_gated_keys.empty())
        return;

    // Release exactly what Auto Stop pressed, then give the movement keys back to the player.
    for (const auto key : m_synthetic_keys)
        (void)platform::windows::bound_input().key(key, false);
    m_synthetic_keys.clear();
    if (!m_gated_keys.empty())
        platform::windows::bound_input().set_movement_gate(platform::windows::input_gateway::gate_owner::auto_stop, {},
                                                           false);
    m_gated_keys.clear();
    m_active = false;
}

void auto_stop_t::reset()
{
    {
        std::scoped_lock lock(m_request_mutex);
        for (auto& request : m_requests)
        {
            ++request.revision;
            request = {.revision = request.revision};
        }
    }
    release();
}

void auto_stop_t::tick()
{
    const auto runtime_settings = config::get_runtime_snapshot();
    if (!runtime_settings->general.m_auto_stop.enabled)
    {
        // Do the synchronized cleanup once on the enabled->disabled edge. The
        // 1 kHz movement worker otherwise acquired two mutexes on every idle tick.
        if (m_enabled_last_tick)
        {
            m_enabled_last_tick = false;
            reset();
        }
        return;
    }
    m_enabled_last_tick = true;
    const auto pawn = game::local_player().pawn();
    const auto usable = pawn && game::local_player().alive() &&
                        game::rules::is_firearm(game::local_player().weapon_type()) && !render::menu_open() &&
                        !game::input_bindings().text_entry_active() && render::combat_input_ready();
    if (!usable)
    {
        reset();
        return;
    }

    const auto flags = game::bound_memory().process.load<std::uint32_t>(pawn + SCHEMA("C_BaseEntity", "m_fFlags"_id));
    if ((flags & 1u) == 0u)
    {
        release();
        return;
    }
    const auto velocity =
        game::bound_memory().process.load<foundation::vec3>(pawn + SCHEMA("C_BaseEntity", "m_vecAbsVelocity"_id));
    if (!finite_velocity(velocity))
    {
        release();
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    bool should_brake{};
    float stop_speed = 150.0f;
    {
        std::scoped_lock lock(m_request_mutex);
        for (auto& request : m_requests)
        {
            if (!request.active)
                continue;
            // The stop starts the moment a shot is wanted and runs to zero.
            request.braking = true;
            const auto completion_speed = std::min(request.required_shoot_speed,
                                                   std::max(runtime_settings->general.m_auto_stop.stop_speed, 0.5f));
            stop_speed = std::min(stop_speed, completion_speed);
            should_brake = should_brake || request.braking;
        }
    }

    if (!should_brake)
    {
        release();
        return;
    }
    engage(velocity, stop_speed);
    {
        // While braking, journal how it is going (4 times a second at most): reason 5, speed, whether the key
        // gate is live, how many counter keys are held and how many movement keys are gated.
        if (m_brake_note.allow(now))
        {
            std::uint32_t held{}, gated{};
            {
                std::scoped_lock control_lock(m_control_mutex);
                held = static_cast<std::uint32_t>(m_synthetic_keys.size());
                gated = static_cast<std::uint32_t>(m_gated_keys.size());
            }
            const auto ready = platform::windows::bound_input().key_gate_ready() ? 1u : 0u;
            diagnostics::emit(
                diagnostics::event::autostop_waiting, diagnostics::severity::information,
                diagnostics::code_domain::mode,
                (5u << 24) | (std::min<std::uint32_t>(static_cast<std::uint32_t>(velocity.length_2d()), 0xFFFu) << 12) |
                    (ready << 8) | (std::min(held, 15u) << 4) | std::min(gated, 15u));
        }
    }
}

} // namespace features::misc
