#pragma once

#include <platform/windows/data_paths.hpp>

#include <filesystem>
#include <string_view>
#include <system_error>

namespace platform::windows::runtime_storage
{

// Initialize once on the bootstrap thread before any consumers/workers.
// Injected paths are explicit adapters for local tests, not runtime settings.
[[nodiscard]] data_storage::migration_result initialize();
[[nodiscard]] data_storage::migration_result initialize(data_storage::resolved_locations locations);
[[nodiscard]] const data_storage::locations& paths() noexcept;

} // namespace platform::windows::runtime_storage

namespace platform::windows
{
// Replaces `target` (creating its folder) with `contents` through a temporary sibling and an atomic rename, so a
// crash or a full disk leaves either the old file or the new one, never a truncated one. Empty on success.
[[nodiscard]] std::error_code write_file_atomically(const std::filesystem::path& target,
                                                    std::string_view contents) noexcept;
} // namespace platform::windows
