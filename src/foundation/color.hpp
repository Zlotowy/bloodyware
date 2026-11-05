#pragma once

#include <cstdint>

namespace foundation
{
// An 8-bit-per-channel colour. Packs as 0xAABBGGRR, the layout of ImGui's ImU32.
struct rgba
{
    std::uint8_t r{};
    std::uint8_t g{};
    std::uint8_t b{};
    std::uint8_t a{};

    constexpr rgba() noexcept = default;
    constexpr rgba(const std::uint8_t red, const std::uint8_t green, const std::uint8_t blue,
                   const std::uint8_t alpha) noexcept
        : r(red)
        , g(green)
        , b(blue)
        , a(alpha)
    {
    }
    constexpr rgba(const std::uint32_t packed) noexcept
        : r(static_cast<std::uint8_t>(packed))
        , g(static_cast<std::uint8_t>(packed >> 8))
        , b(static_cast<std::uint8_t>(packed >> 16))
        , a(static_cast<std::uint8_t>(packed >> 24))
    {
    }

    [[nodiscard]] constexpr std::uint32_t packed() const noexcept
    {
        return static_cast<std::uint32_t>(r) | static_cast<std::uint32_t>(g) << 8 |
               static_cast<std::uint32_t>(b) << 16 | static_cast<std::uint32_t>(a) << 24;
    }
    constexpr operator std::uint32_t() const noexcept { return packed(); }

    friend constexpr bool operator==(const rgba&, const rgba&) noexcept = default;
};
} // namespace foundation
