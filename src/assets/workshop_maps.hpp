#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace assets
{
// Workshop maps are not stored under their map name: Steam keeps each item in <content>/<workshop id>/ as
// <id>_dir.vpk (+ numbered parts) or a single <id>.vpk, while the game reports only the map name
// ("maps/complete_skill_training_labs.vpk"). The map is found by what the archive contains: usually the whole map as
// an entry maps/<name>.vpk (returned as "outer.vpk|maps/<name>.vpk", see vpk_archive::open), or the map files
// themselves (maps/<name>/world_physics.vmdl_c).
//
// find() reads the directory of every workshop archive it has not seen yet (name, size and time identify an archive),
// remembers which map each one holds, and answers from that index afterwards. When several items hold the same map
// the most recently written archive wins. Thread-safe; meant for the map loader thread, never for a per-frame path.
class workshop_map_index
{
public:
    // `content_roots` are .../steamapps/workshop/content/730 folders. Returns a path vpk_archive::open accepts, or empty.
    [[nodiscard]] std::string find(const std::vector<std::filesystem::path>& content_roots, std::string_view map_name);

private:
    struct holder
    {
        std::string archive;
        std::filesystem::file_time_type written{};
    };
    void scan(const std::vector<std::filesystem::path>& content_roots);

    std::mutex m_mutex;
    std::unordered_map<std::string, holder> m_maps; // lower-case map name -> archive
    std::unordered_set<std::string> m_seen;         // archives already read (path + size + time)
};
} // namespace assets
