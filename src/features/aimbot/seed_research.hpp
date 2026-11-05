#pragma once

// Seed trigger research telemetry: per-second counters, impact probes and a raw trace file in the research log
// folder. Compiled only into Debug builds configured with VESTA_SEED_LOG.
#if defined(VESTA_SEED_LOG) && VESTA_SEED_LOG
#include <diagnostics/log.hpp>
#include <foundation/math/vector.hpp>
#include <game/memory/bound_services.hpp>
#include <game/state/services.hpp>
#include <platform/windows/storage.hpp>
#include <simulation/ballistics.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <string>

namespace features::aimbot::research
{

struct seed_stats
{
    int scans{};           // ticks that reached the candidate evaluation
    int no_enemy{};        // acquire phase found nobody in view
    int window_skip{};     // outside the 60% scan window
    int pending_skip{};    // previous press still being consumed
    int held_skip{};       // button still held from the previous press
    int not_ready{};       // ctx.weapon_ready was false
    int snapshot_skip{};   // tick advanced while the snapshot was assembled
    int restricted_skip{}; // restricted mode: ordinary trace missed
    int hit_current{};     // hits_at_tick( current_tick )
    int hit_target{};      // hits_at_tick( target_tick )
    int hit_both{};        // both - the current fire condition
    int guard_tick{};      // final re-validation: tick or window moved
    int guard_angle{};     // final re-validation: quantized angle moved
    int guard_bucket{};    // refused near a quantization boundary
    int presses{};         // injected clicks
    int shots{};           // observed m_iShotsFired increments after a press
    int cmd_angles_ok{};   // usercmd ring read succeeded
    int cmd_angles_fail{}; // fell back to eye + punch
    int seq_ok{};          // command sequence drove the tick boundary
    int seq_fail{};        // fell back to the tickcount boundary
    int memo_hit{};        // command already decided, scan skipped
    int stale_aim{};       // aim moved within its bucket between scan and press
    int reaction_wait{};   // target not in view long enough yet
};


struct seed_impact_entry
{
    foundation::vec3 position{};
    float timestamp{};
    float expiry{};
};

struct seed_remote_vector
{
    std::int32_t size{};
    std::int32_t padding{};
    std::uintptr_t data{};
    std::int32_t capacity{};
    std::uint32_t flags{};
};

[[nodiscard]] inline bool read_impact_queue(std::uintptr_t pawn, int& size, std::uintptr_t& data)
{
    if (!pawn)
    {
        return false;
    }
    const auto services =
        game::bound_memory().process.load<std::uintptr_t>(pawn + SCHEMA("C_CSPlayerPawn", "m_pBulletServices"_id));
    if (!game::is_user_pointer(services))
    {
        return false;
    }
    const auto entries = game::bound_memory().process.load<seed_remote_vector>(services + 0x50);
    if (entries.size < 0 || entries.size > 1000000 || entries.capacity < entries.size || entries.capacity > 4000000 ||
        (entries.size && !game::is_user_pointer(entries.data)))
    {
        return false;
    }
    size = entries.size;
    data = entries.data;
    return true;
}


inline constexpr float g_scan_window_fraction = 0.60f;

// Everything needed to re-derive either candidate's bullet after the fact.
struct impact_probe
{
    bool armed{};
    std::chrono::steady_clock::time_point at{};
    int queue_baseline{};
    int tick{};
    // Same tick recovered from the command sequence instead of tickcount.
    int sequence_tick{};
    foundation::vec3 eye{};
    foundation::vec3 fwd{};
    foundation::vec3 right{};
    foundation::vec3 up{};
    foundation::vec3 angles{};
    float inaccuracy{};
    float spread{};
    float recoil{};
    int item_def{};
    int fire_mode{};
    int num_bullets{};
    float speed{};

    foundation::vec3 raw_eye{};
    foundation::vec3 velocity{};
    bool predictive{};
    // The target the seeded bullet was accepted against, so the poll can compare
    // the velocity we led it by against the displacement it actually made.
    std::uintptr_t target_pawn{};
    foundation::vec3 target_origin{};
    foundation::vec3 target_velocity{};
    std::uintptr_t target_scene_node{};
};

struct probe_shape
{
    double cosine_sum{};
    double ratio_sum{};
    int samples{};

    void add(double cosine, double ratio)
    {
        cosine_sum += cosine;
        ratio_sum += ratio;
        ++samples;
    }

    [[nodiscard]] double mean_cosine() const { return samples ? cosine_sum / samples : 0.0; }
    [[nodiscard]] double mean_ratio() const { return samples ? ratio_sum / samples : 0.0; }
};

inline probe_shape g_shape_still{};
inline probe_shape g_shape_moving{};

inline impact_probe g_probe{};

inline int g_verdict_lost{};

[[nodiscard]] inline foundation::vec2 probe_spread(const impact_probe& p, int tick)
{
    const auto seed = simulation::ballistics().derive_command_seed(p.angles, tick) + 1u;
    return simulation::ballistics().sample_spread_offset(static_cast<int>(seed), p.inaccuracy, p.spread, p.recoil,
                                                         p.item_def, p.fire_mode, p.num_bullets, 0);
}

struct probe_fit
{
    double numerator{};
    double denominator{};
    int samples{};

    void add(const foundation::vec2& observed, const foundation::vec2& predicted)
    {
        numerator += static_cast<double>(observed.x) * predicted.x + static_cast<double>(observed.y) * predicted.y;
        denominator += static_cast<double>(predicted.x) * predicted.x + static_cast<double>(predicted.y) * predicted.y;
        ++samples;
    }

    [[nodiscard]] double slope() const { return denominator > 1e-12 ? numerator / denominator : 0.0; }
};

inline probe_fit g_fit_a{};
inline probe_fit g_fit_b{};
inline probe_fit g_fit_control{};
// Candidate S: the tick recovered from the command sequence.
inline probe_fit g_fit_sequence{};

inline void impact_probe_poll(std::uintptr_t pawn, std::chrono::steady_clock::time_point now)
{
    if (!g_probe.armed)
    {
        return;
    }
    if (now - g_probe.at > std::chrono::milliseconds(700))
    {
        g_probe.armed = false;
        ++g_verdict_lost;
        return;
    }

    int size{};
    std::uintptr_t data{};
    if (!read_impact_queue(pawn, size, data) || size <= g_probe.queue_baseline)
    {
        return;
    }

    seed_impact_entry entry{};
    const auto address = data + static_cast<std::uintptr_t>(g_probe.queue_baseline) * sizeof(entry);
    if (!game::bound_memory().process.copy(address, &entry, sizeof(entry)))
    {
        g_probe.armed = false;
        ++g_verdict_lost;
        return;
    }
    g_probe.armed = false;

    const auto to_impact = entry.position - g_probe.eye;
    const auto distance = to_impact.length();
    if (!std::isfinite(distance) || distance < 1.0f)
    {
        ++g_verdict_lost;
        return;
    }
    const auto actual = to_impact * (1.0f / distance);

    // Short range gives the origin error too much angular leverage to be worth
    // including; it only adds variance.
    if (distance < 300.0f)
    {
        ++g_verdict_lost;
        return;
    }

    // Invert the model the ray is built with: dir ~ fwd + right*x + up*y.
    const auto along = actual.dot(g_probe.fwd);
    if (along < 0.1f)
    {
        ++g_verdict_lost;
        return;
    }
    const foundation::vec2 observed{actual.dot(g_probe.right) / along, actual.dot(g_probe.up) / along};

    const auto pred_a = probe_spread(g_probe, g_probe.tick);
    const auto pred_b = probe_spread(g_probe, g_probe.tick + 1);
    // Control: a tick neither candidate can be. Its slope is what pure noise
    // looks like for this data, so it calibrates the other two.
    const auto pred_control = probe_spread(g_probe, g_probe.tick + 37);
    const auto pred_sequence = probe_spread(g_probe, g_probe.sequence_tick);

    // A bullet that penetrated or hit a player can land far off its own ray;
    // such a sample says nothing about the seed and would dominate the fit.
    const auto observed_magnitude = std::sqrt(observed.x * observed.x + observed.y * observed.y);
    const auto predicted_magnitude = std::sqrt(pred_a.x * pred_a.x + pred_a.y * pred_a.y);
    if (observed_magnitude > std::max(0.02f, predicted_magnitude * 8.0f))
    {
        ++g_verdict_lost;
        return;
    }

    g_fit_a.add(observed, pred_a);
    g_fit_b.add(observed, pred_b);
    g_fit_control.add(observed, pred_control);
    g_fit_sequence.add(observed, pred_sequence);

    // Direction versus radius. A cone that points correctly but is drawn the
    // wrong size shows up as cosine near 1 with ratio away from 1.
    auto cosine = 0.0;
    auto ratio = 0.0;
    const auto pred_magnitude = std::sqrt(pred_a.x * pred_a.x + pred_a.y * pred_a.y);
    if (observed_magnitude > 1e-6f && pred_magnitude > 1e-6f)
    {
        cosine = (static_cast<double>(observed.x) * pred_a.x + static_cast<double>(observed.y) * pred_a.y) /
                 (static_cast<double>(observed_magnitude) * pred_magnitude);
        ratio = static_cast<double>(observed_magnitude) / pred_magnitude;
        // 5 u/s is the same threshold the trigger itself treats as standing.
        (g_probe.speed > 5.0f ? g_shape_moving : g_shape_still).add(cosine, ratio);
    }

    if (g_probe.target_scene_node)
    {
        const auto now_origin = game::bound_memory().process.load<foundation::vec3>(
            g_probe.target_scene_node + SCHEMA("CGameSceneNode", "m_vecAbsOrigin"_id));
        const auto elapsed = std::chrono::duration<float>(now - g_probe.at).count();
        if (std::isfinite(now_origin.x) && elapsed > 1e-4f)
        {
            const auto moved = now_origin - g_probe.target_origin;
            const auto measured = moved * (1.0f / elapsed);
            const auto measured_speed = measured.length_2d();
            const auto read_speed = g_probe.target_velocity.length_2d();
            game::bound_memory().log.info("[seed] target read={:.0f} measured={:.0f} ratio={:.2f} "
                                          "moved={:.1f}u over {:.1f}ms",
                                          read_speed, measured_speed,
                                          read_speed > 1.0f ? measured_speed / read_speed : 0.0f, moved.length_2d(),
                                          elapsed * 1000.0f);
        }
    }

    auto best_lead = -1;
    auto best_lead_cos = -2.0;
    for (int lead = 0; lead <= 3; ++lead)
    {
        const auto origin =
            g_probe.raw_eye + g_probe.velocity * (game::rules::simulation_step * static_cast<float>(lead));
        const auto delta = entry.position - origin;
        const auto delta_length = delta.length();
        if (!std::isfinite(delta_length) || delta_length < 1.0f)
        {
            continue;
        }
        const auto direction = delta * (1.0f / delta_length);
        const auto lead_along = direction.dot(g_probe.fwd);
        if (lead_along < 0.1f)
        {
            continue;
        }
        const foundation::vec2 lead_observed{direction.dot(g_probe.right) / lead_along,
                                             direction.dot(g_probe.up) / lead_along};
        const auto lead_magnitude = std::sqrt(lead_observed.x * lead_observed.x + lead_observed.y * lead_observed.y);
        if (lead_magnitude < 1e-6f || pred_magnitude < 1e-6f)
        {
            continue;
        }
        const auto lead_cos =
            (static_cast<double>(lead_observed.x) * pred_a.x + static_cast<double>(lead_observed.y) * pred_a.y) /
            (static_cast<double>(lead_magnitude) * pred_magnitude);
        if (lead_cos > best_lead_cos)
        {
            best_lead_cos = lead_cos;
            best_lead = lead;
        }
    }

    game::bound_memory().log.info("[seed] lead best={} cos={:.3f} (applied={}) speed={:.0f}", best_lead, best_lead_cos,
                                  g_probe.predictive ? 1 : 0, g_probe.speed);

    game::bound_memory().log.info("[seed] fit n={} dist={:.0f}u speed={:.0f} obs=({:+.5f},{:+.5f}) "
                                  "A=({:+.5f},{:+.5f}) | slope A={:.3f} S={:.3f} B={:.3f} ctl={:.3f} "
                                  "| cos={:.3f} ratio={:.3f} | still n={} cos={:.3f} ratio={:.3f} "
                                  "| moving n={} cos={:.3f} ratio={:.3f}",
                                  g_fit_a.samples, distance, g_probe.speed, observed.x, observed.y, pred_a.x, pred_a.y,
                                  g_fit_a.slope(), g_fit_sequence.slope(), g_fit_b.slope(), g_fit_control.slope(),
                                  cosine, ratio, g_shape_still.samples, g_shape_still.mean_cosine(),
                                  g_shape_still.mean_ratio(), g_shape_moving.samples, g_shape_moving.mean_cosine(),
                                  g_shape_moving.mean_ratio());
}

inline seed_stats g_seed_stats{};
inline std::chrono::steady_clock::time_point g_seed_stats_at{};

inline std::ofstream g_seed_file{};

[[nodiscard]] inline long long unix_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

inline void seed_file_write(const std::string& line)
{
    if (!g_seed_file.is_open())
    {
        const auto& directory = platform::windows::runtime_storage::paths().research_logs;
        if (directory.empty())
            return;
        g_seed_file.open(directory / L"seed_trace.log", std::ios::out | std::ios::app);
        if (g_seed_file.is_open())
        {
            g_seed_file << "# vesta seed trace; unix_ms is std::system_clock epoch\n";
        }
    }
    if (g_seed_file.is_open())
    {
        g_seed_file << line << '\n';
        g_seed_file.flush();
    }
}

// Set when a press is injected, consumed when the shot shows up.
inline int g_press_sequence{-1};
inline int g_press_tick{-1};
inline std::chrono::steady_clock::time_point g_press_at{};

inline void seed_stats_flush(std::chrono::steady_clock::time_point now)
{
    if (g_seed_stats_at == std::chrono::steady_clock::time_point{})
    {
        g_seed_stats_at = now;
        return;
    }
    if (now - g_seed_stats_at < std::chrono::seconds(1))
    {
        return;
    }
    g_seed_stats_at = now;

    const auto& s = g_seed_stats;
    // Only speak up when the trigger was actually trying to do something.
    if (s.scans || s.presses || s.no_enemy)
    {
        game::bound_memory().log.info(
            "[seed] scan={} | skip: enemy={} win={} pend={} held={} ready={} snap={} restr={} "
            "| hit: cur={} tgt={} both={} | guard: tick={} ang={} bucket={} "
            "| press={} shot={} | cmd: ok={} fail={} | seq: ok={} fail={} memo={} stale={} react={}",
            s.scans, s.no_enemy, s.window_skip, s.pending_skip, s.held_skip, s.not_ready, s.snapshot_skip,
            s.restricted_skip, s.hit_current, s.hit_target, s.hit_both, s.guard_tick, s.guard_angle, s.guard_bucket,
            s.presses, s.shots, s.cmd_angles_ok, s.cmd_angles_fail, s.seq_ok, s.seq_fail, s.memo_hit, s.stale_aim,
            s.reaction_wait);
    }
    g_seed_stats = {};
}
} // namespace features::aimbot::research

#define VESTA_SEED_COUNT(field) (++::features::aimbot::research::g_seed_stats.field)
#else
#define VESTA_SEED_COUNT(field) ((void)0)
#endif
