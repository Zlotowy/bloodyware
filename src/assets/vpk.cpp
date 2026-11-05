#include <assets/vpk.hpp>
#include <assets/workshop_maps.hpp>

#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_set>

namespace assets
{

namespace
{

constexpr std::uint32_t k_vpk_signature{0x55AA1234};

// Reads a NUL-terminated string out of the directory tree, advancing the
// cursor past the terminator.
[[nodiscard]] bool read_cstring(const std::vector<std::uint8_t>& buf, std::size_t& cursor, std::string& out)
{
    const auto start = cursor;
    while (cursor < buf.size() && buf[cursor] != 0)
    {
        ++cursor;
    }

    if (cursor >= buf.size())
    {
        return false;
    }

    out.assign(reinterpret_cast<const char*>(buf.data() + start), cursor - start);
    ++cursor;
    return true;
}

template<typename T>
[[nodiscard]] bool read_pod(const std::vector<std::uint8_t>& buf, std::size_t& cursor, T& out)
{
    if (cursor + sizeof(T) > buf.size())
    {
        return false;
    }

    std::memcpy(&out, buf.data() + cursor, sizeof(T));
    cursor += sizeof(T);
    return true;
}

[[nodiscard]] std::string steam_path_from_registry()
{
    HKEY key{};
    if (::RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", 0, KEY_READ, &key) != ERROR_SUCCESS)
    {
        return {};
    }

    char buffer[MAX_PATH]{};
    auto size = static_cast<DWORD>(sizeof(buffer));
    DWORD type{};
    const auto status = ::RegQueryValueExA(key, "SteamPath", nullptr, &type, reinterpret_cast<LPBYTE>(buffer), &size);
    ::RegCloseKey(key);

    if (status != ERROR_SUCCESS || type != REG_SZ)
    {
        return {};
    }

    return std::string{buffer};
}

// libraryfolders.vdf is a small keyvalues file; every library shows up as a
// "path" entry. Full VDF parsing is overkill just to collect those.
[[nodiscard]] std::vector<std::string> steam_libraries(const std::string& steam_path)
{
    std::vector<std::string> libraries{};
    if (steam_path.empty())
    {
        return libraries;
    }

    libraries.push_back(steam_path);

    const auto vdf = std::filesystem::path{steam_path} / "steamapps" / "libraryfolders.vdf";
    std::ifstream file{vdf};
    if (!file.is_open())
    {
        return libraries;
    }

    std::string line{};
    while (std::getline(file, line))
    {
        const auto key = line.find("\"path\"");
        if (key == std::string::npos)
        {
            continue;
        }

        const auto open_quote = line.find('"', key + 6);
        if (open_quote == std::string::npos)
        {
            continue;
        }

        const auto close_quote = line.find('"', open_quote + 1);
        if (close_quote == std::string::npos)
        {
            continue;
        }

        auto value = line.substr(open_quote + 1, close_quote - open_quote - 1);

        // VDF escapes backslashes; collapse them back to a real path.
        std::string unescaped{};
        unescaped.reserve(value.size());
        for (std::size_t i = 0; i < value.size(); ++i)
        {
            if (value[i] == '\\' && i + 1 < value.size() && value[i + 1] == '\\')
            {
                ++i;
            }

            unescaped.push_back(value[i]);
        }

        libraries.push_back(unescaped);
    }

    return libraries;
}

[[nodiscard]] std::filesystem::path running_cs2_game_directory()
{
    const auto snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return {};
    PROCESSENTRY32W entry{.dwSize = sizeof(PROCESSENTRY32W)};
    std::filesystem::path result{};
    if (::Process32FirstW(snapshot, &entry))
    {
        do
        {
            if (_wcsicmp(entry.szExeFile, L"cs2.exe") != 0)
                continue;
            const auto process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (!process)
                continue;
            wchar_t image[32768]{};
            DWORD length = static_cast<DWORD>(std::size(image));
            if (::QueryFullProcessImageNameW(process, 0, image, &length))
            {
                auto directory = std::filesystem::path{std::wstring_view{image, length}};
                // .../game/bin/win64/cs2.exe -> .../game
                result = directory.parent_path().parent_path().parent_path();
            }
            ::CloseHandle(process);
            if (!result.empty())
                break;
        } while (::Process32NextW(snapshot, &entry));
    }
    ::CloseHandle(snapshot);
    return result;
}

} // namespace

std::string vpk_archive::locate_cs2_pak()
{
    std::error_code ec{};
    if (const auto game = running_cs2_game_directory(); !game.empty())
    {
        const auto candidate = game / "csgo" / "pak01_dir.vpk";
        if (std::filesystem::exists(candidate, ec) && !ec)
            return candidate.string();
    }

    for (const auto& library : steam_libraries(steam_path_from_registry()))
    {
        const auto candidate = std::filesystem::path{library} / "steamapps" / "common" /
                               "Counter-Strike Global Offensive" / "game" / "csgo" / "pak01_dir.vpk";

        if (std::filesystem::exists(candidate, ec) && !ec)
        {
            return candidate.string();
        }
    }

    return {};
}

std::string vpk_archive::locate_map_vpk(const std::string& map_name)
{
    if (map_name.empty())
    {
        return {};
    }

    std::error_code ec{};
    if (const auto game = running_cs2_game_directory(); !game.empty())
    {
        const auto candidate = game / "csgo" / "maps" / (map_name + ".vpk");
        if (std::filesystem::exists(candidate, ec) && !ec)
            return candidate.string();
    }
    for (const auto& library : steam_libraries(steam_path_from_registry()))
    {
        const auto candidate = std::filesystem::path{library} / "steamapps" / "common" /
                               "Counter-Strike Global Offensive" / "game" / "csgo" / "maps" / (map_name + ".vpk");

        if (std::filesystem::exists(candidate, ec) && !ec)
        {
            return candidate.string();
        }
    }

    // Workshop maps: the archive is named after the workshop id, so look inside the archives instead. The index is
    // built once per process and reused; only the map loader thread asks for maps.
    static workshop_map_index workshop{};
    return workshop.find(workshop_content_roots(), map_name);
}

std::vector<std::filesystem::path> vpk_archive::workshop_content_roots()
{
    std::vector<std::filesystem::path> roots{};
    const auto add = [&roots](const std::filesystem::path& steamapps)
    {
        auto content = steamapps / "workshop" / "content" / "730";
        if (std::find(roots.begin(), roots.end(), content) == roots.end())
            roots.push_back(std::move(content));
    };
    // game -> "Counter-Strike Global Offensive" -> "common" -> "steamapps"
    if (const auto game = running_cs2_game_directory(); !game.empty())
        add(game.parent_path().parent_path().parent_path());
    for (const auto& library : steam_libraries(steam_path_from_registry()))
        add(std::filesystem::path{library} / "steamapps");
    return roots;
}

bool vpk_archive::open(const std::string& dir_vpk_path)
{
    // '|' cannot appear in a Windows file name, so it safely separates the outer archive from the entry.
    if (const auto bar = dir_vpk_path.rfind('|'); bar != std::string::npos)
        return open_embedded(dir_vpk_path.substr(0, bar), dir_vpk_path.substr(bar + 1));
    return open_selected(dir_vpk_path, {});
}

bool vpk_archive::open_embedded(const std::string& outer_dir_vpk_path, const std::string& inner_path)
{
    vpk_archive outer{};
    if (!outer.open_selected(outer_dir_vpk_path, {inner_path}))
        return false;
    const auto* record = outer.find(inner_path);
    // The inner archive must be one contiguous blob; bytes stored in the directory itself would split it.
    if (!record || record->length == 0 || !record->preload.empty())
        return false;

    std::string source{};
    std::uint64_t seek{};
    if (record->archive_index == 0x7FFF)
    {
        source = outer_dir_vpk_path;
        seek = static_cast<std::uint64_t>(outer.m_data_start) + record->offset;
    }
    else
    {
        const auto suffix = outer_dir_vpk_path.rfind("_dir.vpk");
        if (suffix == std::string::npos)
            return false;
        char part[16]{};
        std::snprintf(part, sizeof(part), "_%03u.vpk", static_cast<unsigned>(record->archive_index));
        source = outer_dir_vpk_path.substr(0, suffix) + part;
        seek = record->offset;
    }
    if (!open_at(source, seek, {}))
        return false;
    m_dir_path = embedded_path(outer_dir_vpk_path, inner_path);
    return true;
}

bool vpk_archive::open_selected(const std::string& dir_vpk_path, const std::vector<std::string>& archive_paths)
{
    return open_at(dir_vpk_path, 0, archive_paths);
}

bool vpk_archive::open_at(const std::string& dir_vpk_path, const std::uint64_t base_offset,
                          const std::vector<std::string>& archive_paths)
{
    m_entries.clear();
    m_open = false;

    std::ifstream file{dir_vpk_path, std::ios::binary};
    if (!file.is_open())
    {
        return false;
    }
    file.seekg(static_cast<std::streamoff>(base_offset));

    std::uint32_t signature{}, version{}, tree_size{};
    file.read(reinterpret_cast<char*>(&signature), sizeof(signature));
    file.read(reinterpret_cast<char*>(&version), sizeof(version));
    file.read(reinterpret_cast<char*>(&tree_size), sizeof(tree_size));

    if (!file || signature != k_vpk_signature || tree_size == 0)
    {
        return false;
    }

    // v1 has a 12-byte header, v2 adds four more fields before the tree.
    const std::uint32_t header_size = version == 1 ? 12u : 28u;
    if (version != 1 && version != 2)
    {
        return false;
    }

    file.seekg(static_cast<std::streamoff>(base_offset + header_size));

    std::vector<std::uint8_t> tree(tree_size);
    file.read(reinterpret_cast<char*>(tree.data()), tree_size);
    if (!file)
    {
        return false;
    }

    m_data_start = header_size + tree_size;

    std::unordered_set<std::string_view> selected{};
    selected.reserve(archive_paths.size());
    for (const auto& path : archive_paths)
        selected.emplace(path);

    // Tree layout: extension \0 { directory \0 { filename \0 <entry> } }, each
    // level terminated by an empty string.
    std::size_t cursor{0};
    while (cursor < tree.size())
    {
        std::string extension{};
        if (!read_cstring(tree, cursor, extension) || extension.empty())
        {
            break;
        }

        while (true)
        {
            std::string directory{};
            if (!read_cstring(tree, cursor, directory) || directory.empty())
            {
                break;
            }

            while (true)
            {
                std::string name{};
                if (!read_cstring(tree, cursor, name) || name.empty())
                {
                    break;
                }

                entry e{};
                std::uint16_t preload_size{}, terminator{};

                if (!read_pod(tree, cursor, e.crc) || !read_pod(tree, cursor, preload_size) ||
                    !read_pod(tree, cursor, e.archive_index) || !read_pod(tree, cursor, e.offset) ||
                    !read_pod(tree, cursor, e.length) || !read_pod(tree, cursor, terminator))
                {
                    return false;
                }

                const auto preload_start = cursor;
                if (preload_size > 0)
                {
                    if (cursor + preload_size > tree.size())
                    {
                        return false;
                    }

                    cursor += preload_size;
                }

                // A single space stands in for the archive root.
                auto full = directory == " " ? name + "." + extension : directory + "/" + name + "." + extension;

                if (selected.empty() || selected.contains(full))
                {
                    if (preload_size > 0)
                        e.preload.assign(tree.begin() + preload_start, tree.begin() + preload_start + preload_size);
                    m_entries.emplace(std::move(full), std::move(e));
                }
            }
        }
    }

    m_dir_path = dir_vpk_path;
    m_data_source = dir_vpk_path;
    m_base_offset = base_offset;
    m_open = !m_entries.empty();
    return m_open;
}

const vpk_archive::entry* vpk_archive::find(const std::string& archive_path) const
{
    const auto it = m_entries.find(archive_path);
    return it == m_entries.end() ? nullptr : &it->second;
}

std::vector<std::string> vpk_archive::list_prefix(const std::string& prefix) const
{
    std::vector<std::string> result{};
    for (const auto& [key, value] : m_entries)
    {
        if (key.rfind(prefix, 0) == 0)
        {
            result.push_back(key);
        }
    }

    std::sort(result.begin(), result.end());
    return result;
}

std::vector<std::uint8_t> vpk_archive::read(const entry& e) const
{
    std::vector<std::uint8_t> out{e.preload};

    if (e.length == 0)
    {
        return out;
    }

    // 0x7FFF means the payload lives in the directory file after the tree;
    // anything else indexes a numbered archive part.
    std::string source{};
    std::uint64_t seek{};

    if (e.archive_index == 0x7FFF)
    {
        source = m_data_source;
        seek = m_base_offset + static_cast<std::uint64_t>(m_data_start) + e.offset;
    }
    else
    {
        const auto suffix = m_dir_path.rfind("_dir.vpk");
        if (suffix == std::string::npos)
        {
            return {};
        }

        char part[16]{};
        std::snprintf(part, sizeof(part), "_%03u.vpk", static_cast<unsigned>(e.archive_index));
        source = m_dir_path.substr(0, suffix) + part;
        seek = e.offset;
    }

    std::ifstream file{source, std::ios::binary};
    if (!file.is_open())
    {
        return {};
    }

    file.seekg(static_cast<std::streamoff>(seek));

    const auto base = out.size();
    out.resize(base + e.length);
    file.read(reinterpret_cast<char*>(out.data() + base), e.length);

    if (!file)
    {
        return {};
    }

    return out;
}

} // namespace assets
