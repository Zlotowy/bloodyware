#include <stdafx.hpp>
#include <foundation/math/vector.hpp>

namespace foundation
{

namespace
{
constexpr float epsilon{1.0e-8f};

[[nodiscard]] float safe_root(float squared) noexcept
{
    return squared > epsilon ? std::sqrt(squared) : 0.0f;
}
} // namespace

float vec2::length() const noexcept
{
    return safe_root(length_sqr());
}

vec2& vec2::normalize() noexcept
{
    if (const auto magnitude = length(); magnitude > epsilon)
        *this /= magnitude;
    return *this;
}

vec2 vec2::normalized() const noexcept
{
    auto copy = *this;
    return copy.normalize();
}

float vec3::length() const noexcept
{
    return safe_root(length_sqr());
}

float vec3::length_2d() const noexcept
{
    return safe_root(x * x + y * y);
}

vec3& vec3::normalize() noexcept
{
    if (const auto magnitude = length(); magnitude > epsilon)
        *this /= magnitude;
    return *this;
}

vec3 vec3::normalized() const noexcept
{
    auto copy = *this;
    return copy.normalize();
}

float vec3::distance(const vec3& value) const noexcept
{
    return (*this - value).length();
}

void vec3::to_directions(vec3* forward, vec3* right, vec3* up) const noexcept
{
    const auto pitch_sine = std::sin(to_radians(x));
    const auto pitch_cosine = std::cos(to_radians(x));
    const auto yaw_sine = std::sin(to_radians(y));
    const auto yaw_cosine = std::cos(to_radians(y));
    const auto roll_sine = std::sin(to_radians(z));
    const auto roll_cosine = std::cos(to_radians(z));

    if (forward)
        *forward = {pitch_cosine * yaw_cosine, pitch_cosine * yaw_sine, -pitch_sine};
    if (right)
        *right = {-roll_sine * pitch_sine * yaw_cosine + roll_cosine * yaw_sine,
                  -roll_sine * pitch_sine * yaw_sine - roll_cosine * yaw_cosine, -roll_sine * pitch_cosine};
    if (up)
        *up = {roll_cosine * pitch_sine * yaw_cosine + roll_sine * yaw_sine,
               roll_cosine * pitch_sine * yaw_sine - roll_sine * yaw_cosine, roll_cosine * pitch_cosine};
}

vec3 rotation::apply(const vec3& value) const noexcept
{
    return foundation::rotate(*this, value);
}

void basis_from_angles(const vec3& angles, vec3& forward, vec3& right, vec3& up) noexcept
{
    angles.to_directions(&forward, &right, &up);
}

vec3 direction_to_angles(const vec3& direction) noexcept
{
    return {to_degrees(std::atan2(-direction.z, direction.length_2d())),
            to_degrees(std::atan2(direction.y, direction.x)), 0.0f};
}

vec3 look_at_angles(const vec3& origin, const vec3& target) noexcept
{
    return direction_to_angles(target - origin);
}

float angular_distance(const vec3& view_angles, const vec3& origin, const vec3& target) noexcept
{
    const auto target_angles = look_at_angles(origin, target);
    return vec2{view_angles.x - target_angles.x, wrap_yaw(view_angles.y - target_angles.y)}.length();
}

float wrap_yaw(float yaw) noexcept
{
    return std::remainder(yaw, 360.0f);
}

vec3 rotate(const rotation& orientation, const vec3& value) noexcept
{
    const vec3 axis{orientation.x, orientation.y, orientation.z};
    const auto twice_cross = axis.cross(value) * 2.0f;
    return value + twice_cross * orientation.w + axis.cross(twice_cross);
}

} // namespace foundation
