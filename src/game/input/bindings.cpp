#include <stdafx.hpp>
#include <game/memory/bound_services.hpp>
#include <game/memory/modules.hpp>
#include <game/memory/remote_pointer.hpp>
#include <game/input/bindings.hpp>
#include <game/input/manual_bindings.hpp>
#include <config/hotkeys.hpp>
#include <config/settings.hpp>

namespace game
{

namespace
{
[[nodiscard]] std::optional<std::ptrdiff_t> byte_getter_offset(const platform::windows::process_session& process,
                                                               const std::uintptr_t function)
{
    std::array<std::uint8_t, 32> code{};
    if (!function || !process.copy(function, code.data(), code.size()))
        return std::nullopt;

    for (auto index = std::size_t{}; index + 4 <= code.size(); ++index)
    {
        if (code[index] == 0x0f && code[index + 1] == 0xb6 && code[index + 2] == 0x41)
        {
            return static_cast<std::ptrdiff_t>(code[index + 3]);
        }
        if (code[index] == 0x8a && code[index + 1] == 0x41)
            return static_cast<std::ptrdiff_t>(code[index + 2]);
        if (index + 7 <= code.size() && ((code[index] == 0x0f && code[index + 1] == 0xb6 && code[index + 2] == 0x81) ||
                                         (code[index] == 0x8a && code[index + 1] == 0x81)))
        {
            const auto displacement_index = code[index] == 0x0f ? index + 3 : index + 2;
            std::int32_t displacement{};
            std::memcpy(&displacement, code.data() + displacement_index, sizeof(displacement));
            if (displacement >= 0 && displacement <= 0x400)
                return static_cast<std::ptrdiff_t>(displacement);
        }
    }
    return std::nullopt;
}

} // namespace

input_binding configured_input_bindings::resolve(const input_action action)
{
    const auto snapshot = config::get_runtime_snapshot();
    if (!snapshot)
        return {};
    auto binding = configured_binding(action, snapshot->general.m_game_controls);
    if (binding.device == input_device::keyboard && config::is_lifecycle_key(binding.virtual_key))
        return {};
    return binding;
}

std::vector<input_binding> configured_input_bindings::candidates(const input_action action)
{
    auto binding = resolve(action);
    return binding ? std::vector<input_binding>{std::move(binding)} : std::vector<input_binding>{};
}

bool configured_input_bindings::text_entry_active()
{
    std::scoped_lock lock(m_mutex);
    const auto now = std::chrono::steady_clock::now();
    if (now < m_next_chat_sample)
        return m_chat_active;
    m_next_chat_sample = now + std::chrono::milliseconds(4);

    auto& process = game::bound_memory().process;
    const auto process_id = process.process_id();
    if (process_id != m_chat_process_id)
    {
        m_hud_global = 0;
        m_hud_chat = 0;
        m_hud_chat_vtable = 0;
        m_chat_active_offset = -1;
        m_chat_process_id = process_id;
        m_next_chat_lookup = {};
    }

    const auto valid_chat = [this, &process]
    {
        return game::is_user_pointer(m_hud_chat) && m_hud_chat_vtable &&
               process.load<std::uintptr_t>(m_hud_chat) == m_hud_chat_vtable && m_chat_active_offset >= 0 &&
               m_chat_active_offset <= 0x400;
    };

    auto chat_valid = valid_chat();
    if (!chat_valid && now >= m_next_chat_lookup)
    {
        m_next_chat_lookup = now + std::chrono::milliseconds(250);
        m_hud_chat = 0;
        m_hud_chat_vtable = 0;
        m_chat_active_offset = -1;

        if (!m_hud_global)
        {
            const auto find_hud = process.scan_signature(
                game::bound_memory().modules.client,
                "40 53 48 83 EC 20 48 8B 05 ? ? ? ? 48 8B D9 48 85 C0 74 ? 48 89 5C 24 ? 48 8D 88 58 02 00 00");
            m_hud_global = find_hud ? process.decode_rip(find_hud + 6) : 0;
        }

        const auto root = m_hud_global ? process.load<std::uintptr_t>(m_hud_global) : 0;
        if (!game::is_user_pointer(root))
        {
            m_hud_global = 0;
        }
        else
        {
            const auto table = root + 0x258;
            const auto capacity = process.load<std::uint32_t>(table + 0x0c) & 0x7fffffffu;
            const auto data = capacity ? process.load<std::uintptr_t>(table + 0x10) : 0;
            if (game::is_user_pointer(data) && capacity <= 1024)
            {
                for (std::uint32_t index = 0; index < capacity; ++index)
                {
                    const auto entry = data + static_cast<std::uintptr_t>(index) * 0x20;
                    const auto name = process.load<std::uintptr_t>(entry + 0x10);
                    const auto value = process.load<std::uintptr_t>(entry + 0x18);
                    if (!game::is_user_pointer(name) || !game::is_user_pointer(value) ||
                        process.load_text(name, 32) != "CCSGO_HudChat")
                        continue;

                    const auto vtable = process.load<std::uintptr_t>(value);
                    const auto getter = game::is_user_pointer(vtable)
                                            ? process.load<std::uintptr_t>(vtable + 8 * sizeof(std::uintptr_t))
                                            : 0;
                    const auto offset = byte_getter_offset(process, getter);
                    if (offset)
                    {
                        m_hud_chat = value;
                        m_hud_chat_vtable = vtable;
                        m_chat_active_offset = *offset;
                    }
                    break;
                }
            }
        }
        chat_valid = valid_chat();
    }

    m_chat_active =
        chat_valid && process.load<std::uint8_t>(m_hud_chat + static_cast<std::uintptr_t>(m_chat_active_offset)) != 0;
    return m_chat_active;
}

} // namespace game
