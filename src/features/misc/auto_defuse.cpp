#include <stdafx.hpp>
#include <foundation/identity.hpp>
#include <config/settings.hpp>
#include <config/misc.hpp>
#include <platform/windows/input.hpp>
#include <game/state/services.hpp>
#include <game/memory/bound_services.hpp>
#include <game/input/bindings.hpp>
#include <render/presentation.hpp>
#include <features/misc/auto_defuse.hpp>
#include <algorithm>
#include <cmath>

namespace features::misc
{
namespace
{
constexpr std::int32_t k_team_ct = 3;
constexpr float k_defuse_seconds_kit = 5.0f;
constexpr float k_defuse_seconds_no_kit = 10.0f;
constexpr float k_time_margin = 0.30f;   // game time is sampled, not live
constexpr float k_assist_range = 110.0f; // only act when this close to the bomb
constexpr float k_use_range = 80.0f;     // never press use further out than this
constexpr float k_bomb_height = 6.0f;    // aim a bit above the model origin
constexpr float k_bomb_radius = 7.0f;    // how far off the centre the crosshair may be
constexpr float k_aim_gain = 0.5f;
constexpr float k_aim_max_step = 10.0f;
constexpr auto k_scan_interval = std::chrono::milliseconds(250);
constexpr auto k_aim_retry = std::chrono::milliseconds(12);
constexpr auto k_rad_to_deg = 57.29577951308232f;

[[nodiscard]] bool game_has_input_focus()
{
    const auto foreground = ::GetForegroundWindow();
    const auto root = foreground ? ::GetAncestor(foreground, GA_ROOT) : nullptr;
    DWORD process_id{};
    if (root)
        ::GetWindowThreadProcessId(root, &process_id);
    return process_id != 0 && process_id == game::bound_memory().process.process_id();
}
} // namespace

void auto_defuse_t::release()
{
    if (!m_use_down)
        return;
    if (platform::windows::bound_input().key(m_key, false))
        m_use_down = false;
}

void auto_defuse_t::tick()
{
    const auto settings = config::get_runtime_snapshot();
    const auto now = std::chrono::steady_clock::now();

    if (!settings || !settings->general.m_auto_defuse.enabled)
    {
        release();
        m_bomb = 0;
        return;
    }
    const auto& cfg = settings->general.m_auto_defuse;

    if (!game::local_player().alive() || game::local_player().team() != k_team_ct || render::menu_open() ||
        game::input_bindings().text_entry_active() || !game_has_input_focus())
    {
        release();
        m_bomb = 0;
        return;
    }

    const auto& process = game::bound_memory().process;
    auto& input = platform::windows::bound_input();

    // Look for the planted bomb a few times a second, it does not exist for most of the round.
    if (!m_bomb)
    {
        if (now < m_next_scan)
            return;
        m_next_scan = now + k_scan_interval;
        const auto entities = game::entity_index().all();
        if (!entities)
            return;
        for (const auto& entry : *entities)
        {
            if (entry.schema_id == "C_PlantedC4"_id &&
                process.load<bool>(entry.ptr + SCHEMA("C_PlantedC4", "m_bBombTicking"_id)))
            {
                m_bomb = entry.ptr;
                break;
            }
        }
        if (!m_bomb)
            return;
    }

    if (!process.load<bool>(m_bomb + SCHEMA("C_PlantedC4", "m_bBombTicking"_id)))
    {
        release();
        m_bomb = 0;
        return;
    }

    const auto node = process.load<std::uintptr_t>(m_bomb + SCHEMA("C_BaseEntity", "m_pGameSceneNode"_id));
    if (!node)
    {
        release();
        m_bomb = 0;
        return;
    }
    const auto bomb_origin = process.load<foundation::vec3>(node + SCHEMA("CGameSceneNode", "m_vecAbsOrigin"_id));
    const auto blow_time = process.load<float>(m_bomb + SCHEMA("C_PlantedC4", "m_flC4Blow"_id));
    const auto being_defused = process.load<bool>(m_bomb + SCHEMA("C_PlantedC4", "m_bBeingDefused"_id));
    const auto defuse_finish = process.load<float>(m_bomb + SCHEMA("C_PlantedC4", "m_flDefuseCountDown"_id));
    if (bomb_origin.length_sqr() < 1.0f || !std::isfinite(blow_time))
    {
        release();
        return;
    }

    // Local player state (kit, already defusing) and enemies that can see us.
    const auto players = game::world().players();
    if (!players)
        return;
    bool has_kit = false, local_defusing = false, enemy_visible = false;
    for (const auto& player : *players)
    {
        if (player.pawn == game::local_player().pawn())
        {
            has_kit = player.has_defuser;
            local_defusing = player.is_defusing;
        }
        else if (player.health > 0 && player.health <= 200 && game::local_player().is_enemy(player.team) &&
                 player.is_visible)
        {
            enemy_visible = true;
        }
    }

    // A teammate is already on it.
    if (being_defused && !local_defusing)
    {
        release();
        return;
    }

    // Can the defuse still finish before the bomb goes off?
    const auto game_time = game::local_player().game_time();
    const auto time_left = blow_time - game_time;
    const auto needed = local_defusing && defuse_finish > game_time
                            ? defuse_finish - game_time
                            : (has_kit ? k_defuse_seconds_kit : k_defuse_seconds_no_kit);
    if (time_left < needed + k_time_margin)
    {
        release();
        return;
    }

    // Do not walk into a fight to start the defuse. Once it runs, it keeps running.
    if (cfg.only_if_safe && enemy_visible && !local_defusing)
    {
        release();
        return;
    }

    const auto eye = game::camera().origin();
    const auto view = game::camera().angles();
    const auto target = bomb_origin + foundation::vec3{0.0f, 0.0f, k_bomb_height};
    const auto dx = target.x - eye.x;
    const auto dy = target.y - eye.y;
    const auto dz = target.z - eye.z;
    const auto horizontal = std::hypot(dx, dy);
    const auto distance = std::hypot(horizontal, dz);
    if (distance > k_assist_range || !std::isfinite(distance))
    {
        release();
        return;
    }

    // Source pitch is positive when looking down.
    const auto yaw_error = std::remainder(std::atan2(dy, dx) * k_rad_to_deg - view.y, 360.0f);
    const auto pitch_error = std::atan2(-dz, horizontal) * k_rad_to_deg - view.x;
    const auto error = std::hypot(yaw_error, pitch_error);
    const auto allowed =
        std::clamp(std::atan2(k_bomb_radius, std::max(distance, 1.0f)) * k_rad_to_deg, 1.5f, 12.0f);

    if (cfg.aim && error > allowed * 0.5f)
    {
        // One correction per camera frame, the camera value only changes per frame.
        const auto new_frame = view.y != m_last_yaw || view.x != m_last_pitch;
        m_last_yaw = view.y;
        m_last_pitch = view.x;
        if (new_frame || now - m_last_aim >= k_aim_retry)
        {
            constexpr auto m_yaw = 0.022f;
            const auto sensitivity = game::variables().get<float>(CONVAR("sensitivity"_id));
            const auto fov_adjust =
                process.load<float>(game::local_player().pawn() + SCHEMA("C_BasePlayerPawn", "m_flFOVSensitivityAdjust"_id));
            const auto degrees_per_pixel = sensitivity * m_yaw * fov_adjust;
            if (std::isfinite(degrees_per_pixel) && degrees_per_pixel > 0.0f)
            {
                const auto step_yaw = std::clamp(yaw_error * k_aim_gain, -k_aim_max_step, k_aim_max_step);
                const auto step_pitch = std::clamp(pitch_error * k_aim_gain, -k_aim_max_step, k_aim_max_step);
                // Mouse right lowers yaw, mouse down raises pitch. Sub-pixel remainder is carried.
                m_carry_x += -step_yaw / degrees_per_pixel;
                m_carry_y += step_pitch / degrees_per_pixel;
                const auto mouse_x = static_cast<int>(m_carry_x);
                const auto mouse_y = static_cast<int>(m_carry_y);
                m_carry_x -= static_cast<float>(mouse_x);
                m_carry_y -= static_cast<float>(mouse_y);
                m_last_aim = now;
                if (mouse_x != 0 || mouse_y != 0)
                    input.pointer(mouse_x, mouse_y, platform::windows::pointer_action::relative_move);
            }
        }
    }

    // Hold use only while the crosshair is on the bomb and it is within reach.
    const auto want_use = error <= allowed && distance <= k_use_range;
    if (want_use && !m_use_down)
    {
        const auto key = static_cast<std::uint16_t>(std::clamp(cfg.use_key, 1, 255));
        if (input.physical_key_down(key))
            return; // the player is already doing it
        if (input.key(key, true))
        {
            m_key = key;
            m_use_down = true;
        }
    }
    else if (!want_use)
    {
        release();
    }
}

} // namespace features::misc
