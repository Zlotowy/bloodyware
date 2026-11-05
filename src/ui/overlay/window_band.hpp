#pragma once
#include <windows.h>

namespace ui::overlay_band
{
// Windows orders windows by z-band first and by TOPMOST only inside a band. An ordinary process may create a
// window in the SYSTEM_TOOLS band (16) on current Windows 10/11 builds, which keeps the overlay above a game that
// presents outside the desktop band, without elevation, UIAccess or any change to the token.
// The function behind it (user32!CreateWindowInBand) is undocumented, so every use is checked and has a fallback.
inline constexpr DWORD system_tools = 16;

struct created_window
{
    HWND window{};
    bool in_band{}; // true: Windows confirmed the window lives in the band
    DWORD error{};  // last error of the failed band attempt (0 if it succeeded or was not tried)
};

// Creates the overlay window in the band; on any failure creates the same window in the desktop band
// (WS_EX_TOPMOST is added only for the band window). Never throws; `window` is null only if both attempts fail.
[[nodiscard]] created_window create_window(DWORD ex_style, LPCWSTR window_class, LPCWSTR title, DWORD style, int x,
                                           int y, int width, int height, HINSTANCE instance) noexcept;
} // namespace ui::overlay_band
