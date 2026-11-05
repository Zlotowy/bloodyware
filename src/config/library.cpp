#include <stdafx.hpp>
#include <config/library.hpp>
#include <config/settings.hpp>
#include <diagnostics/log.hpp>
#include <platform/windows/storage.hpp>
#include <platform/windows/user_settings.hpp>

#include <algorithm>

namespace config
{
namespace
{
using diagnostics::process_log;

constexpr std::size_t k_max_files = 500;

[[nodiscard]] std::string utf8(const std::filesystem::path& path)
{
    const auto encoded = path.u8string();
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

[[nodiscard]] std::int64_t unix_now()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

[[nodiscard]] bool same_name(const std::string_view left, const std::string_view right)
{
    return left.size() == right.size() &&
           std::ranges::equal(
               left, right, [](const char a, const char b)
               { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
}

[[nodiscard]] bool is_config_file(const std::filesystem::directory_entry& item)
{
    std::error_code error{};
    return item.is_regular_file(error) && ::_wcsicmp(item.path().extension().c_str(), L".cfg") == 0;
}

} // namespace

const char* config_library::describe(const outcome value) noexcept
{
    switch (value)
    {
        case outcome::done:
            return "";
        case outcome::no_folder:
            return "Choose a folder first";
        case outcome::folder_unusable:
            return "Folder unavailable";
        case outcome::invalid_name:
            return "Enter a name";
        case outcome::name_taken:
            return "Name already used";
        case outcome::rejected:
            return "Not a valid config";
        case outcome::missing:
            return "File not found";
        case outcome::write_failed:
            return "Save failed";
        case outcome::delete_failed:
            return "Delete failed";
    }
    return "";
}

const std::optional<std::filesystem::path>& config_library::folder()
{
    if (!m_folder_read)
    {
        m_folder_read = true;
        m_folder = platform::windows::read_config_folder();
    }
    return m_folder;
}

config_library::outcome config_library::set_folder(const std::filesystem::path& folder)
{
    std::error_code error{};
    if (folder.empty() || !folder.is_absolute() || !std::filesystem::is_directory(folder, error))
        return outcome::folder_unusable;
    if (!platform::windows::write_config_folder(folder))
    {
        process_log().warning("[config] the config folder could not be remembered");
        return outcome::folder_unusable;
    }
    m_folder_read = true;
    m_folder = folder;
    m_cache.clear();
    m_entries.clear();
    m_active.clear();
    refresh(true);
    return outcome::done;
}

void config_library::refresh(const bool force)
{
    const auto now = std::chrono::steady_clock::now();
    if (!force && now < m_next_scan)
        return;
    m_next_scan = now + std::chrono::milliseconds(500);

    const auto& root = folder();
    m_entries.clear();
    if (!root)
        return;
    std::error_code error{};
    std::filesystem::directory_iterator iterator{*root, std::filesystem::directory_options::skip_permission_denied,
                                                 error};
    if (error)
        return;

    std::unordered_map<std::wstring, cached_file> next{};
    for (const auto& item : iterator)
    {
        if (next.size() >= k_max_files)
            break;
        if (!is_config_file(item))
            continue;
        const auto written = item.last_write_time(error);
        const auto size = error ? 0 : item.file_size(error);
        if (error)
            continue;
        const auto& path = item.path();
        if (const auto known = m_cache.find(path.native());
            known != m_cache.end() && known->second.written == written && known->second.size == size)
        {
            next.emplace(path.native(), known->second);
            continue;
        }
        cached_file cached{written, size, {path}};
        if (const auto document = file::read(path, true))
        {
            cached.entry.name = document->legacy ? file::clean_name(utf8(path.stem())) : document->name;
            cached.entry.saved_at = document->saved_at;
            cached.entry.legacy = document->legacy;
        }
        else
        {
            cached.entry.name = file::clean_name(utf8(path.stem()));
            cached.entry.problem = document.error();
        }
        if (cached.entry.name.empty())
            cached.entry.name = "config";
        next.emplace(path.native(), std::move(cached));
    }
    m_cache = std::move(next);
    for (const auto& [path, cached] : m_cache)
        m_entries.push_back(cached.entry);
    std::ranges::sort(
        m_entries,
        [](const library_entry& left, const library_entry& right)
        {
            // Usable configs first, refused files at the end.
            const bool left_broken = left.problem != file::problem::none;
            const bool right_broken = right.problem != file::problem::none;
            if (left_broken != right_broken)
                return right_broken;
            return std::ranges::lexicographical_compare(
                left.name, right.name, [](const char a, const char b)
                { return std::tolower(static_cast<unsigned char>(a)) < std::tolower(static_cast<unsigned char>(b)); });
        });
}

bool config_library::name_taken(const std::string_view name, const std::filesystem::path& except) const
{
    return std::ranges::any_of(m_entries, [&](const library_entry& entry)
                               { return entry.path != except && same_name(entry.name, name); });
}

std::filesystem::path config_library::free_path_for(const std::string_view name) const
{
    const auto stem = file::file_stem_for(name);
    std::error_code error{};
    auto candidate = *m_folder / (stem + L".cfg");
    for (int suffix = 2; std::filesystem::exists(candidate, error) && suffix < 1000; ++suffix)
        candidate = *m_folder / (stem + L" (" + std::to_wstring(suffix) + L").cfg");
    return candidate;
}

std::string config_library::unique_name(const std::string_view base) const
{
    auto name = file::clean_name(base);
    if (name.empty())
        name = "config";
    if (!name_taken(name))
        return name;
    for (int suffix = 2; suffix < 1000; ++suffix)
    {
        const auto tail = " (" + std::to_string(suffix) + ")";
        auto candidate = file::clean_name(name.substr(0, file::k_max_name_bytes - tail.size()) + tail);
        if (!name_taken(candidate))
            return candidate;
    }
    return name;
}

config_library::outcome config_library::write(const std::filesystem::path& path, const std::string_view name,
                                              const nlohmann::json& settings)
{
    if (const auto error = platform::windows::write_file_atomically(path, file::serialize(name, settings, unix_now())))
    {
        process_log().warning("[config] cannot save {}: {}", utf8(path), error.message());
        return outcome::write_failed;
    }
    return outcome::done;
}

config_library::outcome config_library::load(const library_entry& entry)
{
    refresh(true);
    const auto document = file::read(entry.path, true);
    if (!document)
    {
        m_last_problem = document.error();
        std::error_code error{};
        return std::filesystem::exists(entry.path, error) ? outcome::rejected : outcome::missing;
    }
    try
    {
        apply_config_json(document->settings);
    }
    catch (...)
    {
        process_log().warning("[config] applying {} failed: {}", utf8(entry.path.filename()),
                              diagnostics::current_exception_message());
        m_last_problem = file::problem::bad_settings;
        return outcome::rejected;
    }
    m_last_problem = file::problem::none;
    m_active = entry.path;
    static_cast<void>(storage.write_cache());
    return outcome::done;
}

config_library::outcome config_library::save_current_as(const std::string_view name)
{
    if (!folder())
        return outcome::no_folder;
    refresh(true);
    const auto cleaned = file::clean_name(name);
    if (cleaned.empty())
        return outcome::invalid_name;
    if (name_taken(cleaned))
        return outcome::name_taken;
    const auto path = free_path_for(cleaned);
    const auto result = write(path, cleaned, build_config_json());
    if (result == outcome::done)
        m_active = path;
    refresh(true);
    return result;
}

config_library::outcome config_library::overwrite(const library_entry& entry)
{
    if (entry.problem != file::problem::none)
        return outcome::rejected;
    const auto result = write(entry.path, entry.name, build_config_json());
    if (result == outcome::done)
        m_active = entry.path;
    refresh(true);
    return result;
}

config_library::outcome config_library::rename(const library_entry& entry, const std::string_view name)
{
    if (!folder())
        return outcome::no_folder;
    const auto cleaned = file::clean_name(name);
    if (cleaned.empty())
        return outcome::invalid_name;
    refresh(true);
    if (name_taken(cleaned, entry.path))
        return outcome::name_taken;
    const auto document = file::read(entry.path, true);
    if (!document)
    {
        m_last_problem = document.error();
        return outcome::rejected;
    }

    // The new content goes to the new file first; the old one is removed only after that succeeded.
    const bool same_stem = ::_wcsicmp(file::file_stem_for(cleaned).c_str(), entry.path.stem().c_str()) == 0;
    const auto target = same_stem ? entry.path : free_path_for(cleaned);
    if (const auto result = write(target, cleaned, document->settings); result != outcome::done)
        return result;
    if (!same_stem)
    {
        std::error_code error{};
        std::filesystem::remove(entry.path, error);
        if (error)
            process_log().warning("[config] the old file {} stayed after a rename: {}", utf8(entry.path.filename()),
                                  error.message());
    }
    if (m_active == entry.path)
        m_active = target;
    refresh(true);
    return outcome::done;
}

config_library::outcome config_library::duplicate(const library_entry& entry)
{
    if (!folder())
        return outcome::no_folder;
    refresh(true);
    const auto document = file::read(entry.path, true);
    if (!document)
    {
        m_last_problem = document.error();
        return outcome::rejected;
    }
    const auto name = unique_name(entry.name);
    const auto result = write(free_path_for(name), name, document->settings);
    refresh(true);
    return result;
}

config_library::outcome config_library::remove(const library_entry& entry)
{
    if (const auto error = platform::windows::move_to_recycle_bin(entry.path))
    {
        process_log().warning("[config] {} was not moved to the Recycle Bin: {}", utf8(entry.path.filename()),
                              error.message());
        return outcome::delete_failed;
    }
    if (m_active == entry.path)
        m_active.clear();
    refresh(true);
    return outcome::done;
}

config_library::outcome config_library::import_file(const std::filesystem::path& source)
{
    if (!folder())
        return outcome::no_folder;
    const auto document = file::read(source, true);
    if (!document)
    {
        m_last_problem = document.error();
        process_log().warning("[config] import refused: {}", file::describe(document.error()));
        return outcome::rejected;
    }
    m_last_problem = file::problem::none;
    refresh(true);
    const auto name = unique_name(document->legacy ? utf8(source.stem()) : document->name);
    const auto path = free_path_for(name);
    const auto result = write(path, name, document->settings);
    refresh(true);
    return result;
}

bool config_library::is_active(const library_entry& entry) const
{
    return !m_active.empty() && entry.path == m_active;
}

config_library& library()
{
    static config_library value{};
    return value;
}
} // namespace config
