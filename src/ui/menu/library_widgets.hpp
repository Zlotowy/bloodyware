#pragma once

#include <ui/menu/internal.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace ui::menu::detail
{
// Shared pieces of the config and script lists: a card that fills the page, tiles, labelled buttons and a menu of
// labelled actions, all in the menu palette.

enum class glyph : std::uint8_t
{
    none,
    import,
    folder,
    shield,
    more,
};

enum class tone : std::uint8_t
{
    normal,
    accent,
    danger,
};

void draw_glyph(ImDrawList* draw, glyph icon, ImVec2 center, ImU32 color);

// A card across both columns from below the taller column down to the bottom of the page. `actions` is drawn at the
// right end of the title row and is `actions_width` wide; the body gets the rest of the card.
void wide_card(const char* id, const char* title, callback_ref body, float actions_width, callback_ref actions);

// Width of a small_button with this label (English key) and icon.
[[nodiscard]] float small_button_width(const char* label, glyph icon);
// A compact button with an optional icon and a label (English key), 28 px high.
bool small_button(const char* id, glyph icon, const char* label, tone kind = tone::normal);

// Rounded button with a label (an English key, translated here).
bool pill_button(const char* id, const char* label, float width, tone kind = tone::normal, float height = 30.0f);

// Single-line input with a placeholder (English key). True when Enter was pressed.
bool hint_input(const char* id, const char* hint, char* buffer, std::size_t size, float width,
                ImGuiInputTextFlags flags = 0);

// One entry of an action menu. An entry with `confirm` turns into "Click again to confirm" on the first click and
// is chosen only on the second.
struct menu_action
{
    const char* label{};
    tone kind{tone::normal};
    bool confirm{};
};

// A "⋯" button that opens a list of labelled actions under it. Returns the index of the chosen action, or -1.
int action_menu(const char* id, std::span<const menu_action> actions);

// Background of one list tile; `active` marks the tile that is in use, `hover` is 0..1.
void tile_background(ImDrawList* draw, ImVec2 min, ImVec2 max, float hover, bool active);

// The result of an operation, shown for a moment where it happened: on the tile it concerns (`key`), or on the line
// under the name field when `key` is empty.
struct feedback
{
    std::string key{};
    const char* text{""}; // English key
    bool failed{};
    std::chrono::steady_clock::time_point at{};

    void raise(std::string target, const char* english, const bool failure)
    {
        key = std::move(target);
        text = english;
        failed = failure;
        at = std::chrono::steady_clock::now();
    }
    [[nodiscard]] bool visible_for(const std::string_view target) const
    {
        return key == target && std::chrono::steady_clock::now() - at < std::chrono::milliseconds(2500);
    }
    [[nodiscard]] ImVec4 color() const
    {
        return failed ? ImVec4{1.0f, 0.42f, 0.45f, 1.0f} : ImVec4{0.42f, 0.86f, 0.58f, 1.0f};
    }
};

// The Configs and Lua pages of the Misc tab.
void draw_config_library();
void draw_script_library();
} // namespace ui::menu::detail
