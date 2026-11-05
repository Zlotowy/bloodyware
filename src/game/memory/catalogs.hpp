#pragma once

#include <game/memory/addresses.hpp>
#include <game/memory/memory_services.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace game
{

class variable_registry
{
public:
    // Binds the adapter to the process it reads; until then every lookup finds nothing.
    void bind(const memory_services& services) noexcept { m_services = &services; }
    // Address of the console variable with this name identity, 0 if it does not exist or the list is unreadable.
    [[nodiscard]] std::uintptr_t find(std::uint32_t name_id);

    template<typename T>
    [[nodiscard]] T get(std::uintptr_t cvar_ptr);
    template<typename T>
    [[nodiscard]] bool try_get(std::uintptr_t cvar_ptr, T& value);

private:
    using clock = std::chrono::steady_clock;
    // How long to wait before reading the console variable list again after it could not be found.
    static constexpr std::chrono::seconds k_retry_interval{5};

    [[nodiscard]] std::unordered_map<std::uint32_t, std::uintptr_t> read_list() const;

    const memory_services* m_services{};
    std::mutex m_mutex{};
    std::unordered_map<std::uint32_t, std::uintptr_t> m_variables{};
    std::optional<clock::time_point> m_last_attempt{};
};

class field_catalog
{
public:
    [[nodiscard]] bool initialize(const memory_services& services);
    [[nodiscard]] std::int32_t lookup(const char* class_name, std::uint32_t field_id);
    [[nodiscard]] std::size_t field_count() const noexcept { return m_entries.size(); }

private:
    struct field_entry
    {
        std::uint64_t key{};
        std::int32_t offset{};

        [[nodiscard]] bool operator<(const field_entry& other) const noexcept { return key < other.key; }
    };

    std::vector<field_entry> m_entries{};
    // Set by initialize; used to report fields that are looked up but missing, once per field.
    diagnostics::logger* m_log{};
    std::mutex m_missing_mutex{};
    std::unordered_set<std::uint64_t> m_missing{};
};

class entity_directory
{
public:
    enum class type : std::uint8_t
    {
        unknown,
        player,
        item,
        projectile,
        impact
    };

    struct cached
    {
        std::uintptr_t ptr{};
        std::uint32_t schema_id{};
        std::int32_t index{};
        type type{type::unknown};
    };

    // The directory reads entities through `services` and finds the list through `addresses`.
    void bind(const memory_services& services, const address_catalog& addresses) noexcept
    {
        m_services = &services;
        m_addresses = &addresses;
    }

    // Re-reads the entity list and publishes a new snapshot. Called by the game-state worker only.
    void refresh();

    // Resolve a real CHandle and reject a slot whose serial no longer matches.
    [[nodiscard]] std::uintptr_t lookup(std::uint32_t handle) const;
    // Resolve a plain entity index such as m_iIDEntIndex or a controller-list slot.
    // These values intentionally have no serial bits and must not pass lookup().
    [[nodiscard]] std::uintptr_t lookup_index(std::uint32_t index) const;
    [[nodiscard]] bool player_controller_slots(std::array<std::uintptr_t, 65>& out) const;
    [[nodiscard]] std::shared_ptr<const std::vector<cached>> all() const;
    [[nodiscard]] std::uintptr_t list_address() const;
    // Runtime class name of an entity ("C_CSPlayerPawn"), empty if it cannot be read.
    [[nodiscard]] std::string class_name(std::uintptr_t entity) const;

private:
    struct class_identity
    {
        std::uintptr_t identity{};
        std::uintptr_t class_info{};
        std::uint32_t schema_id{};
    };

    struct class_cache_entry
    {
        class_identity identity{};
        type entity_type{type::unknown};
        bool seen{};
    };

    // Number of list chunks scanned (2048 entity indices); everything Vesta tracks lives below that.
    static constexpr std::size_t k_scanned_chunks = 4;

    [[nodiscard]] std::uintptr_t lookup_slot(std::uint32_t value, bool validate_serial) const;
    [[nodiscard]] std::optional<class_identity> resolve_class(std::uintptr_t entity, std::string* name = nullptr) const;
    [[nodiscard]] static type classify(std::uint32_t schema_id) noexcept;

    const memory_services* m_services{};
    const address_catalog* m_addresses{};
    std::atomic<std::shared_ptr<const std::vector<cached>>> m_entities{std::make_shared<const std::vector<cached>>()};
    // Owned by the refreshing thread.
    std::unordered_map<std::uintptr_t, class_cache_entry> m_class_cache{};
    std::array<std::vector<std::uint8_t>, k_scanned_chunks> m_chunk_buffers{};
    std::uint32_t m_validation_phase{};
    // Index of the identity -> class-info -> name chain that last worked; shared by readers on any thread.
    mutable std::atomic<int> m_active_chain{-1};
};

} // namespace game
