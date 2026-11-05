#pragma once

namespace ui
{
class overlay_t;
class menu_t;

// The overlay and the menu the application created. Binding them also publishes them to the layers below as the
// render::presentation. Bound once at start-up by the composition root; before that they refer to an overlay that
// was never launched and a closed menu.
void bind_presentation(overlay_t& overlay, menu_t& menu) noexcept;
[[nodiscard]] overlay_t& bound_overlay() noexcept;
[[nodiscard]] menu_t& bound_menu() noexcept;
} // namespace ui
