#include <stdafx.hpp>
#include <config/settings.hpp>
#include <config/misc.hpp>
#include <platform/windows/input.hpp>
#include <game/state/services.hpp>
#include <game/memory/bound_services.hpp>
#include <render/presentation.hpp>
#include <features/misc/anti_afk.hpp>
#include <array>
#include <random>

namespace features::misc
{
namespace
{
constexpr std::array<std::uint16_t, 4> k_move_keys{'W', 'A', 'S', 'D'};

constexpr int k_interval_min_ms = 15000;
constexpr int k_interval_max_ms = 30000;
constexpr int k_hold_min_ms = 90;
constexpr int k_hold_max_ms = 160;
constexpr int k_wiggle_min = 2;
constexpr int k_wiggle_max = 6;

[[nodiscard]] int random_between(const int minimum, const int maximum)
{
    static std::mt19937 engine{std::random_device{}()};
    return std::uniform_int_distribution<int>(minimum, maximum)(engine);
}

[[nodiscard]] bool game_has_input_focus()
{
    const auto foreground = ::GetForegroundWindow();
    const auto root = foreground ? ::GetAncestor(foreground, GA_ROOT) : nullptr;
    DWORD process_id{};
    if (root)
        ::GetWindowThreadProcessId(root, &process_id);
    return process_id != 0 && process_id == game::bound_memory().process.process_id();
}
} // namespace

void anti_afk_t::release()
{
    if (!m_pressed)
        return;
    auto& input = platform::windows::bound_input();
    input.key(m_key, false);
    if (m_wiggle_x != 0)
        input.pointer(-m_wiggle_x, 0, platform::windows::pointer_action::relative_move);
    m_pressed = false;
    m_wiggle_x = 0;
}

void anti_afk_t::tick()
{
    const auto settings = config::get_runtime_snapshot();
    const auto now = std::chrono::steady_clock::now();

    if (!settings || !settings->general.anti_afk)
    {
        release();
        m_next = {};
        return;
    }

    const bool active = game::local_player().valid() && game::local_player().alive() && !render::menu_open();

    // Always finish a started tap, even if the state changed meanwhile.
    if (m_pressed)
    {
        if (!active || now >= m_release_at)
            release();
        return;
    }

    if (!active)
    {
        m_next = {};
        return;
    }

    if (m_next == std::chrono::steady_clock::time_point{})
        m_next = now + std::chrono::milliseconds{random_between(k_interval_min_ms, k_interval_max_ms)};
    if (now < m_next)
        return;

    m_next = now + std::chrono::milliseconds{random_between(k_interval_min_ms, k_interval_max_ms)};

    if (!game_has_input_focus())
        return;
    auto& input = platform::windows::bound_input();
    for (const auto key : k_move_keys)
        if (input.physical_key_down(key))
            return;

    m_key = k_move_keys[static_cast<std::size_t>(random_between(0, static_cast<int>(k_move_keys.size()) - 1))];
    const int amount = random_between(k_wiggle_min, k_wiggle_max);
    m_wiggle_x = random_between(0, 1) ? amount : -amount;

    if (!input.key(m_key, true))
        return;
    input.pointer(m_wiggle_x, 0, platform::windows::pointer_action::relative_move);
    m_pressed = true;
    m_release_at = now + std::chrono::milliseconds{random_between(k_hold_min_ms, k_hold_max_ms)};
}

} // namespace features::misc
