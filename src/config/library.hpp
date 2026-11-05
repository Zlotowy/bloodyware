#pragma once

#include <config/config_file.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace config
{
// One file in the config folder. A file that fails validation is still listed, with its problem, so it can be
// deleted; it can never be loaded.
struct library_entry
{
    std::filesystem::path path{};
    std::string name{};
    std::int64_t saved_at{};
    file::problem problem{};
    bool legacy{};
};

// The named configs: every *.cfg in the folder the user chose. The folder path is the only thing stored (see
// platform/windows/user_settings.hpp); names and times are read from the files. Render thread only.
class config_library
{
public:
    enum class outcome : std::uint8_t
    {
        done,
        no_folder,
        folder_unusable,
        invalid_name,
        name_taken,
        rejected,
        missing,
        write_failed,
        delete_failed,
    };

    [[nodiscard]] static const char* describe(outcome value) noexcept;

    [[nodiscard]] const std::optional<std::filesystem::path>& folder();
    outcome set_folder(const std::filesystem::path& folder);

    // Rescans the folder at most twice a second unless forced; unchanged files are not read again.
    void refresh(bool force = false);
    [[nodiscard]] const std::vector<library_entry>& entries() const noexcept { return m_entries; }

    outcome load(const library_entry& entry);
    outcome save_current_as(std::string_view name);
    outcome overwrite(const library_entry& entry);
    outcome rename(const library_entry& entry, std::string_view name);
    outcome duplicate(const library_entry& entry);
    outcome remove(const library_entry& entry);
    outcome import_file(const std::filesystem::path& source);

    // The config last loaded or saved in this session, if it is still in the folder.
    [[nodiscard]] bool is_active(const library_entry& entry) const;
    // Why the last load or import was refused.
    [[nodiscard]] file::problem last_problem() const noexcept { return m_last_problem; }

    // The result the menu shows for a few seconds after an operation, including the ones the overlay carries out
    // after a file dialog.
    struct notice
    {
        outcome result{outcome::done};
        file::problem problem{};
        const char* success{""}; // English key shown when the operation succeeded
        std::chrono::steady_clock::time_point at{};
    };
    void note(const outcome result, const char* success = "")
    {
        m_notice = {result, m_last_problem, success, std::chrono::steady_clock::now()};
    }
    [[nodiscard]] const notice& last_notice() const noexcept { return m_notice; }

private:
    struct cached_file
    {
        std::filesystem::file_time_type written{};
        std::uintmax_t size{};
        library_entry entry{};
    };

    [[nodiscard]] bool name_taken(std::string_view name, const std::filesystem::path& except = {}) const;
    [[nodiscard]] std::filesystem::path free_path_for(std::string_view name) const;
    [[nodiscard]] std::string unique_name(std::string_view base) const;
    outcome write(const std::filesystem::path& path, std::string_view name, const nlohmann::json& settings);

    bool m_folder_read{};
    std::optional<std::filesystem::path> m_folder{};
    std::vector<library_entry> m_entries{};
    std::unordered_map<std::wstring, cached_file> m_cache{};
    std::chrono::steady_clock::time_point m_next_scan{};
    std::filesystem::path m_active{};
    file::problem m_last_problem{};
    notice m_notice{};
};

[[nodiscard]] config_library& library();
} // namespace config
