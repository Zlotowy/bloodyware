#pragma once
#include <game/memory/remote_pointer.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace game
{
// Layout of CGameEntitySystem's identity list: an array of chunk pointers, each chunk holding 512 identity slots.
namespace entity_list
{
inline constexpr std::uintptr_t chunk_table_offset = 0x10;
inline constexpr std::size_t slots_per_chunk = 512;
inline constexpr std::size_t slot_stride = 0x70;
inline constexpr std::size_t chunk_bytes = slots_per_chunk * slot_stride;
// Offset of the handle (index | serial) a slot currently holds.
inline constexpr std::size_t slot_handle_offset = 0x10;
inline constexpr std::uint32_t index_mask = 0x7fff;
inline constexpr std::uint32_t slot_mask = 0x1ff;
inline constexpr std::uint32_t chunk_shift = 9;
inline constexpr std::uint32_t invalid_handle = 0xffffffffu;
inline constexpr std::uint32_t empty_handle = 0xfffffffeu;
// Player controllers occupy indices 1..64 of the first chunk.
inline constexpr std::size_t controller_slots = 65;
} // namespace entity_list

template<class Reader>
[[nodiscard]] bool read_player_controller_slots(std::uintptr_t list,
                                                std::array<std::uintptr_t, entity_list::controller_slots>& out,
                                                Reader&& read)
{
    out.fill(0);
    if (!list)
        return false;
    std::uintptr_t chunk{};
    if (!read(list + entity_list::chunk_table_offset, &chunk, sizeof(chunk)) || !chunk)
        return false;
    std::array<std::uint8_t, entity_list::controller_slots * entity_list::slot_stride> bytes{};
    if (!read(chunk, bytes.data(), bytes.size()))
        return false;
    for (std::size_t index = 1; index < out.size(); ++index)
    {
        std::uintptr_t entity{};
        std::memcpy(&entity, bytes.data() + index * entity_list::slot_stride, sizeof(entity));
        if (entity >= k_lowest_user_address)
            out[index] = entity;
    }
    return true;
}

template<class Reader>
[[nodiscard]] std::uintptr_t read_entity_slot(std::uintptr_t list, std::uint32_t handle, bool validate_serial,
                                              Reader&& read)
{
    if (!list || handle == entity_list::invalid_handle || handle == entity_list::empty_handle)
        return 0;
    const auto chunk_index = (handle & entity_list::index_mask) >> entity_list::chunk_shift;
    std::uintptr_t chunk{};
    if (!read(list + entity_list::chunk_table_offset + chunk_index * sizeof(std::uintptr_t), &chunk, sizeof(chunk)) ||
        !chunk)
        return 0;
    const auto address = chunk + (handle & entity_list::slot_mask) * entity_list::slot_stride;
    std::array<std::uint8_t, entity_list::slot_handle_offset + sizeof(std::uint32_t)> slot{};
    if (!read(address, slot.data(), validate_serial ? slot.size() : sizeof(std::uintptr_t)))
        return 0;
    std::uintptr_t entity{};
    std::uint32_t resident{};
    std::memcpy(&entity, slot.data(), sizeof(entity));
    std::memcpy(&resident, slot.data() + entity_list::slot_handle_offset, sizeof(resident));
    return entity >= k_lowest_user_address && (!validate_serial || resident == handle) ? entity : 0;
}
} // namespace game
