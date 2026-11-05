#pragma once
#include <cstdint>
#include <diagnostics/log_directory.hpp>
#include <filesystem>
#include <string_view>

namespace diagnostics
{
struct report_result
{
    bool ok{};
    std::uint32_t error{};
    std::filesystem::path path;
};

[[nodiscard]] std::filesystem::path desktop_directory();

// Collects crash.log and the rotated journal files (oldest first) into one text file
// the user can attach to an issue. Runs only on an explicit user action; nothing is
// sent anywhere. Lines that look like file paths are withheld as a safety net, since
// the journal is anonymous by construction. `summary` is one short caller-supplied line
// of fixed fragments (for example current readiness). Missing logs are not an error.
[[nodiscard]] report_result export_report(const std::filesystem::path& logs_directory,
                                          const std::filesystem::path& destination_directory,
                                          std::string_view summary) noexcept;

inline constexpr std::string_view report_file_name = "vesta-report.txt";
} // namespace diagnostics
