#include <stdafx.hpp>
#include <foundation/identity.hpp>
#include <game/state/services.hpp>
#include <game/state/entities.hpp>
#include <game/state/snapshot.hpp>
#include <diagnostics/performance.hpp>
#include <diagnostics/journal.hpp>

namespace game
{
void world_sampler::seed_players_into(std::vector<player_snapshot>& fresh, std::uintptr_t local_pawn,
                                      std::uintptr_t local_controller, int local_team, bool free_for_all,
                                      std::uintptr_t only_pawn, std::uintptr_t only_controller) const
{
    if (!m_services || !m_addresses)
        return;
    VESTA_PERF_SCOPE(seed_targets_read);
    // This is the standalone-equivalent reader: it owns every target input used by
    // seed evaluation and never consumes/publishes the normal ESP snapshot.
    static const auto controller_pawn = SCHEMA("CCSPlayerController", "m_hPlayerPawn"_id);
    static const auto controller_helmet = SCHEMA("CCSPlayerController", "m_bPawnHasHelmet"_id);
    static const auto pawn_health = SCHEMA("C_BaseEntity", "m_iHealth"_id);
    static const auto pawn_team = SCHEMA("C_BaseEntity", "m_iTeamNum"_id);
    static const auto pawn_scene = SCHEMA("C_BaseEntity", "m_pGameSceneNode"_id);
    static const auto pawn_immunity = SCHEMA("C_CSPlayerPawn", "m_bGunGameImmunity"_id);
    static const auto pawn_armor = SCHEMA("C_CSPlayerPawn", "m_ArmorValue"_id);
    static const auto scene_origin = SCHEMA("CGameSceneNode", "m_vecAbsOrigin"_id);
    static const auto scene_model_state = SCHEMA("CSkeletonInstance", "m_modelState"_id);

    fresh.clear();
    if (fresh.capacity() < 64)
        fresh.reserve(64);

    std::array<std::uintptr_t, 65> controllers{};
    if (only_pawn && only_controller)
        controllers[1] = only_controller;
    else if (!game::entity_index().player_controller_slots(controllers))
        return;
    for (std::uint32_t index = 1; index <= 64; ++index)
    {
        const auto controller = controllers[index];
        if (!controller || controller == local_controller)
        {
            continue;
        }

        const auto pawn_handle = m_services->process.load<std::uint32_t>(controller + controller_pawn);
        const auto pawn = game::entity_index().lookup(pawn_handle);
        if (!pawn || pawn == local_pawn)
        {
            continue;
        }
        if (only_pawn && pawn != only_pawn)
        {
            continue;
        }
        player_snapshot value{};
        value.controller = controller;
        value.pawn = pawn;
        value.health = m_services->process.load<std::int32_t>(pawn + pawn_health);
        value.team = m_services->process.load<std::int32_t>(pawn + pawn_team);
        value.invulnerable = m_services->process.load<bool>(pawn + pawn_immunity);
        if (value.health <= 0 || value.health > 100 || (value.team != 2 && value.team != 3) ||
            (!free_for_all && value.team == local_team) || value.invulnerable)
        {
            continue;
        }

        value.armor = m_services->process.load<std::int32_t>(pawn + pawn_armor);
        value.game_scene_node = m_services->process.load<std::uintptr_t>(pawn + pawn_scene);
        if (!value.game_scene_node)
        {
            continue;
        }

        value.origin = m_services->process.load<foundation::vec3>(value.game_scene_node + scene_origin);
        value.velocity =
            m_services->process.load<foundation::vec3>(pawn + SCHEMA("C_BaseEntity", "m_vecAbsVelocity"_id));
        value.simulation_tick =
            m_services->process.load<std::int32_t>(pawn + SCHEMA("C_BaseEntity", "m_nSimulationTick"_id));
        value.simulation_time = m_services->process.load<float>(pawn + SCHEMA("C_BaseEntity", "m_flSimulationTime"_id));
        if (!std::isfinite(value.origin.x) || !std::isfinite(value.origin.y) || !std::isfinite(value.origin.z))
        {
            continue;
        }

        value.bone_cache = m_services->process.load<std::uintptr_t>(value.game_scene_node + scene_model_state + 0x80);
        if (!value.bone_cache)
        {
            continue;
        }
        value.bones = game::skeletons().get(value.bone_cache);
        if (!value.bones.is_valid())
        {
            // code 1: no usable skeleton (journaled with a repeat count, never silent)
            diagnostics::emit(diagnostics::event::player_skipped, diagnostics::severity::warning,
                              diagnostics::code_domain::mode, 1u);
            continue;
        }

        value.has_helmet = m_services->process.load<bool>(controller + controller_helmet);
        // The model handle guards immutable hitbox geometry; bones remain freshly read.
        value.hitboxes = game::hitbox_data().query(value.game_scene_node, &m_seed_hitbox_cache);
        // A hitbox on a bone that was not read would sit at the origin: never plan a shot against it.
        if (std::ranges::any_of(value.hitboxes,
                                [&value](const auto& box) { return box.bone >= 0 && !value.bones.covers(box.bone); }))
        {
            diagnostics::emit(diagnostics::event::player_skipped, diagnostics::severity::warning,
                              diagnostics::code_domain::mode, 3u);
            continue;
        }
        if (value.hitboxes.count < 3)
        {
            // code 2: the model exposes fewer than 3 hitboxes
            diagnostics::emit(diagnostics::event::player_skipped, diagnostics::severity::warning,
                              diagnostics::code_domain::mode, 2u);
            continue;
        }
        fresh.push_back(std::move(value));
        if (only_pawn)
            break;
    }
}

} // namespace game
