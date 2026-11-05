#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace assets
{

class vpk_archive
{
public:
    struct entry
    {
        std::uint32_t crc{};
        std::uint16_t archive_index{};
        std::uint32_t offset{};
        std::uint32_t length{};
        // Leading bytes stored inside the directory file itself. They prefix
        // whatever is read out of the numbered archive.
        std::vector<std::uint8_t> preload{};
    };

    // A plain path opens a directory (.../x_dir.vpk) or single-file archive. "outer.vpk|maps/name.vpk" opens the
    // archive stored inside `outer` as that entry (workshop maps are shipped that way).
    bool open(const std::string& dir_vpk_path);

    // Opens the archive that is stored as the file `inner_path` inside the archive `outer_dir_vpk_path`.
    bool open_embedded(const std::string& outer_dir_vpk_path, const std::string& inner_path);

    [[nodiscard]] static std::string embedded_path(const std::string& outer, const std::string& inner)
    {
        return outer + '|' + inner;
    }

    bool open_selected(const std::string& dir_vpk_path, const std::vector<std::string>& archive_paths);

    [[nodiscard]] bool is_open() const { return m_open; }
    [[nodiscard]] std::size_t count() const { return m_entries.size(); }
    [[nodiscard]] const std::string& path() const { return m_dir_path; }

    // Paths are archive-relative and lower case, e.g.
    // "agents/models/ctm_sas/ctm_sas.vmdl_c".
    [[nodiscard]] const entry* find(const std::string& archive_path) const;
    [[nodiscard]] std::vector<std::uint8_t> read(const entry& e) const;
    [[nodiscard]] std::vector<std::string> list_prefix(const std::string& prefix) const;

    [[nodiscard]] static std::string locate_cs2_pak();

    // Stock maps live in game/csgo/maps/<name>.vpk; workshop maps are found by content in the Steam workshop
    // folders (see workshop_maps.hpp), because their archives are named after the workshop id.
    [[nodiscard]] static std::string locate_map_vpk(const std::string& map_name);

    // .../steamapps/workshop/content/730 of the running game's library and of every Steam library.
    [[nodiscard]] static std::vector<std::filesystem::path> workshop_content_roots();

private:
    std::unordered_map<std::string, entry> m_entries{};
    bool open_at(const std::string& path, std::uint64_t base_offset, const std::vector<std::string>& archive_paths);

    std::string m_dir_path{};
    // File holding the in-archive payload (entries with index 0x7FFF) and where this archive begins in it.
    std::string m_data_source{};
    std::uint64_t m_base_offset{};
    std::uint32_t m_data_start{};
    bool m_open{false};
};

} // namespace assets
