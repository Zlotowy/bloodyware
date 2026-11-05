#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace app
{
// Fatal startup failures that end the process before any window exists. Each one
// tells the user what happened and what to do next; details go to the journal.
enum class startup_failure : std::uint8_t
{
    game_connection,
    input_unavailable,
    modules_not_found,
    addresses_failed,
    schema_failed,
    count
};

struct failure_text
{
    std::wstring_view what, action;
};

// Shown in a message box before any window exists, so it stays in English: short and explicit.
[[nodiscard]] constexpr failure_text describe(startup_failure failure) noexcept
{
    switch (failure)
    {
        case startup_failure::game_connection:
            return {L"CS2 is not running.", L"Start CS2 first, then start Vesta."};
        case startup_failure::input_unavailable:
            return {L"Input setup failed.",
                    L"Close other tools that capture the keyboard or mouse, then start Vesta again."};
        case startup_failure::modules_not_found:
            return {L"CS2 is still loading.", L"Wait for the CS2 main menu, then start Vesta again."};
        case startup_failure::addresses_failed:
            return {L"This Vesta version does not support the current CS2 update.",
                    L"Download the latest Vesta release."};
        case startup_failure::schema_failed:
            return {L"This Vesta version cannot read the game data of the current CS2 update.",
                    L"Download the latest Vesta release."};
        case startup_failure::count:
            break;
    }
    return {};
}
} // namespace app
