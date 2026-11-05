#pragma once

#include <optional>

namespace app
{
// Debug builds answer `vesta --<report> <file>` with a live diagnostic report instead of starting the overlay:
// --compatibility-report, --seed-snapshot-report, --auto-accept-report. Returns the exit code, or nothing when the
// command line asks for no report.
[[nodiscard]] std::optional<int> run_diagnostic_report(int argc, char** argv);
} // namespace app
