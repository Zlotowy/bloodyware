#pragma once

#include <render/fonts.hpp>

#include <windows.h>

struct ID3D11Device;

namespace render
{
// What the user interface provides to the layers below it (features, scripting) without them depending on it:
// its fonts, whether the player is using the menu, whether combat input is routed, its window and its device.
// The UI implements it; the composition root binds the implementation once at start-up.
class presentation
{
public:
    virtual ~presentation() = default;

    [[nodiscard]] virtual const font_set& fonts() const noexcept = 0;
    // The menu or the first-run guide has keyboard and mouse focus.
    [[nodiscard]] virtual bool menu_open() const noexcept = 0;
    // The overlay routes combat input (its window and input hooks are live).
    [[nodiscard]] virtual bool combat_input_ready() const noexcept = 0;
    // The overlay window, null before it exists.
    [[nodiscard]] virtual HWND window() const noexcept = 0;
    // The overlay's Direct3D device, null before it exists.
    [[nodiscard]] virtual ID3D11Device* device() const noexcept = 0;
};

// Before binding, a presentation without fonts, window or device whose menu is closed.
void bind_presentation(const presentation& implementation) noexcept;
[[nodiscard]] const presentation& bound_presentation() noexcept;

[[nodiscard]] inline const font_set& fonts() noexcept
{
    return bound_presentation().fonts();
}
[[nodiscard]] inline bool menu_open() noexcept
{
    return bound_presentation().menu_open();
}
[[nodiscard]] inline bool combat_input_ready() noexcept
{
    return bound_presentation().combat_input_ready();
}
[[nodiscard]] inline HWND overlay_window() noexcept
{
    return bound_presentation().window();
}
} // namespace render
