#include <assets/workshop_maps.hpp>
#include <assets/vpk.hpp>
#include <algorithm>
#include <cctype>
#include <system_error>

namespace assets
{
namespace
{
std::string lower(std::string_view text)
{
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

bool is_part_name(const std::string& name)
{
    // <id>_000.vpk, <id>_001.vpk ...: data parts of a split archive, never opened on their own.
    if (name.size() < 8 || name.compare(name.size() - 4, 4, ".vpk") != 0)
        return false;
    const auto digits = name.substr(name.size() - 8, 3);
    return name[name.size() - 9] == '_' &&
           std::all_of(digits.begin(), digits.end(), [](const unsigned char c) { return std::isdigit(c) != 0; });
}

// The archive that holds an item's directory: <id>_dir.vpk, else a single .vpk that is not a numbered part.
std::filesystem::path item_archive(const std::filesystem::path& item)
{
    std::error_code error;
    std::filesystem::path single;
    for (std::filesystem::directory_iterator it(item, error), end; !error && it != end; it.increment(error))
    {
        if (!it->is_regular_file(error))
            continue;
        const auto name = lower(it->path().filename().string());
        if (name.size() <= 4 || name.compare(name.size() - 4, 4, ".vpk") != 0)
            continue;
        if (name.size() >= 8 && name.compare(name.size() - 8, 8, "_dir.vpk") == 0)
            return it->path();
        if (single.empty() && !is_part_name(name))
            single = it->path();
    }
    return single;
}
} // namespace

void workshop_map_index::scan(const std::vector<std::filesystem::path>& content_roots)
{
    std::error_code error;
    for (const auto& root : content_roots)
    {
        for (std::filesystem::directory_iterator item(root, error), end; !error && item != end; item.increment(error))
        {
            std::error_code item_error;
            if (!item->is_directory(item_error))
                continue;
            const auto archive = item_archive(item->path());
            if (archive.empty())
                continue;
            const auto size = std::filesystem::file_size(archive, item_error);
            const auto written = std::filesystem::last_write_time(archive, item_error);
            if (item_error)
                continue;
            const auto key = archive.string() + "|" + std::to_string(size) + "|" +
                             std::to_string(written.time_since_epoch().count());
            if (!m_seen.insert(key).second)
                continue;

            vpk_archive opened;
            if (!opened.open(archive.string()))
                continue; // not a readable archive: skip it, keep going
            // Two shapes exist: the map's files directly in the archive (maps/<name>/world_physics.vmdl_c), and the
            // usual workshop shape, where the map is a whole archive stored as the entry maps/<name>.vpk.
            constexpr std::string_view physics = "/world_physics.vmdl_c";
            constexpr std::string_view nested = ".vpk";
            for (const auto& path : opened.list_prefix("maps/"))
            {
                std::string name, location;
                if (path.size() > 5 + physics.size() &&
                    path.compare(path.size() - physics.size(), physics.size(), physics) == 0)
                {
                    name = path.substr(5, path.size() - 5 - physics.size());
                    location = archive.string();
                }
                else if (path.size() > 5 + nested.size() &&
                         path.compare(path.size() - nested.size(), nested.size(), nested) == 0)
                {
                    name = path.substr(5, path.size() - 5 - nested.size());
                    location = vpk_archive::embedded_path(archive.string(), path);
                }
                if (name.empty() || name.find('/') != std::string::npos)
                    continue;
                auto& slot = m_maps[lower(name)];
                if (slot.archive.empty() || written >= slot.written)
                    slot = {std::move(location), written};
            }
        }
        error.clear();
    }
}

std::string workshop_map_index::find(const std::vector<std::filesystem::path>& content_roots, std::string_view map_name)
{
    if (map_name.empty())
        return {};
    std::scoped_lock lock(m_mutex);
    const auto key = lower(map_name);
    // An archive that disappeared (item unsubscribed) must not be returned; forget it and look again.
    if (const auto known = m_maps.find(key); known != m_maps.end())
    {
        std::error_code error;
        const auto bar = known->second.archive.rfind('|');
        const auto file = bar == std::string::npos ? known->second.archive : known->second.archive.substr(0, bar);
        if (std::filesystem::exists(file, error) && !error)
            return known->second.archive;
        m_maps.erase(known);
    }
    scan(content_roots);
    const auto found = m_maps.find(key);
    return found == m_maps.end() ? std::string{} : found->second.archive;
}
} // namespace assets
