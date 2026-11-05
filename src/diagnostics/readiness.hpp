#pragma once

#include <cstdint>
#include <mutex>
#include <string_view>

namespace diagnostics::readiness
{
// One publisher per subsystem instance. UI and journal read the same snapshot,
// so a reason shown to the user is the reason written to the diagnostics log.
enum class state : std::uint8_t
{
    not_started,
    loading,
    ready,
    waiting_dependency,
    unsupported,
    error,
    stopping
};
enum class reason : std::uint8_t
{
    none,
    no_map,
    geometry_building,
    geometry_missing,
    geometry_failed,
    scripting_storage_failed,
    overlay_band_unavailable,
    shutdown,
    geometry_partial
};

struct status
{
    state value{state::not_started};
    reason cause{reason::none};
    // Serial of the request that produced this state; 0 before any request.
    std::uint64_t serial{};
};

[[nodiscard]] constexpr std::string_view name(state value) noexcept
{
    switch (value)
    {
        case state::not_started:
            return "not_started";
        case state::loading:
            return "loading";
        case state::ready:
            return "ready";
        case state::waiting_dependency:
            return "waiting_dependency";
        case state::unsupported:
            return "unsupported";
        case state::error:
            return "error";
        case state::stopping:
            return "stopping";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view name(reason value) noexcept
{
    switch (value)
    {
        case reason::none:
            return "none";
        case reason::no_map:
            return "no_map";
        case reason::geometry_building:
            return "geometry_building";
        case reason::geometry_missing:
            return "geometry_missing";
        case reason::geometry_failed:
            return "geometry_failed";
        case reason::scripting_storage_failed:
            return "scripting_storage_failed";
        case reason::overlay_band_unavailable:
            return "overlay_band_unavailable";
        case reason::shutdown:
            return "shutdown";
        case reason::geometry_partial:
            return "geometry_partial";
    }
    return "unknown";
}

// Transitions are rare (map change, load completion), so a mutex is acceptable.
class channel
{
public:
    // A result from an older serial never overwrites a newer state.
    bool publish(std::uint64_t serial, state value, reason cause = reason::none)
    {
        std::lock_guard lock(m_mutex);
        if (serial < m_current.serial)
            return false;
        m_current = {value, cause, serial};
        return true;
    }

    // A new publisher (e.g. a restarted session) starts its own serial sequence.
    void reset()
    {
        std::lock_guard lock(m_mutex);
        m_current = {};
    }

    // Shutdown ends every serial and must not be lost to a stale-result check.
    void stop()
    {
        std::lock_guard lock(m_mutex);
        m_current.value = state::stopping;
        m_current.cause = reason::shutdown;
    }

    [[nodiscard]] status snapshot() const
    {
        std::lock_guard lock(m_mutex);
        return m_current;
    }

private:
    mutable std::mutex m_mutex;
    status m_current{};
};
} // namespace diagnostics::readiness
