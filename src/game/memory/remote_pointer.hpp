#pragma once

#include <cstdint>

namespace game
{
// The range a pointer read out of the game process may fall into: above the reserved low 64 KiB and inside the
// 47-bit user address space. Anything else is a stale, uninitialized or garbage value and must not be followed.
inline constexpr std::uintptr_t k_lowest_user_address = 0x10000;
inline constexpr std::uintptr_t k_highest_user_address = 0x00007fffffffffff;

[[nodiscard]] constexpr bool is_user_pointer(const std::uintptr_t value) noexcept
{
    return value >= k_lowest_user_address && value <= k_highest_user_address;
}
} // namespace game
