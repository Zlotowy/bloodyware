#pragma once

#include <filesystem>
#include <optional>
#include <system_error>

namespace platform::windows
{
// The only machine-persistent setting Vesta keeps outside %TEMP%\vesta: the folder the user chose for named configs,
// as one string value under HKCU\Software\Vesta. Nothing else is written there.
[[nodiscard]] std::optional<std::filesystem::path> read_config_folder() noexcept;
[[nodiscard]] bool write_config_folder(const std::filesystem::path& folder) noexcept;

// Moves a file to the Recycle Bin, so a deleted config or script can be restored. Fails rather than deleting
// permanently.
[[nodiscard]] std::error_code move_to_recycle_bin(const std::filesystem::path& path) noexcept;
} // namespace platform::windows
