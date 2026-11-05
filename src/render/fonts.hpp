#pragma once

#include <render/draw.hpp>

namespace render
{
// The fonts the overlay loads once its device exists; null until then.
struct font_set
{
    draw::font* weapons_15{};
    draw::font* weapons_esp_15{};
    draw::font* notosans_medium_12{};
    draw::font* esp_text_11{};
    draw::font* menu_regular_12{};
    draw::font* menu_semibold_13{};
    draw::font* menu_brand_30{};
};
} // namespace render
