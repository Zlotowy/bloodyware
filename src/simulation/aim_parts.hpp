#pragma once

#include <config/combat.hpp>
#include <game/state/constants.hpp>
#include <game/body/body.hpp>

#include <string_view>

namespace simulation
{
// Which configurable body part (config::combat_profile::aim_part) a hitbox belongs to.
[[nodiscard]] inline int aim_part_of(const game::hitbox_catalog::entry& hitbox, const int hitgroup) noexcept
{
    using part = config::combat_profile::aim_part;
    namespace group = game::rules::hitgroup;

    if (hitbox.index == 0)
        return part::head;
    if (hitbox.index == 1)
        return part::body;
    const std::string_view name{hitbox.name.data()};
    if (name == "head")
        return part::head;
    if (name == "neck_0" || name == "neck")
        return part::body;
    switch (hitgroup)
    {
        case group::left_arm:
        case group::right_arm:
            return part::arms;
        case group::left_leg:
        case group::right_leg:
            return part::legs;
        default:
            return part::body;
    }
}

[[nodiscard]] inline int aim_part_of(const int hitbox_index, const int hitgroup) noexcept
{
    game::hitbox_catalog::entry hitbox{};
    hitbox.index = hitbox_index;
    return aim_part_of(hitbox, hitgroup);
}
} // namespace simulation
