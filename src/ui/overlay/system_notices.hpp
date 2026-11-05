#pragma once

#include <diagnostics/readiness.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ui::notices
{
enum class level : std::uint8_t
{
    information,
    warning,
    error
};

// A system message for the player. `text` is an English sentence that the HUD translates when it draws it;
// `key` and `serial` identify the situation: closing a notice hides that situation only, so a new map (new serial)
// or a different problem shows again.
struct notice
{
    std::string key;
    level severity{};
    const char* text{};
    std::uint64_t serial{};
};

struct inputs
{
    diagnostics::readiness::status geometry{};
    diagnostics::readiness::status overlay{};
    diagnostics::readiness::status scripting{};
    bool previous_session_crashed{};
    bool input_hook_failed{};
    // How long the geometry has been loading; a short load is normal and shows nothing.
    double geometry_loading_seconds{};
};

inline constexpr double loading_notice_delay_seconds = 3.0;

// The notices that currently apply, most severe first. Pure function of the application's readiness state.
[[nodiscard]] inline std::vector<notice> collect(const inputs& current)
{
    using diagnostics::readiness::state;
    std::vector<notice> result;
    switch (current.geometry.value)
    {
        case state::waiting_dependency:
            result.push_back({"geometry", level::warning,
                              "Map geometry was not found for this map. Penetration and wall checks are off.",
                              current.geometry.serial});
            break;
        case state::error:
        case state::unsupported:
            result.push_back({"geometry", level::error,
                              "Map geometry failed to load. Penetration and wall checks are off.",
                              current.geometry.serial});
            break;
        case state::ready:
            if (current.geometry.cause == diagnostics::readiness::reason::geometry_partial)
                result.push_back({"geometry", level::warning,
                                  "Map geometry could not be read from the map files. A partial live copy is used, "
                                  "wall checks may be wrong.",
                                  current.geometry.serial});
            break;
        case state::loading:
            if (current.geometry_loading_seconds >= loading_notice_delay_seconds)
                result.push_back({"geometry", level::information, "Loading map geometry...", current.geometry.serial});
            break;
        default:
            break;
    }
    if (current.overlay.value == state::waiting_dependency)
        result.push_back(
            {"overlay", level::warning,
             "Overlay is in compatibility mode and may be hidden in true fullscreen. Set CS2 to Fullscreen Windowed.",
             1});
    if (current.scripting.value == state::error)
        result.push_back(
            {"scripting", level::warning,
             "Lua is unavailable: its data folder could not be prepared. Check free disk space, then restart Vesta.",
             1});
    if (current.input_hook_failed)
        result.push_back(
            {"input", level::warning,
             "Keyboard blocking could not start. Auto Stop and the grenade helper may not hold movement keys.", 1});
    if (current.previous_session_crashed)
        result.push_back({"crash", level::information,
                          "The previous session ended unexpectedly. Save a report in Misc > Configs > Diagnostics.",
                          1});
    std::stable_sort(result.begin(), result.end(),
                     [](const notice& a, const notice& b) { return a.severity > b.severity; });
    return result;
}

// Remembers which situations the player closed during this session.
class dismissals
{
public:
    [[nodiscard]] bool closed(const notice& item) const
    {
        for (const auto& entry : m_closed)
            if (entry.key == item.key && entry.serial == item.serial)
                return true;
        return false;
    }
    void close(const notice& item)
    {
        if (!closed(item))
            m_closed.push_back({item.key, item.serial});
    }
    // Drops the notices the player has closed.
    [[nodiscard]] std::vector<notice> visible(std::vector<notice> all) const
    {
        all.erase(std::remove_if(all.begin(), all.end(), [this](const notice& n) { return closed(n); }), all.end());
        return all;
    }

private:
    struct closed_entry
    {
        std::string key;
        std::uint64_t serial;
    };
    std::vector<closed_entry> m_closed;
};
} // namespace ui::notices
