#pragma once

#include <game/memory/memory_services.hpp>
#include <cstdint>

namespace game
{

class address_catalog
{
public:
    bool initialize(const memory_services& services);

    std::uintptr_t csgo_input{};
    std::uintptr_t entity_list{};
    std::uintptr_t local_player_controller{};
    std::uintptr_t global_vars{};
    std::uintptr_t view_matrix{};
    std::uintptr_t auto_accept{};
};

} // namespace game
