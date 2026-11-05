#include <stdafx.hpp>
#include <ui/menu/library_widgets.hpp>
#include <render/fonts.hpp>
#include <render/presentation.hpp>
#include <render/text/localization.hpp>
#include <imgui.h>

#include <unordered_map>

namespace ui::menu::detail
{
namespace
{
constexpr float k_tile_rounding = 10.0f;
constexpr float k_small_height = 28.0f;

[[nodiscard]] ImVec4 tone_color(const tone kind)
{
    switch (kind)
    {
        case tone::accent:
            return k_accent;
        case tone::danger:
            return {0.94f, 0.33f, 0.38f, 1.0f};
        case tone::normal:
            break;
    }
    return k_text_main;
}

void stroke(ImDrawList* draw, const ImVec2 center, std::initializer_list<ImVec2> points, const ImU32 color,
            const float thickness = 1.5f)
{
    const ImVec2* previous = nullptr;
    for (const auto& point : points)
    {
        if (previous)
            draw->AddLine(center + *previous, center + point, color, thickness);
        previous = &point;
    }
}

// The entry of each open action menu that waits for its confirming second click.
std::unordered_map<ImGuiID, int> g_armed_actions{};
} // namespace

void draw_glyph(ImDrawList* draw, const glyph icon, const ImVec2 center, const ImU32 color)
{
    switch (icon)
    {
        case glyph::none:
            break;
        case glyph::import:
            stroke(draw, center, {{-6.0f, 1.0f}, {-6.0f, 6.0f}, {6.0f, 6.0f}, {6.0f, 1.0f}}, color, 1.5f);
            stroke(draw, center, {{0.0f, -6.5f}, {0.0f, 2.5f}}, color, 1.5f);
            stroke(draw, center, {{-3.5f, -1.0f}, {0.0f, 2.5f}, {3.5f, -1.0f}}, color, 1.5f);
            break;
        case glyph::folder:
            draw_action_icon(draw, row_action_icon::folder, center, color);
            break;
        case glyph::shield:
            stroke(draw, center,
                   {{0.0f, -6.5f},
                    {5.5f, -4.5f},
                    {5.0f, 1.5f},
                    {0.0f, 6.5f},
                    {-5.0f, 1.5f},
                    {-5.5f, -4.5f},
                    {0.0f, -6.5f}},
                   color, 1.4f);
            stroke(draw, center, {{-2.5f, -0.5f}, {-0.5f, 1.8f}, {2.8f, -2.2f}}, color, 1.4f);
            break;
        case glyph::more:
            for (const float x : {-5.5f, 0.0f, 5.5f})
                draw->AddCircleFilled(center + ImVec2{x, 0.0f}, 1.7f, color, 10);
            break;
    }
}

void wide_card(const char* id, const char* title, const callback_ref body, const float actions_width,
               const callback_ref actions)
{
    // A fixed window inside the page: from the top of the free space to the bottom of the page.
    const auto top = std::max(g_cards_y[0], g_cards_y[1]);
    const auto width = g_cards_width * 2.0f + 20.0f;
    const auto page_bottom = ImGui::GetWindowPos().y + ImGui::GetWindowHeight();
    const auto height = std::max(160.0f, page_bottom - (g_cards_origin.y + top) - 1.0f);
    const auto* title_font = render::fonts().menu_semibold_13;
    const auto title_size = title_font && title_font->im_font ? title_font->font_size : ImGui::GetFontSize();
    ImGui::SetCursorScreenPos({g_cards_origin.x, g_cards_origin.y + top});
    ImGui::PushStyleColor(ImGuiCol_ChildBg, k_bg_card);
    ImGui::PushStyleColor(ImGuiCol_Border, k_border);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 14.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {20.0f, 16.0f});
    ImGui::BeginChild(id, {width, height}, true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const auto title_pos = ImGui::GetCursorScreenPos();
    const auto right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    draw_section_title(ImGui::GetWindowDrawList(), title_pos, right - actions_width - 12.0f,
                       render::localization::tr(title));
    ImGui::SetCursorScreenPos({right - actions_width, title_pos.y + (title_size - k_small_height) * 0.5f});
    actions();
    ImGui::SetCursorScreenPos(title_pos);
    ImGui::Dummy({0.0f, title_size});
    ImGui::Dummy({0.0f, 12.0f});
    body();
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
    g_cards_y[0] = g_cards_y[1] = top + height + 20.0f;
}

float small_button_width(const char* label, const glyph icon)
{
    const auto text = ImGui::CalcTextSize(render::localization::tr(label));
    return text.x + 24.0f + (icon != glyph::none ? 20.0f : 0.0f);
}

bool small_button(const char* id, const glyph icon, const char* label, const tone kind)
{
    const auto width = small_button_width(label, icon);
    ImGui::InvisibleButton(id, {width, k_small_height});
    const auto clicked = ImGui::IsItemClicked();
    const auto hovered = ImGui::IsItemHovered();
    pointer_cursor_if_hovered();
    const auto item = ImGui::GetItemID();
    const auto hover_t = animate_state(g_hover_animations, item, hovered, 0.12f);
    const auto min = ImGui::GetItemRectMin();
    const auto max = ImGui::GetItemRectMax();
    auto* draw = ImGui::GetWindowDrawList();
    const auto base = tone_color(kind);
    auto fill = mix(ImVec4{1.0f, 1.0f, 1.0f, 0.04f}, ImVec4{base.x, base.y, base.z, 0.16f}, hover_t);
    draw->AddRectFilled(min, max, packed(fill), 8.0f);
    draw->AddRect(min, max, packed(mix(k_border, k_border_light, hover_t)), 8.0f);
    const auto ink = packed(mix(k_text_muted, k_text_main, hover_t));
    auto x = min.x + 12.0f;
    if (icon != glyph::none)
    {
        draw_glyph(draw, icon, {x + 7.0f, (min.y + max.y) * 0.5f}, ink);
        x += 20.0f;
    }
    const auto* text = render::localization::tr(label);
    draw->AddText({x, min.y + (k_small_height - ImGui::GetTextLineHeight()) * 0.5f}, ink, text);
    return clicked;
}

bool pill_button(const char* id, const char* label, const float width, const tone kind, const float height)
{
    ImGui::InvisibleButton(id, {width, height});
    const auto clicked = ImGui::IsItemClicked();
    const auto hovered = ImGui::IsItemHovered();
    pointer_cursor_if_hovered();
    const auto item = ImGui::GetItemID();
    const auto hover_t = animate_state(g_hover_animations, item, hovered, 0.12f);
    const auto held_t = animate_state(g_active_animations, item, ImGui::IsItemActive(), 0.08f);
    const auto min = ImGui::GetItemRectMin();
    const auto max = ImGui::GetItemRectMax();
    auto* draw = ImGui::GetWindowDrawList();
    const auto base = tone_color(kind);
    ImVec4 fill{};
    if (kind == tone::normal)
        fill = mix(button_surface(), ImVec4{k_accent.x, k_accent.y, k_accent.z, 0.78f}, hover_t * 0.72f);
    else
        fill = mix(ImVec4{base.x, base.y, base.z, 0.85f}, ImVec4{base.x * 1.12f, base.y * 1.12f, base.z * 1.12f, 1.0f},
                   hover_t);
    fill = mix(fill, ImVec4{fill.x * 0.8f, fill.y * 0.8f, fill.z * 0.8f, fill.w}, held_t);
    if (hover_t > 0.01f)
        soft_shadow(draw, min, max, 9.0f, {0.0f, 3.0f}, 7.0f, {base.x, base.y, base.z, 1.0f}, hover_t * 0.24f);
    draw->AddRectFilled(min, max, packed(fill), 9.0f);
    if (kind == tone::normal)
        draw->AddRect(min, max, packed(mix(k_border, ImVec4{k_accent.x, k_accent.y, k_accent.z, 0.55f}, hover_t)),
                      9.0f);
    const auto* text = render::localization::tr(label);
    const auto text_size = ImGui::CalcTextSize(text);
    const auto ink = kind == tone::normal ? mix(k_text_muted, k_text_main, hover_t) : k_text_main;
    draw->PushClipRect(min, max, true);
    draw->AddText(
        {min.x + std::max(8.0f, (width - text_size.x) * 0.5f), min.y + (height - text_size.y) * 0.5f + held_t},
        packed(ink), text);
    draw->PopClipRect();
    return clicked;
}

bool hint_input(const char* id, const char* hint, char* buffer, const std::size_t size, const float width,
                const ImGuiInputTextFlags flags)
{
    ImGui::PushStyleColor(ImGuiCol_FrameBg, k_bg_panel);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, k_bg_hover);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, k_bg_hover);
    ImGui::PushStyleColor(ImGuiCol_Border, k_border_light);
    ImGui::PushStyleColor(ImGuiCol_Text, k_text_main);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, k_text_muted);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {10.0f, 7.0f});
    ImGui::SetNextItemWidth(width);
    const auto submitted = ImGui::InputTextWithHint(id, render::localization::tr(hint), buffer, size,
                                                    flags | ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(6);
    return submitted;
}

int action_menu(const char* id, const std::span<const menu_action> actions)
{
    constexpr float button = 30.0f;
    constexpr float row = 32.0f;
    ImGui::PushID(id);
    ImGui::InvisibleButton("##more", {button, button});
    const auto popup_id = ImGui::GetID("##actions");
    if (ImGui::IsItemClicked())
    {
        ImGui::OpenPopup("##actions");
        g_armed_actions.erase(popup_id);
    }
    pointer_cursor_if_hovered();
    const auto open = ImGui::IsPopupOpen("##actions");
    const auto hover_t = animate_state(g_hover_animations, ImGui::GetItemID(), ImGui::IsItemHovered() || open, 0.12f);
    const auto button_min = ImGui::GetItemRectMin();
    const auto button_max = ImGui::GetItemRectMax();
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(button_min, button_max, packed(mix(ImVec4{1, 1, 1, 0.04f}, ImVec4{1, 1, 1, 0.12f}, hover_t)),
                        8.0f);
    draw->AddRect(button_min, button_max, packed(mix(k_border, k_border_light, hover_t)), 8.0f);
    draw_glyph(draw, glyph::more, (button_min + button_max) * 0.5f, packed(mix(k_text_muted, k_text_main, hover_t)));

    float width = 170.0f;
    for (const auto& action : actions)
        width = std::max(width, ImGui::CalcTextSize(render::localization::tr(action.label)).x + 36.0f);
    width = std::max(width, ImGui::CalcTextSize(render::localization::tr("Click again to confirm")).x + 36.0f);
    const auto height = 8.0f + row * static_cast<float>(actions.size());
    ImGui::SetNextWindowPos({button_max.x - width, button_max.y + 6.0f}, ImGuiCond_Always);
    ImGui::SetNextWindowSize({width, height}, ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4{0, 0, 0, 0});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0.0f, 0.0f});
    int chosen = -1;
    if (ImGui::BeginPopup("##actions", ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar))
    {
        const auto min = ImGui::GetWindowPos();
        const ImVec2 max{min.x + width, min.y + height};
        auto* popup = ImGui::GetWindowDrawList();
        popup->PushClipRectFullScreen();
        soft_shadow(popup, min, max, 10.0f, {0.0f, 6.0f}, 12.0f, {0.0f, 0.0f, 0.0f, 1.0f}, 0.72f);
        popup->AddRectFilled(min, max, popup_backing(), 10.0f);
        draw_popup_surface(popup, min, max, 10.0f);
        popup->PopClipRect();
        const auto armed = g_armed_actions.contains(popup_id) ? g_armed_actions[popup_id] : -1;
        for (int index = 0; index < static_cast<int>(actions.size()); ++index)
        {
            const auto& action = actions[static_cast<std::size_t>(index)];
            ImGui::PushID(index);
            ImGui::SetCursorScreenPos({min.x + 4.0f, min.y + 4.0f + row * static_cast<float>(index)});
            ImGui::InvisibleButton("##action", {width - 8.0f, row});
            const auto hovered = ImGui::IsItemHovered();
            pointer_cursor_if_hovered();
            const auto item_min = ImGui::GetItemRectMin();
            const auto item_max = ImGui::GetItemRectMax();
            const auto item_hover = animate_state(g_hover_animations, ImGui::GetItemID(), hovered, 0.10f);
            const auto waiting = armed == index;
            const auto color = action.kind == tone::danger ? tone_color(tone::danger) : k_text_main;
            if (item_hover > 0.01f || waiting)
                popup->AddRectFilled(item_min + ImVec2{0.0f, 1.0f}, item_max - ImVec2{0.0f, 1.0f},
                                     packed(waiting ? ImVec4{color.x, color.y, color.z, 0.16f}
                                                    : mix(ImVec4{1, 1, 1, 0}, k_bg_hover, item_hover)),
                                     7.0f);
            const auto* text = render::localization::tr(waiting ? "Click again to confirm" : action.label);
            popup->AddText({item_min.x + 12.0f, item_min.y + (row - ImGui::GetTextLineHeight()) * 0.5f},
                           packed(action.kind == tone::danger ? color : mix(k_text_muted, k_text_main, item_hover)),
                           text);
            if (ImGui::IsItemClicked())
            {
                if (action.confirm && !waiting)
                    g_armed_actions[popup_id] = index;
                else
                {
                    chosen = index;
                    g_armed_actions.erase(popup_id);
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::PopID();
        }
        ImGui::EndPopup();
    }
    else
        g_armed_actions.erase(popup_id);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
    ImGui::PopID();
    return chosen;
}

void tile_background(ImDrawList* draw, const ImVec2 min, const ImVec2 max, const float hover, const bool active)
{
    auto fill = mix(ImVec4{1.0f, 1.0f, 1.0f, 0.035f}, ImVec4{1.0f, 1.0f, 1.0f, 0.07f}, hover);
    if (active)
        fill = mix(fill, ImVec4{k_accent.x, k_accent.y, k_accent.z, 0.16f}, 0.85f);
    draw->AddRectFilled(min, max, packed(fill), k_tile_rounding);
    const auto edge = active ? ImVec4{k_accent.x, k_accent.y, k_accent.z, 0.55f} : mix(k_border, k_border_light, hover);
    draw->AddRect(min, max, packed(edge), k_tile_rounding);
    if (active)
        draw->AddRectFilled({min.x + 1.0f, min.y + 12.0f}, {min.x + 4.0f, max.y - 12.0f}, packed(k_accent), 2.0f);
}
} // namespace ui::menu::detail
