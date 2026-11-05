#include <stdafx.hpp>
#include <foundation/identity.hpp>
#include <render/presentation.hpp>
#include <game/state/services.hpp>
#include <game/memory/bound_services.hpp>
#include <game/memory/remote_pointer.hpp>
#include <features/visuals/visuals.hpp>
#include <game/memory/compatibility.hpp>
#include <features/visuals/radar_geometry.hpp>

namespace
{
[[nodiscard]] bool radar_active()
{
    const auto runtime_settings = config::get_runtime_snapshot();
    return runtime_settings->visual.m_radar.active();
}

struct rect_t
{
    float x{};
    float y{};
    float w{};
    float h{};

    [[nodiscard]] bool valid(float display_w, float display_h) const
    {
        return std::isfinite(x) && std::isfinite(y) && std::isfinite(w) && std::isfinite(h) && w >= 64.0f &&
               h >= 64.0f && w <= display_w * 2.5f && h <= display_h * 2.5f;
    }
};

struct panel_info
{
    std::uintptr_t wrapper{};
    std::uintptr_t ui{};
    rect_t image{};
    rect_t viewport{};
    float scale_x{}, scale_y{};
};

struct overview_info
{
    std::uintptr_t object{};
    std::uintptr_t vtable{};
    panel_info panel{};
    foundation::vec2 map_origin{};
    float map_scale{};
    float map_size{};
    float canvas_size{};
    float viewport_size{};
    float world_to_pixels{};
    foundation::vec2 map_center{};
    float radar_angle{};
    float alternate_scale{};
    bool alternate_transform{};
    rect_t marker_space{};
    float layout_width{};
    float layout_height{};
};

struct panel_chain_node
{
    std::uintptr_t ui{};
    float offset_x{};
    float offset_y{};
    float width{};
    float height{};
};

struct panel_chain
{
    std::array<panel_chain_node, 32> nodes{};
    std::array<float, 32> accumulated_x{};
    std::array<float, 32> accumulated_y{};
    std::size_t count{};
    float scale_x{};
    float scale_y{};
};

[[nodiscard]] rect_t scale_layout_rect(const rect_t& rect, const overview_info& overview, const float display_width,
                                       const float display_height)
{
    if (overview.layout_width <= 0.0f || overview.layout_height <= 0.0f)
        return rect;
    const auto scale_x = display_width / overview.layout_width;
    const auto scale_y = display_height / overview.layout_height;
    return {rect.x * scale_x, rect.y * scale_y, rect.w * scale_x, rect.h * scale_y};
}

std::uintptr_t hud_radar()
{
    return game::compatibility::hud_element("CCSGO_HudRadar");
}

[[nodiscard]] bool read_panel_chain(std::uintptr_t ui, float display_w, float display_h, panel_chain& out)
{
    out = {};
    if (!game::is_user_pointer(ui))
    {
        return false;
    }

    std::uintptr_t current = ui;
    for (; out.count < out.nodes.size() && game::is_user_pointer(current);)
    {
        const auto layout = game::compatibility::panel(current);
        if (!layout)
            return false;
        constexpr std::uintptr_t fields_begin{0x100};
        constexpr std::uintptr_t fields_end{0x810};
        std::array<std::byte, fields_end - fields_begin> fields{};
        if (!game::bound_memory().process.copy(current + fields_begin, fields.data(), fields.size()))
        {
            return false;
        }
        const auto field = [&]<typename T>(const std::uintptr_t offset)
        {
            T value{};
            std::memcpy(&value, fields.data() + offset - fields_begin, sizeof(value));
            return value;
        };
        const auto ox = field.template operator()<float>(layout.x);
        const auto oy = field.template operator()<float>(layout.y);
        if (!std::isfinite(ox) || !std::isfinite(oy) || std::abs(ox) > 20000.0f || std::abs(oy) > 20000.0f)
        {
            return false;
        }

        const auto visibility = field.template operator()<std::uint8_t>(layout.visible);
        if ((visibility & 0x08) == 0)
        {
            return false;
        }
        out.nodes[out.count++] = {
            current,
            ox,
            oy,
            field.template operator()<float>(layout.width),
            field.template operator()<float>(layout.height)
        };

        const auto parent = game::bound_memory().process.load<std::uintptr_t>(current + 0x18);
        if (!game::is_user_pointer(parent) || parent == current)
        {
            break;
        }
        for (std::size_t i = 0; i < out.count; ++i)
            if (out.nodes[i].ui == parent)
                return false;
        if (out.count == out.nodes.size())
            return false;
        current = parent;
    }

    if (out.count == 0)
        return false;
    const auto& root = out.nodes[out.count - 1];
    const auto root_width = root.width;
    const auto root_height = root.height;
    if (!std::isfinite(root_width) || !std::isfinite(root_height) || root_width < 320.0f || root_height < 240.0f)
    {
        return false;
    }

    out.scale_x = display_w / root_width;
    out.scale_y = display_h / root_height;
    float x{};
    float y{};
    for (auto index = out.count; index-- > 0;)
    {
        x += out.nodes[index].offset_x;
        y += out.nodes[index].offset_y;
        out.accumulated_x[index] = x;
        out.accumulated_y[index] = y;
    }
    return true;
}

[[nodiscard]] rect_t chain_rect(const panel_chain& chain, const std::size_t index)
{
    if (index >= chain.count)
        return {};
    const auto& node = chain.nodes[index];
    return {chain.accumulated_x[index] * chain.scale_x, chain.accumulated_y[index] * chain.scale_y,
            node.width * chain.scale_x, node.height * chain.scale_y};
}

[[nodiscard]] panel_info inspect_panel(std::uintptr_t wrapper, float display_w, float display_h)
{
    panel_info result{};
    result.wrapper = wrapper;
    result.ui = game::bound_memory().process.load<std::uintptr_t>(wrapper + 0x8);
    panel_chain chain{};
    if (!read_panel_chain(result.ui, display_w, display_h, chain))
        return {};
    result.scale_x = chain.scale_x;
    result.scale_y = chain.scale_y;
    result.image = chain_rect(chain, 0);
    if (!result.image.valid(display_w, display_h))
    {
        return {};
    }

    // Shape is measured in Panorama layout units, before 4:3 stretch or DPI scaling.
    for (std::size_t depth = 0; depth < std::min<std::size_t>(24, chain.count); ++depth)
    {
        const auto candidate = chain_rect(chain, depth);
        if (candidate.valid(display_w, display_h))
        {
            const auto& node = chain.nodes[depth];
            if (features::visuals::detail::square_radar_layout(node.width, node.height) &&
                candidate.w <= result.image.w + 2.0f && candidate.h <= result.image.h + 2.0f)
            {
                result.viewport = candidate;
            }
        }
    }

    if (!result.viewport.valid(display_w, display_h))
    {
        result.viewport = result.image;
    }
    return result;
}

[[nodiscard]] std::uintptr_t image_panel_vtable()
{
    static const auto value =
        game::bound_memory().process.locate_vtable(game::bound_memory().modules.client, "CImagePanel@panorama");
    return value;
}

[[nodiscard]] panel_info find_radar_panel(std::uintptr_t object, float expected_size, float display_w, float display_h)
{
    panel_info best{};
    float best_size_error = std::numeric_limits<float>::max();
    const auto image_vtable = image_panel_vtable();
    if (!image_vtable)
    {
        return best;
    }

    for (std::uintptr_t offset = 0x1e0; offset <= 0x430; offset += sizeof(std::uintptr_t))
    {
        const auto wrapper = game::bound_memory().process.load<std::uintptr_t>(object + offset);
        if (!game::is_user_pointer(wrapper) ||
            game::bound_memory().process.load<std::uintptr_t>(wrapper) != image_vtable)
        {
            continue;
        }

        auto panel = inspect_panel(wrapper, display_w, display_h);
        const auto layout = game::compatibility::panel(panel.ui);
        if (!layout)
            continue;
        const auto raw_width = game::bound_memory().process.load<float>(panel.ui + layout.width);
        const auto raw_height = game::bound_memory().process.load<float>(panel.ui + layout.height);
        const auto size_error = std::abs(raw_width - expected_size) + std::abs(raw_height - expected_size);
        if (panel.viewport.valid(display_w, display_h) && std::isfinite(size_error) && size_error < best_size_error)
        {
            best = panel;
            best_size_error = size_error;
        }
    }
    return best;
}


[[nodiscard]] rect_t marker_space_rect(std::uintptr_t ui, float display_w, float display_h)
{
    panel_chain chain{};
    if (!read_panel_chain(ui, display_w, display_h, chain))
        return {};
    for (std::size_t i = 0; i < chain.count; ++i)
    {
        const auto& node = chain.nodes[i];
        if (std::abs(node.width - 300.0f) < 1.0f && std::abs(node.height - 300.0f) < 1.0f)
            return chain_rect(chain, i);
    }
    return {};
}

bool read_transform(overview_info& overview)
{
    const auto layout = game::compatibility::radar();
    if (!layout || game::bound_memory().process.load<std::uintptr_t>(overview.object) != overview.vtable)
        return false;
    std::array<std::byte, 0x50> data{};
    if (!game::bound_memory().process.copy(overview.object + layout->origin, data.data(), data.size()))
        return false;
    const auto read = [&]<typename T>(std::size_t off)
    {
        T result{};
        std::memcpy(&result, data.data() + off, sizeof(result));
        return result;
    };
    overview.map_origin = read.template operator()<foundation::vec2>(0);
    const auto inverse = read.template operator()<float>(0x20);
    overview.map_scale = inverse > 0.0001f ? 1.0f / inverse : 0.0f;
    overview.map_size = read.template operator()<float>(0x18);
    overview.canvas_size = read.template operator()<float>(0x0c);
    overview.viewport_size = read.template operator()<float>(0x10);
    overview.world_to_pixels = read.template operator()<float>(0x24);
    overview.map_center = read.template operator()<foundation::vec2>(0x40);
    overview.radar_angle = read.template operator()<float>(0x4c);
    std::uint8_t mode{};
    if (!game::bound_memory().process.copy(overview.object + 0x40, &mode, sizeof(mode)) ||
        !game::bound_memory().process.copy(overview.object + layout->alternate_scale, &overview.alternate_scale,
                                           sizeof(float)))
        return false;
    overview.alternate_transform = mode != 0;
    return std::isfinite(overview.map_origin.x) && std::isfinite(overview.map_origin.y) && std::isfinite(inverse) &&
           overview.map_scale >= 0.01f && overview.map_scale <= 100 && std::isfinite(overview.map_size) &&
           overview.map_size >= 128 && overview.map_size <= 8192 && std::isfinite(overview.canvas_size) &&
           overview.canvas_size >= 64 && overview.canvas_size <= 4096 && std::isfinite(overview.viewport_size) &&
           overview.viewport_size >= 64 && overview.viewport_size <= 4096 && std::isfinite(overview.world_to_pixels) &&
           overview.world_to_pixels >= 0.001f && overview.world_to_pixels <= 10 &&
           std::isfinite(overview.map_center.x) && std::isfinite(overview.map_center.y) &&
           std::isfinite(overview.radar_angle) &&
           (!overview.alternate_transform || (std::isfinite(overview.alternate_scale) &&
                                              overview.alternate_scale > 0.001f && overview.alternate_scale <= 10));
}

std::vector<overview_info> discover_overviews(float display_w, float display_h)
{
    std::vector<overview_info> result{};
    const auto object = hud_radar();
    if (!object || !image_panel_vtable())
    {
        return result;
    }

    overview_info overview{};
    overview.object = object;
    overview.vtable = game::bound_memory().process.load<std::uintptr_t>(object);
    if (!read_transform(overview))
        return result;

    overview.panel = find_radar_panel(object, std::max(overview.viewport_size, 290.0f), display_w, display_h);
    overview.marker_space = marker_space_rect(overview.panel.ui, display_w, display_h);
    overview.layout_width = display_w;
    overview.layout_height = display_h;

    if (overview.panel.viewport.valid(display_w, display_h))
    {
        result.push_back(overview);
    }

    return result;
}

// The radar panels the game currently shows, rediscovered while there are none and re-measured a few times a second.
// Game worker only.
class overview_tracker
{
public:
    [[nodiscard]] const std::vector<overview_info>& overviews() const noexcept { return m_overviews; }

    void refresh(const float display_width, const float display_height)
    {
        const auto now = std::chrono::steady_clock::now();
        if (m_overviews.empty() && now >= m_next_probe)
        {
            m_overviews = discover_overviews(display_width, display_height);
            m_next_probe = now + std::chrono::seconds(1);
        }

        const auto refresh_layout = now >= m_next_layout_refresh;
        if (refresh_layout)
            m_next_layout_refresh = now + std::chrono::milliseconds(50);

        auto any_active = false;
        for (auto& overview : m_overviews)
        {
            if (!read_transform(overview))
            {
                overview.object = 0;
                continue;
            }
            if (refresh_layout)
                measure_layout(overview, display_width, display_height);
            const auto viewport = scale_layout_rect(overview.panel.viewport, overview, display_width, display_height);
            any_active |= on_screen(viewport, display_width, display_height);
        }

        std::erase_if(m_overviews, [](const auto& overview) { return !overview.object; });
        if (!any_active)
            m_overviews.clear();
    }

    [[nodiscard]] static bool on_screen(const rect_t& viewport, const float display_width, const float display_height)
    {
        return viewport.valid(display_width, display_height) && viewport.x + viewport.w >= 0.0f &&
               viewport.y + viewport.h >= 0.0f && viewport.x <= display_width && viewport.y <= display_height;
    }

private:
    // Keeps the panel while it has the expected size; a resized panel (HUD scale, resolution) is searched again.
    static void measure_layout(overview_info& overview, const float display_width, const float display_height)
    {
        const auto expected_panel_size = std::max(overview.viewport_size, 290.0f);
        const auto layout = game::compatibility::panel(overview.panel.ui);
        const auto current_raw_width =
            layout ? game::bound_memory().process.load<float>(overview.panel.ui + layout.width) : 0.0f;
        if (!std::isfinite(current_raw_width) || std::abs(current_raw_width - expected_panel_size) > 2.0f)
            overview.panel = find_radar_panel(overview.object, expected_panel_size, display_width, display_height);
        else
            overview.panel = inspect_panel(overview.panel.wrapper, display_width, display_height);
        overview.marker_space = marker_space_rect(overview.panel.ui, display_width, display_height);
        overview.layout_width = display_width;
        overview.layout_height = display_height;
    }

    std::vector<overview_info> m_overviews{};
    std::chrono::steady_clock::time_point m_next_probe{};
    std::chrono::steady_clock::time_point m_next_layout_refresh{};
};

[[nodiscard]] bool inside_viewport(const foundation::vec2& point, const rect_t& viewport, bool square,
                                   float margin = 3.0f)
{
    const auto center_x = viewport.x + viewport.w * 0.5f;
    const auto center_y = viewport.y + viewport.h * 0.5f;
    if (square)
    {
        return point.x >= viewport.x + margin && point.x <= viewport.x + viewport.w - margin &&
               point.y >= viewport.y + margin && point.y <= viewport.y + viewport.h - margin;
    }

    const auto nx = (point.x - center_x) / std::max(viewport.w * 0.5f - margin, 1.0f);
    const auto ny = (point.y - center_y) / std::max(viewport.h * 0.5f - margin, 1.0f);
    return nx * nx + ny * ny <= 1.0f;
}

// The largest radar panel that is on screen, or null.
[[nodiscard]] const overview_info* pick_overview(const std::vector<overview_info>& overviews, const float display_width,
                                                 const float display_height, rect_t& active_viewport)
{
    const overview_info* active{};
    for (const auto& overview : overviews)
    {
        const auto viewport = scale_layout_rect(overview.panel.viewport, overview, display_width, display_height);
        if (!overview_tracker::on_screen(viewport, display_width, display_height))
            continue;
        if (!active || viewport.w * viewport.h > active_viewport.w * active_viewport.h)
        {
            active = &overview;
            active_viewport = viewport;
        }
    }
    return active;
}

using radar_settings = config::visual_profile::radar;

[[nodiscard]] ImU32 im_color(const render::draw::rgba& color)
{
    return render::draw::draw_list::to_im_color(color);
}

[[nodiscard]] ImU32 with_alpha(const ImU32 color, const ImU32 alpha)
{
    return (color & 0x00ffffffu) | (alpha << 24);
}

// One frame of drawing onto the game's radar panel: the panel's viewport and the transform CCSGO_HudRadar itself uses
// (project into map space, subtract the HUD's live centre, apply its live rotation, scale to the panel).
struct radar_frame
{
    ImDrawList* draw{};
    const radar_settings* settings{};
    rect_t viewport{};
    bool square{};
    float icon_scale{};
    float ui_scale_x{};
    float ui_scale_y{};
    float marker_center_x{};
    float marker_center_y{};
    float world_to_radar{};
    foundation::vec2 map_origin{};
    foundation::vec2 map_center{};
    float cosine{};
    float sine{};

    [[nodiscard]] foundation::vec2 to_radar(const foundation::vec3& world) const
    {
        const auto dx = (world.x - map_origin.x) * world_to_radar - map_center.x;
        const auto dy = (map_origin.y - world.y) * world_to_radar - map_center.y;
        return {marker_center_x + (dx * cosine - dy * sine) * ui_scale_x,
                marker_center_y + (dx * sine + dy * cosine) * ui_scale_y};
    }

    // Text with the configured outline.
    void text(ImFont* font, const float size, const ImVec2 position, const ImU32 color,
              const std::string_view value) const
    {
        const auto* first = value.data();
        const auto* last = value.data() + value.size();
        if (settings->text_outline)
        {
            const auto offset = std::max(0.5f, settings->text_outline_thickness * icon_scale);
            const auto outline = im_color(settings->text_outline_color);
            for (const auto delta : {ImVec2{-offset, 0}, ImVec2{offset, 0}, ImVec2{0, -offset}, ImVec2{0, offset}})
                draw->AddText(font, size, position + delta, outline, first, last);
        }
        draw->AddText(font, size, position, color, first, last);
    }
};

[[nodiscard]] radar_frame make_frame(ImDrawList* draw, const radar_settings& settings, const overview_info& overview,
                                     const rect_t& viewport, const float display_width, const float display_height,
                                     const bool square)
{
    radar_frame frame{.draw = draw, .settings = &settings, .viewport = viewport, .square = square};
    const auto marker_space = scale_layout_rect(overview.marker_space, overview, display_width, display_height);
    const auto marker_space_valid = marker_space.valid(display_width, display_height);
    frame.ui_scale_x = overview.panel.scale_x * display_width / overview.layout_width;
    frame.ui_scale_y = overview.panel.scale_y * display_height / overview.layout_height;
    frame.icon_scale = std::min(frame.ui_scale_x, frame.ui_scale_y);
    frame.marker_center_x =
        marker_space_valid ? marker_space.x + marker_space.w * 0.5f : viewport.x + viewport.w * 0.5f;
    frame.marker_center_y =
        marker_space_valid ? marker_space.y + marker_space.h * 0.5f : viewport.y + viewport.h * 0.5f;
    const auto canvas_ratio =
        overview.alternate_transform && std::isfinite(overview.alternate_scale) && overview.alternate_scale > 0.001f
            ? overview.alternate_scale
        : overview.canvas_size > 1.0f ? overview.viewport_size / overview.canvas_size
                                      : 1.0f;
    frame.world_to_radar = overview.world_to_pixels * canvas_ratio;
    frame.map_origin = overview.map_origin;
    frame.map_center = overview.map_center;
    const auto rotation = foundation::to_radians(overview.radar_angle);
    frame.cosine = std::cos(rotation);
    frame.sine = std::sin(rotation);
    return frame;
}

// Convex hull (monotone chain) of `points`, which it sorts; empty for fewer than three points.
void build_hull(std::vector<ImVec2>& points, std::vector<ImVec2>& hull)
{
    hull.clear();
    if (points.size() < 3)
        return;
    std::ranges::sort(points, {}, [](const ImVec2& p) { return std::pair{p.x, p.y}; });
    const auto cross = [](const ImVec2& a, const ImVec2& b, const ImVec2& c)
    { return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x); };
    for (const auto& point : points)
    {
        while (hull.size() >= 2 && cross(hull[hull.size() - 2], hull.back(), point) <= 0.0f)
            hull.pop_back();
        hull.push_back(point);
    }
    const auto lower = hull.size();
    for (auto it = points.rbegin() + 1; it != points.rend(); ++it)
    {
        while (hull.size() > lower && cross(hull[hull.size() - 2], hull.back(), *it) <= 0.0f)
            hull.pop_back();
        hull.push_back(*it);
    }
    if (hull.size() > 1)
        hull.pop_back();
}

struct projectile_style
{
    std::uintptr_t weapon{}; // grenade weapon for trajectory prediction; 0 when none applies
    const char* label{};
    ImU32 color{};
    float zone_radius{}; // radius of the drawn danger zone, world units
};

[[nodiscard]] projectile_style style_of(const game::projectile_kind kind, const radar_settings& settings)
{
    switch (kind)
    {
        case game::projectile_kind::he_grenade:
            return {"weapon_hegrenade"_id, "HE", im_color(settings.he_color), 0.0f};
        case game::projectile_kind::flashbang:
            return {"weapon_flashbang"_id, "FLASH", im_color(settings.flash_color), 0.0f};
        case game::projectile_kind::smoke_grenade:
            return {"weapon_smokegrenade"_id, "SMOKE", im_color(settings.smoke_color), 144.0f};
        case game::projectile_kind::molotov:
        case game::projectile_kind::molotov_fire:
            return {"weapon_molotov"_id, "FIRE", im_color(settings.molotov_color), 0.0f};
        case game::projectile_kind::decoy:
            return {"weapon_decoy"_id, "DECOY", im_color(settings.decoy_color), 0.0f};
        default:
            return {0, "GRENADE", IM_COL32(230, 230, 235, 255), 100.0f};
    }
}

[[nodiscard]] bool finite(const foundation::vec3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

struct cached_path
{
    foundation::vec3 initial_position{};
    foundation::vec3 initial_velocity{};
    std::uintptr_t weapon{};
    simulation::grenade_path path{};
};

// Predicted grenade routes and scratch buffers of the projectile layer. Render thread only.
struct projectile_layer
{
    std::unordered_map<std::uintptr_t, cached_path> paths{};
    simulation::grenade_trajectory_engine trajectory{};
    std::vector<std::uintptr_t> live{};
    std::vector<ImVec2> trajectory_points{};
    std::vector<ImVec2> zone_points{};
    std::vector<ImVec2> zone_hull{};

    void draw(const radar_frame& frame, const std::vector<game::projectile_snapshot>& projectiles)
    {
        live.clear();
        live.reserve(projectiles.size());
        for (const auto& projectile : projectiles)
        {
            if (!projectile.entity || !std::isfinite(projectile.origin.x) || !std::isfinite(projectile.origin.y))
                continue;
            live.push_back(projectile.entity);
            draw_projectile(frame, projectile, style_of(projectile.subtype, *frame.settings));
        }
        std::erase_if(paths, [&](const auto& item) { return std::ranges::find(live, item.first) == live.end(); });
    }

private:
    void draw_projectile(const radar_frame& frame, const game::projectile_snapshot& projectile,
                         const projectile_style& style)
    {
        const auto& settings = *frame.settings;
        if (settings.show_trajectories && projectile.launch_valid && projectile.in_flight)
            draw_trajectory(frame, projectile, style);

        const auto display_origin = projectile.smoke_active && projectile.smoke_detonation_pos.length_sqr() > 1.0f
                                        ? projectile.smoke_detonation_pos
                                        : projectile.origin;
        const auto point = frame.to_radar(display_origin);
        if (!inside_viewport(point, frame.viewport, frame.square, 3.0f))
            return;
        // HE and flash are instantaneous events, not persistent danger volumes.
        // Draw only the two effects that actually occupy space after detonation.
        if (settings.show_grenade_zones &&
            (projectile.smoke_active || projectile.subtype == game::projectile_kind::molotov_fire))
            draw_zone(frame, projectile, style, display_origin, point);
        if (settings.show_projectiles)
            draw_marker(frame, style, point);
    }

    // The route is the launch solution, predicted once per launch. Current origin/velocity are used only by the
    // moving marker and must not bend the predicted path in flight.
    void draw_trajectory(const radar_frame& frame, const game::projectile_snapshot& projectile,
                         const projectile_style& style)
    {
        auto& cached = paths[projectile.entity];
        const auto has_initial_velocity =
            finite(projectile.initial_velocity) && projectile.initial_velocity.length_sqr() > 25.0f;
        const auto launch_position =
            finite(projectile.initial_position) && projectile.initial_position.length_sqr() > 1.0f
                ? projectile.initial_position
                : projectile.origin;
        const auto launch_velocity = has_initial_velocity ? projectile.initial_velocity : projectile.velocity;
        if (style.weapon && (cached.weapon != style.weapon || !cached.path.valid ||
                             cached.initial_position.distance_sqr(launch_position) > 1.0f ||
                             cached.initial_velocity.distance_sqr(launch_velocity) > 1.0f))
        {
            cached.initial_position = launch_position;
            cached.initial_velocity = launch_velocity;
            cached.weapon = style.weapon;
            cached.path = trajectory.predict(launch_position, launch_velocity, style.weapon, -1.0f);
        }
        if (!cached.path.valid || cached.path.points.size() <= 1)
            return;

        trajectory_points.clear();
        trajectory_points.reserve(cached.path.points.size());
        for (const auto& world : cached.path.points)
        {
            const auto p = frame.to_radar(world);
            trajectory_points.push_back({p.x, p.y});
        }
        frame.draw->AddPolyline(trajectory_points.data(), static_cast<int>(trajectory_points.size()), style.color,
                                ImDrawFlags_None,
                                std::max(0.5f, frame.settings->trajectory_thickness * frame.icon_scale));
        frame.draw->AddCircleFilled(trajectory_points.back(),
                                    std::max(1.0f, frame.settings->trajectory_endpoint_size * frame.icon_scale),
                                    style.color, 16);
    }

    // A burning molotov is the hull of its fire points; a smoke is a circle of the smoke radius.
    void draw_zone(const radar_frame& frame, const game::projectile_snapshot& projectile, const projectile_style& style,
                   const foundation::vec3& origin, const foundation::vec2& point)
    {
        const auto& settings = *frame.settings;
        const auto fill = with_alpha(
            style.color, static_cast<ImU32>(std::lround(std::clamp(settings.zone_fill_alpha, 0.0f, 100.0f) * 2.55f)));
        const auto outline =
            with_alpha(style.color,
                       static_cast<ImU32>(std::lround(std::clamp(settings.zone_outline_alpha, 0.0f, 100.0f) * 2.55f)));
        const auto outline_thickness = std::max(0.5f, settings.zone_outline_thickness * frame.icon_scale);
        if (projectile.subtype == game::projectile_kind::molotov_fire && !projectile.fire_points.empty())
        {
            constexpr int samples_per_flame = 12;
            constexpr auto flame_radius = 60.0f;
            zone_points.clear();
            zone_points.reserve(projectile.fire_points.size() * samples_per_flame);
            for (const auto& fire : projectile.fire_points)
            {
                for (int sample = 0; sample < samples_per_flame; ++sample)
                {
                    const auto angle =
                        static_cast<float>(sample) * (2.0f * std::numbers::pi_v<float> / samples_per_flame);
                    const auto edge = frame.to_radar(
                        fire + foundation::vec3{std::cos(angle) * flame_radius, std::sin(angle) * flame_radius, 0.0f});
                    zone_points.push_back({edge.x, edge.y});
                }
            }
            build_hull(zone_points, zone_hull);
            if (zone_hull.size() >= 3)
            {
                frame.draw->AddConvexPolyFilled(zone_hull.data(), static_cast<int>(zone_hull.size()), fill);
                frame.draw->AddPolyline(zone_hull.data(), static_cast<int>(zone_hull.size()), outline,
                                        ImDrawFlags_Closed, outline_thickness);
            }
            return;
        }

        const auto edge = frame.to_radar(origin + foundation::vec3{style.zone_radius, 0.0f, 0.0f});
        const auto screen_radius = std::clamp(std::hypot(edge.x - point.x, edge.y - point.y), 3.0f, frame.viewport.w);
        frame.draw->AddCircleFilled({point.x, point.y}, screen_radius, fill, 32);
        frame.draw->AddCircle({point.x, point.y}, screen_radius, outline, 32, outline_thickness);
    }

    static void draw_marker(const radar_frame& frame, const projectile_style& style, const foundation::vec2& point)
    {
        frame.draw->AddCircleFilled({point.x, point.y}, std::clamp(3.0f * frame.icon_scale, 2.0f, 5.0f), style.color);
        if (auto* font = ImGui::GetFont())
        {
            const auto size = std::max(7.0f, ImGui::GetFontSize() * frame.icon_scale * 0.62f);
            frame.text(font, size, {point.x + 4.0f * frame.icon_scale, point.y - size * 0.5f}, style.color,
                       style.label);
        }
    }
};

// An enemy outside the panel: an outlined arrow on the panel's edge pointing towards it.
void draw_edge_arrow(const radar_frame& frame, const foundation::vec2& point, const float marker_radius,
                     const ImU32 color)
{
    const auto& viewport = frame.viewport;
    const auto center_x = viewport.x + viewport.w * 0.5f;
    const auto center_y = viewport.y + viewport.h * 0.5f;
    const auto edge_dx = point.x - center_x;
    const auto edge_dy = point.y - center_y;
    const auto edge_length = std::sqrt(edge_dx * edge_dx + edge_dy * edge_dy);
    const auto max_x = viewport.w * 0.5f - marker_radius - 3.0f;
    const auto max_y = viewport.h * 0.5f - marker_radius - 3.0f;
    float edge_scale{};
    if (frame.square)
    {
        edge_scale = std::min(max_x / std::max(std::abs(edge_dx), 0.001f), max_y / std::max(std::abs(edge_dy), 0.001f));
    }
    else
    {
        // In stretched 4:3 the HUD's physical X/Y radii differ. Intersect the
        // direction with that ellipse instead of assuming a square pixel scale.
        const auto normalized_x = edge_dx / std::max(max_x, 1.0f);
        const auto normalized_y = edge_dy / std::max(max_y, 1.0f);
        edge_scale = 1.0f / std::sqrt(normalized_x * normalized_x + normalized_y * normalized_y);
    }

    const auto direction_x = edge_dx / edge_length;
    const auto direction_y = edge_dy / edge_length;
    const ImVec2 tip{center_x + edge_dx * edge_scale, center_y + edge_dy * edge_scale};
    const auto triangle = [&](const float back, const float half_width, const ImU32 fill)
    {
        const auto base_x = tip.x - direction_x * marker_radius * back;
        const auto base_y = tip.y - direction_y * marker_radius * back;
        const auto perpendicular_x = -direction_y * marker_radius * half_width;
        const auto perpendicular_y = direction_x * marker_radius * half_width;
        frame.draw->AddTriangleFilled(tip, {base_x + perpendicular_x, base_y + perpendicular_y},
                                      {base_x - perpendicular_x, base_y - perpendicular_y}, fill);
    };
    triangle(2.0f, 1.0f, IM_COL32(10, 10, 13, 225));
    triangle(1.7f, 0.8f, color);
}

// The enemy's dot with a triangle showing where it looks, in screen space so the stretched panel does not skew it.
void draw_enemy_marker(const radar_frame& frame, const game::player_snapshot& player, const foundation::vec2& point,
                       const float marker_radius)
{
    const auto eye_angle = foundation::to_radians(player.eye_angles.y);
    const auto direction_x = std::cos(eye_angle);
    const auto direction_y = -std::sin(eye_angle);
    const auto screen_x = (direction_x * frame.cosine - direction_y * frame.sine) * frame.ui_scale_x;
    const auto screen_y = (direction_x * frame.sine + direction_y * frame.cosine) * frame.ui_scale_y;
    const auto screen_length = std::max(std::sqrt(screen_x * screen_x + screen_y * screen_y), 0.001f);
    const auto forward_x = screen_x / screen_length;
    const auto forward_y = screen_y / screen_length;

    const auto tip = ImVec2{point.x + forward_x * marker_radius * 1.40f, point.y + forward_y * marker_radius * 1.40f};
    const auto base = ImVec2{point.x + forward_x * marker_radius * 0.55f, point.y + forward_y * marker_radius * 0.55f};
    const auto half_width = ImVec2{-forward_y, forward_x} * (marker_radius * 0.65f);
    frame.draw->AddCircleFilled({point.x, point.y}, marker_radius, im_color(frame.settings->enemy_color));
    frame.draw->AddTriangleFilled(tip, base + half_width, base - half_width, im_color(frame.settings->direction_color));
}

// Name above the marker; weapon, health and armor bars and their values below it.
void draw_enemy_info(const radar_frame& frame, const game::player_snapshot& player, const foundation::vec2& point,
                     const float marker_radius)
{
    const auto& settings = *frame.settings;
    const auto text_size =
        std::max(7.0f, ImGui::GetFontSize() * frame.icon_scale * std::clamp(settings.information_scale, 0.5f, 1.5f));
    auto* font = ImGui::GetFont();
    const auto centered_text = [&](const std::string_view value, const float y, const ImU32 color)
    {
        if (value.empty())
            return;
        const auto width = font->CalcTextSizeA(text_size, FLT_MAX, 0.0f, value.data(), value.data() + value.size()).x;
        frame.text(font, text_size, {point.x - width * 0.5f, y}, color, value);
    };
    if (settings.show_names)
        centered_text(player.display_name, point.y - marker_radius - text_size - 2.0f, im_color(settings.name_color));

    auto info_y = point.y + marker_radius + 2.0f;
    if (settings.show_weapon && !player.weapon.name.empty())
    {
        auto name = std::string_view(player.weapon.name);
        if (name.starts_with("weapon_"))
            name.remove_prefix(7);
        centered_text(name, info_y, im_color(settings.weapon_color));
        info_y += text_size + 2.0f;
    }

    const auto bar_height = std::max(1.0f, 1.5f * frame.icon_scale);
    const auto bar_width = marker_radius * 2.8f;
    const auto bar = [&](const int value, const render::draw::rgba& color)
    {
        const auto left = point.x - bar_width * 0.5f;
        frame.draw->AddRectFilled({left, info_y}, {left + bar_width, info_y + bar_height}, IM_COL32(15, 18, 24, 220));
        frame.draw->AddRectFilled({left, info_y},
                                  {left + bar_width * std::clamp(value / 100.0f, 0.0f, 1.0f), info_y + bar_height},
                                  im_color(color));
        info_y += bar_height + 2.0f;
    };
    if (settings.show_health)
        bar(player.health, settings.health_color);
    if (settings.show_armor)
        bar(player.armor, settings.armor_color);
    if (!settings.show_health && !settings.show_armor)
        return;

    char status[32]{};
    const auto health = std::clamp(player.health, 0, 100);
    const auto armor = std::clamp(player.armor, 0, 100);
    if (settings.show_health && settings.show_armor)
        std::snprintf(status, sizeof(status), "%d HP  %d AR", health, armor);
    else if (settings.show_health)
        std::snprintf(status, sizeof(status), "%d HP", health);
    else
        std::snprintf(status, sizeof(status), "%d AR", armor);
    centered_text(status, info_y, im_color(settings.status_color));
}

void draw_enemy(const radar_frame& frame, const game::player_snapshot& player)
{
    const auto point = frame.to_radar(player.origin);
    const auto marker_radius =
        std::clamp(5.0f * frame.icon_scale * std::clamp(frame.settings->marker_scale, 0.5f, 2.0f), 2.0f, 14.0f);
    if (!inside_viewport(point, frame.viewport, frame.square, marker_radius * 2.0f + 2.0f))
    {
        const auto center =
            ImVec2{frame.viewport.x + frame.viewport.w * 0.5f, frame.viewport.y + frame.viewport.h * 0.5f};
        if (std::hypot(point.x - center.x, point.y - center.y) > 0.001f)
        {
            draw_edge_arrow(frame, point, marker_radius, im_color(frame.settings->enemy_color));
            return;
        }
    }
    draw_enemy_marker(frame, player, point, marker_radius);
    draw_enemy_info(frame, player, point, marker_radius);
}
} // namespace

namespace features::visuals
{
// `tick` runs on the game worker and publishes the overviews; `on_render` reads them and owns the projectile layer on
// the render thread.
struct radar_t::state
{
    overview_tracker tracker{}; // game worker
    std::atomic<std::shared_ptr<const std::vector<overview_info>>> overviews{
        std::make_shared<std::vector<overview_info>>()};
    std::atomic<bool> square_always{};
    std::atomic<bool> square_with_scoreboard{};
    projectile_layer projectiles{}; // render thread
};

radar_t::radar_t()
    : m_state(std::make_unique<state>())
{
}

radar_t::~radar_t() = default;

void radar_t::tick()
{
    if (!radar_active() || !game::local_player().pawn())
        return;

    RECT overlay_client{};
    const auto overlay_window = render::overlay_window();
    if (!overlay_window || !::GetClientRect(overlay_window, &overlay_client))
        return;
    const auto display_width = static_cast<float>(overlay_client.right - overlay_client.left);
    const auto display_height = static_cast<float>(overlay_client.bottom - overlay_client.top);
    if (display_width < 64.0f || display_height < 64.0f)
        return;

    m_state->tracker.refresh(display_width, display_height);
    m_state->square_always.store(game::variables().get<bool>(CONVAR("cl_radar_square_always"_id)),
                                 std::memory_order_relaxed);
    m_state->square_with_scoreboard.store(game::variables().get<bool>(CONVAR("cl_radar_square_with_scoreboard"_id)),
                                          std::memory_order_relaxed);
    m_state->overviews.store(std::make_shared<const std::vector<overview_info>>(m_state->tracker.overviews()),
                             std::memory_order_release);
}

void radar_t::on_render(render::draw::draw_list& draw_list)
{
    const auto runtime_settings = config::get_runtime_snapshot();
    if (!radar_active() || !draw_list.m_im_draw_list)
        return;
    if (runtime_settings->visual.m_player.spectator_sync && game::world().local_spectated())
        return;

    const auto [display_width_i, display_height_i] = render::draw::get_display_size();
    const auto display_width = static_cast<float>(display_width_i);
    const auto display_height = static_cast<float>(display_height_i);
    if (display_width < 64.0f || display_height < 64.0f || !game::local_player().pawn())
        return;

    const auto overviews = m_state->overviews.load(std::memory_order_acquire);
    rect_t viewport{};
    const auto* active = pick_overview(*overviews, display_width, display_height, viewport);
    if (!active)
        return;

    // The scoreboard (Tab) switches the radar to its square form when the game is set to.
    const auto tab_mode = (::GetAsyncKeyState(VK_TAB) & 0x8000) != 0;
    const auto square = m_state->square_always.load(std::memory_order_relaxed) ||
                        (tab_mode && m_state->square_with_scoreboard.load(std::memory_order_relaxed));
    const auto& settings = runtime_settings->visual.m_radar;
    const auto frame =
        make_frame(draw_list.m_im_draw_list, settings, *active, viewport, display_width, display_height, square);

    frame.draw->PushClipRect({viewport.x, viewport.y}, {viewport.x + viewport.w, viewport.y + viewport.h}, true);
    if (settings.show_projectiles || settings.show_trajectories || settings.show_grenade_zones)
        m_state->projectiles.draw(frame, *game::world().projectiles());
    for (const auto& player : *game::world().players())
    {
        if (player.health <= 0 || !game::local_player().is_enemy(player.team) || !player.pawn ||
            !player.game_scene_node || !std::isfinite(player.origin.x) || !std::isfinite(player.origin.y))
            continue;
        draw_enemy(frame, player);
    }
    frame.draw->PopClipRect();
}

} // namespace features::visuals
