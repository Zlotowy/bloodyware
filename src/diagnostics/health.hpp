#pragma once

#include <diagnostics/readiness.hpp>

#include <atomic>

namespace diagnostics
{
// The state of the subsystems the player can see in the menu and the notice plates. Each channel has one
// publisher (map loader, scripting start-up, overlay); the UI only reads.
struct subsystem_health
{
    readiness::channel geometry{};
    readiness::channel scripting{};
    // ready = band window; waiting_dependency = compatibility window.
    readiness::channel overlay{};
    // Set during bootstrap when the crash recorder found an unfinished previous session.
    std::atomic<bool> previous_session_crashed{};
};

[[nodiscard]] inline subsystem_health& health() noexcept
{
    static subsystem_health value{};
    return value;
}
} // namespace diagnostics
