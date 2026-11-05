#include <diagnostics/log.hpp>
#include <features/chams/mesh_cache.hpp>
#include <features/chams/mesh_extract.hpp>
#include <features/chams/mesh_file.hpp>
#include <platform/windows/storage.hpp>
#include <windows.h>
#include <chrono>
#include <utility>

#include <filesystem>

namespace features::chams
{

namespace
{

[[nodiscard]] std::string sanitize_filename(const std::string& model_path)
{
    std::string out{};
    out.reserve(model_path.size());
    for (const char c : model_path)
    {
        out.push_back((c == '/' || c == '\\' || c == ':') ? '_' : c);
    }
    return out;
}

} // namespace

std::filesystem::path mesh_cache::cache_file_path(const std::string& model_path)
{
    // Rebuildable versioned meshes live separately from durable user settings.
    auto dir = platform::windows::runtime_storage::paths().mesh_cache;
    if (dir.empty())
    {
        return {};
    }

    std::error_code ec{};
    std::filesystem::create_directories(dir, ec);

    if (ec)
        return {};
    const auto filename = sanitize_filename(model_path) + ".chmesh";
    return platform::windows::data_storage::extended_path(
        dir / std::filesystem::path(std::u8string(filename.begin(), filename.end())));
}

bool mesh_cache::load_from_disk(const std::filesystem::path& path, skinned_mesh& out)
{
    return mesh_file::load(path, out) == mesh_file::result::success;
}

void mesh_cache::save_to_disk(const std::filesystem::path& path, const skinned_mesh& mesh)
{
    // The cache is rebuildable and the extracted mesh stays usable, so a failed save only costs a re-extraction.
    if (const auto result = mesh_file::save(path, mesh); result != mesh_file::result::success)
        diagnostics::process_log().warning("[chams] mesh cache not saved (result {})", static_cast<int>(result));
}

const skinned_mesh& mesh_cache::get_or_build(assets::vpk_archive& vpk, const std::string& model_path)
{
    static const skinned_mesh pending_mesh{};

    if (m_memory.empty())
    {
        m_memory.reserve(32);
    }

    for (auto pending = m_pending.begin(); pending != m_pending.end();)
    {
        if (pending->second.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
        {
            auto path = pending->first;
            auto mesh = pending->second.get();
            pending = m_pending.erase(pending);
            m_memory.emplace(std::move(path), std::move(mesh));
        }
        else
        {
            ++pending;
        }
    }

    if (const auto it = m_memory.find(model_path); it != m_memory.end())
    {
        return it->second;
    }

    if (const auto pending = m_pending.find(model_path); pending != m_pending.end())
    {
        return pending_mesh;
    }

    if (m_pending.empty())
    {
        m_pending.reserve(8);
    }

    constexpr std::size_t k_max_extract_workers{2};
    if (m_pending.size() >= k_max_extract_workers)
    {
        return pending_mesh;
    }

    m_pending.emplace(
        model_path, std::async(std::launch::async,
                               [&vpk, model_path]
                               {
                                   const auto background_mode =
                                       ::SetThreadPriority(::GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN) != FALSE;
                                   if (!background_mode)
                                       ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

                                   const auto disk_path = cache_file_path(model_path);
                                   skinned_mesh mesh{};
                                   if (!disk_path.empty() && load_from_disk(disk_path, mesh))
                                   {
                                       if (background_mode)
                                           ::SetThreadPriority(::GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
                                       return mesh;
                                   }

                                   mesh = extract_mesh(vpk, model_path);
                                   if (mesh.valid)
                                   {
                                       save_to_disk(disk_path, mesh);
                                   }
                                   if (background_mode)
                                       ::SetThreadPriority(::GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
                                   return mesh;
                               }));
    return pending_mesh;
}

void mesh_cache::drain_background() noexcept
{
    // std::async futures join before releasing their captured VPK reference.
    m_pending.clear();
}

} // namespace features::chams
