#include <stdafx.hpp>
#include <game/state/constants.hpp>
#include <game/state/services.hpp>
#include <foundation/identity.hpp>
#include <game/body/body.hpp>
#include <foundation/color.hpp>
#include <game/memory/bound_services.hpp>
#include <game/memory/remote_pointer.hpp>

namespace game
{
namespace
{
constexpr std::size_t k_hitbox_stride = 0x70;
constexpr int k_max_hitboxes = 40;
// CHitBoxSet: hitbox count and the CHitBox array.
constexpr std::uintptr_t k_set_count_offset = 0x28;
constexpr std::uintptr_t k_set_array_offset = 0x30;
// Used when the schema does not report CSkeletonInstance::m_modelState.
constexpr std::uintptr_t k_default_model_state_offset = 0x160;
// Bone driven by each hitbox index of the player model.
constexpr std::array<int, 19> k_bone_map{7, 6, 1, 2, 3, 4, 5, 17, 20, 18, 21, 19, 22, 11, 15, 9, 10, 13, 14};
// Model caches are dropped wholesale beyond this many scene nodes (players come and go between rounds).
constexpr std::size_t k_max_cached_nodes = 256;

// CHitBox field offsets; the layout changed between game updates and both are still accepted.
struct field_layout
{
    std::size_t mins;
    std::size_t maxs;
    std::size_t radius;
    std::size_t name;
};

constexpr std::array<field_layout, 2> k_layouts{{
    {0x20, 0x2C, 0x50, 0x38}, // m_vMinBounds, m_vMaxBounds, m_flShapeRadius, m_name
    {0x18, 0x24, 0x30, 0x00},
}};

// Path from the skeleton's model state to the hitbox set:
// model_state + handle -> model handle -> CModel (+ intermediate, optionally dereferenced) + set -> CHitBoxSet.
struct chain
{
    std::uintptr_t handle_offset;
    std::uintptr_t intermediate_offset;
    bool dereference_intermediate;
    std::uintptr_t set_offset;
};

constexpr std::array<chain, 6> k_chains{{
    {0xC0, 0x78, true, 0x168},
    {0xC0, 0x78, true, 0x150},
    {0xC0, 0x78, true, 0x140},
    {0xD0, 0x78, true, 0x140},
    {0xD0, 0x78, true, 0x150},
    {0xD0, 0x78, true, 0x168},
}};

[[nodiscard]] bool plausible_vector(const foundation::vec3& v) noexcept
{
    constexpr auto limit = 200.0f;
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::abs(v.x) < limit &&
           std::abs(v.y) < limit && std::abs(v.z) < limit;
}

template<typename value_t>
[[nodiscard]] value_t field(const std::byte* entry, const std::size_t offset) noexcept
{
    value_t value{};
    std::memcpy(&value, entry + offset, sizeof(value));
    return value;
}

[[nodiscard]] bool plausible_entry(const std::byte* entry, const field_layout& layout) noexcept
{
    const auto radius = field<float>(entry, layout.radius);
    return std::isfinite(radius) && radius >= 0.5f && radius <= 40.0f &&
           plausible_vector(field<foundation::vec3>(entry, layout.mins)) &&
           plausible_vector(field<foundation::vec3>(entry, layout.maxs));
}

struct hitbox_array
{
    std::vector<std::byte> bytes{};
    int count{};
    const field_layout* layout{};
    std::uintptr_t model_handle{};

    [[nodiscard]] const std::byte* entry(const int index) const noexcept
    {
        return bytes.data() + static_cast<std::size_t>(index) * k_hitbox_stride;
    }
};

// A real CHitBox array is static and uniform, so most entries must pass the plausibility check under one layout;
// a random buffer with a few lucky floats does not.
[[nodiscard]] std::optional<hitbox_array> read_hitboxes(const std::uintptr_t model_state, const chain& path)
{
    const auto& process = bound_memory().process;
    const auto model_handle = process.load<std::uintptr_t>(model_state + path.handle_offset);
    if (!is_user_pointer(model_handle))
        return std::nullopt;
    const auto model = process.load<std::uintptr_t>(model_handle);
    if (!is_user_pointer(model))
        return std::nullopt;
    auto base = process.load<std::uintptr_t>(model + path.intermediate_offset);
    if (path.dereference_intermediate && is_user_pointer(base))
        base = process.load<std::uintptr_t>(base);
    if (!is_user_pointer(base))
        return std::nullopt;
    const auto hitbox_set = process.load<std::uintptr_t>(base + path.set_offset);
    if (!is_user_pointer(hitbox_set))
        return std::nullopt;

    const auto count = process.load<int>(hitbox_set + k_set_count_offset);
    const auto array = process.load<std::uintptr_t>(hitbox_set + k_set_array_offset);
    constexpr int min_count = 4;
    if (!is_user_pointer(array) || count < min_count || count > k_max_hitboxes)
        return std::nullopt;

    hitbox_array result{.bytes = std::vector<std::byte>(static_cast<std::size_t>(count) * k_hitbox_stride),
                        .count = count,
                        .model_handle = model_handle};
    if (!process.copy(array, result.bytes.data(), result.bytes.size()))
        return std::nullopt;

    const auto required = std::max(3, count / 2);
    for (const auto& layout : k_layouts)
    {
        int valid = 0;
        for (int index = 0; index < count; ++index)
            valid += plausible_entry(result.entry(index), layout) ? 1 : 0;
        if (valid >= required)
        {
            result.layout = &layout;
            return result;
        }
    }
    return std::nullopt;
}

[[nodiscard]] hitbox_catalog::set parse_hitboxes(const hitbox_array& array)
{
    const auto& layout = *array.layout;
    hitbox_catalog::set result{};
    for (int index = 0; index < array.count && result.count < static_cast<int>(result.entries.size()); ++index)
    {
        const auto* entry = array.entry(index);
        if (!plausible_entry(entry, layout))
            continue;

        auto& hitbox = result.entries[static_cast<std::size_t>(result.count++)];
        hitbox.index = index;
        hitbox.bone = index < static_cast<int>(k_bone_map.size()) ? k_bone_map[static_cast<std::size_t>(index)] : index;
        if (const auto name = field<std::uintptr_t>(entry, layout.name); is_user_pointer(name))
            static_cast<void>(bound_memory().process.copy(name, hitbox.name.data(), hitbox.name.size() - 1));
        // The hitbox name is authoritative where the index order and the model disagree.
        const std::string_view name{hitbox.name.data()};
        if (name == "head")
            hitbox.bone = rules::joint_id::head;
        else if (name == "neck_0" || name == "neck")
            hitbox.bone = rules::joint_id::neck;
        hitbox.mins = field<foundation::vec3>(entry, layout.mins);
        hitbox.maxs = field<foundation::vec3>(entry, layout.maxs);
        hitbox.radius = field<float>(entry, layout.radius);
    }
    return result;
}
} // namespace

hitbox_catalog::set hitbox_catalog::query(const std::uintptr_t game_scene_node, geometry_cache* const cache) const
{
    if (!game_scene_node)
        return {};

    const auto schema_offset = SCHEMA("CSkeletonInstance", "m_modelState"_id);
    const auto model_state =
        game_scene_node + (schema_offset ? static_cast<std::uintptr_t>(schema_offset) : k_default_model_state_offset);

    if (cache)
    {
        if (const auto it = cache->m_nodes.find(game_scene_node); it != cache->m_nodes.end())
        {
            const auto current = bound_memory().process.load<std::uintptr_t>(model_state + it->second.handle_offset);
            if (current == it->second.model_handle)
                return it->second.geometry;
            cache->m_nodes.erase(it);
        }
    }

    // The chain that worked last time first; the others only when it stops working.
    std::optional<hitbox_array> array;
    auto active = m_active_chain.load(std::memory_order_relaxed);
    if (active >= 0)
        array = read_hitboxes(model_state, k_chains[static_cast<std::size_t>(active)]);
    for (std::size_t index = 0; !array && index < k_chains.size(); ++index)
    {
        if (static_cast<int>(index) == active)
            continue;
        if ((array = read_hitboxes(model_state, k_chains[index])))
        {
            active = static_cast<int>(index);
            m_active_chain.store(active, std::memory_order_relaxed);
        }
    }
    if (!array)
        return {};

    auto result = parse_hitboxes(*array);
    if (cache && result.count > 0)
    {
        if (cache->m_nodes.size() >= k_max_cached_nodes)
            cache->m_nodes.clear();
        cache->m_nodes[game_scene_node] = {static_cast<int>(k_chains[static_cast<std::size_t>(active)].handle_offset),
                                           array->model_handle, result};
    }
    return result;
}

int hitbox_catalog::hitgroup_from_hitbox(const int hitbox) const
{
    namespace group = rules::hitgroup;
    // Hitbox indices of the current player models (0-18).
    switch (hitbox)
    {
        case 0:
        case 1:
            return group::head; // head, neck
        case 2:
        case 3:
            return group::stomach; // pelvis, spine 0
        case 4:
        case 5:
        case 6:
            return group::chest; // spine 1-3
        case 7:
        case 9:
        case 11:
            return group::left_leg; // upper, lower, ankle
        case 8:
        case 10:
        case 12:
            return group::right_leg;
        case 13:
        case 15:
        case 16:
            return group::left_arm; // hand, upper, lower
        case 14:
        case 17:
        case 18:
            return group::right_arm;
        default:
            return group::chest;
    }
}

void hitbox_catalog::remember(const set& snapshot)
{
    if (snapshot.count <= 0)
        return;
    std::unique_lock lock(m_snapshot_mutex);
    m_snapshot = snapshot;
}

hitbox_catalog::set hitbox_catalog::snapshot() const
{
    std::shared_lock lock(m_snapshot_mutex);
    return m_snapshot;
}

} // namespace game
