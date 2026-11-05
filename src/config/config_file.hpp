#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace config::file
{
// A saved configuration is one JSON document:
//
//   { "vesta": "config", "format": 2, "name": "Legit", "saved_at": 1760400000,
//     "settings": { "combat_global": {...}, "combat_overrides": [...], "esp": {...}, "misc": {...} } }
//
// The name and the time live in the file itself, so a folder of these files is the whole list. Older files are the
// bare settings object; they are accepted only where `accept_legacy` says so (import and the live configuration).

inline constexpr int k_format = 2;
inline constexpr std::size_t k_max_bytes = 1u << 20;
inline constexpr std::size_t k_max_name_bytes = 48;

// Why a file was refused. The UI shows `describe()`, an English key that the menu translates.
enum class problem : std::uint8_t
{
    none,
    unreadable,
    too_large,
    not_json,
    too_complex,
    not_a_config,
    newer_format,
    bad_name,
    bad_settings,
};

[[nodiscard]] const char* describe(problem value) noexcept;

struct document
{
    std::string name{};
    std::int64_t saved_at{};
    nlohmann::json settings = nlohmann::json::object();
    bool legacy{};
};

// Validates untrusted bytes: bounded size and nesting, finite numbers, short strings and arrays, the header, and that
// every known settings section decodes with the right types. Nothing is applied.
[[nodiscard]] std::expected<document, problem> parse(std::string_view bytes, bool accept_legacy);
[[nodiscard]] std::expected<document, problem> read(const std::filesystem::path& path, bool accept_legacy);

[[nodiscard]] std::string serialize(std::string_view name, const nlohmann::json& settings, std::int64_t saved_at);

// A display name: control characters removed, spaces collapsed and trimmed, at most k_max_name_bytes of UTF-8.
[[nodiscard]] std::string clean_name(std::string_view name);
[[nodiscard]] bool valid_name(std::string_view name);

// A file name for a display name: characters Windows forbids are replaced, reserved device names avoided.
[[nodiscard]] std::wstring file_stem_for(std::string_view name);
} // namespace config::file
