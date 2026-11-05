#include <stdafx.hpp>
#include <atomic>
#include <game/state/runtime.hpp>
#include <game/memory/addresses.hpp>
#include <game/memory/memory_services.hpp>

namespace game
{
namespace
{
// CViewRender: the current view (origin, angles, field of view) sits at +0x10.
constexpr std::uintptr_t k_view_state_offset = 0x10;
} // namespace

void camera_state::bind(const memory_services& services, const address_catalog& addresses) noexcept
{
    m_memory = &services;
    m_addresses = &addresses;
}

std::uintptr_t camera_state::view_render_instance() const
{
    if (const auto known = m_view_render.load(std::memory_order_acquire))
        return known;
    if (!m_memory)
        return 0;
    const auto found = m_memory->process.locate_vtable_object(m_memory->modules.client, "CViewRender");
    if (found)
        m_view_render.store(found, std::memory_order_release);
    return found;
}

bool camera_state::sample(foundation::vec3& origin, foundation::vec3& angles) const
{
    const auto view_render = view_render_instance();
    if (!view_render)
    {
        return false;
    }

    const auto view_state = view_render + k_view_state_offset;
    struct camera_sample
    {
        foundation::vec3 origin{};
        foundation::vec3 angles{};
    } sample{};
    static_assert(sizeof(camera_sample) == 0x18);

    if (!m_memory->process.copy(view_state, &sample, sizeof(sample)))
        return false;
    origin = sample.origin;
    angles = sample.angles;
    return std::isfinite(origin.x) && std::isfinite(origin.y) && std::isfinite(origin.z) && std::isfinite(angles.x) &&
           std::isfinite(angles.y) && std::isfinite(angles.z);
}

foundation::vec2 camera_state::project(const foundation::vec3& world_pos) const
{
    const auto& m = m_frame_matrix;

    if (m[3][3] == 0.0f)
    {
        return {static_cast<float>(k_invalid), static_cast<float>(k_invalid)};
    }

    const auto w = m[3][0] * world_pos.x + m[3][1] * world_pos.y + m[3][2] * world_pos.z + m[3][3];

    constexpr auto k_near_plane{7.0f};
    if (!(w >= k_near_plane))
    {
        return {static_cast<float>(k_invalid), static_cast<float>(k_invalid)};
    }

    const auto x = m[0][0] * world_pos.x + m[0][1] * world_pos.y + m[0][2] * world_pos.z + m[0][3];
    const auto y = m[1][0] * world_pos.x + m[1][1] * world_pos.y + m[1][2] * world_pos.z + m[1][3];

    const auto inv_w = 1.0f / w;
    const auto ndc_x = x * inv_w;
    const auto ndc_y = y * inv_w;

    constexpr auto k_ndc_limit{64.0f};
    if (!std::isfinite(ndc_x) || !std::isfinite(ndc_y) || std::abs(ndc_x) > k_ndc_limit ||
        std::abs(ndc_y) > k_ndc_limit)
    {
        return {static_cast<float>(k_invalid), static_cast<float>(k_invalid)};
    }

    return {m_frame_width * 0.5f * (1.0f + ndc_x), m_frame_height * 0.5f * (1.0f - ndc_y)};
}

bool camera_state::sample_presentation(presentation_camera_sample& sample) const
{
    if (!m_memory || !m_addresses)
        return false;
    if (!m_memory->process.copy((*m_addresses).view_matrix, &sample.matrix, sizeof(sample.matrix)))
    {
        return false;
    }

    const auto view_render = view_render_instance();
    if (!view_render)
    {
        return false;
    }
    struct view_sample
    {
        foundation::vec3 origin{};
        foundation::vec3 angles{};
        float fov{};
    } view{};
    static_assert(sizeof(view_sample) == 0x1c);
    if (!m_memory->process.copy(view_render + k_view_state_offset, &view, sizeof(view)))
    {
        return false;
    }

    sample.origin = view.origin;
    sample.angles = view.angles;
    sample.fov = view.fov;
    return std::isfinite(sample.origin.x) && std::isfinite(sample.origin.y) && std::isfinite(sample.origin.z) &&
           std::isfinite(sample.angles.x) && std::isfinite(sample.angles.y) && std::isfinite(sample.angles.z) &&
           std::isfinite(sample.fov) && sample.fov > 0.0f && sample.fov < 180.0f && [&sample]
    {
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                if (!std::isfinite(sample.matrix[r][c]))
                    return false;
        return true;
    }();
}

void camera_state::begin_presentation_frame(const presentation_camera_sample& sample, const std::uint32_t width,
                                            const std::uint32_t height)
{
    m_frame_matrix = sample.matrix;
    m_frame_width = static_cast<float>(width);
    m_frame_height = static_cast<float>(height);
    std::unique_lock lock(m_state_mutex);
    m_matrix = sample.matrix;
    m_origin = sample.origin;
    m_angles = sample.angles;
    m_fov = sample.fov;
}

} // namespace game
