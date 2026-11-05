#pragma once
#include <filesystem>

namespace diagnostics
{
// %TEMP%\vesta\logs: the same folder the application's typed storage uses for logs. Only builds the path;
// it never touches the disk. Empty if the system cannot report a temporary directory.
[[nodiscard]] std::filesystem::path default_logs_directory();
} // namespace diagnostics
