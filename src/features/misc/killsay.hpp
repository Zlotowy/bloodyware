#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace features::misc
{

// Types a chat line into the console after every kill: open console, say <message>, Enter, close. The kill is detected
// read-only from the local controller's round kill counter, the typing is batched key events with no per-key sleep,
// only the console open/close gets a short delay.
class killsay_t
{
public:
    void tick();

private:
    std::uintptr_t m_services{};
    std::int32_t m_last_kills{};
    std::int32_t m_last_index{-1};
    bool m_initialized{};
    bool m_pending{};
    std::chrono::steady_clock::time_point m_pending_since{};
    std::chrono::steady_clock::time_point m_next_allowed{};
};

inline killsay_t& killsay()
{
    static killsay_t value{};
    return value;
}

} // namespace features::misc
