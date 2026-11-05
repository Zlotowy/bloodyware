#include <stdafx.hpp>
#include <foundation/identity.hpp>
#include <config/settings.hpp>
#include <config/misc.hpp>
#include <game/state/services.hpp>
#include <game/memory/bound_services.hpp>
#include <game/input/bindings.hpp>
#include <render/presentation.hpp>
#include <features/misc/killsay.hpp>
#include <features/misc/console_input.hpp>
#include <algorithm>
#include <random>
#include <vector>

namespace features::misc
{
namespace
{
constexpr auto k_pending_timeout = std::chrono::seconds(4); // give up if it never becomes safe
constexpr auto k_min_gap = std::chrono::milliseconds(1500); // chat flood guard
constexpr std::size_t k_max_length = 120;

[[nodiscard]] bool game_has_input_focus()
{
    const auto foreground = ::GetForegroundWindow();
    const auto root = foreground ? ::GetAncestor(foreground, GA_ROOT) : nullptr;
    DWORD process_id{};
    if (root)
        ::GetWindowThreadProcessId(root, &process_id);
    return process_id != 0 && process_id == game::bound_memory().process.process_id();
}

[[nodiscard]] int random_below(const int count)
{
    static std::mt19937 engine{std::random_device{}()};
    return std::uniform_int_distribution<int>(0, std::max(count - 1, 0))(engine);
}
} // namespace

void killsay_t::tick()
{
    const auto settings = config::get_runtime_snapshot();
    const auto now = std::chrono::steady_clock::now();
    if (!settings || !settings->general.m_killsay.enabled)
    {
        m_pending = false;
        m_initialized = false;
        return;
    }
    const auto& cfg = settings->general.m_killsay;

    const auto controller = game::local_player().controller();
    if (!controller)
        return;
    const auto& process = game::bound_memory().process;

    std::uintptr_t services{};
    std::int32_t kills{};
    if (!process.copy(controller + SCHEMA("CCSPlayerController", "m_pActionTrackingServices"_id), &services,
                      sizeof(services)) ||
        !services ||
        !process.copy(services + SCHEMA("CCSPlayerController_ActionTrackingServices", "m_iNumRoundKills"_id), &kills,
                      sizeof(kills)) ||
        kills < 0 || kills > 128)
        return;

    // First sample, a new controller state or a new round (counter went down): just resync.
    if (!m_initialized || services != m_services || kills < m_last_kills)
    {
        m_services = services;
        m_last_kills = kills;
        m_initialized = true;
        return;
    }
    if (kills > m_last_kills)
    {
        m_pending = true;
        m_pending_since = now;
    }
    m_last_kills = kills;

    if (!m_pending)
        return;
    if (now - m_pending_since > k_pending_timeout || !game::local_player().alive())
    {
        m_pending = false;
        return;
    }
    if (now < m_next_allowed || render::menu_open() || game::input_bindings().text_entry_active() ||
        !game_has_input_focus())
        return;

    // Opening the console swallows game input for a moment, so wait until nobody is shooting at us.
    if (cfg.only_if_safe)
    {
        const auto players = game::world().players();
        if (!players)
            return;
        for (const auto& player : *players)
        {
            if (player.pawn != game::local_player().pawn() && player.health > 0 && player.health <= 200 &&
                game::local_player().is_enemy(player.team) && player.is_visible)
                return;
        }
    }

    // Pick a random usable message, not the same one twice in a row.
    std::vector<int> usable;
    for (int index = 0; index < static_cast<int>(cfg.messages.size()); ++index)
    {
        const auto& text = cfg.messages[static_cast<std::size_t>(index)];
        if (std::any_of(text.begin(), text.end(), [](const char c) { return c != ' ' && c != '\0'; }))
            usable.push_back(index);
    }
    m_pending = false;
    if (usable.empty())
        return;
    int choice = usable[static_cast<std::size_t>(random_below(static_cast<int>(usable.size())))];
    if (usable.size() > 1 && choice == m_last_index)
        choice = usable[static_cast<std::size_t>((std::find(usable.begin(), usable.end(), choice) - usable.begin() + 1) %
                                                 static_cast<std::ptrdiff_t>(usable.size()))];
    m_last_index = choice;

    std::string message = cfg.messages[static_cast<std::size_t>(choice)].data();
    if (message.size() > k_max_length)
        message.resize(k_max_length);

    run_console_command(std::string(cfg.team_chat ? "say_team " : "say ") + message, cfg.console_key,
                        cfg.console_delay, cfg.paste);
    m_next_allowed = std::chrono::steady_clock::now() + k_min_gap;
}

} // namespace features::misc
