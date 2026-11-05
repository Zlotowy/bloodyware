#include <stdafx.hpp>
#include <platform/windows/user_settings.hpp>

#include <shellapi.h>

namespace platform::windows
{
namespace
{
constexpr const wchar_t* k_key = L"Software\\Vesta";
constexpr const wchar_t* k_config_folder = L"ConfigFolder";
constexpr DWORD k_max_path_chars = 32767;
} // namespace

std::optional<std::filesystem::path> read_config_folder() noexcept
{
    try
    {
        DWORD bytes = 0;
        if (::RegGetValueW(HKEY_CURRENT_USER, k_key, k_config_folder, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) !=
                ERROR_SUCCESS ||
            bytes < sizeof(wchar_t) * 2 || bytes > (k_max_path_chars + 1) * sizeof(wchar_t))
            return std::nullopt;
        std::wstring value(bytes / sizeof(wchar_t), L'\0');
        if (::RegGetValueW(HKEY_CURRENT_USER, k_key, k_config_folder, RRF_RT_REG_SZ, nullptr, value.data(), &bytes) !=
            ERROR_SUCCESS)
            return std::nullopt;
        value.resize(::wcsnlen(value.c_str(), value.size()));
        std::filesystem::path folder{value};
        if (!folder.is_absolute())
            return std::nullopt;
        return folder;
    }
    catch (...)
    {
        return std::nullopt;
    }
}

bool write_config_folder(const std::filesystem::path& folder) noexcept
{
    try
    {
        if (!folder.is_absolute())
            return false;
        const auto& text = folder.native();
        if (text.size() > k_max_path_chars)
            return false;
        HKEY key{};
        if (::RegCreateKeyExW(HKEY_CURRENT_USER, k_key, 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr,
                              &key, nullptr) != ERROR_SUCCESS)
            return false;
        const auto result =
            ::RegSetValueExW(key, k_config_folder, 0, REG_SZ, reinterpret_cast<const BYTE*>(text.c_str()),
                             static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t)));
        ::RegCloseKey(key);
        return result == ERROR_SUCCESS;
    }
    catch (...)
    {
        return false;
    }
}

std::error_code move_to_recycle_bin(const std::filesystem::path& path) noexcept
{
    try
    {
        // SHFileOperation takes a list terminated by two NULs.
        std::wstring from = std::filesystem::absolute(path).native();
        from.push_back(L'\0');
        SHFILEOPSTRUCTW operation{};
        operation.wFunc = FO_DELETE;
        operation.pFrom = from.c_str();
        operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT | FOF_WANTNUKEWARNING;
        const auto result = ::SHFileOperationW(&operation);
        if (result != 0)
            return {result, std::system_category()};
        if (operation.fAnyOperationsAborted)
            return std::make_error_code(std::errc::operation_canceled);
        return {};
    }
    catch (...)
    {
        return std::make_error_code(std::errc::not_enough_memory);
    }
}
} // namespace platform::windows
