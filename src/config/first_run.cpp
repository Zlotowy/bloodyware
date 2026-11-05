#include <config/first_run.hpp>
#include <diagnostics/log.hpp>
#include <windows.h>
#include <atomic>
#include <fstream>

namespace config::first_run
{
namespace
{
std::atomic<bool> g_pending{false};

std::filesystem::path marker(const std::filesystem::path& configs)
{
    return configs / L"guide.done";
}

bool write_marker(const std::filesystem::path& configs)
{
    if (configs.empty())
        return false;
    std::error_code error;
    std::filesystem::create_directories(configs, error);
    if (error)
    {
        diagnostics::process_log().warning("[guide] cannot create the configuration folder: {}", error.message());
        return false;
    }
    std::ofstream out(marker(configs), std::ios::binary | std::ios::trunc);
    out << "vesta-guide=1\n";
    out.flush();
    if (!out)
    {
        // The guide then shows again on the next start; nothing else depends on the marker.
        diagnostics::process_log().warning("[guide] cannot write guide.done");
        return false;
    }
    return true;
}
} // namespace

void evaluate(const bool profile_existed, const std::filesystem::path& configs)
{
    std::error_code error;
    const bool marked = !configs.empty() && std::filesystem::exists(marker(configs), error);
    if (marked)
    {
        g_pending.store(false, std::memory_order_release);
        return;
    }
    if (profile_existed)
    {
        // A returning user updated Vesta: do not greet them as a new one.
        static_cast<void>(write_marker(configs));
        g_pending.store(false, std::memory_order_release);
        return;
    }
    g_pending.store(true, std::memory_order_release);
}

bool pending() noexcept
{
    return g_pending.load(std::memory_order_acquire);
}

bool complete(const std::filesystem::path& configs)
{
    g_pending.store(false, std::memory_order_release);
    return write_marker(configs);
}

void repeat(const std::filesystem::path& configs)
{
    std::error_code error;
    if (!configs.empty() && !std::filesystem::remove(marker(configs), error) && error)
        diagnostics::process_log().warning("[guide] cannot remove guide.done: {}", error.message());
    g_pending.store(true, std::memory_order_release);
}
} // namespace config::first_run
