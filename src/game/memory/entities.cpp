#include <stdafx.hpp>
#include <foundation/identity.hpp>
#include <game/state/pose.hpp>
#include <game/memory/catalogs.hpp>
#include <game/memory/entity_slot.hpp>
#include <foundation/read_pair.hpp>
#include <game/memory/remote_pointer.hpp>

namespace game
{
namespace
{
// Where the runtime class name hangs off an entity: entity + identity -> CEntityIdentity, identity + 8 ->
// class info, class info + name (optionally through one more pointer at +8) -> "C_..." string. Builds differ in
// which offsets hold, so every combination is tried once and the working one is remembered.
struct class_chain
{
    std::uint32_t identity_offset;
    std::uint32_t name_offset;
    bool through_binding;
};

constexpr std::array<class_chain, 8> k_class_chains{{
    {0x10, 0x8, false},
    {0x10, 0x8, true},
    {0x10, 0x10, false},
    {0x10, 0x10, true},
    {0x8, 0x8, false},
    {0x8, 0x8, true},
    {0x8, 0x10, false},
    {0x8, 0x10, true},
}};

constexpr std::uintptr_t k_class_info_offset = 0x8;
constexpr std::uintptr_t k_binding_name_offset = 0x8;
constexpr std::size_t k_class_name_capacity = 64;
// One cached entity in eight has its class identity re-validated per refresh.
constexpr std::uint32_t k_validation_period = 8;

[[nodiscard]] bool is_valid_class_name(const std::string_view name) noexcept
{
    return name.size() >= 3 && name.size() < k_class_name_capacity && name.front() == 'C' &&
           std::ranges::all_of(
               name, [](const char c)
               { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}
} // namespace

void entity_directory::refresh()
{
    const auto list = list_address();
    if (!list)
        return;

    const auto& process = m_services->process;
    std::array<std::uintptr_t, k_scanned_chunks> chunks{};
    // Never publish an empty directory assembled from a failed header read.
    if (!process.copy(list + entity_list::chunk_table_offset, chunks.data(), sizeof(chunks)))
        return;

    // Read every active chunk (one call per 56 KiB chunk) before touching the class cache, so a failed read
    // leaves both the published directory and the cache as they were.
    for (std::size_t chunk = 0; chunk < k_scanned_chunks; ++chunk)
    {
        auto& buffer = m_chunk_buffers[chunk];
        buffer.resize(entity_list::chunk_bytes);
        if (chunks[chunk] && !process.copy(chunks[chunk], buffer.data(), buffer.size()))
            return;
    }

    for (auto& [address, entry] : m_class_cache)
        entry.seen = false;
    m_validation_phase = (m_validation_phase + 1) % k_validation_period;

    std::vector<cached> fresh{};
    fresh.reserve(64);
    const auto publish = [&fresh](const std::uintptr_t entity, const std::int32_t index, const class_cache_entry& entry)
    {
        if (entry.entity_type != type::unknown)
            fresh.push_back(
                {.ptr = entity, .schema_id = entry.identity.schema_id, .index = index, .type = entry.entity_type});
    };

    for (std::size_t chunk = 0; chunk < k_scanned_chunks; ++chunk)
    {
        if (!chunks[chunk])
            continue;

        const auto& buffer = m_chunk_buffers[chunk];
        for (std::size_t slot = 0; slot < entity_list::slots_per_chunk; ++slot)
        {
            std::uintptr_t entity{};
            std::memcpy(&entity, buffer.data() + slot * entity_list::slot_stride, sizeof(entity));
            if (entity < k_lowest_user_address)
                continue;

            const auto index = static_cast<std::int32_t>(chunk * entity_list::slots_per_chunk + slot);
            if (const auto it = m_class_cache.find(entity); it != m_class_cache.end())
            {
                auto& entry = it->second;
                entry.seen = true;
                const auto audit = (static_cast<std::uint32_t>(index) + m_validation_phase) % k_validation_period == 0;
                if (!audit)
                {
                    publish(entity, index, entry);
                    continue;
                }

                // Entity slots and allocations are reused between rounds; a pointer match alone does not mean this
                // is still an instance of the same class.
                const auto [identity_8, identity_10] = foundation::read_pair<std::uintptr_t, std::uintptr_t, 8>(
                    entity + 0x8,
                    [&process](auto address, auto* out, auto size) { return process.copy(address, out, size); });
                const auto known = entry.identity.identity;
                const auto same_identity = known != 0 && (identity_8 == known || identity_10 == known);
                if (same_identity &&
                    process.load<std::uintptr_t>(known + k_class_info_offset) == entry.identity.class_info)
                {
                    publish(entity, index, entry);
                    continue;
                }
                m_class_cache.erase(it);
            }

            // A failed read is transient: retry next refresh instead of caching the entity as unknown.
            const auto resolved = resolve_class(entity);
            if (!resolved)
                continue;

            const class_cache_entry entry{*resolved, classify(resolved->schema_id), true};
            m_class_cache.emplace(entity, entry);
            publish(entity, index, entry);
        }
    }

    // Forget entities as soon as they disappear so stale class identities never survive a round change.
    std::erase_if(m_class_cache, [](const auto& item) { return !item.second.seen; });

    m_entities.store(std::make_shared<const std::vector<cached>>(std::move(fresh)), std::memory_order_release);
}

std::uintptr_t entity_directory::lookup(std::uint32_t handle) const
{
    return lookup_slot(handle, true);
}

std::uintptr_t entity_directory::lookup_index(std::uint32_t index) const
{
    if (index == 0 || index > entity_list::index_mask)
        return 0;
    return lookup_slot(index, false);
}

bool entity_directory::player_controller_slots(std::array<std::uintptr_t, 65>& out) const
{
    return read_player_controller_slots(list_address(), out, [this](auto address, void* destination, auto size)
                                        { return m_services->process.copy(address, destination, size); });
}

std::uintptr_t entity_directory::lookup_slot(std::uint32_t value, bool validate_serial) const
{
    return read_entity_slot(list_address(), value, validate_serial, [this](auto address, void* out, auto size)
                            { return m_services->process.copy(address, out, size); });
}

std::shared_ptr<const std::vector<entity_directory::cached>> entity_directory::all() const
{
    return m_entities.load(std::memory_order_acquire);
}

std::uintptr_t entity_directory::list_address() const
{
    return m_services->process.load<std::uintptr_t>(m_addresses->entity_list);
}

std::string entity_directory::class_name(const std::uintptr_t entity) const
{
    std::string name;
    return resolve_class(entity, &name) ? name : std::string{};
}

std::optional<entity_directory::class_identity> entity_directory::resolve_class(const std::uintptr_t entity,
                                                                                std::string* name_out) const
{
    const auto& process = m_services->process;
    const auto try_chain = [&](const class_chain& chain) -> std::optional<class_identity>
    {
        const auto identity = process.load<std::uintptr_t>(entity + chain.identity_offset);
        if (!is_user_pointer(identity))
            return std::nullopt;
        const auto class_info = process.load<std::uintptr_t>(identity + k_class_info_offset);
        if (!is_user_pointer(class_info))
            return std::nullopt;
        auto name_ptr = process.load<std::uintptr_t>(class_info + chain.name_offset);
        if (chain.through_binding && is_user_pointer(name_ptr))
            name_ptr = process.load<std::uintptr_t>(name_ptr + k_binding_name_offset);
        if (!is_user_pointer(name_ptr))
            return std::nullopt;

        std::array<char, k_class_name_capacity> buffer{};
        if (!process.copy(name_ptr, buffer.data(), buffer.size() - 1))
            return std::nullopt;
        const std::string_view name{buffer.data()};
        if (!is_valid_class_name(name))
            return std::nullopt;
        if (name_out)
            name_out->assign(name);
        return class_identity{identity, class_info, foundation::identity::of(name)};
    };

    const auto active = m_active_chain.load(std::memory_order_relaxed);
    if (active >= 0)
        if (auto resolved = try_chain(k_class_chains[static_cast<std::size_t>(active)]))
            return resolved;

    for (std::size_t index = 0; index < k_class_chains.size(); ++index)
    {
        if (static_cast<int>(index) == active)
            continue;
        if (auto resolved = try_chain(k_class_chains[index]))
        {
            m_active_chain.store(static_cast<int>(index), std::memory_order_relaxed);
            const auto& chain = k_class_chains[index];
            m_services->log.info("[entities] class chain: identity+{:#x} name+{:#x} binding={}", chain.identity_offset,
                                 chain.name_offset, chain.through_binding);
            return resolved;
        }
    }
    return std::nullopt;
}

entity_directory::type entity_directory::classify(std::uint32_t schema_id) noexcept
{
    switch (schema_id)
    {
        case "CCSPlayerController"_id:
            return type::player;

        case "C_PlantedC4"_id:
        case "C_AK47"_id:
        case "C_WeaponM4A1"_id:
        case "C_WeaponM4A1Silencer"_id:
        case "C_WeaponAWP"_id:
        case "C_WeaponAug"_id:
        case "C_WeaponFamas"_id:
        case "C_WeaponGalilAR"_id:
        case "C_WeaponSG556"_id:
        case "C_WeaponG3SG1"_id:
        case "C_WeaponSCAR20"_id:
        case "C_WeaponSSG08"_id:
        case "C_WeaponMAC10"_id:
        case "C_WeaponMP5SD"_id:
        case "C_WeaponMP7"_id:
        case "C_WeaponMP9"_id:
        case "C_WeaponBizon"_id:
        case "C_WeaponP90"_id:
        case "C_WeaponUMP45"_id:
        case "C_WeaponNOVA"_id:
        case "C_WeaponSawedoff"_id:
        case "C_WeaponXM1014"_id:
        case "C_WeaponMag7"_id:
        case "C_WeaponM249"_id:
        case "C_WeaponNegev"_id:
        case "C_DEagle"_id:
        case "C_WeaponElite"_id:
        case "C_WeaponFiveSeven"_id:
        case "C_WeaponGlock"_id:
        case "C_WeaponHKP2000"_id:
        case "C_WeaponUSPSilencer"_id:
        case "C_WeaponP250"_id:
        case "C_WeaponCZ75a"_id:
        case "C_WeaponTec9"_id:
        case "C_WeaponRevolver"_id:
        case "C_WeaponTaser"_id:
        case "C_Knife"_id:
        case "C_C4"_id:
        case "C_Item_Healthshot"_id:
        case "C_HEGrenade"_id:
        case "C_Flashbang"_id:
        case "C_SmokeGrenade"_id:
        case "C_MolotovGrenade"_id:
        case "C_IncendiaryGrenade"_id:
        case "C_DecoyGrenade"_id:
            return type::item;

        case "C_HEGrenadeProjectile"_id:
        case "C_FlashbangProjectile"_id:
        case "C_SmokeGrenadeProjectile"_id:
        case "C_MolotovProjectile"_id:
        case "C_Inferno"_id:
        case "C_DecoyProjectile"_id:
            return type::projectile;

        case "C_BulletHitModel"_id:
        case "C_LocalTempEntity"_id:
            return type::impact;

        default:
            return type::unknown;
    }
}

} // namespace game
