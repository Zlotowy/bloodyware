#pragma once
#include <filesystem>
#include <string>

namespace platform::windows
{
// Command line that opens `folder` in Explorer: "<windows>\explorer.exe" "<folder>". Empty if the folder text cannot
// be quoted safely (it contains a quote or a control character) or an input is empty.
[[nodiscard]] std::wstring explorer_command_line(const std::wstring& windows_directory,
                                                 const std::filesystem::path& folder);

// Opens an existing folder in Explorer and nothing else: the program is fixed to the system's explorer.exe and the
// argument must be a directory. The generic shell-open handler stays out of the executable by design.
[[nodiscard]] bool open_folder(const std::filesystem::path& folder) noexcept;
} // namespace platform::windows
