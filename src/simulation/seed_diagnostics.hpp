#pragma once

#include <game/memory/memory_services.hpp>

#include <iosfwd>

namespace simulation::seed_diagnostics
{
// Writes the live seed snapshot report for a diagnostic run; 0 when the snapshot was complete.
int report(std::ostream& out, const game::attach_status& attached);
} // namespace simulation::seed_diagnostics
