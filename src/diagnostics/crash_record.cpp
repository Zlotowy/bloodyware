#include <diagnostics/crash_record.hpp>
#include <diagnostics/log_directory.hpp>
#include <diagnostics/build_identity.hpp>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <shlobj.h>
#include <windows.h>

namespace diagnostics
{
namespace
{
constexpr const char* build_configuration = VESTA_JOURNAL_CONFIGURATION;
constexpr std::size_t path_capacity = 1024;
constexpr LONGLONG record_limit = 16 * 1024;

wchar_t g_path[path_capacity];
std::atomic_flag g_recording = ATOMIC_FLAG_INIT;
LPTOP_LEVEL_EXCEPTION_FILTER g_previous{};

// Hand-rolled formatting: no CRT locks or heap inside a possibly damaged process.
struct line_buffer
{
    char data[384];
    std::size_t size{};
    void text(const char* value) noexcept
    {
        while (*value && size < sizeof(data) - 1)
            data[size++] = *value++;
    }
    void hex(std::uint64_t value) noexcept
    {
        text("0x");
        char digits[16];
        int count = 0;
        do
        {
            digits[count++] = "0123456789ABCDEF"[value & 0xF];
            value >>= 4;
        } while (value && count < 16);
        while (count && size < sizeof(data) - 1)
            data[size++] = digits[--count];
    }
};

LONG WINAPI filter(EXCEPTION_POINTERS* info) noexcept
{
    if (!g_recording.test_and_set())
    {
        line_buffer line;
        line.text("crash version=" VESTA_JOURNAL_VERSION " build=" VESTA_JOURNAL_REVISION " config=");
        line.text(build_configuration);
        line.text(" code=");
        const auto* record = info ? info->ExceptionRecord : nullptr;
        line.hex(record ? record->ExceptionCode : 0);
        line.text(" module=");
        HMODULE module{};
        const auto address = record ? record->ExceptionAddress : nullptr;
        wchar_t name[MAX_PATH];
        DWORD length = 0;
        if (address &&
            ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                 static_cast<LPCWSTR>(address), &module))
            length = ::GetModuleFileNameW(module, name, MAX_PATH);
        if (length && length < MAX_PATH)
        {
            // Basename only, ASCII only: directories can contain the user name.
            DWORD start = length;
            while (start && name[start - 1] != L'\\' && name[start - 1] != L'/')
                --start;
            for (DWORD i = start; i < length && line.size < sizeof(line.data) - 32; ++i)
            {
                const char c = name[i] < 0x80 ? static_cast<char>(name[i]) : '?';
                line.data[line.size++] = c;
            }
            line.text(" offset=");
            line.hex(reinterpret_cast<std::uintptr_t>(address) - reinterpret_cast<std::uintptr_t>(module));
        }
        else
            line.text("unknown offset=0x0");
        line.text("\n");
        const HANDLE file =
            ::CreateFileW(g_path, FILE_APPEND_DATA | FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                          OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            LARGE_INTEGER size{};
            if (::GetFileSizeEx(file, &size) && size.QuadPart > record_limit)
            {
                // Keep the record bounded: the newest crash replaces older history.
                LARGE_INTEGER zero{};
                ::SetFilePointerEx(file, zero, nullptr, FILE_BEGIN);
                ::SetEndOfFile(file);
            }
            DWORD written{};
            ::WriteFile(file, line.data, static_cast<DWORD>(line.size), &written, nullptr);
            ::CloseHandle(file);
        }
    }
    return g_previous ? g_previous(info) : EXCEPTION_CONTINUE_SEARCH;
}
} // namespace

bool take_previous_crash(const std::filesystem::path& directory) noexcept
{
    try
    {
        auto target = directory;
        if (target.empty())
        {
            target = default_logs_directory();
            if (target.empty())
                return false;
        }
        std::error_code failure;
        const auto current = target / L"crash.log";
        const auto size = std::filesystem::file_size(current, failure);
        if (failure || size == 0)
            return false;
        return ::MoveFileExW(current.c_str(), (target / L"crash.previous.log").c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    }
    catch (...)
    {
        return false;
    }
}

bool install_crash_recorder(const std::filesystem::path& directory) noexcept
{
    try
    {
        auto target = directory;
        if (target.empty())
        {
            target = default_logs_directory();
            if (target.empty())
                return false;
        }
        std::error_code failure;
        std::filesystem::create_directories(target, failure);
        if (failure)
            return false;
        const auto file = (target / L"crash.log").native();
        if (file.size() >= path_capacity)
            return false;
        for (std::size_t i = 0; i < file.size(); ++i)
            g_path[i] = file[i];
        g_path[file.size()] = L'\0';
        g_previous = ::SetUnhandledExceptionFilter(filter);
        return true;
    }
    catch (...)
    {
        return false;
    }
}
} // namespace diagnostics
