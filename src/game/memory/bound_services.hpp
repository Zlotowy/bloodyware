#pragma once

#include <game/memory/addresses.hpp>
#include <game/memory/memory_services.hpp>

namespace game
{
// The memory services and addresses the application bound with bind_services, for modules made of free functions
// that have no object to hold them (collision, ballistics, penetration, ...). The accessors never return null:
// before binding they refer to an unattached process, an empty module list and a silent log, so a lookup simply
// finds nothing instead of crashing. Set once at startup by the owner of the process; read-only afterwards.
void bind_services(const memory_services& services, const address_catalog& addresses) noexcept;

[[nodiscard]] const memory_services& bound_memory() noexcept;
[[nodiscard]] const address_catalog& bound_addresses() noexcept;
} // namespace game
