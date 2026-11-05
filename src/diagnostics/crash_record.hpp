#pragma once
#include <filesystem>

namespace diagnostics
{
// Best-effort record of an unhandled exception: one short line in Logs/crash.log with
// the exception code, module name and module-relative offset. No stack walk, no
// symbolization, no heap use and no ordinary locks inside the handler. The exception
// is then passed on, so system crash reporting behaves as before. Nothing is sent.
//
// Call once from the process owner. An empty directory selects %TEMP%\vesta\logs.
// Returns false if the location could not be prepared; the process continues without it.
// Startup check: if a crash record exists, move it to crash.previous.log (replacing an older
// one) and return true, so one crash is reported once. Never throws.
[[nodiscard]] bool take_previous_crash(const std::filesystem::path& directory = {}) noexcept;

[[nodiscard]] bool install_crash_recorder(const std::filesystem::path& directory = {}) noexcept;
} // namespace diagnostics
