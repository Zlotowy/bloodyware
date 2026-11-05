#include <stdafx.hpp>
#include <features/misc/console_input.hpp>
#include <platform/windows/input.hpp>
#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <string>

namespace features::misc
{
namespace
{
constexpr std::size_t k_batch_size = 8; // input_gateway::keys limit

struct typed_key
{
    std::uint16_t virtual_key{};
    bool shift{};
};

[[nodiscard]] bool map_char(const char c, typed_key& out)
{
    if (c >= 'a' && c <= 'z')
    {
        out = {static_cast<std::uint16_t>(c - 'a' + 'A'), false};
        return true;
    }
    if (c >= 'A' && c <= 'Z')
    {
        out = {static_cast<std::uint16_t>(c), true};
        return true;
    }
    if (c >= '0' && c <= '9')
    {
        out = {static_cast<std::uint16_t>(c), false};
        return true;
    }
    switch (c)
    {
    case ' ': out = {VK_SPACE, false}; return true;
    case '.': out = {VK_OEM_PERIOD, false}; return true;
    case ',': out = {VK_OEM_COMMA, false}; return true;
    case '-': out = {VK_OEM_MINUS, false}; return true;
    case '_': out = {VK_OEM_MINUS, true}; return true;
    case '=': out = {VK_OEM_PLUS, false}; return true;
    case '+': out = {VK_OEM_PLUS, true}; return true;
    case '/': out = {VK_OEM_2, false}; return true;
    case '?': out = {VK_OEM_2, true}; return true;
    case '\'': out = {VK_OEM_7, false}; return true;
    case ':': out = {VK_OEM_1, true}; return true;
    case '[': out = {VK_OEM_4, false}; return true;
    case ']': out = {VK_OEM_6, false}; return true;
    case '<': out = {VK_OEM_COMMA, true}; return true;
    case '>': out = {VK_OEM_PERIOD, true}; return true;
    case '!': out = {'1', true}; return true;
    case '@': out = {'2', true}; return true;
    case '#': out = {'3', true}; return true;
    case '$': out = {'4', true}; return true;
    case '%': out = {'5', true}; return true;
    case '^': out = {'6', true}; return true;
    case '&': out = {'7', true}; return true;
    case '*': out = {'8', true}; return true;
    case '(': out = {'9', true}; return true;
    case ')': out = {'0', true}; return true;
    default: return false;
    }
}

// ---- clipboard paste ------------------------------------------------------------------------------------------
[[nodiscard]] std::wstring widen(const std::string& text)
{
    if (text.empty())
        return {};
    const auto size =
        ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0)
        return {};
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
    return wide;
}

// SetClipboardData needs a real owner window, OpenClipboard(nullptr) makes it fail.
[[nodiscard]] HWND clipboard_window()
{
    static const HWND window = ::CreateWindowExW(0, L"STATIC", L"vesta_clipboard", 0, 0, 0, 0, 0, HWND_MESSAGE,
                                                 nullptr, ::GetModuleHandleW(nullptr), nullptr);
    return window;
}

[[nodiscard]] bool open_clipboard()
{
    const auto owner = clipboard_window();
    if (!owner)
        return false;
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        if (::OpenClipboard(owner))
            return true;
        ::Sleep(2); // another program is holding it for a moment
    }
    return false;
}

// Clipboard must be open and emptied by the caller.
[[nodiscard]] bool store_text(const std::wstring& text)
{
    const auto bytes = (text.size() + 1) * sizeof(wchar_t);
    const auto memory = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory)
        return false;
    auto* destination = static_cast<wchar_t*>(::GlobalLock(memory));
    if (!destination)
    {
        ::GlobalFree(memory);
        return false;
    }
    std::memcpy(destination, text.c_str(), bytes);
    ::GlobalUnlock(memory);
    if (!::SetClipboardData(CF_UNICODETEXT, memory))
    {
        ::GlobalFree(memory);
        return false;
    }
    return true;
}

struct clipboard_backup
{
    bool had_text{};
    std::wstring text;
};

// Puts the text on the clipboard and remembers what was there (text only).
[[nodiscard]] bool put_clipboard_text(const std::wstring& text, clipboard_backup& backup)
{
    if (!open_clipboard())
        return false;
    if (::IsClipboardFormatAvailable(CF_UNICODETEXT))
    {
        if (const auto handle = ::GetClipboardData(CF_UNICODETEXT))
        {
            if (const auto* data = static_cast<const wchar_t*>(::GlobalLock(handle)))
            {
                backup.had_text = true;
                backup.text = data;
                ::GlobalUnlock(handle);
            }
        }
    }
    const auto ok = ::EmptyClipboard() && store_text(text);
    ::CloseClipboard();
    return ok;
}

void restore_clipboard(const clipboard_backup& backup)
{
    if (!open_clipboard())
        return;
    if (::EmptyClipboard() && backup.had_text)
        static_cast<void>(store_text(backup.text));
    ::CloseClipboard();
}

[[nodiscard]] std::string strip_command_breakers(const std::string& line)
{
    std::string clean;
    clean.reserve(line.size());
    for (const char c : line)
        if (c != ';' && c != '"' && static_cast<unsigned char>(c) >= 32 && c != 127)
            clean.push_back(c);
    return clean;
}

// Collects key transitions and flushes them to the gateway in groups of at most 8.
class key_batch
{
public:
    void add(const std::uint16_t key, const bool pressed)
    {
        if (m_count == k_batch_size)
            flush();
        m_items[m_count++] = {key, pressed};
    }
    // A key tap never gets split across two batches.
    void tap(const typed_key key)
    {
        const auto needed = key.shift ? std::size_t{4} : std::size_t{2};
        if (m_count + needed > k_batch_size)
            flush();
        if (key.shift)
            add(VK_SHIFT, true);
        add(key.virtual_key, true);
        add(key.virtual_key, false);
        if (key.shift)
            add(VK_SHIFT, false);
    }
    void flush()
    {
        if (!m_count)
            return;
        platform::windows::bound_input().keys(
            std::span<const platform::windows::input_gateway::key_transition>(m_items.data(), m_count));
        m_count = 0;
    }

private:
    std::array<platform::windows::input_gateway::key_transition, k_batch_size> m_items{};
    std::size_t m_count{};
};
} // namespace

void run_console_command(const std::string& line, const int console_key, const int delay_ms, const bool paste)
{
    const auto toggle_key = static_cast<std::uint16_t>(std::clamp(console_key, 1, 255));
    const auto wait = static_cast<DWORD>(std::clamp(delay_ms, 0, 500));
    const auto clean = strip_command_breakers(line);

    clipboard_backup backup;
    const auto pasted = paste && !clean.empty() && put_clipboard_text(widen(clean), backup);

    key_batch batch;
    batch.tap({toggle_key, false}); // open console
    batch.flush();
    ::Sleep(wait);

    if (pasted)
    {
        // The whole line in one go.
        batch.add(VK_LCONTROL, true);
        batch.add('V', true);
        batch.add('V', false);
        batch.add(VK_LCONTROL, false);
        batch.flush();
        ::Sleep(8);
    }
    else
    {
        for (const char c : clean)
        {
            typed_key key{};
            if (map_char(c, key))
                batch.tap(key);
        }
    }
    batch.tap({VK_RETURN, false});
    batch.flush();

    // The game handles input once per frame: closing too soon toggles the console before the command was executed.
    // Never wait less than 30 ms here.
    ::Sleep(std::max<DWORD>(wait, 30));

    batch.tap({toggle_key, false}); // close console
    batch.flush();

    // The paste is long consumed by now, give the user their clipboard back.
    if (pasted)
        restore_clipboard(backup);
}

} // namespace features::misc
