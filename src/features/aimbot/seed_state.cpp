#include <stdafx.hpp>
#include <game/state/services.hpp>
#include <foundation/identity.hpp>
#include <game/memory/bound_services.hpp>
#include <features/aimbot/seed_state.hpp>
#include <simulation/shot_model.hpp>
#include <simulation/punch_snapshot.hpp>

namespace features::aimbot
{
float seed_quantize_angle(float angle)
{
    return simulation::shot_model::quantize(angle);
}

std::optional<foundation::vec3> read_seed_punch(std::uintptr_t pawn)
{
    return simulation::read_cached_punch(game::bound_memory().process, pawn,
                                         SCHEMA("C_CSPlayerPawn", "m_pAimPunchServices"_id));
}
} // namespace features::aimbot
