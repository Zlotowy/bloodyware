#pragma once

#include <imgui.h>

#include <windows.h>

namespace ui::imgui_frame
{
// What the overlay needs from a Win32 "new frame" and nothing more: the display size and the time step. The overlay
// window gets its input through Vesta's own routing, so the standard backend's per-frame mouse, keyboard-modifier,
// cursor and gamepad polling is not wanted. Kept here, outside ImGui, so the backend can stay unmodified upstream.
inline void begin(const float width, const float height) noexcept
{
    static LARGE_INTEGER frequency{}, previous{};
    if (frequency.QuadPart == 0)
    {
        ::QueryPerformanceFrequency(&frequency);
        ::QueryPerformanceCounter(&previous);
    }
    LARGE_INTEGER now{};
    ::QueryPerformanceCounter(&now);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(width, height);
    io.DeltaTime = static_cast<float>(now.QuadPart - previous.QuadPart) / static_cast<float>(frequency.QuadPart);
    previous = now;
}
} // namespace ui::imgui_frame
