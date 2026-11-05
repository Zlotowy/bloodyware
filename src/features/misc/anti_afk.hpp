#pragma once

#include <chrono>
#include <cstdint>

namespace features::misc
{

// Keeps the server from seeing an idle player: every 15-30 s a short random W/A/S/D tap plus a tiny mouse wiggle that
// is undone on release. Skipped while the player is dead, the menu is open, CS2 is not focused, or any movement key is
// already held.
class anti_afk_t
{
public:
    void tick();

private:
    void release();

    bool m_pressed{};
    std::uint16_t m_key{};
    int m_wiggle_x{};
    std::chrono::steady_clock::time_point m_next{};
    std::chrono::steady_clock::time_point m_release_at{};
};

inline anti_afk_t& anti_afk()
{
    static anti_afk_t value{};
    return value;
}

} // namespace features::misc
