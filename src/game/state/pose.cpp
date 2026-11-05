#include <stdafx.hpp>
#include <diagnostics/performance.hpp>
#include <game/state/services.hpp>

#include <game/state/pose.hpp>
#include <game/state/level.hpp>

namespace game
{

namespace
{
constexpr auto k_pose_fallback_lifetime = detail::pose_fallback_lifetime;

detail::pose_identity pose_context(const local_snapshot& local, const std::shared_ptr<const std::string>& level,
                                   std::uint64_t presentation)
{
    return {presentation,
            local.controller,
            local.pawn,
            local.alive ? local.pawn : local.observer_pawn,
            reinterpret_cast<std::uintptr_t>(level.get()),
            local.pawn_handle,
            local.view_team,
            local.team_mode};
}
} // namespace

void player_pose_sampler::set_presentation_state(const bool active, const std::uint32_t sample_rate)
{
    auto state = m_presentation.load(std::memory_order_acquire);
    const bool changed = static_cast<bool>(state & 1u) != active;
    if (changed)
    {
        const auto next = ((state + 2u) & ~std::uint64_t{1}) | std::uint64_t{active};
        m_presentation.store(next, std::memory_order_release);
        m_latest.store({}, std::memory_order_release);
        m_last_valid.clear();
        m_last_identity = {};
        m_last_level.reset();
        m_frames.clear();
        m_schedule.reset();
    }
    m_rate = sample_rate;
}

std::shared_ptr<const player_pose_frame> player_pose_sampler::acquire_for_presentation()
{
    // Single producer: only the render thread samples, after all frame pacing waits.
    if (!(m_presentation.load(std::memory_order_acquire) & 1u))
        return {};
    const auto now = std::chrono::steady_clock::now();
    m_schedule.configure(m_rate, now);
    if (m_schedule.due(now))
    {
        sample_once();
        m_schedule.sampled(std::chrono::steady_clock::now());
    }
    return latest();
}

std::shared_ptr<const player_pose_frame> player_pose_sampler::latest() const
{
    const auto presentation = m_presentation.load(std::memory_order_acquire);
    if (!(presentation & 1u))
        return {};
    const auto frame = m_latest.load(std::memory_order_acquire);
    const auto local = game::local_player().snapshot();
    const auto level = game::level().name();
    if (!frame || !local ||
        !detail::pose_frame_usable(frame->identity, pose_context(*local, level, presentation), frame->timestamp,
                                   std::chrono::steady_clock::now()) ||
        m_presentation.load(std::memory_order_acquire) != presentation)
        return {};
    return frame;
}

void player_pose_sampler::sample_once()
{
    VESTA_PERF_SCOPE(pose_sample);
    const auto now = std::chrono::steady_clock::now();
    const auto presentation = m_presentation.load(std::memory_order_acquire);
    const auto local = game::local_player().snapshot();
    const auto level = game::level().name();
    const auto identity = local ? pose_context(*local, level, presentation) : detail::pose_identity{};
    if (identity != m_last_identity)
    {
        m_last_valid.clear();
        m_latest.store({}, std::memory_order_release);
        m_last_identity = identity;
        m_last_level = level;
    }
    if (!detail::valid_pose_identity(identity))
        return;
    auto frame = m_frames.acquire();
    frame->players.clear();
    frame->camera = {};
    frame->identity = identity;
    frame->level = level;
    frame->sequence = m_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    frame->timestamp = now;
    frame->world = game::world().players();
    if (!frame->world)
    {
        return;
    }

    frame->players.reserve(frame->world->size());
    const auto local_pawn = identity.pawn;
    const auto view_pawn = identity.view_pawn;
    for (std::size_t index = 0; index < frame->world->size(); ++index)
    {
        const auto& player = (*frame->world)[index];
        if (!player.pawn || !player.bone_cache || player.health <= 0)
        {
            continue;
        }
        if (player.pawn == local_pawn || player.pawn == view_pawn ||
            (identity.team_mode && identity.view_team == player.team))
        {
            continue;
        }

        auto bones = game::skeletons().get(player.bone_cache);
        bool reused{};
        if (bones.is_valid())
        {
            m_last_valid[player.pawn] = {player.bone_cache, player.model_path, bones, now};
        }
        else
        {
            const auto cached = m_last_valid.find(player.pawn);
            if (cached == m_last_valid.end() || cached->second.bone_cache != player.bone_cache ||
                cached->second.model_path != player.model_path ||
                now - cached->second.timestamp > k_pose_fallback_lifetime)
            {
                continue;
            }
            bones = cached->second.bones;
            reused = true;
        }

        frame->players.push_back({index, player.pawn, player.bone_cache, player.model_path, bones, reused});
    }

    std::erase_if(m_last_valid,
                  [&](const auto& entry) { return now - entry.second.timestamp > k_pose_fallback_lifetime; });

    if (!game::camera().sample_presentation(frame->camera))
        return;
    const auto final_local = game::local_player().snapshot();
    const auto final_level = game::level().name();
    const auto final_presentation = m_presentation.load(std::memory_order_acquire);
    if (!final_local ||
        !detail::pose_frame_usable(identity, pose_context(*final_local, final_level, final_presentation), now,
                                   std::chrono::steady_clock::now()))
        return;
    m_latest.store(std::move(frame), std::memory_order_release);
}

} // namespace game
