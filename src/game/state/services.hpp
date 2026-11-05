#pragma once

#include <game/memory/catalogs.hpp>
#include <game/memory/bound_services.hpp>
#include <game/memory/compatibility.hpp>
#include <game/state/runtime.hpp>
#include <game/state/pose.hpp>
#include <game/body/body.hpp>
#include <game/state/snapshot.hpp>
#include <cstdint>

namespace game
{

inline variable_registry& variables()
{
    static variable_registry value{};
    return value;
}
inline field_catalog& fields()
{
    static field_catalog value{};
    return value;
}
inline entity_directory& entity_index()
{
    static entity_directory value{};
    return value;
}
inline local_state& local_player()
{
    static local_state value{};
    return value;
}
inline camera_state& camera()
{
    static camera_state value{};
    return value;
}
inline player_pose_sampler& render_poses()
{
    static player_pose_sampler value{};
    return value;
}
inline skeleton_reader& skeletons()
{
    static skeleton_reader value{};
    return value;
}
inline bounds_projector& projection_bounds()
{
    static bounds_projector value{};
    return value;
}
inline hitbox_catalog& hitbox_data()
{
    static hitbox_catalog value{};
    return value;
}
inline world_sampler& world()
{
    static world_sampler value{};
    return value;
}

// Connects the adapters that read game memory to the process they read. Called once by whoever attached the
// process (the application, or a diagnostic tool) before the first lookup; until then they find nothing.
inline void bind_memory(const memory_services& services, const address_catalog& addresses)
{
    variables().bind(services);
    entity_index().bind(services, addresses);
    compatibility::bind(services);
    bind_services(services, addresses);
    local_player().bind(services, addresses);
    camera().bind(services, addresses);
    world().bind(services, addresses);
}

} // namespace game

#define SCHEMA(class_name, field_id)                                                                                   \
    []() -> std::int32_t                                                                                               \
    {                                                                                                                  \
        static const auto value = game::fields().lookup(class_name, field_id);                                         \
        return value;                                                                                                  \
    }()

// Console variables live as long as the game process, so a found address is kept per call site; a miss is
// looked up again on the next use.
#define CONVAR(name_id)                                                                                                \
    []() -> std::uintptr_t                                                                                             \
    {                                                                                                                  \
        static std::atomic<std::uintptr_t> cached{};                                                                   \
        auto value = cached.load(std::memory_order_relaxed);                                                           \
        if (!value)                                                                                                    \
        {                                                                                                              \
            value = game::variables().find(name_id);                                                                   \
            cached.store(value, std::memory_order_relaxed);                                                            \
        }                                                                                                              \
        return value;                                                                                                  \
    }()
