#include <game/memory/bound_services.hpp>
#include <atomic>

namespace game
{
namespace
{
// Stand-ins used until the application binds the real ones.
struct unbound
{
    platform::windows::process_session process;
    game::module_catalog modules;
    diagnostics::logger log;
    memory_services services{process, modules, log};
    address_catalog addresses;
};

unbound& fallback() noexcept
{
    static unbound value;
    return value;
}

std::atomic<const memory_services*> g_services{};
std::atomic<const address_catalog*> g_addresses{};
} // namespace

void bind_services(const memory_services& services, const address_catalog& addresses) noexcept
{
    g_services.store(&services, std::memory_order_release);
    g_addresses.store(&addresses, std::memory_order_release);
}

const memory_services& bound_memory() noexcept
{
    const auto* bound = g_services.load(std::memory_order_acquire);
    return bound ? *bound : fallback().services;
}

const address_catalog& bound_addresses() noexcept
{
    const auto* bound = g_addresses.load(std::memory_order_acquire);
    return bound ? *bound : fallback().addresses;
}
} // namespace game
