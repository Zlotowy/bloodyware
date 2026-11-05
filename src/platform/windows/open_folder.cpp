#include <platform/windows/open_folder.hpp>
#include <diagnostics/log.hpp>
#include <windows.h>

namespace platform::windows
{
std::wstring explorer_command_line(const std::wstring& windows_directory, const std::filesystem::path& folder)
{
    const std::wstring target = folder.native();
    if (windows_directory.empty() || target.empty())
        return {};
    for (const wchar_t c : windows_directory + target)
        if (c == L'"' || c < 0x20)
            return {};
    std::wstring directory = windows_directory;
    while (!directory.empty() && (directory.back() == L'\\' || directory.back() == L'/'))
        directory.pop_back();
    return L"\"" + directory + L"\\explorer.exe\" \"" + target + L"\"";
}

bool open_folder(const std::filesystem::path& folder) noexcept
{
    try
    {
        const DWORD attributes = ::GetFileAttributesW(folder.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY))
        {
            diagnostics::process_log().warning("[shell] cannot open {}: not a folder", folder.string());
            return false;
        }
        wchar_t windows[MAX_PATH]{};
        const UINT length = ::GetWindowsDirectoryW(windows, static_cast<UINT>(std::size(windows)));
        if (length == 0 || length >= std::size(windows))
        {
            diagnostics::process_log().warning("[shell] cannot locate the Windows folder (Win32 {})", ::GetLastError());
            return false;
        }
        auto command = explorer_command_line(windows, folder);
        if (command.empty())
        {
            diagnostics::process_log().warning("[shell] cannot open {}: the path cannot be quoted", folder.string());
            return false;
        }
        const std::wstring program = std::wstring(windows) + L"\\explorer.exe";
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        if (!::CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup,
                              &process))
        {
            diagnostics::process_log().warning("[shell] cannot start Explorer (Win32 {})", ::GetLastError());
            return false;
        }
        ::CloseHandle(process.hThread);
        ::CloseHandle(process.hProcess);
        return true;
    }
    catch (...)
    {
        diagnostics::process_log().warning("[shell] cannot open a folder: {}",
                                           diagnostics::current_exception_message());
        return false;
    }
}
} // namespace platform::windows
