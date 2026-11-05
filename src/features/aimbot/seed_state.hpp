#pragma once

#include <cstdint>
#include <foundation/math/vector.hpp>
#include <optional>

namespace features::aimbot
{
[[nodiscard]] float seed_quantize_angle(float angle);
[[nodiscard]] std::optional<foundation::vec3> read_seed_punch(std::uintptr_t pawn);
} // namespace features::aimbot
