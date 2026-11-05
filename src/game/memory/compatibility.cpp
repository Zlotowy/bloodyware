#include <stdafx.hpp>
#include <game/memory/remote_pointer.hpp>
#include <game/memory/compatibility.hpp>
#include <game/memory/compatibility_contracts.hpp>

namespace game::compatibility
{
namespace
{
// Panel vtable slots of the float getters for width, height, x and y, and of the visibility getter.
constexpr std::uintptr_t k_panel_width_getter = 0x320;
constexpr std::uintptr_t k_panel_height_getter = 0x328;
constexpr std::uintptr_t k_panel_x_getter = 0x398;
constexpr std::uintptr_t k_panel_y_getter = 0x3a0;
constexpr std::uintptr_t k_panel_visible_getter = 0xb8;
constexpr std::size_t k_max_cached_panel_tables = 16;
// HUD directory: CUtlVector of {.., name, element} records, 32 bytes each.
constexpr std::uintptr_t k_hud_directory_vector = 0x258;
constexpr std::size_t k_hud_entry_stride = 32;
constexpr std::size_t k_hud_entry_name = 16;
constexpr std::size_t k_hud_entry_object = 24;
constexpr std::size_t k_max_hud_entries = 1024;

// A value worked out from the game image once, the first time it is asked for after binding.
template<typename value_t>
class probe
{
public:
    template<typename compute_t>
    [[nodiscard]] value_t get(compute_t&& compute)
    {
        std::call_once(m_once, [&] { m_value = compute(); });
        return m_value;
    }

private:
    std::once_flag m_once{};
    value_t m_value{};
};

struct module_state
{
    std::atomic<const memory_services*> services{};
    probe<std::uint32_t> shot_punch{};
    probe<std::uintptr_t> spread_patterns{};
    probe<std::optional<radar_layout>> radar{};
    probe<std::uintptr_t> hud_directory{};
    // Panel layouts by vtable; panels of one class share a layout.
    std::mutex panel_mutex{};
    std::unordered_map<std::uintptr_t, panel_layout> panels{};
};

module_state& state() noexcept
{
    static module_state value{};
    return value;
}

[[nodiscard]] const memory_services* services() noexcept
{
    return state().services.load(std::memory_order_acquire);
}

[[nodiscard]] const platform::windows::process_session& process()
{
    return services()->process;
}

[[nodiscard]] bool inside_client(const std::uintptr_t address, const std::size_t bytes)
{
    const auto base = services()->modules.client;
    const auto size = process().module_image_size(base);
    return address >= base && address - base <= size && bytes <= size - (address - base);
}

[[nodiscard]] std::uintptr_t unique(const std::string_view name, const std::string_view pattern)
{
    std::size_t count{};
    const auto address = process().scan_unique_code_signature(services()->modules.client, pattern, count);
    if (!address)
        services()->log.warning("[compat] {}: signature matches={}, feature unavailable", name, count);
    return address;
}

[[nodiscard]] std::uint32_t read_float_getter(const std::uintptr_t table, const std::uintptr_t slot)
{
    std::array<std::byte, 16> bytes{};
    const auto function = process().load<std::uintptr_t>(table + slot);
    return is_user_pointer(function) && process().copy(function, bytes.data(), bytes.size())
               ? detail::float_getter(bytes)
               : 0;
}

// movzx eax, byte ptr [rcx+disp32]; shr al, 3; ...; ret
[[nodiscard]] std::uint32_t read_visibility_getter(const std::uintptr_t table)
{
    std::array<std::byte, 13> code{};
    const auto function = process().load<std::uintptr_t>(table + k_panel_visible_getter);
    if (!is_user_pointer(function) || !process().copy(function, code.data(), code.size()))
        return 0;
    const auto matches = code[0] == std::byte{0x0f} && code[1] == std::byte{0xb6} && code[2] == std::byte{0x81} &&
                         code[7] == std::byte{0xc0} && code[8] == std::byte{0xe8} && code[9] == std::byte{3} &&
                         code[12] == std::byte{0xc3};
    return matches ? detail::field<std::uint32_t>(code, 3) : 0;
}

[[nodiscard]] panel_layout read_panel_layout(const std::uintptr_t table)
{
    panel_layout result{.width = read_float_getter(table, k_panel_width_getter),
                        .height = read_float_getter(table, k_panel_height_getter),
                        .x = read_float_getter(table, k_panel_x_getter),
                        .y = read_float_getter(table, k_panel_y_getter),
                        .visible = read_visibility_getter(table)};
    const auto consistent = result.height == result.width + 4 && result.y == result.x + 4 && result.visible >= 0x100 &&
                            result.visible <= 0x800;
    return consistent ? result : panel_layout{};
}
} // namespace

void bind(const memory_services& services) noexcept
{
    state().services.store(&services, std::memory_order_release);
}

std::uint32_t shot_punch_offset()
{
    if (!services())
        return 0;
    return state().shot_punch.get(
        []
        {
            const auto code =
                unique("pre-shot punch", "F2 0F 10 4C 24 ? 8B 44 24 ? F2 0F 11 4F 2C 89 47 34 F2 0F 11 8E "
                                         "? ? ? ? 89 86 ? ? ? ?");
            std::array<std::byte, 32> bytes{};
            return code && process().copy(code, bytes.data(), bytes.size()) ? detail::decode_shot_punch(bytes) : 0;
        });
}

std::uintptr_t spread_patterns()
{
    if (!services())
        return 0;
    return state().spread_patterns.get(
        []() -> std::uintptr_t
        {
            auto code = unique("spread map v2", "48 8D 0D ? ? ? ? 45 03 CC E8 ? ? ? ? EB ? 41 0F 28 CA");
            if (!code)
                code = unique("spread map v1",
                              "48 8D 0D ? ? ? ? 48 8D 44 24 30 0F B7 D6 48 89 44 24 20 45 0F AF CD 45 03 CC E8");
            const auto map = code ? process().decode_rip(code) : 0;
            return inside_client(map, 32) ? map : 0;
        });
}

std::optional<radar_layout> radar()
{
    if (!services())
        return std::nullopt;
    return state().radar.get(
        []() -> std::optional<radar_layout>
        {
            const auto code = unique("radar transform",
                                     "84 C0 74 0A F3 0F 10 81 ? ? ? ? EB 10 F3 0F 10 81 ? ? ? ? F3 0F 5E 81 ? ? ? ? "
                                     "F3 44 0F 10 B2 ? ? ? ? 0F 57 FF F3 44 0F 10 B9 ? ? ? ? F3 44 0F 5C B1 ? ? ? ?");
            std::array<std::byte, 60> bytes{};
            if (!code || !process().copy(code, bytes.data(), bytes.size()))
                return std::nullopt;
            return detail::decode_radar(bytes);
        });
}

panel_layout panel(const std::uintptr_t ui)
{
    if (!services() || !is_user_pointer(ui))
        return {};
    const auto table = process().load<std::uintptr_t>(ui);
    auto& module = state();
    {
        std::scoped_lock lock(module.panel_mutex);
        if (const auto it = module.panels.find(table); it != module.panels.end())
            return it->second;
    }
    const auto result = read_panel_layout(table);
    std::scoped_lock lock(module.panel_mutex);
    if (module.panels.size() >= k_max_cached_panel_tables)
        module.panels.clear();
    module.panels.emplace(table, result);
    return result;
}

std::uintptr_t hud_element(const std::string_view wanted)
{
    if (!services())
        return 0;
    const auto directory = state().hud_directory.get(
        []() -> std::uintptr_t
        {
            const auto code =
                unique("HUD directory",
                       "40 53 48 83 EC 20 48 8B 05 ? ? ? ? 48 8B D9 48 85 C0 74 ? 48 89 5C 24 ? 48 8D 88 58 02 00 00");
            return code ? process().decode_rip(code + 6) : 0;
        });
    if (!directory)
        return 0;
    const auto root = process().load<std::uintptr_t>(directory);
    if (!is_user_pointer(root))
        return 0;
    std::array<std::byte, 32> header{};
    if (!process().copy(root + k_hud_directory_vector, header.data(), header.size()))
        return 0;
    const auto capacity = detail::field<std::uint32_t>(header, 12) & 0x7fffffff;
    const auto data = detail::field<std::uintptr_t>(header, 16);
    if (!is_user_pointer(data) || !capacity || capacity > k_max_hud_entries)
        return 0;
    std::vector<std::byte> entries(capacity * k_hud_entry_stride);
    if (!process().copy(data, entries.data(), entries.size()))
        return 0;
    for (std::size_t index = 0; index < capacity; ++index)
    {
        const auto entry = index * k_hud_entry_stride;
        const auto name = detail::field<std::uintptr_t>(entries, entry + k_hud_entry_name);
        const auto object = detail::field<std::uintptr_t>(entries, entry + k_hud_entry_object);
        if (is_user_pointer(name) && is_user_pointer(object) && process().load_text(name, 64) == wanted)
            return object;
    }
    return 0;
}

} // namespace game::compatibility
