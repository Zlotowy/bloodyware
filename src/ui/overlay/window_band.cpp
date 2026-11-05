#include <ui/overlay/window_band.hpp>

namespace ui::overlay_band
{
namespace
{
using create_in_band_fn = HWND(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE,
                                        LPVOID, DWORD);
using get_window_band_fn = BOOL(WINAPI*)(HWND, PDWORD);
} // namespace

created_window create_window(DWORD ex_style, LPCWSTR window_class, LPCWSTR title, DWORD style, int x, int y, int width,
                             int height, HINSTANCE instance) noexcept
{
    created_window result;
    const HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
    const auto create =
        user32 ? reinterpret_cast<create_in_band_fn>(::GetProcAddress(user32, "CreateWindowInBand")) : nullptr;
    const auto get_band =
        user32 ? reinterpret_cast<get_window_band_fn>(::GetProcAddress(user32, "GetWindowBand")) : nullptr;
    if (create && get_band)
    {
        ::SetLastError(0);
        const HWND window = create(ex_style | WS_EX_TOPMOST, window_class, title, style, x, y, width, height, nullptr,
                                   nullptr, instance, nullptr, system_tools);
        if (window)
        {
            DWORD actual = ~0u;
            result.window = window;
            if (get_band(window, &actual) && actual == system_tools)
                result.in_band = true;
            else
                result.error =
                    ERROR_NOT_SUPPORTED; // accepted but placed elsewhere: usable as a topmost window, not as a band window
            return result;
        }
        else
            result.error = ::GetLastError();
    }
    else
        result.error = ERROR_PROC_NOT_FOUND;

    result.window = ::CreateWindowExW(ex_style, window_class, title, style, x, y, width, height, nullptr, nullptr,
                                      instance, nullptr);
    return result;
}
} // namespace ui::overlay_band
