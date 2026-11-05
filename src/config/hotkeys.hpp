#pragma once

#include <cstdint>
#include <span>

namespace config
{

struct lifecycle_key_bindings
{
    std::uint16_t menu{};
    std::uint16_t exit{};
};

// A key the user may pick for the menu / exit actions in the guide.
struct lifecycle_key_choice
{
    std::uint16_t virtual_key{};
    const char* name{};
};

enum class key_change_result : std::uint8_t
{
    saved,
    rejected_invalid,
    rejected_duplicate,
    write_failed
};

// Thread-safe snapshot of the current keys (read from hotkeys.cfg on first use).
[[nodiscard]] lifecycle_key_bindings lifecycle_keys();
[[nodiscard]] bool is_lifecycle_key(std::uint16_t virtual_key);

[[nodiscard]] std::span<const lifecycle_key_choice> lifecycle_key_choices();
[[nodiscard]] const char* lifecycle_key_name(std::uint16_t virtual_key);

// Applies the keys immediately and rewrites hotkeys.cfg atomically. Both keys must be choices and differ.
// write_failed: the new keys work for this session but could not be remembered.
[[nodiscard]] key_change_result set_lifecycle_keys(std::uint16_t menu, std::uint16_t exit);

} // namespace config
