#pragma once

#include <platform/windows/input.hpp>
#include <game/memory/addresses.hpp>
#include <game/memory/memory_services.hpp>
#include <game/memory/modules.hpp>
#include <platform/windows/process.hpp>
#include <ui/menu/menu.hpp>
#include <ui/overlay/overlay.hpp>
#include <diagnostics/log.hpp>

namespace app
{

struct context_t
{
    diagnostics::logger diagnostics{};
    platform::windows::input_gateway input{};
    platform::windows::process_session process{};
    game::module_catalog modules{};
    game::address_catalog addresses{};
    // What the game-memory adapters need, bound to the objects above; they never reach for this context.
    game::memory_services memory{process, modules, diagnostics};
    ui::overlay_t overlay{};
    ui::menu_t menu{};
};

inline context_t& context()
{
    static context_t instance{};
    return instance;
}

} // namespace app
