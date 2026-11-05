#include <stdafx.hpp>
#include <config/settings.hpp>
#include <config/first_run.hpp>
#include <config/hotkeys.hpp>
#include <render/text/localization.hpp>
#include <ui/menu/menu.hpp>
#include <platform/windows/open_folder.hpp>
#include <platform/windows/storage.hpp>
#include <imgui.h>
#include <imgui_internal.h>

#include <ui/menu/internal.hpp>

namespace ui
{
using namespace ui::menu::detail;

namespace
{
constexpr int guide_steps = 4;
constexpr float k_width = 600.0f;
constexpr float k_height = 470.0f;
constexpr float k_margin = 48.0f;
constexpr float k_radius = 22.0f;
constexpr float k_content_width = k_width - 2.0f * k_margin;
constexpr const char* k_window_name = "##vesta_guide";
constexpr const char* k_dim_name = "##vesta_guide_dim";

float ease_out(float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return 1.0f - std::pow(1.0f - t, 3.0f);
}

ImU32 faded(const ImVec4& color, float alpha)
{
    return packed(ImVec4{color.x, color.y, color.z, color.w * alpha});
}

// Frame-rate independent exponential approach used for hover and indicator animations.
void relax(float& value, float target, float delta_time, float rate = 16.0f)
{
    value += (target - value) * (1.0f - std::exp(-rate * delta_time));
}

void card(ImDrawList* draw, ImVec2 min, ImVec2 max, float alpha, float emphasis = 0.0f)
{
    draw->AddRectFilled(min, max, faded({1.0f, 1.0f, 1.0f, 0.045f + 0.035f * emphasis}, alpha), 12.0f);
    draw->AddRect(min, max, faded({1.0f, 1.0f, 1.0f, 0.07f + 0.05f * emphasis}, alpha), 12.0f);
}

// Plain pill button in the menu palette. The primary one carries the accent.
bool guide_button(const char* label, float width, bool primary)
{
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, primary ? k_accent : ImVec4{1.0f, 1.0f, 1.0f, 0.06f});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          primary ? ImVec4{k_accent.x * 1.15f, k_accent.y * 1.15f, k_accent.z * 1.15f, 1.0f}
                                  : ImVec4{1.0f, 1.0f, 1.0f, 0.12f});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, primary ? k_accent : ImVec4{1.0f, 1.0f, 1.0f, 0.18f});
    ImGui::PushStyleColor(ImGuiCol_Text, k_text_main);
    const bool pressed = ImGui::Button(label, {width, 34.0f});
    pointer_cursor_if_hovered();
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();
    return pressed;
}

// Drop-down in the look of the menu's own select rows: control, chevron, blurred popup with a check on the chosen row.
void key_select(const char* label, std::uint16_t current, std::uint16_t& chosen, float width)
{
    namespace pw = platform::windows;
    chosen = current;
    constexpr float control_height = 30.0f, option_height = 30.0f;
    constexpr std::size_t visible_options = 7;

    ImGui::PushID(label);
    const auto button_id = ImGui::GetID("##key");
    ImGui::InvisibleButton("##key", {width, control_height});
    const bool opened = ImGui::IsItemClicked();
    if (opened)
        ImGui::OpenPopup("##keys");
    pointer_cursor_if_hovered();
    const bool hovered = ImGui::IsItemHovered();
    const bool popup_open = ImGui::IsPopupOpen("##keys");
    const auto hover_t = animate_state(g_hover_animations, button_id, hovered, 0.14f);
    const auto open_t = animate_state(g_active_animations, button_id, popup_open, 0.16f);
    const auto min = ImGui::GetItemRectMin();
    const auto max = ImGui::GetItemRectMax();

    auto* draw = ImGui::GetWindowDrawList();
    const float alpha = ImGui::GetStyle().Alpha;
    const auto control_bg = control_surface(std::max(hover_t, open_t));
    draw->AddRectFilled(min, max, faded(control_bg, alpha), 9.0f);
    auto border = mix(k_border, k_border_light, hover_t);
    border = mix(border, k_accent, open_t * 0.65f);
    border.w = 0.08f + hover_t * 0.08f + open_t * 0.20f;
    draw->AddRect(min, max, faded(border, alpha), 9.0f, 0, 1.0f);
    const auto* text = config::lifecycle_key_name(current);
    draw->AddText({min.x + 12.0f, min.y + (control_height - ImGui::GetFontSize()) * 0.5f}, faded(k_text_main, alpha),
                  text);
    draw_dropdown_chevron(draw, {max.x - 15.0f, min.y + control_height * 0.5f},
                          faded(mix(k_text_muted, k_text_main, std::max(hover_t, open_t)), alpha), open_t);

    const auto choices = config::lifecycle_key_choices();
    const auto shown = std::min(choices.size(), visible_options);
    const float popup_height = 8.0f + static_cast<float>(shown) * option_height;
    const auto* viewport = ImGui::GetMainViewport();
    ImVec2 position{max.x - width, max.y + 4.0f};
    if (position.y + popup_height > viewport->Pos.y + viewport->Size.y - 8.0f)
        position.y = min.y - popup_height - 4.0f;
    ImGui::SetNextWindowPos(position, ImGuiCond_Always);
    ImGui::SetNextWindowSize({width, popup_height}, ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4{0, 0, 0, 0});
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4{0, 0, 0, 0});
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, k_border_light);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0.0f, 0.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 4.0f);
    if (ImGui::BeginPopup("##keys", ImGuiWindowFlags_NoBackground))
    {
        const auto popup_min = ImGui::GetWindowPos();
        const ImVec2 popup_max{popup_min.x + width, popup_min.y + popup_height};
        auto* popup_draw = ImGui::GetWindowDrawList();
        popup_draw->PushClipRectFullScreen();
        soft_shadow(popup_draw, popup_min, popup_max, 10.0f, {0.0f, 6.0f}, 12.0f, {0.0f, 0.0f, 0.0f, 1.0f}, 0.72f);
        // Under the translucent surface: the text of the page below must not show through the list.
        popup_draw->AddRectFilled(popup_min, popup_max, popup_backing(), 10.0f);
        draw_popup_surface(popup_draw, popup_min, popup_max, 10.0f);
        popup_draw->PopClipRect();
        ImGui::SetCursorPos({0.0f, 4.0f});
        for (std::size_t i = 0; i < choices.size(); ++i)
        {
            ImGui::PushID(static_cast<int>(i));
            const auto row_min = ImGui::GetCursorScreenPos();
            const auto option_id = ImGui::GetID("##option");
            ImGui::InvisibleButton("##option",
                                   {width - 8.0f - (choices.size() > visible_options ? 4.0f : 0.0f), option_height});
            const bool option_hovered = ImGui::IsItemHovered();
            pointer_cursor_if_hovered();
            const bool selected = choices[i].virtual_key == current;
            const auto option_hover_t = animate_state(g_hover_animations, option_id, option_hovered, 0.11f);
            const auto selected_t = animate_state(g_active_animations, option_id, selected, 0.14f);
            auto option_bg = mix(ImVec4{1, 1, 1, 0}, k_bg_hover, option_hover_t);
            option_bg = mix(option_bg, ImVec4{k_accent.x, k_accent.y, k_accent.z, 0.13f}, selected_t);
            const auto item_min = ImVec2{popup_min.x + 4.0f, row_min.y + 1.0f};
            const auto item_max = ImVec2{ImGui::GetItemRectMax().x + 4.0f, row_min.y + option_height - 1.0f};
            popup_draw->AddRectFilled(item_min, item_max, packed(option_bg), 7.0f);
            popup_draw->AddText(
                {item_min.x + 10.0f + option_hover_t * 2.0f, row_min.y + (option_height - ImGui::GetFontSize()) * 0.5f},
                packed(mix(k_text_muted, k_text_main, std::max(option_hover_t, selected_t))), choices[i].name);
            draw_checkmark(popup_draw, {item_max.x - 14.0f, row_min.y + option_height * 0.5f},
                           packed(ImVec4{k_accent.x, k_accent.y, k_accent.z, selected_t}), selected_t);
            if (ImGui::IsItemClicked())
            {
                chosen = choices[i].virtual_key;
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopID();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(5);
    ImGui::PopStyleColor(3);
    ImGui::PopID();
}

// A key shown as a small cap; returns its width.
float keycap(ImDrawList* draw, ImVec2 position, const char* name, float alpha)
{
    const auto size = ImGui::CalcTextSize(name);
    const ImVec2 max{position.x + size.x + 20.0f, position.y + 24.0f};
    draw->AddRectFilled(position, max, faded({1.0f, 1.0f, 1.0f, 0.08f}, alpha), 7.0f);
    draw->AddRect(position, max, faded({1.0f, 1.0f, 1.0f, 0.14f}, alpha), 7.0f);
    draw->AddText({position.x + 10.0f, position.y + (24.0f - size.y) * 0.5f}, faded(k_text_main, alpha), name);
    return max.x - position.x;
}

// Scales a finished window's draw list around its centre: the opening and closing pop.
void scale_window_geometry(const char* name, ImVec2 center, float scale)
{
    auto* window = ImGui::FindWindowByName(name);
    if (!window || !window->DrawList || std::abs(scale - 1.0f) < 0.0001f)
        return;
    for (auto& vertex : window->DrawList->VtxBuffer)
    {
        vertex.pos.x = center.x + (vertex.pos.x - center.x) * scale;
        vertex.pos.y = center.y + (vertex.pos.y - center.y) * scale;
    }
    for (auto& command : window->DrawList->CmdBuffer)
    {
        command.ClipRect.x = center.x + (command.ClipRect.x - center.x) * scale;
        command.ClipRect.y = center.y + (command.ClipRect.y - center.y) * scale;
        command.ClipRect.z = center.x + (command.ClipRect.z - center.x) * scale;
        command.ClipRect.w = center.y + (command.ClipRect.w - center.y) * scale;
    }
}
constexpr float k_content_top = 112.0f;
constexpr float k_footer_y = k_height - 68.0f;

// The content of one step: it slides in from the side it came from and fades with the guide.
struct guide_content
{
    ImDrawList* draw{};
    ImVec2 origin{};
    float slide{};
    float alpha{};

    [[nodiscard]] ImVec2 screen(const float y) const { return {origin.x + k_margin + slide, origin.y + y}; }
    void place(const float y) const { ImGui::SetCursorPos({k_margin + slide, y}); }
    void paragraph(const float y, const char* english, const bool muted) const
    {
        place(y);
        if (muted)
            ImGui::PushStyleColor(ImGuiCol_Text, k_text_muted);
        ImGui::PushTextWrapPos(k_margin + slide + k_content_width);
        ImGui::TextUnformatted(render::localization::tr(english));
        ImGui::PopTextWrapPos();
        if (muted)
            ImGui::PopStyleColor();
    }
};

// The page behind is dimmed and cannot be clicked while the guide is up.
void draw_backdrop(const ImGuiViewport* viewport, const float appear)
{
    ImGui::SetNextWindowPos(viewport->Pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(viewport->Size, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::Begin(k_dim_name, nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::GetWindowDrawList()->AddRectFilled(viewport->Pos,
                                              {viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y},
                                              IM_COL32(3, 3, 8, static_cast<int>(170.0f * appear)));
    ImGui::End();
    ImGui::PopStyleVar(3);
}

void draw_panel(ImDrawList* draw, const ImVec2 origin, const float appear)
{
    const ImVec2 panel_max{origin.x + k_width, origin.y + k_height};
    draw->PushClipRectFullScreen();
    soft_shadow(draw, origin, panel_max, k_radius, {0.0f, 18.0f}, 44.0f, {0, 0, 0, 1}, appear);
    draw->PopClipRect();
    draw->AddRectFilled(origin, panel_max, faded(k_bg_base, appear), k_radius);
    draw->AddRect(origin, panel_max, faded(k_border, appear), k_radius);
    draw->AddLine({origin.x + k_radius, origin.y + 1.0f}, {panel_max.x - k_radius, origin.y + 1.0f},
                  faded({1.0f, 1.0f, 1.0f, 0.08f}, appear));
}

void draw_title(const guide_content& content, const int step)
{
    namespace loc = render::localization;
    constexpr const char* titles[guide_steps]{"Welcome to Vesta", "Hotkeys", "Good to know", "Performance"};
    const auto size = ImGui::GetFontSize() * 1.6f;
    const auto position = content.screen(40.0f);
    content.draw->AddText(ImGui::GetFont(), size, position, faded(k_text_main, content.alpha), loc::tr(titles[step]));
    char counter[64]{};
    std::snprintf(counter, sizeof(counter), loc::tr("Step %d of %d"), step + 1, guide_steps);
    content.draw->AddText({position.x, position.y + size + 4.0f}, faded(k_text_muted, content.alpha), counter);
}

// Step 1: two cards, English and Russian; a click switches the language at once.
void draw_language_step(const guide_content& content, float (&hover)[2], const float dt)
{
    content.paragraph(k_content_top, "Choose your language. It applies at once and is saved with your settings.",
                      false);
    constexpr float gap = 14.0f, card_height = 112.0f;
    const float card_width = (k_content_width - gap) * 0.5f;
    constexpr const char* names[]{"English", "Русский"};
    constexpr const char* hints[]{"Interface in English", "Интерфейс на русском"};
    const int current = config::general_settings.language;
    auto* draw = content.draw;
    for (int i = 0; i < 2; ++i)
    {
        const auto row = content.screen(k_content_top + 62.0f);
        const ImVec2 min{row.x + static_cast<float>(i) * (card_width + gap), row.y};
        const ImVec2 max{min.x + card_width, min.y + card_height};
        ImGui::SetCursorScreenPos(min);
        ImGui::PushID(i);
        const bool clicked = ImGui::InvisibleButton("##language", {card_width, card_height});
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        pointer_cursor_if_hovered();
        relax(hover[i], hovered ? 1.0f : 0.0f, dt);
        const bool selected = current == i;
        if (selected)
        {
            draw->AddRectFilled(min, max, faded({k_accent.x, k_accent.y, k_accent.z, 0.16f}, content.alpha), 12.0f);
            draw->AddRect(min, max, faded({k_accent.x, k_accent.y, k_accent.z, 0.95f}, content.alpha), 12.0f, 0, 1.5f);
        }
        else
        {
            card(draw, min, max, content.alpha, hover[i]);
        }
        const auto size = ImGui::GetFontSize() * 1.6f;
        draw->AddText(ImGui::GetFont(), size, {min.x + 22.0f, min.y + 26.0f}, faded(k_text_main, content.alpha),
                      names[i]);
        draw->AddText({min.x + 22.0f, min.y + 26.0f + size + 6.0f}, faded(k_text_muted, content.alpha), hints[i]);
        if (selected)
        {
            const ImVec2 badge{max.x - 26.0f, min.y + 26.0f};
            draw->AddCircleFilled(badge, 10.0f, faded(k_accent, content.alpha), 24);
            const auto tick = faded({1, 1, 1, 1}, content.alpha);
            draw->AddLine({badge.x - 4.5f, badge.y + 0.5f}, {badge.x - 1.5f, badge.y + 3.5f}, tick, 1.8f);
            draw->AddLine({badge.x - 1.5f, badge.y + 3.5f}, {badge.x + 4.5f, badge.y - 3.0f}, tick, 1.8f);
        }
        if (clicked && !selected)
            config::general_settings.language = i;
    }
}

[[nodiscard]] const char* key_change_message(const config::key_change_result result)
{
    switch (result)
    {
        case config::key_change_result::saved:
            return nullptr;
        case config::key_change_result::rejected_duplicate:
            return "The two actions need different keys.";
        case config::key_change_result::rejected_invalid:
            return "This key cannot be used.";
        case config::key_change_result::write_failed:
            return "The new keys work now, but hotkeys.cfg could not be saved.";
    }
    return nullptr;
}

// Step 2: the menu and exit keys, changeable here; `message` keeps the outcome of the last change.
void draw_hotkeys_step(const guide_content& content, const char*& message)
{
    content.paragraph(k_content_top,
                      "These keys work in the game. Change them if they clash with something. Changes apply "
                      "immediately and are saved to hotkeys.cfg.",
                      false);
    const auto keys = config::lifecycle_keys();
    std::uint16_t menu_choice{}, exit_choice{};
    constexpr float row_height = 50.0f, combo_width = 170.0f;
    const struct
    {
        const char* label;
        std::uint16_t current;
        std::uint16_t* chosen;
    } rows[]{{"Open / close the menu", keys.menu, &menu_choice}, {"Exit Vesta", keys.exit, &exit_choice}};
    float y = k_content_top + 62.0f;
    for (const auto& row : rows)
    {
        const auto min = content.screen(y);
        const ImVec2 max{min.x + k_content_width, min.y + row_height};
        card(content.draw, min, max, content.alpha);
        content.draw->AddText({min.x + 18.0f, min.y + (row_height - ImGui::GetFontSize()) * 0.5f},
                              faded(k_text_main, content.alpha), render::localization::tr(row.label));
        ImGui::SetCursorScreenPos({max.x - 14.0f - combo_width, min.y + (row_height - ImGui::GetFrameHeight()) * 0.5f});
        key_select(row.label, row.current, *row.chosen, combo_width);
        y += row_height + 10.0f;
    }
    if (menu_choice != keys.menu || exit_choice != keys.exit)
        message = key_change_message(config::set_lifecycle_keys(menu_choice, exit_choice));

    if (!message)
    {
        content.paragraph(y + 6.0f,
                          "Movement keys are released while the menu is open, so your character does not keep running.",
                          true);
        return;
    }
    content.place(y + 6.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.96f, 0.72f, 0.26f, 1.0f});
    ImGui::PushTextWrapPos(k_margin + content.slide + k_content_width);
    ImGui::TextUnformatted(render::localization::tr(message));
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

// Step 3: where the data lives, the chosen keys, and where to get help.
void draw_notes_step(const guide_content& content)
{
    namespace loc = render::localization;
    namespace pw = platform::windows;
    constexpr float data_height = 100.0f;
    auto min = content.screen(k_content_top - 4.0f);
    card(content.draw, min, {min.x + k_content_width, min.y + data_height}, content.alpha);
    ImGui::SetCursorPos({k_margin + content.slide + 18.0f, k_content_top + 10.0f});
    ImGui::PushTextWrapPos(k_margin + content.slide + k_content_width - 18.0f);
    ImGui::TextUnformatted(loc::tr("Settings, cache and logs live in %TEMP%\\vesta. Cleaning tools may remove it; "
                                   "Vesta then starts with default settings."));
    ImGui::PopTextWrapPos();
    ImGui::SetCursorPos({k_margin + content.slide + 18.0f, k_content_top + data_height - 44.0f});
    if (guide_button(loc::tr("Open data folder"), 190.0f, false))
        static_cast<void>(pw::open_folder(pw::runtime_storage::paths().root)); // logs its own failure

    const auto keys = config::lifecycle_keys();
    min = content.screen(k_content_top + data_height + 8.0f);
    constexpr float keys_height = 78.0f;
    card(content.draw, min, {min.x + k_content_width, min.y + keys_height}, content.alpha);
    const struct
    {
        const char* name;
        const char* label;
    } hints[]{{config::lifecycle_key_name(keys.menu), "Open / close the menu"},
              {config::lifecycle_key_name(keys.exit), "Exit Vesta"}};
    float line = min.y + 14.0f;
    for (const auto& hint : hints)
    {
        const auto width = keycap(content.draw, {min.x + 18.0f, line}, hint.name, content.alpha);
        content.draw->AddText({min.x + 18.0f + width + 12.0f, line + (24.0f - ImGui::GetFontSize()) * 0.5f},
                              faded(k_text_main, content.alpha), loc::tr(hint.label));
        line += 30.0f;
    }
    content.paragraph(k_content_top + data_height + keys_height + 20.0f,
                      "Something wrong? Misc > Configs > Diagnostics saves a report you can attach to a bug report. "
                      "You can repeat this guide there.",
                      true);
}

// Step 4: Vesta runs beside the game and shares its hardware; what to do when the game leaves nothing spare.
void draw_performance_step(const guide_content& content)
{
    namespace loc = render::localization;
    content.paragraph(k_content_top,
                      "Vesta is external: it runs next to the game as its own program and uses your computer's "
                      "resources, mostly the CPU, and the GPU for some features.",
                      false);

    constexpr const char* tips[]{
        "Lower the in-game graphics to the minimum: shaders, particles, shadows.",
        "Limit the game's frame rate, for example with fps_max in the console.",
        "Turn off GPU Effects in Misc > General: every feature that uses the GPU is disabled.",
        "Lower the overlay's own FPS Limiter there: fewer frames, less CPU time.",
    };
    constexpr float badge_size = 22.0f, gap = 12.0f, padding = 18.0f;
    const float text_x = padding + badge_size + gap;
    const float wrap_width = k_content_width - text_x - padding;
    const float line_height = ImGui::GetTextLineHeight();

    float height = padding * 2.0f + ImGui::GetTextLineHeightWithSpacing() + 6.0f;
    float tip_heights[std::size(tips)]{};
    for (std::size_t i = 0; i < std::size(tips); ++i)
    {
        tip_heights[i] =
            std::max(badge_size, ImGui::CalcTextSize(loc::tr(tips[i]), nullptr, false, wrap_width).y) + 10.0f;
        height += tip_heights[i];
    }

    const auto min = content.screen(k_content_top + 66.0f);
    card(content.draw, min, {min.x + k_content_width, min.y + height}, content.alpha);
    content.draw->AddText({min.x + padding, min.y + padding}, faded(k_text_muted, content.alpha),
                          loc::tr("If the game uses all or almost all of your hardware:"));

    float y = min.y + padding + ImGui::GetTextLineHeightWithSpacing() + 6.0f;
    for (std::size_t i = 0; i < std::size(tips); ++i)
    {
        const ImVec2 badge{min.x + padding + badge_size * 0.5f, y + badge_size * 0.5f};
        content.draw->AddCircleFilled(badge, badge_size * 0.5f,
                                      faded({k_accent.x, k_accent.y, k_accent.z, 0.22f}, content.alpha), 24);
        char number[4]{};
        std::snprintf(number, sizeof(number), "%zu", i + 1);
        const auto number_size = ImGui::CalcTextSize(number);
        content.draw->AddText({badge.x - number_size.x * 0.5f, badge.y - number_size.y * 0.5f},
                              faded(k_text_main, content.alpha), number);

        const float text_y = y + std::max(0.0f, (badge_size - line_height) * 0.5f);
        ImGui::SetCursorScreenPos({min.x + text_x, text_y});
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap_width);
        ImGui::TextUnformatted(loc::tr(tips[i]));
        ImGui::PopTextWrapPos();
        y += tip_heights[i];
    }
}

// Progress dots; the current step stretches into a pill.
void draw_progress(ImDrawList* draw, const ImVec2 origin, const float (&dots)[guide_steps], const float appear)
{
    constexpr float dot = 7.0f, wide = 24.0f, gap = 8.0f;
    float total = gap * (guide_steps - 1);
    for (const auto value : dots)
        total += dot + (wide - dot) * value;
    float x = origin.x + (k_width - total) * 0.5f;
    const float y = origin.y + k_footer_y + 17.0f - dot * 0.5f;
    for (const auto value : dots)
    {
        const float width = dot + (wide - dot) * value;
        draw->AddRectFilled({x, y}, {x + width, y + dot},
                            faded(mix(ImVec4{1.0f, 1.0f, 1.0f, 0.18f}, k_accent, value), appear), dot * 0.5f);
        x += width + gap;
    }
}

// The guide is done or skipped: it is not shown again (the marker write logs its own failure).
void finish_guide()
{
    static_cast<void>(config::first_run::complete(platform::windows::runtime_storage::paths().configs));
}

} // namespace

void menu_t::draw_guide()
{
    namespace loc = render::localization;

    const float dt = std::min(ImGui::GetIO().DeltaTime, 1.0f / 30.0f);
    const bool wanted = config::first_run::pending();

    // Opening is a soft rise, closing is quicker.
    m_guide_anim = wanted ? std::min(1.0f, m_guide_anim + dt / 0.36f) : std::max(0.0f, m_guide_anim - dt / 0.20f);
    if (!wanted && m_guide_anim <= 0.0f)
    {
        m_guide_visible.store(false, std::memory_order_release);
        m_guide_step = 0;
        m_guide_shown_step = 0;
        m_guide_step_anim = 1.0f;
        m_guide_message = nullptr;
        return;
    }
    const float appear = wanted ? ease_out(m_guide_anim) : m_guide_anim;

    // A change of step slides the new content in from the side it came from.
    if (m_guide_step != m_guide_shown_step)
    {
        m_guide_direction = m_guide_step > m_guide_shown_step ? 1 : -1;
        m_guide_shown_step = m_guide_step;
        m_guide_step_anim = 0.0f;
    }
    m_guide_step_anim = std::min(1.0f, m_guide_step_anim + dt / 0.30f);
    const float settle = ease_out(m_guide_step_anim);
    for (int i = 0; i < guide_steps; ++i)
        relax(m_guide_dot[i], i == m_guide_step ? 1.0f : 0.0f, dt, 14.0f);

    const auto* viewport = ImGui::GetMainViewport();
    draw_backdrop(viewport, appear);

    const ImVec2 center{viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.5f};
    ImGui::SetNextWindowPos({center.x - k_width * 0.5f, center.y - k_height * 0.5f + (1.0f - appear) * 18.0f},
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize({k_width, k_height}, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, k_radius);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, k_text_main);
    ImGui::Begin(k_window_name, nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse);

    const auto origin = ImGui::GetWindowPos();
    auto* draw = ImGui::GetWindowDrawList();
    draw_panel(draw, origin, appear);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, std::max(appear, 0.001f));

    const guide_content content{.draw = draw,
                                .origin = origin,
                                .slide = (1.0f - settle) * 26.0f * static_cast<float>(m_guide_direction),
                                .alpha = appear * settle};
    draw_title(content, m_guide_step);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, std::max(content.alpha, 0.001f));
    if (m_guide_step == 0)
        draw_language_step(content, m_guide_hover, dt);
    else if (m_guide_step == 1)
        draw_hotkeys_step(content, m_guide_message);
    else if (m_guide_step == 2)
        draw_notes_step(content);
    else
        draw_performance_step(content);
    ImGui::PopStyleVar(); // content alpha

    // Footer: navigation and progress. It does not slide, only the page above does.
    const bool last = m_guide_step == guide_steps - 1;
    if (m_guide_step > 0)
    {
        ImGui::SetCursorPos({k_margin, k_footer_y});
        if (guide_button(loc::tr("Back"), 110.0f, false))
        {
            --m_guide_step;
            m_guide_message = nullptr;
        }
    }
    ImGui::SetCursorPos({k_width - k_margin - 130.0f, k_footer_y});
    if (guide_button(loc::tr(last ? "Finish" : "Next"), 130.0f, true))
    {
        if (last)
        {
            finish_guide();
        }
        else
        {
            ++m_guide_step;
            m_guide_message = nullptr;
        }
    }
    if (!last)
    {
        const auto label = loc::tr("Skip");
        const auto size = ImGui::CalcTextSize(label);
        ImGui::SetCursorPos({k_width - k_margin - size.x - 8.0f, 42.0f});
        ImGui::InvisibleButton("##skip", {size.x + 8.0f, size.y + 8.0f});
        const bool hovered = ImGui::IsItemHovered();
        pointer_cursor_if_hovered();
        draw->AddText({origin.x + k_width - k_margin - size.x - 4.0f, origin.y + 46.0f},
                      faded(hovered ? k_text_main : k_text_muted, appear), label);
        if (ImGui::IsItemClicked())
            finish_guide();
    }
    draw_progress(draw, origin, m_guide_dot, appear);

    ImGui::PopStyleVar(); // window alpha
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);

    scale_window_geometry(k_window_name, {origin.x + k_width * 0.5f, origin.y + k_height * 0.5f},
                          0.95f + 0.05f * appear);

    if (m_guide_front_pending)
    {
        // The menu may have been focused since the guide was last shown: put the dim and the guide above it.
        if (auto* dim = ImGui::FindWindowByName(k_dim_name))
            ImGui::BringWindowToDisplayFront(dim);
        if (auto* window = ImGui::FindWindowByName(k_window_name))
            ImGui::BringWindowToDisplayFront(window);
        m_guide_front_pending = false;
    }
}
} // namespace ui
