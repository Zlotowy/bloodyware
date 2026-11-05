#pragma once

#include <game/memory/modules.hpp>
#include <platform/windows/process.hpp>
#include <diagnostics/log.hpp>

namespace game
{
// What the game-memory adapters (addresses, schema fields, console variables, entity directory) need, and nothing
// else: the process to read, the modules to search, and a place to report. The composition root creates it and the
// referenced objects must outlive every adapter bound to it. Adapters never reach for the application context.
// What a diagnostic entry point managed to set up before running a report against the live game.
struct attach_status
{
    bool process{};
    bool schema{};
    bool addresses{};
};

struct memory_services
{
    const platform::windows::process_session& process;
    const game::module_catalog& modules;
    diagnostics::logger& log;
};
} // namespace game
