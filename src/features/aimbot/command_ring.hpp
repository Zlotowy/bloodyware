#pragma once

#include <foundation/math/vector.hpp>

#include <array>
#include <chrono>
#include <cstdint>

namespace features::aimbot
{
// Reads the client's user-command ring: the live sequence number and the view angles of the newest command.
//
// The client keeps several rings; the live one is found by watching which counter advances (a live ring ticks 64
// times a second) and is then followed until it goes quiet. The search state belongs to one caller: each thread that
// reads commands owns its own reader.
class command_ring_reader
{
public:
    struct view
    {
        std::uintptr_t base{};
        int sequence{};
    };

    [[nodiscard]] bool read(view& out);

    // Angles of the newest command, from one resolve of the ring so the slot and the object cannot disagree.
    [[nodiscard]] bool read_angles(foundation::vec3& out, int* out_sequence = nullptr);

private:
    static constexpr int k_candidates = 64;

    [[nodiscard]] bool probe(std::uintptr_t array, int index, std::chrono::steady_clock::time_point now, view& out);
    [[nodiscard]] bool try_index(std::uintptr_t array, int index, std::chrono::steady_clock::time_point now, view& out);
    bool fail(std::chrono::steady_clock::time_point now);

    int m_consecutive_failures{};
    std::chrono::steady_clock::time_point m_next_attempt{};
    int m_cached_index{};
    std::array<int, k_candidates> m_seen_counter{};
    std::array<int, k_candidates> m_observed_changes{};
    std::array<std::chrono::steady_clock::time_point, k_candidates> m_changed_at{};
};
} // namespace features::aimbot
