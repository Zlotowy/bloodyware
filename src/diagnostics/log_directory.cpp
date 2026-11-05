#include <diagnostics/log_directory.hpp>
#include <windows.h>

namespace diagnostics
{
std::filesystem::path default_logs_directory()
{
    wchar_t buffer[MAX_PATH + 2]{};
    const DWORD length = ::GetTempPathW(static_cast<DWORD>(std::size(buffer)), buffer);
    if (length == 0 || length >= std::size(buffer))
        return {};
    return std::filesystem::path(buffer) / L"vesta" / L"logs";
}
} // namespace diagnostics
