#pragma once

#include <features/chams/mesh.hpp>
#include <assets/vpk.hpp>

#include <string>
#include <filesystem>
#include <future>
#include <unordered_map>

namespace features::chams
{

class mesh_cache
{
public:
    [[nodiscard]] const skinned_mesh& get_or_build(assets::vpk_archive& vpk, const std::string& model_path);

    // No producers may call get_or_build while shutdown drains these tasks.
    void drain_background() noexcept;

private:
    [[nodiscard]] static std::filesystem::path cache_file_path(const std::string& model_path);
    [[nodiscard]] static bool load_from_disk(const std::filesystem::path& path, skinned_mesh& out);
    static void save_to_disk(const std::filesystem::path& path, const skinned_mesh& mesh);

    std::unordered_map<std::string, skinned_mesh> m_memory{};
    std::unordered_map<std::string, std::future<skinned_mesh>> m_pending{};
};

inline mesh_cache g_mesh_cache{};

} // namespace features::chams
