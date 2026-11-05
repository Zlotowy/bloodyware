#pragma once

#include <game/memory/memory_services.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace game::compatibility
{

struct radar_layout
{
    std::uint32_t origin{};
    std::uint32_t alternate_scale{};
};

struct panel_layout
{
    std::uint32_t width{}, height{}, x{}, y{}, visible{};
    [[nodiscard]] explicit operator bool() const { return width && height && x && y && visible; }
};

// Connects the signature probes to the process they scan; until then every probe reports "unavailable".
void bind(const memory_services& services) noexcept;

[[nodiscard]] std::uint32_t shot_punch_offset();
[[nodiscard]] std::uintptr_t spread_patterns();
[[nodiscard]] std::optional<radar_layout> radar();
[[nodiscard]] panel_layout panel(std::uintptr_t ui);
[[nodiscard]] std::uintptr_t hud_element(std::string_view name);

} // namespace game::compatibility
