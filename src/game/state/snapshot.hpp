#pragma once

#include <atomic>
#include <memory>
#include <chrono>
#include <unordered_map>
#include <vector>

#include <game/memory/addresses.hpp>
#include <game/memory/catalogs.hpp>
#include <game/memory/memory_services.hpp>
#include <game/state/entities.hpp>
#include <cstdint>

namespace game
{

enum class script_data_demand : std::uint32_t
{
    none = 0,
    players = 1u << 0,
    items = 1u << 1,
    projectiles = 1u << 2,
    spectators = 1u << 3,
    bomb = 1u << 4,
    all = players | items | projectiles | spectators | bomb
};

[[nodiscard]] constexpr script_data_demand operator|(const script_data_demand left,
                                                     const script_data_demand right) noexcept
{
    return static_cast<script_data_demand>(static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right));
}

class world_sampler
{
public:
    world_sampler();
    ~world_sampler();
    world_sampler(const world_sampler&) = delete;
    world_sampler& operator=(const world_sampler&) = delete;

    // Connects the sampler to the process and addresses it reads; until then it samples nothing.
    void bind(const memory_services& services, const address_catalog& addresses) noexcept;
    // Whether nothing solid lies between two points; provided by the map geometry. Until it is set, every
    // visibility test fails (no player is reported visible).
    using line_of_sight_test = bool (*)(const foundation::vec3& from, const foundation::vec3& to);
    void set_line_of_sight(const line_of_sight_test test) noexcept
    {
        m_line_of_sight.store(test, std::memory_order_release);
    }
    // `menu_open`: the menu is a consumer of every player; it is the caller's state, not read from the UI here.
    void run(bool menu_open);
    [[nodiscard]] bool entity_directory_requested(bool menu_open) const;
    void set_script_demand(const script_data_demand demand) noexcept
    {
        m_script_demand.store(static_cast<std::uint32_t>(demand), std::memory_order_release);
    }
    [[nodiscard]] bool script_demand() const noexcept { return m_script_demand.load(std::memory_order_acquire) != 0; }
    [[nodiscard]] bool script_demand(const script_data_demand demand) const noexcept
    {
        return (m_script_demand.load(std::memory_order_acquire) & static_cast<std::uint32_t>(demand)) != 0;
    }

    [[nodiscard]] std::shared_ptr<const std::vector<player_snapshot>> players() const;
    [[nodiscard]] std::shared_ptr<const std::vector<world_item_snapshot>> items() const;
    [[nodiscard]] std::shared_ptr<const std::vector<projectile_snapshot>> projectiles() const;
    [[nodiscard]] std::shared_ptr<const std::vector<spectator_snapshot>> spectators() const;
    [[nodiscard]] bool local_spectated() const noexcept { return m_local_spectated.load(std::memory_order_acquire); }
    [[nodiscard]] std::vector<player_snapshot> seed_players(std::uintptr_t local_pawn, std::uintptr_t local_controller,
                                                            int local_team, bool free_for_all) const;
    void seed_players_into(std::vector<player_snapshot>& destination, std::uintptr_t local_pawn,
                           std::uintptr_t local_controller, int local_team, bool free_for_all,
                           std::uintptr_t only_pawn = 0, std::uintptr_t only_controller = 0) const;

private:
    // Name, model and weapon caches and the per-pass scratch lists, kept between passes of the game worker.
    struct caches;

    const memory_services* m_services{};
    const address_catalog* m_addresses{};
    std::atomic<line_of_sight_test> m_line_of_sight{};
    std::unique_ptr<caches> m_caches;

    [[nodiscard]] bool clear_line(const foundation::vec3& from, const foundation::vec3& to) const
    {
        const auto test = m_line_of_sight.load(std::memory_order_acquire);
        return test && test(from, to);
    }

    // Context shared by the steps of one player pass: settings snapshot, camera origin and pass time.
    struct player_pass;
    struct legit_fade_state;

    void collect_players(const std::vector<entity_directory::cached>& raw);
    [[nodiscard]] bool read_player(const player_pass& pass, const entity_directory::cached& entry, player_snapshot& p);
    void read_bones(const player_pass& pass, player_snapshot& p);
    void track_emit_sound(const player_pass& pass, const player_snapshot& p);
    void read_active_weapon(std::uintptr_t weapon_services, std::uint32_t pawn_handle,
                            equipped_weapon_snapshot& weapon);
    [[nodiscard]] std::vector<std::string> read_loadout(const player_pass& pass, std::uintptr_t weapon_services,
                                                        std::uint32_t pawn_handle);
    void update_legit_visibility(const player_pass& pass, std::vector<player_snapshot>& players,
                                 std::int32_t local_controller_index);
    [[nodiscard]] bool seen_by_teammate(const player_snapshot& enemy,
                                        const std::vector<player_snapshot>& players) const;
    [[nodiscard]] static float advance_legit_fade(legit_fade_state& fade, const player_pass& pass, bool direct_signal,
                                                  bool indirect_signal);
    void forget_departed_players();
    void collect_items(const std::vector<entity_directory::cached>& raw);
    void collect_projectiles(const std::vector<entity_directory::cached>& raw);
    void collect_spectators(const std::vector<entity_directory::cached>& raw);

    [[nodiscard]] static world_item_kind classify_item(std::uint32_t schema_id);
    [[nodiscard]] static projectile_kind classify_projectile(std::uint32_t schema_id);

    std::atomic<std::shared_ptr<const std::vector<player_snapshot>>> m_players{
        std::make_shared<std::vector<player_snapshot>>()};
    std::atomic<std::shared_ptr<const std::vector<world_item_snapshot>>> m_items{
        std::make_shared<std::vector<world_item_snapshot>>()};
    std::atomic<std::shared_ptr<const std::vector<projectile_snapshot>>> m_projectiles{
        std::make_shared<std::vector<projectile_snapshot>>()};
    std::atomic<std::shared_ptr<const std::vector<spectator_snapshot>>> m_spectators{
        std::make_shared<std::vector<spectator_snapshot>>()};
    std::atomic_bool m_local_spectated{};
    std::atomic<std::uint32_t> m_script_demand{};
    std::unordered_map<std::uintptr_t, float> m_last_emit_sound{};
    std::unordered_map<std::uintptr_t, std::chrono::steady_clock::time_point> m_last_radar_seen{};
    std::unordered_map<std::uintptr_t, std::uint64_t> m_last_spotted_mask{};
    std::unordered_map<std::uintptr_t, std::chrono::steady_clock::time_point> m_last_sound_heard{};
    struct legit_fade_state
    {
        float opacity{};
        std::uint8_t signal_mode{};
        std::chrono::steady_clock::time_point updated{};
        std::chrono::steady_clock::time_point pulse_started{};
    };
    std::unordered_map<std::uintptr_t, legit_fade_state> m_legit_fades{};
    struct cached_player_bones
    {
        std::uintptr_t bone_cache{};
        skeleton_reader::data bones{};
        std::chrono::steady_clock::time_point timestamp{};
    };
    std::unordered_map<std::uintptr_t, cached_player_bones> m_last_valid_bones{};
    // Hitbox geometry caches, one per reading thread: the game worker (run) and the seed reader
    // (seed_players_into, called only from the seed trigger thread).
    hitbox_catalog::geometry_cache m_hitbox_cache{};
    mutable hitbox_catalog::geometry_cache m_seed_hitbox_cache{};
};

} // namespace game
