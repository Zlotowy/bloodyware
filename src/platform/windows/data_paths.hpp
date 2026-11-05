#pragma once
#include <filesystem>
#include <cstdint>
#include <stop_token>

namespace platform::windows::data_storage
{
// Pure Windows path conversion; no I/O, current-directory lookup or policy edit.
// Absolute DOS/UNC paths use the extended namespace so native file operations
// work independently of MAX_PATH opt-in. Relative input stays relative/rejected
// by migration validation. Normalize separators/dot components before prefixing.
[[nodiscard]] std::filesystem::path extended_path(const std::filesystem::path& path);
// The inverse, for paths shown to the user or handed to Lua scripts and other programs: many tools (Windows
// PowerShell's Join-Path among them) do not accept the \\?\ prefix.
[[nodiscard]] std::filesystem::path plain_path(const std::filesystem::path& path);
struct locations
{
    std::filesystem::path root, configs, cache, logs, lua, mesh_cache, research_logs;
    [[nodiscard]] static locations under(std::filesystem::path root);
};
struct resolved_locations
{
    locations destination;
    // Optional explicit import source; empty selects lazy default preparation.
    std::filesystem::path legacy;
    std::uint32_t error{};
};
// Resolves the data root %TEMP%\vesta (the owner keeps cache and configuration temporary). The old flat
// layout in the same directory is imported in place by prepare(). Does not create files/directories.
[[nodiscard]] resolved_locations resolve_current_user() noexcept;

enum class migration_stage : std::uint8_t
{
    none,
    validate_paths,
    create_directories,
    lock,
    marker,
    enumerate,
    open_source,
    copy,
    verify,
    publish,
    cancelled
};
struct migration_result
{
    migration_stage stage{};
    std::uint32_t error{};
    std::uint64_t copied{}, retained{};
    bool already_complete{};
    [[nodiscard]] explicit operator bool() const noexcept { return error == 0; }
};
// Startup-only, caller-owned operation. Original files are never modified.
// Existing destination files win. Incomplete runs may be retried; no marker is
// published until every allowlisted file was handled. Cancellation is checked
// between directory entries and bounded 64KiB copy/verification chunks.
// Default preparation resolves legacy TEMP only if the v1 marker is absent.
// A completed destination remains independent of TEMP/legacy availability.
[[nodiscard]] migration_result prepare(const locations& destination, std::stop_token stop = {}) noexcept;
[[nodiscard]] migration_result migrate(const locations& destination, const std::filesystem::path& legacy,
                                       std::stop_token stop = {}) noexcept;
} // namespace platform::windows::data_storage
