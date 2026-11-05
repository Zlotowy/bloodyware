#include <stdafx.hpp>
#include <game/memory/remote_pointer.hpp>
#include <game/memory/bound_services.hpp>

#include <simulation/collision_segments.hpp>
#include <simulation/mesh_leaves.hpp>
#include <simulation/bvh_capacity.hpp>
#include <simulation/swept_hull.hpp>
#include <simulation/swept_sphere.hpp>

namespace simulation
{

namespace detail
{

static constexpr std::size_t k_inner_node_size{32};
static constexpr std::size_t k_outer_node_size{48};


struct half_edge_record
{
    std::uint8_t next;
    std::uint8_t twin;
    std::uint8_t vert;
    std::uint8_t face;
};

struct quaternion
{
    float x, y, z, w;
};
struct rotation_basis
{
    float m[3][3];
};

static rotation_basis make_rotation_basis(const quaternion& q)
{
    const auto xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const auto xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const auto wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;

    rotation_basis m{};
    m.m[0][0] = 1 - 2 * (yy + zz);
    m.m[0][1] = 2 * (xy + wz);
    m.m[0][2] = 2 * (xz - wy);
    m.m[1][0] = 2 * (xy - wz);
    m.m[1][1] = 1 - 2 * (xx + zz);
    m.m[1][2] = 2 * (yz + wx);
    m.m[2][0] = 2 * (xz + wy);
    m.m[2][1] = 2 * (yz - wx);
    m.m[2][2] = 1 - 2 * (xx + yy);
    return m;
}

static foundation::vec3 rotate_vector(const rotation_basis& m, const foundation::vec3& v)
{
    return {m.m[0][0] * v.x + m.m[1][0] * v.y + m.m[2][0] * v.z, m.m[0][1] * v.x + m.m[1][1] * v.y + m.m[2][1] * v.z,
            m.m[0][2] * v.x + m.m[1][2] * v.y + m.m[2][2] * v.z};
}

static foundation::vec3 place_vertex(const rotation_basis& rot, const float scale[3], const float pos[3],
                                     const foundation::vec3& local)
{
    const auto scaled = foundation::vec3{local.x * scale[0], local.y * scale[1], local.z * scale[2]};
    const auto rotated = rotate_vector(rot, scaled);
    return {rotated.x + pos[0], rotated.y + pos[1], rotated.z + pos[2]};
}


template<typename T>
[[nodiscard]] std::optional<std::vector<T>> read_remote_array(std::uintptr_t address, std::size_t count)
{
    if (!address || count == 0 || count > remote_array_budget / sizeof(T) || count * sizeof(T) > UINTPTR_MAX - address)
        return std::nullopt;
    std::vector<T> values(count);
    if (!game::bound_memory().process.copy(address, values.data(), values.size() * sizeof(T)))
    {
        return std::nullopt;
    }
    return values;
}

template<typename T, std::size_t Size>
[[nodiscard]] T blob_field(const std::array<std::uint8_t, Size>& bytes, std::size_t offset)
{
    T value{};
    if (offset + sizeof(T) <= bytes.size())
        std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

static bool decode_mesh_shape(std::uintptr_t bvh_ptr, std::uintptr_t vert_ptr, std::uintptr_t tri_ptr,
                              std::uint32_t node_count, const rotation_basis& rotation, const float scale[3],
                              const float position[3], std::uintptr_t material_ptr, std::int32_t material_count,
                              const std::vector<collision_world::global_surface_entry>& surface_table,
                              const collision_world::surface_info& fallback_surface,
                              std::vector<collision_world::triangle>& output, std::uint64_t solid_id,
                              std::stop_token stop)
{
    if (!bvh_ptr || !vert_ptr || !tri_ptr || node_count == 0 ||
        node_count > remote_array_budget / sizeof(packed_mesh_node))
    {
        return false;
    }
    static_assert(sizeof(packed_mesh_node) == k_inner_node_size);
    const auto nodes = read_remote_array<packed_mesh_node>(bvh_ptr, node_count);
    if (!nodes)
        return false;
    const auto leaves = discover_mesh_leaves(std::span<const packed_mesh_node>{*nodes}, stop);
    if (!leaves)
        return false;

    const auto triangle_count = leaves->past_last_triangle - leaves->first_triangle;
    if (stop.stop_requested() || triangle_count > remote_array_budget / 12 ||
        output.size() + triangle_count > 4 * remote_array_budget / sizeof(collision_world::triangle))
        return false;
    const auto indices =
        read_remote_array<std::int32_t>(tri_ptr + static_cast<std::uintptr_t>(leaves->first_triangle) * 12,
                                        static_cast<std::size_t>(triangle_count) * 3);
    if (!indices)
        return false;
    const auto highest_vertex = *std::ranges::max_element(*indices);
    if (highest_vertex <= 0 || highest_vertex > 0x1000000)
        return false;

    const auto vertex_count = static_cast<std::size_t>(highest_vertex) + 1;
    const auto vertices = read_remote_array<float>(vert_ptr, vertex_count * 3);
    if (!vertices)
        return false;

    std::vector<std::uint8_t> materials{};
    if (game::is_user_pointer(material_ptr) && material_count > 0)
    {
        if (const auto remote = read_remote_array<std::uint8_t>(material_ptr + leaves->first_triangle, triangle_count))
        {
            materials = std::move(*remote);
        }
    }

    const auto vertex_at = [&](std::int32_t index)
    {
        const auto offset = static_cast<std::size_t>(index) * 3;
        return place_vertex(rotation, scale, position,
                            {(*vertices)[offset], (*vertices)[offset + 1], (*vertices)[offset + 2]});
    };
    const auto initial_size = output.size();
    for (const auto span : leaves->spans)
    {
        for (auto ordinal = 0u; ordinal < span.count; ++ordinal)
        {
            if ((ordinal & 255u) == 0 && stop.stop_requested())
                return false;
            const auto local = span.first - leaves->first_triangle + ordinal;
            if (local >= triangle_count)
                continue;
            const auto base = static_cast<std::size_t>(local) * 3;
            const std::array vertex_indices{(*indices)[base], (*indices)[base + 1], (*indices)[base + 2]};
            if (std::ranges::any_of(vertex_indices, [&](auto index)
                                    { return index < 0 || static_cast<std::size_t>(index) >= vertex_count; }))
            {
                continue;
            }

            auto material = fallback_surface;
            if (local < materials.size() && materials[local] < surface_table.size())
            {
                const auto global_index = materials[local];
                const auto& source = surface_table[global_index];
                material = {source.penetration_mod, source.surface_type, global_index};
            }
            output.push_back({vertex_at(vertex_indices[0]), vertex_at(vertex_indices[1]), vertex_at(vertex_indices[2]),
                              material, solid_id});
        }
    }
    return output.size() != initial_size;
}

static bool decode_convex_shape(std::uintptr_t hull_data, float uniform_scale,
                                const collision_world::surface_info& surface,
                                std::vector<collision_world::triangle>& out, std::uint64_t solid_id)
{
    std::array<std::uint8_t, 0x100> header{};
    if (!hull_data || !game::bound_memory().process.copy(hull_data, header.data(), header.size()))
    {
        return false;
    }
    const auto vertex_count = blob_field<std::int32_t>(header, 0x88);
    const auto vertex_address = blob_field<std::uintptr_t>(header, 0x90);
    const auto edge_count = blob_field<std::int32_t>(header, 0xa0);
    const auto edge_address = blob_field<std::uintptr_t>(header, 0xa8);
    const auto face_count = blob_field<std::int32_t>(header, 0xb8);
    const auto face_address = blob_field<std::uintptr_t>(header, 0xc0);
    const auto plausible_count = [](auto count) { return count > 0 && count <= 0xffff; };
    if (!plausible_count(vertex_count) || !plausible_count(edge_count) || !plausible_count(face_count))
    {
        return false;
    }

    const auto vertices = read_remote_array<float>(vertex_address, static_cast<std::size_t>(vertex_count) * 3);
    const auto edges = read_remote_array<half_edge_record>(edge_address, edge_count);
    const auto faces = read_remote_array<std::uint8_t>(face_address, face_count);
    if (!vertices || !edges || !faces)
        return false;

    const auto face_loop = [&](std::uint8_t first)
    {
        std::vector<std::uint8_t> loop{};
        loop.reserve(8);
        auto cursor = first;
        for (auto steps = 0; steps < 64; ++steps)
        {
            if (cursor >= edges->size())
                break;
            loop.push_back((*edges)[cursor].vert);
            cursor = (*edges)[cursor].next;
            if (cursor == first)
                break;
        }
        return loop;
    };
    const auto vertex_at = [&](std::uint8_t index)
    {
        const auto base = static_cast<std::size_t>(index) * 3;
        return foundation::vec3{(*vertices)[base] * uniform_scale, (*vertices)[base + 1] * uniform_scale,
                                (*vertices)[base + 2] * uniform_scale};
    };

    const auto initial_size = out.size();
    for (const auto first_edge : *faces)
    {
        const auto loop = face_loop(first_edge);
        if (loop.size() < 3 || std::ranges::any_of(loop, [&](auto index) { return index >= vertex_count; }))
        {
            continue;
        }
        const auto anchor = vertex_at(loop.front());
        for (std::size_t index = 1; index + 1 < loop.size(); ++index)
        {
            out.push_back({anchor, vertex_at(loop[index]), vertex_at(loop[index + 1]), surface, solid_id});
        }
    }
    return out.size() != initial_size;
}

static void decode_convex_body(std::uintptr_t shape_body, std::vector<collision_world::triangle>& output)
{
    const auto data = game::bound_memory().process.load<std::uintptr_t>(shape_body + 0xb8);
    if (!game::is_user_pointer(data))
        return;
    const auto raw_scale = game::bound_memory().process.load<float>(shape_body + 0xb0);
    const auto scale = raw_scale > 0.0f && std::isfinite(raw_scale) ? raw_scale : 1.0f;
    collision_world::surface_info material{};
    material.penetration = game::bound_memory().process.load<float>(shape_body + 0x28);
    decode_convex_shape(data, scale, material, output, shape_body);
}

static void decode_mesh_body(std::uintptr_t shape_body,
                             const std::vector<collision_world::global_surface_entry>& surface_table,
                             std::vector<collision_world::triangle>& output, std::stop_token stop)
{
    const auto mesh_data = game::bound_memory().process.load<std::uintptr_t>(shape_body + 0xc0);
    if (!mesh_data || game::bound_memory().process.load<float>(shape_body + 0x2c) < 0.0f)
        return;

    std::array<std::uint8_t, 0xa0> descriptor{};
    if (!game::bound_memory().process.copy(mesh_data, descriptor.data(), descriptor.size()))
        return;
    std::array<float, 3> scale{};
    std::array<float, 3> position{};
    quaternion orientation{};
    if (!game::bound_memory().process.copy(shape_body + 0xb0, scale.data(), sizeof(scale)) ||
        !game::bound_memory().process.copy(shape_body + 0x100, position.data(), sizeof(position)) ||
        !game::bound_memory().process.copy(shape_body + 0x130, &orientation, sizeof(orientation)))
    {
        return;
    }
    if (std::ranges::any_of(scale, [](float value) { return !std::isfinite(value); }) ||
        std::ranges::all_of(scale, [](float value) { return value == 0.0f; }))
    {
        return;
    }
    if (std::ranges::any_of(position, [](float value) { return !std::isfinite(value); }))
        return;
    const auto quaternion_length = orientation.x * orientation.x + orientation.y * orientation.y +
                                   orientation.z * orientation.z + orientation.w * orientation.w;
    if (!std::isfinite(quaternion_length) || quaternion_length < 0.5f || quaternion_length > 1.5f)
        orientation = {0.0f, 0.0f, 0.0f, 1.0f};

    const std::array count_candidates{
        blob_field<std::int32_t>(descriptor, 0x28), blob_field<std::int32_t>(descriptor, 0x30),
        blob_field<std::int32_t>(descriptor, 0x48), blob_field<std::int32_t>(descriptor, 0x58)};
    const auto count =
        std::ranges::find_if(count_candidates, [](auto value) { return value > 0 && value < 0x1000000; });
    if (count == count_candidates.end())
        return;

    collision_world::surface_info fallback{};
    fallback.penetration = game::bound_memory().process.load<float>(shape_body + 0x28);
    decode_mesh_shape(blob_field<std::uintptr_t>(descriptor, 0x20), blob_field<std::uintptr_t>(descriptor, 0x38),
                      blob_field<std::uintptr_t>(descriptor, 0x50), static_cast<std::uint32_t>(*count),
                      make_rotation_basis(orientation), scale.data(), position.data(),
                      blob_field<std::uintptr_t>(descriptor, 0x98), blob_field<std::int32_t>(descriptor, 0x90),
                      surface_table, fallback, output, shape_body, stop);
}

static void append_shape_triangles(std::uintptr_t shape_body, std::uintptr_t hull_vtable, std::uintptr_t mesh_vtable,
                                   const std::vector<collision_world::global_surface_entry>& surface_table,
                                   std::vector<collision_world::triangle>& output, std::stop_token stop)
{
    const auto type = game::bound_memory().process.load<std::uintptr_t>(shape_body);
    if (type == hull_vtable)
        decode_convex_body(shape_body, output);
    else if (type == mesh_vtable)
        decode_mesh_body(shape_body, surface_table, output, stop);
}

// Layout of the physics world as the client holds it (all offsets in bytes).
namespace world_layout
{
constexpr std::uintptr_t inner_world = 0x30;   // CPhysicsWorld -> inner world
constexpr std::uintptr_t body_array = 0x110;   // inner world -> body array
constexpr std::uintptr_t body_count = 0x268;   // body array -> int32 body count
constexpr std::uintptr_t body_stride = 88;     // one body in the array
constexpr std::uintptr_t body_bvh_root = 0x00; // int32, -1 for a body with a single shape
constexpr std::uintptr_t body_node_count_a = 0x08;
constexpr std::uintptr_t body_node_count_b = 0x10;
constexpr std::uintptr_t body_bvh_nodes = 0x18; // outer BVH nodes
constexpr std::uintptr_t body_simulation_type = 0x40;
constexpr std::uintptr_t body_single_shape = 0x28;
constexpr std::size_t node_left = 12;    // int32 child, -1 on a leaf
constexpr std::size_t node_right = 28;   // int32 child
constexpr std::size_t node_shape = 0x28; // shape pointer of a leaf
constexpr std::uint32_t max_outer_nodes = 0x100000;
constexpr std::int32_t max_bodies = 1'000'000;
} // namespace world_layout

struct physics_world
{
    std::uintptr_t holder{}; // client .data object whose first field is the world
    std::uintptr_t world{};
    std::uintptr_t body_array{};
    std::int32_t body_count{};
};

// Finds the client's physics world through the instruction that loads it next to a known call site.
[[nodiscard]] std::optional<physics_world> locate_physics_world()
{
    const auto& memory = game::bound_memory();
    const auto anchor =
        memory.process.scan_signature(memory.modules.client, "E8 ? ? ? ? C7 87 ? ? ? ? ? ? ? ? 48 8D 54 24 ? 48 8B CF");
    if (!anchor)
    {
        memory.log.warning("[bvh] world anchor signature not found");
        return std::nullopt;
    }

    constexpr std::size_t window_size = 0x40;
    std::array<std::uint8_t, window_size> window{};
    if (!memory.process.copy(anchor - window_size, window.data(), window.size()))
    {
        memory.log.warning("[bvh] world anchor window unreadable");
        return std::nullopt;
    }
    // mov rcx, [rip+disp32] == 48 8B 0D; the nearest one before the call wins.
    std::uintptr_t world_global{};
    for (int i = static_cast<int>(window_size) - 7; i >= 0; --i)
    {
        if (window[i] == 0x48 && window[i + 1] == 0x8B && window[i + 2] == 0x0D)
        {
            world_global = memory.process.decode_rip(anchor - window_size + i);
            break;
        }
    }
    if (!world_global)
    {
        memory.log.warning("[bvh] world-global mov not found near anchor {:#x}", anchor);
        return std::nullopt;
    }

    // Double indirection: the static slot holds a pointer to a holder object
    // (itself in client.dll .data), whose first field is the actual heap world.
    physics_world result{};
    result.holder = memory.process.load<std::uintptr_t>(world_global);
    if (!result.holder)
    {
        memory.log.warning("[bvh] world holder empty (global at {:#x})", world_global);
        return std::nullopt;
    }
    result.world = memory.process.load<std::uintptr_t>(result.holder);
    if (!result.world)
    {
        memory.log.warning("[bvh] world pointer empty (holder at {:#x})", result.holder);
        return std::nullopt;
    }
    return result;
}

// Reads where the bodies live; separate from locate_physics_world so the surface table can be read in between, as
// before (it is optional and must not abort parsing).
[[nodiscard]] bool locate_bodies(physics_world& world)
{
    const auto& memory = game::bound_memory();
    const auto inner_world = memory.process.load<std::uintptr_t>(world.world + world_layout::inner_world);
    if (!inner_world)
    {
        memory.log.warning("[bvh] inner world null (world={:#x})", world.world);
        return false;
    }
    world.body_array = memory.process.load<std::uintptr_t>(inner_world + world_layout::body_array);
    if (!world.body_array)
    {
        memory.log.warning("[bvh] body array null (inner={:#x})", inner_world);
        return false;
    }
    world.body_count = memory.process.load<std::int32_t>(world.body_array + world_layout::body_count);
    if (world.body_count <= 0 || world.body_count > world_layout::max_bodies)
    {
        memory.log.warning("[bvh] implausible body count {} (body_array={:#x})", world.body_count, world.body_array);
        return false;
    }
    return true;
}

// Shape pointers at the leaves of a body's outer BVH, each once; nullopt when the nodes cannot be read or do not form
// a tree.
[[nodiscard]] std::optional<std::vector<std::uintptr_t>> body_leaf_shapes(const std::uintptr_t body,
                                                                          const std::int32_t root,
                                                                          const std::uintptr_t nodes,
                                                                          const std::stop_token& stop)
{
    const auto& process = game::bound_memory().process;
    const auto node_count =
        std::max({static_cast<std::uint32_t>(root) + 1u,
                  static_cast<std::uint32_t>(process.load<std::int32_t>(body + world_layout::body_node_count_a)),
                  static_cast<std::uint32_t>(process.load<std::int32_t>(body + world_layout::body_node_count_b))});
    if (node_count > world_layout::max_outer_nodes)
        return std::nullopt;
    std::vector<std::uint8_t> node_bytes(node_count * k_outer_node_size);
    if (!process.copy(nodes, node_bytes.data(), node_bytes.size()))
        return std::nullopt;

    std::vector<std::uintptr_t> leaves;
    leaves.reserve(256);
    std::vector<std::int32_t> pending;
    pending.reserve(128);
    pending.push_back(root);
    traversal_guard visited(node_count);
    while (!pending.empty() && !stop.stop_requested())
    {
        const auto index = pending.back();
        pending.pop_back();
        if (index < 0 || !visited.enter(static_cast<std::size_t>(index)))
            return std::nullopt;

        const auto* node = node_bytes.data() + static_cast<std::size_t>(index) * k_outer_node_size;
        std::int32_t left{};
        std::memcpy(&left, node + world_layout::node_left, sizeof(left));
        if (left == -1)
        {
            std::uintptr_t shape{};
            std::memcpy(&shape, node + world_layout::node_shape, sizeof(shape));
            if (shape)
                leaves.push_back(shape);
            continue;
        }
        std::int32_t right{};
        std::memcpy(&right, node + world_layout::node_right, sizeof(right));
        if (left >= 0)
            pending.push_back(left);
        if (right >= 0)
            pending.push_back(right);
    }

    std::unordered_set<std::uintptr_t> seen;
    std::erase_if(leaves, [&](const std::uintptr_t shape) { return !seen.insert(shape).second; });
    return leaves;
}

} // namespace detail

std::uintptr_t collision_world::surface_manager() const
{
    if (m_surface_manager || m_surface_manager_attempts >= k_surface_manager_attempts)
    {
        return m_surface_manager;
    }

    ++m_surface_manager_attempts;

    const auto plausible = [](std::uintptr_t mgr)
    {
        if (!game::is_user_pointer(mgr))
        {
            return false;
        }

        const auto count = game::bound_memory().process.load<std::int32_t>(mgr + 0x20);
        if (count <= 0 || count > 4096)
        {
            return false;
        }

        const auto array_base = game::bound_memory().process.load<std::uintptr_t>(mgr + 0x28);
        if (!game::is_user_pointer(array_base))
        {
            return false;
        }

        global_surface_entry first{};
        if (!game::bound_memory().process.copy(array_base, &first, sizeof(first)))
        {
            return false;
        }

        return std::isfinite(first.penetration_mod) && first.penetration_mod > 0.0f && first.penetration_mod <= 16.0f;
    };

    if (const auto load_site = game::bound_memory().process.scan_signature(
            game::bound_memory().modules.client,
            "48 8B 1D ? ? ? ? 33 FF 48 85 DB 74 ? 8B 43 34 89 7B 20 A9 00 00 00 C0"))
    {
        const auto mgr =
            game::bound_memory().process.load<std::uintptr_t>(game::bound_memory().process.decode_rip(load_site));
        if (plausible(mgr))
        {
            m_surface_manager = mgr;
            game::bound_memory().log.info("[bvh] surface manager {:#x} via client.dll load site {:#x}", mgr, load_site);
            return mgr;
        }
    }

    for (const auto module :
         {game::bound_memory().modules.client, game::bound_memory().process.module_base("server.dll")})
    {
        if (!module)
        {
            continue;
        }

        const auto getter =
            game::bound_memory().process.scan_signature(module, "48 63 41 ? 48 8B 0D ? ? ? ? 48 C1 E0 05 48 03 41 28");
        if (!getter)
        {
            continue;
        }

        const auto mgr =
            game::bound_memory().process.load<std::uintptr_t>(game::bound_memory().process.decode_rip(getter + 4));
        if (plausible(mgr))
        {
            m_surface_manager = mgr;
            game::bound_memory().log.info("[bvh] surface manager {:#x} via GetSurfaceDataFromHandle {:#x}", mgr,
                                          getter);
            return mgr;
        }
    }

    game::bound_memory().log.warning("[bvh] surface manager not found -- geometry will trace with neutral penetration");
    return 0;
}

std::vector<collision_world::global_surface_entry> collision_world::read_surface_table() const
{
    std::vector<global_surface_entry> table{};

    const auto manager = surface_manager();
    if (!manager)
    {
        return table;
    }

    const auto array_base = game::bound_memory().process.load<std::uintptr_t>(manager + 0x28);
    const auto surface_count = game::bound_memory().process.load<std::int32_t>(manager + 0x20);

    if (!array_base || surface_count <= 0 || surface_count > 4096)
    {
        game::bound_memory().log.warning("[bvh] surface table unreadable (manager={:#x}, array={:#x}, count={})",
                                         manager, array_base, surface_count);
        return table;
    }

    table.resize(surface_count);
    if (!game::bound_memory().process.copy(array_base, table.data(),
                                           static_cast<std::size_t>(surface_count) * sizeof(global_surface_entry)))
    {
        table.clear();
    }

    return table;
}

void collision_world::parse(const std::stop_token stop)
{
    if (stop.stop_requested())
        return;
    auto world = detail::locate_physics_world();
    if (!world)
        return;
    // Surface table is optional: without it triangles still trace, they just get
    // default penetration values -- so a broken signature must not abort parsing.
    if (stop.stop_requested())
        return;
    const auto global_table = read_surface_table();
    if (!detail::locate_bodies(*world))
        return;

    const auto& memory = game::bound_memory();
    const auto hull_vtable = memory.process.locate_vtable(memory.modules.physics, "CRnHullShape");
    const auto mesh_vtable = memory.process.locate_vtable(memory.modules.physics, "CRnMeshShape");
    if (!hull_vtable || !mesh_vtable)
    {
        memory.log.warning("[bvh] shape vtables not found (hull={:#x}, mesh={:#x})", hull_vtable, mesh_vtable);
        return;
    }

    std::vector<triangle> fresh;
    fresh.reserve(16384);
    std::int32_t bodies_with_nodes{};
    std::int32_t bodies_unreadable{};
    // Simulation types of the bodies seen; logged when nothing was extracted, to re-derive a drifted layout.
    std::unordered_map<std::uint32_t, std::int32_t> simulation_types{};
    for (std::int32_t body_index = 0; body_index < world->body_count && !stop.stop_requested(); ++body_index)
    {
        if (fresh.size() > 4 * detail::remote_array_budget / sizeof(triangle))
            throw std::length_error("live geometry exceeds memory budget");
        const auto body =
            world->body_array + static_cast<std::uintptr_t>(body_index) * detail::world_layout::body_stride;
        const auto bvh_root = memory.process.load<std::int32_t>(body + detail::world_layout::body_bvh_root);
        const auto bvh_nodes = memory.process.load<std::uintptr_t>(body + detail::world_layout::body_bvh_nodes);
        if (!bvh_nodes)
            continue;
        ++bodies_with_nodes;
        ++simulation_types[memory.process.load<std::uint32_t>(body + detail::world_layout::body_simulation_type)];

        if (bvh_root < 0)
        {
            if (const auto shape = memory.process.load<std::uintptr_t>(body + detail::world_layout::body_single_shape))
                detail::append_shape_triangles(shape, hull_vtable, mesh_vtable, global_table, fresh, stop);
            continue;
        }
        const auto shapes = detail::body_leaf_shapes(body, bvh_root, bvh_nodes, stop);
        if (!shapes)
        {
            ++bodies_unreadable;
            continue;
        }
        for (const auto shape : *shapes)
        {
            if (stop.stop_requested())
                break;
            detail::append_shape_triangles(shape, hull_vtable, mesh_vtable, global_table, fresh, stop);
        }
    }
    if (stop.stop_requested())
        return;

    memory.log.info("[bvh] bodies={} with_nodes={} unreadable={} triangles={}", world->body_count, bodies_with_nodes,
                    bodies_unreadable, fresh.size());
    if (fresh.empty() && bodies_with_nodes > 0)
    {
        std::string histogram{};
        for (const auto& [type, count] : simulation_types)
            histogram += std::format("{:#x}:{} ", type, count);
        memory.log.warning("[bvh] no triangles extracted; body simulation types: {}", histogram);
    }

    // The map changed while it was being read: this geometry belongs to neither map.
    if (memory.process.load<std::uintptr_t>(world->holder) != world->world)
    {
        memory.log.info("[bvh] physics world replaced during the read; geometry discarded");
        return;
    }
    collision_world built;
    built.m_triangles = std::move(fresh);
    built.m_world_triangle_count = built.m_triangles.size();
    built.rebuild_accel();
    if (stop.stop_requested())
        return;
    replace_with(built);
}

void collision_world::replace_with(collision_world& built)
{
    if (this == &built)
        return;
    std::scoped_lock lock(m_mutex, built.m_mutex);
    std::swap(m_triangles, built.m_triangles);
    std::swap(m_world_triangle_count, built.m_world_triangle_count);
    std::swap(m_map_name, built.m_map_name);
    std::swap(m_entity_triangle_count, built.m_entity_triangle_count);
    std::swap(m_nodes, built.m_nodes);
    std::swap(m_indices, built.m_indices);
    std::swap(m_tri_bounds, built.m_tri_bounds);
    std::swap(m_centroids, built.m_centroids);
    std::swap(m_world_render, built.m_world_render);
    std::swap(m_entity_render, built.m_entity_render);
    std::swap(m_entity_collision, built.m_entity_collision);
    std::swap(m_entity_state_hash, built.m_entity_state_hash);
    ++m_world_render_revision;
    ++m_entity_render_revision;
    m_geometry_revision.fetch_add(1, std::memory_order_release);
}

void collision_world::clear()
{
    collision_world retired;
    replace_with(retired);
}

std::optional<collision_world::ray_query> collision_world::make_ray_query(const foundation::vec3& start,
                                                                          const foundation::vec3& end)
{
    const auto displacement = end - start;
    const auto length = displacement.length();
    if (length < 1e-8f)
        return std::nullopt;

    ray_query query{};
    query.origin = start;
    query.direction = displacement / length;
    query.length = length;
    query.origin_components[0] = start.x;
    query.origin_components[1] = start.y;
    query.origin_components[2] = start.z;

    const float direction[3]{query.direction.x, query.direction.y, query.direction.z};
    for (int axis = 0; axis < 3; ++axis)
    {
        query.inverse_direction[axis] =
            std::abs(direction[axis]) > 1e-8f ? 1.0f / direction[axis] : std::copysign(1e12f, direction[axis]);
    }
    return query;
}

std::optional<collision_world::ray_contact> collision_world::intersect_triangle(const ray_query& ray,
                                                                                const triangle& candidate,
                                                                                std::int32_t triangle_index,
                                                                                float distance_limit)
{
    if ((candidate.surface.contents & grenade_clip_contents) != 0)
        return std::nullopt;
    constexpr float parallel_epsilon = 1e-8f;
    constexpr float contact_epsilon = 1e-5f;
    const auto edge_a = candidate.v1 - candidate.v0;
    const auto edge_b = candidate.v2 - candidate.v0;
    const auto determinant_vector = ray.direction.cross(edge_b);
    const auto determinant = edge_a.dot(determinant_vector);
    if (std::abs(determinant) < parallel_epsilon)
        return std::nullopt;

    const auto inverse_determinant = 1.0f / determinant;
    const auto from_vertex = ray.origin - candidate.v0;
    const auto barycentric_u = from_vertex.dot(determinant_vector) * inverse_determinant;
    if (barycentric_u < 0.0f || barycentric_u > 1.0f)
        return std::nullopt;

    const auto barycentric_vector = from_vertex.cross(edge_a);
    const auto barycentric_v = ray.direction.dot(barycentric_vector) * inverse_determinant;
    if (barycentric_v < 0.0f || barycentric_u + barycentric_v > 1.0f)
        return std::nullopt;

    const auto distance = edge_b.dot(barycentric_vector) * inverse_determinant;
    if (distance <= contact_epsilon || distance >= distance_limit)
        return std::nullopt;

    auto normal = edge_a.cross(edge_b);
    const auto normal_length = normal.length();
    if (normal_length > parallel_epsilon)
        normal = normal / normal_length;
    else
        normal = {};

    return ray_contact{.distance = distance,
                       .position = ray.origin + ray.direction * distance,
                       .normal = normal,
                       .surface = candidate.surface,
                       .triangle_index = triangle_index,
                       .entering = normal.dot(ray.direction) < 0.0f,
                       .solid_id = candidate.solid_id};
}

void collision_world::traverse_ray(const ray_query& ray, std::int32_t excluded_triangle, bool nearest_only,
                                   std::vector<ray_contact>& contacts) const
{
    if (m_nodes.empty())
        return;

    static thread_local std::vector<std::int32_t> pending{};
    pending.clear();
    if (pending.capacity() < 64)
        pending.reserve(64);
    pending.push_back(0);
    auto distance_limit = ray.length;

    while (!pending.empty())
    {
        const auto node_index = pending.back();
        pending.pop_back();
        if (node_index < 0 || static_cast<std::size_t>(node_index) >= m_nodes.size())
            continue;

        const auto& node = m_nodes[node_index];
        if (!node.bounds.intersects_ray(ray.origin_components, ray.inverse_direction, distance_limit))
            continue;

        if (node.left >= 0)
        {
            if (node.right >= 0)
                pending.push_back(node.right);
            pending.push_back(node.left);
            continue;
        }

        const auto first = std::max(node.tri_start, 0);
        const auto last =
            std::min<std::int32_t>(node.tri_start + node.tri_count, static_cast<std::int32_t>(m_indices.size()));
        for (auto slot = first; slot < last; ++slot)
        {
            const auto triangle_index = m_indices[slot];
            if (triangle_index == excluded_triangle || triangle_index < 0 ||
                static_cast<std::size_t>(triangle_index) >= m_triangles.size())
            {
                continue;
            }

            const auto contact = intersect_triangle(ray, m_triangles[triangle_index], triangle_index, distance_limit);
            if (!contact)
                continue;

            if (nearest_only)
            {
                contacts.assign(1, *contact);
                distance_limit = contact->distance;
            }
            else
            {
                contacts.push_back(*contact);
            }
        }
    }
}

void collision_world::traverse_sphere(const ray_query& ray, const float radius, std::int32_t excluded_triangle,
                                      std::vector<ray_contact>& contacts) const
{
    if (m_nodes.empty() || radius <= 0.0f)
        return;
    static thread_local std::vector<std::int32_t> pending{};
    pending.clear();
    if (pending.capacity() < 64)
        pending.reserve(64);
    pending.push_back(0);
    auto distance_limit = ray.length;

    while (!pending.empty())
    {
        const auto node_index = pending.back();
        pending.pop_back();
        if (node_index < 0 || static_cast<std::size_t>(node_index) >= m_nodes.size())
            continue;
        const auto& node = m_nodes[node_index];
        if (!node.bounds.intersects_ray(ray.origin_components, ray.inverse_direction, distance_limit, radius))
            continue;
        if (node.left >= 0)
        {
            if (node.right >= 0)
                pending.push_back(node.right);
            pending.push_back(node.left);
            continue;
        }

        const auto first = std::max(node.tri_start, 0);
        const auto last =
            std::min<std::int32_t>(node.tri_start + node.tri_count, static_cast<std::int32_t>(m_indices.size()));
        for (auto slot = first; slot < last; ++slot)
        {
            const auto triangle_index = m_indices[slot];
            if (triangle_index == excluded_triangle || triangle_index < 0 ||
                static_cast<std::size_t>(triangle_index) >= m_triangles.size())
                continue;
            const auto& triangle = m_triangles[triangle_index];
            const auto contact = simulation::geometry::sweep_sphere_triangle(
                ray.origin, ray.direction, distance_limit, radius, triangle.v0, triangle.v1, triangle.v2);
            if (!contact)
                continue;
            contacts.assign(1, ray_contact{.distance = contact->distance,
                                           .position = contact->center,
                                           .normal = contact->normal,
                                           .surface = triangle.surface,
                                           .triangle_index = triangle_index,
                                           .entering = true,
                                           .solid_id = triangle.solid_id});
            distance_limit = contact->distance;
        }
    }
}

void collision_world::traverse_hull(const ray_query& ray, const foundation::vec3& half_extents,
                                    std::int32_t excluded_triangle, std::vector<ray_contact>& contacts) const
{
    if (m_nodes.empty())
        return;
    static thread_local std::vector<std::int32_t> pending{};
    pending.clear();
    if (pending.capacity() < 64)
        pending.reserve(64);
    pending.push_back(0);
    auto distance_limit = ray.length;
    const auto broadphase_padding = std::max({half_extents.x, half_extents.y, half_extents.z});

    while (!pending.empty())
    {
        const auto node_index = pending.back();
        pending.pop_back();
        if (node_index < 0 || static_cast<std::size_t>(node_index) >= m_nodes.size())
            continue;
        const auto& node = m_nodes[node_index];
        if (!node.bounds.intersects_ray(ray.origin_components, ray.inverse_direction, distance_limit,
                                        broadphase_padding))
            continue;
        if (node.left >= 0)
        {
            if (node.right >= 0)
                pending.push_back(node.right);
            pending.push_back(node.left);
            continue;
        }

        const auto first = std::max(node.tri_start, 0);
        const auto last =
            std::min<std::int32_t>(node.tri_start + node.tri_count, static_cast<std::int32_t>(m_indices.size()));
        for (auto slot = first; slot < last; ++slot)
        {
            const auto triangle_index = m_indices[slot];
            if (triangle_index == excluded_triangle || triangle_index < 0 ||
                static_cast<std::size_t>(triangle_index) >= m_triangles.size())
                continue;
            const auto& triangle = m_triangles[triangle_index];
            const auto contact = simulation::geometry::sweep_aabb_triangle(
                ray.origin, ray.direction, distance_limit, half_extents, triangle.v0, triangle.v1, triangle.v2);
            if (!contact)
                continue;
            contacts.assign(1, ray_contact{.distance = contact->distance,
                                           .position = contact->center,
                                           .normal = contact->normal,
                                           .surface = triangle.surface,
                                           .triangle_index = triangle_index,
                                           .entering = true,
                                           .solid_id = triangle.solid_id});
            distance_limit = contact->distance;
        }
    }
}

collision_world::trace_result collision_world::trace_ray(const foundation::vec3& start, const foundation::vec3& end,
                                                         std::int32_t exclude_tri) const
{
    trace_result result{};
    result.end_pos = end;
    const auto ray = make_ray_query(start, end);
    if (!ray)
        return result;

    std::shared_ptr<const collision_world> entity_collision{};
    std::shared_lock lock(m_mutex);
    static thread_local std::vector<ray_contact> contacts{};
    contacts.clear();
    if (contacts.capacity() < 1)
        contacts.reserve(1);
    traverse_ray(*ray, exclude_tri, true, contacts);
    entity_collision = m_entity_collision;
    if (!contacts.empty())
    {
        const auto& contact = contacts.front();
        result.hit = true;
        result.fraction = contact.distance / ray->length;
        result.distance = contact.distance;
        result.end_pos = contact.position;
        result.normal = contact.normal;
        result.surface = contact.surface;
        result.triangle_index = contact.triangle_index;
    }
    lock.unlock();

    if (entity_collision)
    {
        const auto entity_exclusion = exclude_tri < -1 ? -exclude_tri - 2 : -1;
        auto dynamic = entity_collision->trace_ray(start, end, entity_exclusion);
        if (dynamic.hit && (!result.hit || dynamic.distance < result.distance))
        {
            dynamic.triangle_index = -dynamic.triangle_index - 2;
            return dynamic;
        }
    }
    return result;
}

collision_world::trace_result collision_world::sweep_sphere(const foundation::vec3& start, const foundation::vec3& end,
                                                            const float radius, const std::int32_t exclude_tri) const
{
    trace_result result{};
    result.end_pos = end;
    const auto ray = make_ray_query(start, end);
    if (!ray || !std::isfinite(radius) || radius <= 0.0f)
        return result;

    std::shared_ptr<const collision_world> entity_collision{};
    std::shared_lock lock(m_mutex);
    static thread_local std::vector<ray_contact> contacts{};
    contacts.clear();
    if (contacts.capacity() < 1)
        contacts.reserve(1);
    traverse_sphere(*ray, radius, exclude_tri, contacts);
    entity_collision = m_entity_collision;
    if (!contacts.empty())
    {
        const auto& contact = contacts.front();
        result.hit = true;
        result.fraction = contact.distance / ray->length;
        result.distance = contact.distance;
        result.end_pos = contact.position;
        result.normal = contact.normal;
        result.surface = contact.surface;
        result.triangle_index = contact.triangle_index;
    }
    lock.unlock();

    if (entity_collision)
    {
        const auto entity_exclusion = exclude_tri < -1 ? -exclude_tri - 2 : -1;
        auto dynamic = entity_collision->sweep_sphere(start, end, radius, entity_exclusion);
        if (dynamic.hit && (!result.hit || dynamic.distance < result.distance))
        {
            dynamic.triangle_index = -dynamic.triangle_index - 2;
            return dynamic;
        }
    }
    return result;
}

collision_world::trace_result collision_world::sweep_hull(const foundation::vec3& start, const foundation::vec3& end,
                                                          const foundation::vec3& half_extents,
                                                          const std::int32_t exclude_tri) const
{
    trace_result result{};
    result.end_pos = end;
    const auto ray = make_ray_query(start, end);
    if (!ray || !std::isfinite(half_extents.x) || !std::isfinite(half_extents.y) || !std::isfinite(half_extents.z) ||
        half_extents.x <= 0.0f || half_extents.y <= 0.0f || half_extents.z <= 0.0f)
        return result;

    std::shared_ptr<const collision_world> entity_collision{};
    std::shared_lock lock(m_mutex);
    static thread_local std::vector<ray_contact> contacts{};
    contacts.clear();
    if (contacts.capacity() < 1)
        contacts.reserve(1);
    traverse_hull(*ray, half_extents, exclude_tri, contacts);
    entity_collision = m_entity_collision;
    if (!contacts.empty())
    {
        const auto& contact = contacts.front();
        result.hit = true;
        result.fraction = contact.distance / ray->length;
        result.distance = contact.distance;
        result.end_pos = contact.position;
        result.normal = contact.normal;
        result.surface = contact.surface;
        result.triangle_index = contact.triangle_index;
    }
    lock.unlock();

    if (entity_collision)
    {
        const auto entity_exclusion = exclude_tri < -1 ? -exclude_tri - 2 : -1;
        auto dynamic = entity_collision->sweep_hull(start, end, half_extents, entity_exclusion);
        if (dynamic.hit && (!result.hit || dynamic.distance < result.distance))
        {
            dynamic.triangle_index = -dynamic.triangle_index - 2;
            return dynamic;
        }
    }
    return result;
}

std::vector<collision_world::hit_entry> collision_world::trace_ray_all(const foundation::vec3& start,
                                                                       const foundation::vec3& end) const
{
    const auto ray = make_ray_query(start, end);
    if (!ray)
        return {};

    std::shared_ptr<const collision_world> entity_collision{};
    std::shared_lock lock(m_mutex);
    static thread_local std::vector<ray_contact> contacts{};
    contacts.clear();
    traverse_ray(*ray, -1, false, contacts);
    entity_collision = m_entity_collision;

    std::vector<hit_entry> hits;
    hits.reserve(contacts.size());
    for (const auto& contact : contacts)
    {
        hits.push_back(hit_entry{.distance = contact.distance,
                                 .fraction = contact.distance / ray->length,
                                 .position = contact.position,
                                 .normal = contact.normal,
                                 .surface = contact.surface,
                                 .triangle_index = contact.triangle_index,
                                 .is_enter = contact.entering,
                                 .solid_id = contact.solid_id});
    }
    lock.unlock();
    if (entity_collision)
    {
        auto dynamic = entity_collision->trace_ray_all(start, end);
        for (auto& hit : dynamic)
        {
            hit.triangle_index = -hit.triangle_index - 2;
            hits.push_back(hit);
        }
    }
    std::ranges::sort(hits, {}, &hit_entry::distance);
    return hits;
}

collision_world::segment_build_result collision_world::build_segments(std::vector<hit_entry> hits,
                                                                      float ray_length) const
{
    return collision_detail::build_segments(std::move(hits), ray_length);
}

std::vector<collision_world::triangle> collision_world::triangles() const
{
    std::shared_lock lock(m_mutex);
    auto result = m_triangles;
    const auto entities = m_entity_collision;
    lock.unlock();
    if (entities)
    {
        auto dynamic = entities->triangles();
        result.insert(result.end(), std::make_move_iterator(dynamic.begin()), std::make_move_iterator(dynamic.end()));
    }
    return result;
}

collision_world::render_snapshot collision_world::render_geometry() const
{
    std::shared_lock lock(m_mutex);
    return {.world = m_world_render,
            .entities = m_entity_render,
            .world_revision = m_world_render_revision,
            .entity_revision = m_entity_render_revision};
}

std::size_t collision_world::count() const
{
    std::shared_lock lock(m_mutex);
    return m_triangles.size() + m_entity_triangle_count;
}

bool collision_world::valid() const
{
    std::shared_lock lock(m_mutex);
    return !m_triangles.empty();
}

void collision_world::aabb::expand(const foundation::vec3& point)
{
    mins[0] = std::min(mins[0], point.x);
    mins[1] = std::min(mins[1], point.y);
    mins[2] = std::min(mins[2], point.z);
    maxs[0] = std::max(maxs[0], point.x);
    maxs[1] = std::max(maxs[1], point.y);
    maxs[2] = std::max(maxs[2], point.z);
}

void collision_world::aabb::expand(const aabb& other)
{
    for (int axis = 0; axis < 3; ++axis)
    {
        mins[axis] = std::min(mins[axis], other.mins[axis]);
        maxs[axis] = std::max(maxs[axis], other.maxs[axis]);
    }
}

int collision_world::aabb::longest_axis() const
{
    const std::array extents{maxs[0] - mins[0], maxs[1] - mins[1], maxs[2] - mins[2]};
    return static_cast<int>(std::distance(extents.begin(), std::ranges::max_element(extents)));
}

bool collision_world::aabb::intersects_ray(const float origin[3], const float inverse_direction[3], float max_distance,
                                           const float padding) const
{
    float entrance{};
    auto exit = max_distance;
    for (int axis = 0; axis < 3; ++axis)
    {
        auto near_distance = (mins[axis] - padding - origin[axis]) * inverse_direction[axis];
        auto far_distance = (maxs[axis] + padding - origin[axis]) * inverse_direction[axis];
        if (near_distance > far_distance)
            std::swap(near_distance, far_distance);

        entrance = std::max(entrance, near_distance);
        exit = std::min(exit, far_distance);
        if (entrance > exit)
            return false;
    }
    return true;
}

void collision_world::rebuild_accel(const bool rebuild_world_render)
{
    m_nodes.clear();
    m_indices.clear();
    m_tri_bounds.clear();
    m_centroids.clear();

    const auto triangle_count = static_cast<std::int32_t>(m_triangles.size());
    if (triangle_count == 0)
    {
        m_world_render.reset();
        m_entity_render.reset();
        return;
    }

    m_indices.resize(triangle_count);
    std::iota(m_indices.begin(), m_indices.end(), 0);
    m_tri_bounds.resize(triangle_count);
    m_centroids.resize(static_cast<std::size_t>(triangle_count) * 3);

    for (std::int32_t index = 0; index < triangle_count; ++index)
    {
        auto& bounds = m_tri_bounds[index];
        bounds.expand(m_triangles[index].v0);
        bounds.expand(m_triangles[index].v1);
        bounds.expand(m_triangles[index].v2);
        const auto base = static_cast<std::size_t>(index) * 3;
        for (int axis = 0; axis < 3; ++axis)
            m_centroids[base + axis] = (bounds.mins[axis] + bounds.maxs[axis]) * 0.5f;
    }

    m_nodes.reserve(detail::bvh_node_capacity(triangle_count, k_max_leaf_tris));
    build_recursive(0, triangle_count, 0);

    m_tri_bounds.clear();
    m_tri_bounds.shrink_to_fit();
    m_centroids.clear();
    m_centroids.shrink_to_fit();

    rebuild_render_geometry(rebuild_world_render, true);
}

void collision_world::rebuild_render_geometry(const bool rebuild_world, const bool rebuild_entities)
{
    using mesh = render_snapshot::mesh;
    struct vertex_key
    {
        std::uint32_t x{}, y{}, z{};
        [[nodiscard]] bool operator==(const vertex_key& other) const noexcept = default;
    };
    struct vertex_hash
    {
        [[nodiscard]] std::size_t operator()(const vertex_key& value) const noexcept
        {
            auto hash = static_cast<std::size_t>(value.x) * 0x9E3779B185EBCA87ull;
            hash ^= static_cast<std::size_t>(value.y) + 0x9E3779B9u + (hash << 6) + (hash >> 2);
            hash ^= static_cast<std::size_t>(value.z) + 0x85EBCA6Bu + (hash << 6) + (hash >> 2);
            return hash;
        }
    };

    const auto world_end = m_world_triangle_count > 0 && m_world_triangle_count <= m_triangles.size()
                               ? m_world_triangle_count
                               : m_triangles.size();
    const auto ordered = m_indices.size() == m_triangles.size();

    const auto build_mesh = [&](const std::size_t first_triangle,
                                const std::size_t end_triangle) -> std::shared_ptr<const mesh>
    {
        if (first_triangle >= end_triangle)
            return {};

        auto result = std::make_shared<mesh>();
        result->indices.reserve((end_triangle - first_triangle) * 3);
        result->vertices.reserve(std::min<std::size_t>((end_triangle - first_triangle) * 3, 1'000'000));
        std::unordered_map<vertex_key, std::uint32_t, vertex_hash> vertex_map{};
        vertex_map.reserve(result->vertices.capacity());

        constexpr std::size_t k_batch_triangles{256};
        mesh::batch batch{};
        batch.mins = {1e12f, 1e12f, 1e12f};
        batch.maxs = {-1e12f, -1e12f, -1e12f};
        batch.first_index = 0;
        std::size_t batch_triangles{};

        const auto flush_batch = [&]
        {
            if (batch.index_count == 0)
                return;
            result->batches.push_back(batch);
            batch = {};
            batch.mins = {1e12f, 1e12f, 1e12f};
            batch.maxs = {-1e12f, -1e12f, -1e12f};
            batch.first_index = static_cast<std::uint32_t>(result->indices.size());
            batch_triangles = 0;
        };

        const auto append_vertex = [&](const foundation::vec3& vertex)
        {
            const vertex_key key{std::bit_cast<std::uint32_t>(vertex.x), std::bit_cast<std::uint32_t>(vertex.y),
                                 std::bit_cast<std::uint32_t>(vertex.z)};
            const auto [it, inserted] =
                vertex_map.try_emplace(key, static_cast<std::uint32_t>(result->vertices.size()));
            if (inserted)
                result->vertices.push_back(vertex);
            result->indices.push_back(it->second);
            ++batch.index_count;
            batch.mins.x = std::min(batch.mins.x, vertex.x);
            batch.mins.y = std::min(batch.mins.y, vertex.y);
            batch.mins.z = std::min(batch.mins.z, vertex.z);
            batch.maxs.x = std::max(batch.maxs.x, vertex.x);
            batch.maxs.y = std::max(batch.maxs.y, vertex.y);
            batch.maxs.z = std::max(batch.maxs.z, vertex.z);
        };

        for (std::size_t slot = 0; slot < m_triangles.size(); ++slot)
        {
            const auto triangle_index = ordered ? static_cast<std::size_t>(m_indices[slot]) : slot;
            if (triangle_index < first_triangle || triangle_index >= end_triangle)
                continue;
            const auto& triangle = m_triangles[triangle_index];
            if ((triangle.surface.contents & grenade_clip_contents) != 0)
                continue;
            append_vertex(triangle.v0);
            append_vertex(triangle.v1);
            append_vertex(triangle.v2);
            if (++batch_triangles >= k_batch_triangles)
                flush_batch();
        }
        flush_batch();
        return result;
    };

    if (rebuild_world)
        m_world_render = build_mesh(0, world_end);
    if (rebuild_entities)
        m_entity_render = build_mesh(world_end, m_triangles.size());
}

std::int32_t collision_world::build_recursive(std::int32_t start, std::int32_t end, std::int32_t depth)
{
    const auto node_index = static_cast<std::int32_t>(m_nodes.size());
    m_nodes.emplace_back();
    for (auto slot = start; slot < end; ++slot)
        m_nodes[node_index].bounds.expand(m_tri_bounds[m_indices[slot]]);

    const auto count = end - start;
    if (count <= k_max_leaf_tris || depth >= k_max_depth)
    {
        m_nodes[node_index].tri_start = start;
        m_nodes[node_index].tri_count = count;
        return node_index;
    }

    aabb centroid_bounds{};
    for (auto slot = start; slot < end; ++slot)
    {
        const auto base = static_cast<std::size_t>(m_indices[slot]) * 3;
        centroid_bounds.expand(foundation::vec3{m_centroids[base], m_centroids[base + 1], m_centroids[base + 2]});
    }

    const auto axis = centroid_bounds.longest_axis();
    const auto split = start + count / 2;
    std::nth_element(m_indices.begin() + start, m_indices.begin() + split, m_indices.begin() + end,
                     [&](std::int32_t lhs, std::int32_t rhs)
                     {
                         return m_centroids[static_cast<std::size_t>(lhs) * 3 + axis] <
                                m_centroids[static_cast<std::size_t>(rhs) * 3 + axis];
                     });

    m_nodes[node_index].left = build_recursive(start, split, depth + 1);
    m_nodes[node_index].right = build_recursive(split, end, depth + 1);
    return node_index;
}

} // namespace simulation
