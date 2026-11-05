#include <stdafx.hpp>
#include <render/presentation.hpp>
#include <platform/windows/input.hpp>
#include <scripting/runtime.hpp>
#include <config/first_run.hpp>
#include <config/settings.hpp>
#include <format>
#include <game/input/bindings.hpp>
#include <config/hotkeys.hpp>
#include <features/visuals/visuals.hpp>
#include <features/visuals/hitsound.hpp>
#include <features/chams/preview.hpp>
#include <features/chams/renderer.hpp>
#include <render/text/localization.hpp>
#include <ui/menu/menu.hpp>
#include <ui/overlay/input.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <ui/menu/internal.hpp>
#include <ui/overlay/overlay.hpp>

namespace ui
{
using namespace ui::menu::detail;

bool menu_t::initialize(HWND hwnd)
{
    m_hwnd = hwnd;
    return true;
}

void menu_t::map_pointer_to_layout(float& x, float& y, const float display_width,
                                   const float display_height) const noexcept
{
    if (!is_open())
        return;
    const auto scale = current_menu_scale(display_width, display_height);
    if (std::abs(scale - 1.0f) < 0.0001f)
        return;
    const auto origin = menu_transform_origin(display_width, display_height);
    x = origin.x + (x - origin.x) / scale;
    y = origin.y + (y - origin.y) / scale;
}

void menu_t::map_pointer_to_screen(float& x, float& y, const float display_width,
                                   const float display_height) const noexcept
{
    if (!is_open())
        return;
    const auto scale = current_menu_scale(display_width, display_height);
    if (std::abs(scale - 1.0f) < 0.0001f)
        return;
    const auto origin = menu_transform_origin(display_width, display_height);
    x = origin.x + (x - origin.x) * scale;
    y = origin.y + (y - origin.y) * scale;
}

void menu_t::reset_content_animation()
{
    m_content_animation = 0.0f;
}

void menu_t::poll_hotkey()
{
    // A fresh installation shows the guide by itself, without the menu; it waits for the movement keys like a normal open.
    const bool guide_wanted = config::first_run::pending();
    if (!guide_wanted)
        m_guide_open_pending = false;
    else if (!m_guide_visible.load(std::memory_order_relaxed) && !m_open.load(std::memory_order_relaxed))
        m_guide_open_pending = true;

    const auto menu_key = config::lifecycle_keys().menu;
    const bool hotkey_is_down = (::GetAsyncKeyState(menu_key) & 0x8000) != 0;
    // While the guide is up it is modal: the menu key waits until the guide is finished.
    const bool guide_modal = guide_wanted && m_guide_visible.load(std::memory_order_relaxed);
    if (hotkey_is_down && !m_menu_hotkey_was_down && !guide_modal)
    {
        if (m_open.load(std::memory_order_relaxed))
        {
            m_open.store(false, std::memory_order_release);
            m_open_pending = false;
        }
        else
            m_open_pending = !m_open_pending;
    }
    m_menu_hotkey_was_down = hotkey_is_down;

    const bool open_menu = m_open_pending && !m_open.load(std::memory_order_relaxed);
    if (!open_menu && !m_guide_open_pending)
        return;

    std::vector<std::uint16_t> movement_keys{};
    constexpr std::array movement_actions{
        game::input_action::forward,
        game::input_action::back,
        game::input_action::left,
        game::input_action::right,
    };
    for (const auto action : movement_actions)
    {
        for (const auto& binding : game::input_bindings().candidates(action))
        {
            if (binding.device == game::input_device::keyboard && binding.virtual_key &&
                std::ranges::find(movement_keys, binding.virtual_key) == movement_keys.end())
            {
                movement_keys.push_back(binding.virtual_key);
            }
        }
    }
    if (std::ranges::any_of(movement_keys, [](const std::uint16_t key)
                            { return platform::windows::bound_input().physical_key_down(key); }))
    {
        return;
    }

    if (open_menu)
        m_open.store(true, std::memory_order_release);
    if (m_guide_open_pending)
    {
        m_guide_front_pending = true;
        m_guide_open_pending = false;
        m_guide_visible.store(true, std::memory_order_release);
    }
    std::vector<platform::windows::input_gateway::key_transition> releases{};
    releases.reserve(movement_keys.size());
    for (const auto key : movement_keys)
        releases.push_back({key, false});
    for (std::size_t offset = 0; offset < releases.size(); offset += 8)
        platform::windows::bound_input().keys(
            std::span{releases}.subspan(offset, std::min<std::size_t>(8, releases.size() - offset)));
    platform::windows::bound_input().clear_movement_gates();
    m_open_pending = false;
    g_toggle_animations.clear();
    reset_content_animation();
}

void menu_t::draw()
{
    // "Show the guide again" from the open menu: nothing to wait for.
    if (config::first_run::pending() && m_open.load(std::memory_order_acquire) &&
        !m_guide_visible.load(std::memory_order_acquire))
    {
        m_guide_front_pending = true;
        m_guide_visible.store(true, std::memory_order_release);
    }
    const bool menu_open = m_open.load(std::memory_order_acquire);
    const bool guide_visible = m_guide_visible.load(std::memory_order_acquire);
    if (!menu_open && !guide_visible)
    {
        return;
    }
    synchronize_menu_palette();

    const auto display = ImGui::GetIO().DisplaySize;
    const auto menu_scale = current_menu_scale(display.x, display.y);
    const auto transform_origin = menu_transform_origin(display.x, display.y);
    const menu_render_scope render_scope{display, transform_origin, menu_scale};
    if (menu_open)
    {
        const auto initial_position = initial_menu_layout_position(display.x, display.y);
        ImGui::SetNextWindowPos(initial_position, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({menu_width(), menu_height()}, ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 3.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 4.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 10.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, k_text_main);
        ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4{0, 0, 0, 0});
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, k_border_light);
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, k_text_muted);
        ImGui::Begin("##vesta_native_menu", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBackground |
                         ImGuiWindowFlags_NoSavedSettings);

        const auto min = ImGui::GetWindowPos();
        const auto max = ImVec2{min.x + menu_width(), min.y + menu_height()};
        g_menu_min = min;
        g_menu_max = max;
        auto* draw = ImGui::GetWindowDrawList();
        draw->PushClipRectFullScreen();
        soft_shadow(draw, min, max, 3.0f, {0.0f, 14.0f}, 34.0f, {0, 0, 0, 1}, 1.0f);
        draw->PopClipRect();
        draw->AddRectFilled(min, max, packed(k_bg_base), 3.0f);
        draw_glass_sheen(draw, min, max);
        draw->AddRect(min, max, packed(k_border), 3.0f);
        draw_accent_edge(draw, min, max);
        draw->AddLine({min.x + 20.0f, min.y + 1.0f}, {max.x - 20.0f, min.y + 1.0f}, IM_COL32(255, 255, 255, 20));

        push_menu_font(render::fonts().menu_regular_12);
        draw_sidebar();
        draw_content();
        ImGui::PopFont();

        ImGui::End();

        push_menu_font(render::fonts().menu_regular_12);
        draw_visual_editor();
        draw_killsay_editor();
        ImGui::PopFont();
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar(5);
    }

    if (guide_visible)
    {
        push_menu_font(render::fonts().menu_regular_12);
        draw_guide();
        ImGui::PopFont();
    }
    scale_menu_draw_lists(transform_origin, menu_scale);
}

// Side window with the killsay chat lines, shown next to the menu on Misc > General while Killsay is on.
void menu_t::draw_killsay_editor()
{
    auto& cfg = config::general_settings.m_killsay;
    const auto requested = m_page == 3 && m_misc_group == 0 && cfg.enabled;
    const auto delta_time = std::min(ImGui::GetIO().DeltaTime, 1.0f / 30.0f);
    if (requested)
        m_killsay_editor_animation = std::min(1.0f, m_killsay_editor_animation + delta_time / 0.12f);
    else
        m_killsay_editor_animation = std::max(0.0f, m_killsay_editor_animation - delta_time / 0.07f);
    if (m_killsay_editor_animation <= 0.0f)
        return;

    const auto progress = std::clamp(m_killsay_editor_animation, 0.0f, 1.0f);
    const auto reveal = requested ? 1.0f - std::pow(1.0f - progress, 3.0f) : progress;
    constexpr auto panel_size = ImVec2{320.0f, 650.0f};
    auto display = ImGui::GetIO().DisplaySize;
    map_pointer_to_layout(display.x, display.y, display.x, display.y);
    auto panel_x = g_menu_max.x + 12.0f;
    if (panel_x + panel_size.x > display.x - 8.0f)
        panel_x = std::max(8.0f, display.x - panel_size.x - 8.0f);
    const auto panel_position = ImVec2{panel_x + (1.0f - reveal) * 18.0f, g_menu_min.y};

    ImGui::SetNextWindowPos(panel_position, ImGuiCond_Always);
    ImGui::SetNextWindowSize(panel_size, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 3.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##killsay_editor", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);

    const auto panel_min = ImGui::GetWindowPos();
    const auto panel_max = ImVec2{panel_min.x + panel_size.x, panel_min.y + panel_size.y};
    auto* draw = ImGui::GetWindowDrawList();
    draw->PushClipRectFullScreen();
    soft_shadow(draw, panel_min, panel_max, 3.0f, {0.0f, 14.0f}, 30.0f, {0, 0, 0, 1}, reveal * 0.85f);
    draw->PopClipRect();
    draw->AddRectFilled(panel_min, panel_max, packed(k_bg_base), 3.0f);
    draw_glass_sheen(draw, panel_min, panel_max);
    draw->AddRect(panel_min, panel_max, packed(k_border), 3.0f);
    draw_accent_edge(draw, panel_min, panel_max);
    draw->AddLine({panel_min.x + 20.0f, panel_min.y + 1.0f}, {panel_max.x - 20.0f, panel_min.y + 1.0f},
                  IM_COL32(255, 255, 255, 20));
    draw_section_title(draw, {panel_min.x + 20.0f, panel_min.y + 20.0f}, panel_max.x - 20.0f,
                       render::localization::tr("KILLSAY MESSAGES"));

    // Filled slots, and the rules for what can be typed.
    int used = 0;
    for (const auto& message : cfg.messages)
        if (std::any_of(message.begin(), message.end(), [](const char c) { return c != ' ' && c != '\0'; }))
            ++used;
    const auto hint = std::format("{}/{}  -  ASCII only, empty slots are skipped", used, cfg.messages.size());
    draw->AddText({panel_min.x + 20.0f, panel_min.y + 50.0f}, packed(k_text_muted), hint.c_str());

    ImGui::SetCursorPos({16.0f, 76.0f});
    ImGui::BeginChild("##killsay_slots", {288.0f, 558.0f}, false, ImGuiWindowFlags_NoBackground);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, k_bg_panel);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, k_bg_hover);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, k_bg_hover);
    ImGui::PushStyleColor(ImGuiCol_Border, k_border_light);
    ImGui::PushStyleColor(ImGuiCol_Text, k_text_main);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {10.0f, 6.0f});
    for (std::size_t i = 0; i < cfg.messages.size(); ++i)
    {
        ImGui::PushID(static_cast<int>(i));
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, k_text_muted);
        ImGui::Text("%2d", static_cast<int>(i) + 1);
        ImGui::PopStyleColor();
        ImGui::SameLine(26.0f);
        ImGui::SetNextItemWidth(252.0f);
        // Edited in place: the config slot is a fixed char buffer.
        ImGui::InputText("##message", cfg.messages[i].data(), cfg.messages[i].size(),
                         ImGuiInputTextFlags_CallbackCharFilter, filter_message_char);
        ImGui::PopID();
    }
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(5);
    ImGui::EndChild();

    ImGui::End();
    ImGui::PopStyleVar(3);
}

void menu_t::draw_sidebar()
{
    const auto side = current_tab_side();
    const auto horizontal = tabs_horizontal();
    const auto size = horizontal ? ImVec2{k_content_width, k_tab_bar_height} : ImVec2{k_sidebar_width, k_content_height};
    ImVec2 origin{};
    ImDrawFlags corners = ImDrawFlags_RoundCornersLeft;
    switch (side)
    {
    case tab_side::left:
        break;
    case tab_side::right:
        origin = {k_content_width, 0.0f};
        corners = ImDrawFlags_RoundCornersRight;
        break;
    case tab_side::top:
        corners = ImDrawFlags_RoundCornersTop;
        break;
    case tab_side::bottom:
        origin = {0.0f, k_content_height};
        corners = ImDrawFlags_RoundCornersBottom;
        break;
    }
    ImGui::SetCursorPos(origin);
    ImGui::BeginChild("##sidebar", size, false, ImGuiWindowFlags_NoScrollbar);
    const auto min = ImGui::GetWindowPos();
    const auto max = ImVec2{min.x + size.x, min.y + size.y};
    auto* draw = ImGui::GetWindowDrawList();

    add_vertical_gradient_rounded(draw, min, max, IM_COL32(0, 0, 0, 26), IM_COL32(0, 0, 0, 76), 3.0f, corners);
    // The divider sits on the edge that faces the page.
    switch (side)
    {
    case tab_side::left:
        draw->AddLine({max.x - 1.0f, min.y}, {max.x - 1.0f, max.y}, packed(k_border));
        break;
    case tab_side::right:
        draw->AddLine({min.x, min.y}, {min.x, max.y}, packed(k_border));
        break;
    case tab_side::top:
        draw->AddLine({min.x, max.y - 1.0f}, {max.x, max.y - 1.0f}, packed(k_border));
        break;
    case tab_side::bottom:
        draw->AddLine({min.x, min.y}, {max.x, min.y}, packed(k_border));
        break;
    }

    constexpr auto brand_zone = 118.0f; // horizontal strip: left edge to the first tab
    const auto* brand = render::fonts().menu_brand_30;
    if (brand && brand->im_font)
    {
        constexpr auto text = std::string_view{"BloodyWare"};
        constexpr auto letter_spacing = 2.5f;
        const auto measure = [&](const float brand_size)
        {
            float width{};
            for (const auto c : text)
                width += brand->im_font->CalcTextSizeA(brand_size, FLT_MAX, 0.0f, &c, &c + 1).x;
            return width + letter_spacing * (text.size() - 1);
        };
        auto brand_size = brand->font_size * (horizontal ? 0.62f : 1.25f);
        auto width = measure(brand_size);
        if (horizontal && width > brand_zone - 14.0f)
        {
            // Never run into the first tab: shrink the wordmark to the room it has.
            brand_size *= (brand_zone - 14.0f) / width;
            width = measure(brand_size);
        }
        auto x = horizontal ? min.x + 14.0f : min.x + (k_sidebar_width - width) * 0.5f;
        const auto y = horizontal ? min.y + (k_tab_bar_height - brand_size) * 0.5f : min.y + (112.0f - brand_size) * 0.5f;
        for (const auto c : text)
        {
            draw->AddText(brand->im_font, brand_size, {x, y}, IM_COL32_WHITE, &c, &c + 1);
            x += brand->im_font->CalcTextSizeA(brand_size, FLT_MAX, 0.0f, &c, &c + 1).x + letter_spacing;
        }
    }

    static constexpr const char* labels[]{"Aimbot", "Triggerbot", "Visuals", "Misc", "Farmbot"};
    constexpr int count = 5;
    if (horizontal)
        ImGui::SetCursorPos({brand_zone + 14.0f, (k_tab_bar_height - 44.0f) * 0.5f});
    else
        ImGui::SetCursorPos({20.0f, 112.0f});
    for (int i = 0; i < count; ++i)
    {
        if (!horizontal)
            ImGui::SetCursorPosX(20.0f);
        ImGui::PushID(i);
        const auto active = m_page == i;
        if (nav_button(labels[i], active, i, horizontal) && !active)
        {
            m_page = i;
            reset_content_animation();
        }
        ImGui::PopID();
        if (i != count - 1)
        {
            if (horizontal)
                ImGui::SameLine(0.0f, 4.0f);
            else
                ImGui::Dummy({0.0f, 6.0f});
        }
    }

    ImGui::EndChild();
}

void menu_t::draw_content()
{
    switch (current_tab_side())
    {
    case tab_side::left:
        ImGui::SetCursorPos({k_sidebar_width, 0.0f});
        break;
    case tab_side::top:
        ImGui::SetCursorPos({0.0f, k_tab_bar_height});
        break;
    case tab_side::right:
    case tab_side::bottom:
        ImGui::SetCursorPos({0.0f, 0.0f});
        break;
    }
    ImGui::BeginChild("##content", {k_content_width, k_content_height}, false, ImGuiWindowFlags_NoScrollbar);
    m_content_animation = std::min(1.0f, m_content_animation + ImGui::GetIO().DeltaTime / 0.14f);
    const auto t = 1.0f - std::pow(1.0f - m_content_animation, 3.0f);
    ImGui::SetCursorPosY((1.0f - t) * 4.0f);

    if (m_page == 0)
        draw_aimbot();
    else if (m_page == 1)
        draw_triggerbot();
    else if (m_page == 2)
        draw_visuals();
    else if (m_page == 3)
        draw_misc();
    else
        draw_farmbot();

    ImGui::EndChild();
}
} // namespace ui
