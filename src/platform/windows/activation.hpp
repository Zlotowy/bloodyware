#pragma once

#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>

namespace platform::windows
{
// Activation of configured key bindings from the physical key state: always on, while held, or toggled by each
// press. Toggle state is kept per virtual key, so bindings that share a key toggle together.
class key_activation
{
public:
    [[nodiscard]] bool active(const int mode, const int key, const int always_mode, const int hold_mode,
                              const int toggle_mode) noexcept
    {
        if (mode == always_mode)
            return true;
        if (key <= 0 || key >= static_cast<int>(k_keys))
            return false;
        const auto down = (::GetAsyncKeyState(key) & 0x8000) != 0;
        if (mode == hold_mode)
            return down;
        if (mode != toggle_mode)
            return false;

        const auto index = static_cast<std::size_t>(key);
        const auto was_down = m_down[index].exchange(down, std::memory_order_acq_rel);
        if (down && !was_down)
        {
            auto current = m_toggled[index].load(std::memory_order_acquire);
            while (!m_toggled[index].compare_exchange_weak(current, !current, std::memory_order_acq_rel,
                                                           std::memory_order_acquire))
            {
            }
        }
        return m_toggled[index].load(std::memory_order_acquire);
    }

private:
    static constexpr std::size_t k_keys = 256;
    std::array<std::atomic<bool>, k_keys> m_down{};
    std::array<std::atomic<bool>, k_keys> m_toggled{};
};

[[nodiscard]] inline key_activation& key_activations() noexcept
{
    static key_activation value{};
    return value;
}

[[nodiscard]] inline bool binding_active(const int mode, const int key, const int always_mode, const int hold_mode,
                                         const int toggle_mode) noexcept
{
    return key_activations().active(mode, key, always_mode, hold_mode, toggle_mode);
}
} // namespace platform::windows
