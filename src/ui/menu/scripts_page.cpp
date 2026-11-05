#include <stdafx.hpp>
#include <config/settings.hpp>
#include <diagnostics/health.hpp>
#include <platform/windows/open_folder.hpp>
#include <render/fonts.hpp>
#include <render/presentation.hpp>
#include <render/text/localization.hpp>
#include <scripting/runtime.hpp>
#include <ui/menu/library_widgets.hpp>
#include <ui/overlay/overlay.hpp>
#include <ui/presentation.hpp>
#include <imgui.h>

#include <array>
#include <cstring>

namespace ui::menu::detail
{
namespace
{
constexpr float k_tile_height = 56.0f;
constexpr float k_tile_gap = 8.0f;

// What the Lua page keeps between frames: the open settings panel and the last result.
struct page_state
{
    std::string expanded{};
    feedback result{};
};

page_state g_page;

[[nodiscard]] ImVec4 state_color(const scripting::script_state state)
{
    switch (state)
    {
        case scripting::script_state::running:
            return {0.38f, 0.86f, 0.55f, 1.0f};
        case scripting::script_state::error:
        case scripting::script_state::over_budget:
            return {1.0f, 0.42f, 0.45f, 1.0f};
        case scripting::script_state::starting:
            return {0.96f, 0.78f, 0.36f, 1.0f};
        case scripting::script_state::stopped:
            break;
    }
    return k_text_muted;
}

// Lua names a chunk by its full path; the tile shows the file name only ("main.lua:12: ...").
[[nodiscard]] std::string short_error(const std::string& error)
{
    const auto location = error.find(".lua:");
    if (location == std::string::npos)
        return error;
    const auto separator = error.find_last_of("\\/", location);
    return separator == std::string::npos ? error : error.substr(separator + 1);
}

// One script-declared control row; a changed value goes straight back to the script.
void draw_script_control(const scripting::script_info& script, const scripting::ui_control& control)
{
    switch (control.kind)
    {
        case scripting::control_kind::text:
            begin_row(control.label.c_str(), 116.0f);
            if (const auto* value = std::get_if<std::string>(&control.value))
                clipped_row_text(*value);
            end_row();
            break;
        case scripting::control_kind::button:
            if (button_row(control.label.c_str(), control.action_text.empty() ? "Run" : control.action_text.c_str()))
                scripting::runtime().press_control(script.id, control.id);
            break;
        case scripting::control_kind::toggle:
        {
            auto value = std::get_if<bool>(&control.value) ? std::get<bool>(control.value) : false;
            const auto before = value;
            toggle_row(control.label.c_str(), value);
            if (value != before)
                scripting::runtime().set_control(script.id, control.id, value);
            break;
        }
        case scripting::control_kind::slider:
        {
            auto value =
                static_cast<float>(std::get_if<double>(&control.value) ? std::get<double>(control.value) : 0.0);
            const auto before = value;
            slider_row(control.label.c_str(), value, static_cast<float>(control.minimum),
                       static_cast<float>(control.maximum), "", static_cast<float>(control.step));
            if (value != before)
                scripting::runtime().set_control(script.id, control.id, static_cast<double>(value));
            break;
        }
        case scripting::control_kind::select:
        {
            auto value = std::get_if<int>(&control.value) ? std::get<int>(control.value) : 0;
            std::vector<const char*> labels{};
            labels.reserve(control.options.size());
            for (const auto& option : control.options)
                labels.push_back(option.c_str());
            const auto before = value;
            select_row(control.label.c_str(), value, labels);
            if (value != before)
                scripting::runtime().set_control(script.id, control.id, value);
            break;
        }
        case scripting::control_kind::input:
        {
            std::array<char, 512> buffer{};
            if (const auto* value = std::get_if<std::string>(&control.value))
                std::memcpy(buffer.data(), value->data(), std::min(value->size(), buffer.size() - 1));
            const auto before = std::string(buffer.data());
            text_input_row(control.label.c_str(), buffer.data(), buffer.size());
            if (before != buffer.data())
                scripting::runtime().set_control(script.id, control.id, std::string(buffer.data()));
            break;
        }
        case scripting::control_kind::color:
        {
            auto value = std::get_if<render::draw::rgba>(&control.value) ? std::get<render::draw::rgba>(control.value)
                                                                         : render::draw::rgba{255, 255, 255, 255};
            const auto before = value.packed();
            color_row(control.label.c_str(), value);
            if (before != value.packed())
                scripting::runtime().set_control(script.id, control.id, value);
            break;
        }
        case scripting::control_kind::keybind:
        {
            auto value = std::get_if<int>(&control.value) ? std::get<int>(control.value) : 0;
            const auto before = value;
            keybind_row(control.label.c_str(), value);
            if (before != value)
                scripting::runtime().set_control(script.id, control.id, value);
            break;
        }
        case scripting::control_kind::separator:
            begin_row(control.label.c_str(), 116.0f);
            ImGui::SeparatorText(control.label.c_str());
            end_row();
            break;
    }
}

[[nodiscard]] int detail_rows(const scripting::script_info& script, const std::vector<scripting::ui_control>& controls)
{
    return 3 + (script.error.empty() ? 0 : 1) + static_cast<int>(controls.size());
}

// The settings under an opened tile: load options, runtime cost, last error and the script's own controls.
void script_details(const scripting::script_info& script, const std::vector<scripting::ui_control>& controls,
                    const float width)
{
    const auto rows = detail_rows(script, controls);
    const auto height = static_cast<float>(rows) * k_row_height + 12.0f;
    const auto min = ImGui::GetCursorScreenPos();
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled({min.x + 10.0f, min.y - 4.0f}, {min.x + width, min.y + height}, packed(k_bg_panel), 10.0f);
    ImGui::SetCursorScreenPos({min.x + 22.0f, min.y + 4.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::BeginChild("##details", {width - 34.0f, height - 8.0f}, false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                          ImGuiWindowFlags_NoBackground);
    auto autoload = script.autoload;
    toggle_row("Autoload", autoload);
    if (autoload != script.autoload)
        scripting::runtime().set_autoload(script.id, autoload);
    auto hot_reload = script.hot_reload;
    toggle_row("Hot Reload", hot_reload);
    if (hot_reload != script.hot_reload)
        scripting::runtime().set_hot_reload(script.id, hot_reload);
    begin_row("Runtime", 150.0f);
    ImGui::TextColored(k_text_muted, "%.2f ms / %.1f MiB", script.last_callback_ms,
                       static_cast<double>(script.memory_bytes) / (1024.0 * 1024.0));
    end_row();
    if (!script.error.empty())
    {
        begin_row("Last Error", 116.0f);
        clipped_row_text(script.error, ImVec4{1.0f, 0.42f, 0.45f, 1.0f});
        end_row();
    }
    for (const auto& control : controls)
    {
        ImGui::PushID(control.id.c_str());
        draw_script_control(script, control);
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::SetCursorScreenPos({min.x, min.y + height + k_tile_gap});
}

// One script. Returns true when the list changed and must not be drawn further this frame.
bool script_tile(const scripting::script_info& script, const float width)
{
    auto& runtime = scripting::runtime();
    auto* draw = ImGui::GetWindowDrawList();
    const auto min = ImGui::GetCursorScreenPos();
    const auto max = ImVec2{min.x + width, min.y + k_tile_height};
    ImGui::Dummy({width, k_tile_height});
    const auto hovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(min, max);
    const auto hover_t = animate_state(g_hover_animations, ImGui::GetID("##tile"), hovered, 0.15f);
    tile_background(draw, min, max, hover_t, script.enabled);

    const auto expanded = g_page.expanded == script.id;
    const auto controls = runtime.controls(script.id);
    bool changed = false;

    // From the right edge: the menu of actions, the on/off switch, and the web radar's link button.
    constexpr float button = 30.0f;
    float x = max.x - 12.0f - button;
    ImGui::SetCursorScreenPos({x, min.y + (k_tile_height - button) * 0.5f});
    const std::array<menu_action, 3> actions{
        {{expanded ? "Hide settings" : "Settings"}, {"Reload"}, {"Delete", tone::danger, true}}};
    switch (action_menu("##menu", actions))
    {
        case 0:
            g_page.expanded = expanded ? std::string{} : script.id;
            break;
        case 1:
            runtime.reload(script.id);
            g_page.result.raise(script.id, "Reloaded", false);
            break;
        case 2:
            if (runtime.remove(script.id))
                changed = true;
            else
                g_page.result.raise(script.id, "Delete failed", true);
            break;
        default:
            break;
    }
    if (!changed)
    {
        x -= 10.0f + 46.0f;
        ImGui::SetCursorScreenPos({x, min.y + (k_tile_height - 24.0f) * 0.5f});
        auto running = script.enabled;
        ImGui::PushID("##enabled");
        toggle_control(running);
        ImGui::PopID();
        if (running != script.enabled)
            runtime.set_enabled(script.id, running);

        if (script.trusted && script.enabled)
            for (const auto& control : controls)
                if (control.id == "copy" && control.kind == scripting::control_kind::button)
                {
                    x -= 10.0f + 110.0f;
                    ImGui::SetCursorScreenPos({x, min.y + (k_tile_height - button) * 0.5f});
                    if (pill_button("##copy_link",
                                    control.action_text.empty() ? "Copy link" : control.action_text.c_str(), 110.0f,
                                    tone::accent))
                    {
                        runtime.press_control(script.id, control.id);
                        g_page.result.raise(script.id, "Link copied", false);
                    }
                }
    }

    // Name with its access badge, then the state or the last result.
    const auto text_x = min.x + 18.0f;
    const auto text_right = x - 10.0f;
    const auto* title_font = render::fonts().menu_semibold_13;
    ImFont* font = title_font && title_font->im_font ? title_font->im_font : ImGui::GetFont();
    const auto size = title_font && title_font->im_font ? title_font->font_size : ImGui::GetFontSize();
    draw->PushClipRect({text_x, min.y}, {text_right, max.y}, true);
    draw->AddText(font, size, {text_x, min.y + 10.0f}, packed(k_text_main), script.name.c_str());
    const auto name_width = font->CalcTextSizeA(size, FLT_MAX, 0.0f, script.name.c_str()).x;
    const auto badge_color = script.trusted ? k_accent : ImVec4{k_text_muted.x, k_text_muted.y, k_text_muted.z, 0.7f};
    const ImVec2 badge{text_x + name_width + 16.0f, min.y + 10.0f + size * 0.5f};
    draw_glyph(draw, glyph::shield, badge, packed(badge_color));
    draw->AddText({badge.x + 11.0f, min.y + 10.0f + (size - ImGui::GetFontSize()) * 0.5f}, packed(badge_color),
                  render::localization::tr(script.trusted ? "Verified" : "Sandbox"));

    std::string subtitle{};
    ImVec4 color = state_color(script.state);
    if (g_page.result.visible_for(script.id))
    {
        subtitle = render::localization::tr(g_page.result.text);
        color = g_page.result.color();
    }
    else if (!script.error.empty() &&
             (script.state == scripting::script_state::error || script.state == scripting::script_state::over_budget))
        subtitle = short_error(script.error);
    else
    {
        subtitle = render::localization::tr(scripting::runtime_t::state_name(script.state));
        if (!script.version.empty())
            subtitle += "  ·  v" + script.version;
        if (!script.author.empty())
            subtitle += "  ·  " + script.author;
    }
    draw->AddText({text_x, min.y + 31.0f}, packed(color), subtitle.c_str());
    draw->PopClipRect();

    ImGui::SetCursorScreenPos({min.x, max.y + k_tile_gap});
    if (expanded && !changed)
        script_details(script, controls, width);
    return changed;
}

constexpr std::array<const char*, 2> k_header_labels{"Import", "Open folder"};

[[nodiscard]] float header_width()
{
    return small_button_width(k_header_labels[0], glyph::import) +
           small_button_width(k_header_labels[1], glyph::folder) + 8.0f;
}

void header_actions()
{
    if (small_button("##import", glyph::import, k_header_labels[0]))
        bound_overlay().request_file(ui::overlay_t::file_request::import_lua);
    ImGui::SameLine(0.0f, 8.0f);
    if (small_button("##open_folder", glyph::folder, k_header_labels[1]))
        static_cast<void>(platform::windows::open_folder(scripting::runtime().scripts_path()));
}
} // namespace

void draw_script_library()
{
    wide_card(
        "script_library", "LUA SCRIPTS",
        [&]
        {
            const auto width = ImGui::GetContentRegionAvail().x;
            if (diagnostics::health().scripting.snapshot().value == diagnostics::readiness::state::error)
            {
                ImGui::TextColored({1.0f, 0.42f, 0.45f, 1.0f}, "%s",
                                   render::localization::tr("Lua storage unavailable"));
                ImGui::Dummy({0.0f, 4.0f});
            }

            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{0, 0, 0, 0});
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
            ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 4.0f);
            ImGui::BeginChild("##script_list", {width, std::max(80.0f, ImGui::GetContentRegionAvail().y)}, false);
            const auto tile_width = width - (ImGui::GetScrollMaxY() > 0.0f ? 10.0f : 0.0f);
            const auto scripts = scripting::runtime().scripts();
            if (scripts.empty())
            {
                const auto* text = render::localization::tr("No scripts");
                const auto text_size = ImGui::CalcTextSize(text);
                ImGui::SetCursorPos(
                    {(tile_width - text_size.x) * 0.5f, std::max(0.0f, ImGui::GetContentRegionAvail().y * 0.4f)});
                ImGui::TextColored(k_text_muted, "%s", text);
            }
            for (const auto& script : scripts)
            {
                ImGui::PushID(script.id.c_str());
                const auto changed = script_tile(script, tile_width);
                ImGui::PopID();
                if (changed)
                    break;
            }
            ImGui::Dummy({0.0f, 0.0f});
            ImGui::EndChild();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor();
        },
        header_width(), [] { header_actions(); });
}
} // namespace ui::menu::detail
