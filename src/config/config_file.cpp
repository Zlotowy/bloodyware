#include <stdafx.hpp>
#include <config/config_file.hpp>
#include <config/settings.hpp>

#include <array>
#include <cmath>
#include <fstream>

namespace config::file
{
namespace
{
using json = nlohmann::json;

constexpr int k_max_depth = 16;
constexpr std::size_t k_max_nodes = 100'000;
constexpr std::size_t k_max_string_bytes = 512;
constexpr std::size_t k_max_array_items = 512;
constexpr double k_max_magnitude = 1e12;
constexpr std::array<std::string_view, 4> k_sections{"combat_global", "combat_overrides", "esp", "misc"};

// Nesting depth of the raw text, before anything is built from it: a file of a million brackets must not reach the
// parser or any recursive walk.
[[nodiscard]] bool nesting_within_limit(const std::string_view bytes) noexcept
{
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (const char c : bytes)
    {
        if (in_string)
        {
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                in_string = false;
            continue;
        }
        if (c == '"')
            in_string = true;
        else if (c == '{' || c == '[')
        {
            if (++depth > k_max_depth)
                return false;
        }
        else if (c == '}' || c == ']')
            --depth;
    }
    return true;
}

// Every value of the document stays small and finite. Depth is already bounded, so the recursion is too.
[[nodiscard]] bool values_within_limits(const json& value, std::size_t& nodes)
{
    if (++nodes > k_max_nodes)
        return false;
    switch (value.type())
    {
        case json::value_t::object:
            if (value.size() > k_max_array_items * 2)
                return false;
            for (const auto& [key, item] : value.items())
                if (key.size() > k_max_string_bytes || !values_within_limits(item, nodes))
                    return false;
            return true;
        case json::value_t::array:
            if (value.size() > k_max_array_items)
                return false;
            for (const auto& item : value)
                if (!values_within_limits(item, nodes))
                    return false;
            return true;
        case json::value_t::string:
            return value.get_ref<const std::string&>().size() <= k_max_string_bytes;
        case json::value_t::number_float:
        {
            const auto number = value.get<double>();
            return std::isfinite(number) && std::abs(number) <= k_max_magnitude;
        }
        case json::value_t::number_integer:
            return std::abs(static_cast<double>(value.get<std::int64_t>())) <= k_max_magnitude;
        case json::value_t::number_unsigned:
            return static_cast<double>(value.get<std::uint64_t>()) <= k_max_magnitude;
        case json::value_t::binary:
        case json::value_t::discarded:
            return false;
        default:
            return true;
    }
}

// Only the known sections are kept; each must decode with the right types into a copy of the settings.
[[nodiscard]] std::expected<json, problem> checked_settings(const json& source)
{
    if (!source.is_object())
        return std::unexpected(problem::bad_settings);
    json settings = json::object();
    for (const auto section : k_sections)
        if (const auto it = source.find(section); it != source.end())
            settings[std::string(section)] = *it;
    if (settings.empty())
        return std::unexpected(problem::not_a_config);
    try
    {
        auto trial = *get_runtime_snapshot();
        decode_config_json(settings, trial);
    }
    catch (...)
    {
        return std::unexpected(problem::bad_settings);
    }
    return settings;
}

// Length of the UTF-8 sequence starting at `lead`, or 0 when `lead` cannot start one.
[[nodiscard]] std::size_t sequence_length(const unsigned char lead) noexcept
{
    if (lead < 0x80)
        return 1;
    if ((lead & 0xe0) == 0xc0 && lead >= 0xc2)
        return 2;
    if ((lead & 0xf0) == 0xe0)
        return 3;
    if ((lead & 0xf8) == 0xf0 && lead <= 0xf4)
        return 4;
    return 0;
}
} // namespace

const char* describe(const problem value) noexcept
{
    switch (value)
    {
        case problem::none:
            return "";
        case problem::unreadable:
            return "Unreadable";
        case problem::too_large:
            return "Too large";
        case problem::not_json:
            return "Not a config";
        case problem::too_complex:
            return "Invalid structure";
        case problem::not_a_config:
            return "Not a config";
        case problem::newer_format:
            return "Newer version";
        case problem::bad_name:
            return "Invalid name";
        case problem::bad_settings:
            return "Invalid values";
    }
    return "Not a config";
}

std::expected<document, problem> parse(const std::string_view bytes, const bool accept_legacy)
{
    if (bytes.size() > k_max_bytes)
        return std::unexpected(problem::too_large);
    if (!nesting_within_limit(bytes))
        return std::unexpected(problem::too_complex);
    const auto root = json::parse(bytes.begin(), bytes.end(), nullptr, false);
    if (root.is_discarded())
        return std::unexpected(problem::not_json);
    if (!root.is_object())
        return std::unexpected(problem::not_a_config);
    std::size_t nodes = 0;
    if (!values_within_limits(root, nodes))
        return std::unexpected(problem::too_complex);

    document result{};
    if (const auto marker = root.find("vesta"); marker != root.end())
    {
        if (!marker->is_string() || marker->get_ref<const std::string&>() != "config")
            return std::unexpected(problem::not_a_config);
        const auto format = root.find("format");
        if (format == root.end() || !format->is_number_integer())
            return std::unexpected(problem::not_a_config);
        if (format->get<std::int64_t>() > k_format)
            return std::unexpected(problem::newer_format);
        const auto name = root.find("name");
        if (name == root.end() || !name->is_string() || !valid_name(name->get_ref<const std::string&>()))
            return std::unexpected(problem::bad_name);
        result.name = name->get<std::string>();
        if (const auto saved = root.find("saved_at"); saved != root.end() && saved->is_number_integer())
            result.saved_at = std::max<std::int64_t>(0, saved->get<std::int64_t>());
        const auto settings = root.find("settings");
        if (settings == root.end())
            return std::unexpected(problem::bad_settings);
        auto checked = checked_settings(*settings);
        if (!checked)
            return std::unexpected(checked.error());
        result.settings = std::move(*checked);
        return result;
    }

    if (!accept_legacy)
        return std::unexpected(problem::not_a_config);
    auto checked = checked_settings(root);
    if (!checked)
        return std::unexpected(checked.error());
    result.settings = std::move(*checked);
    result.legacy = true;
    return result;
}

std::expected<document, problem> read(const std::filesystem::path& path, const bool accept_legacy)
{
    std::error_code error{};
    const auto size = std::filesystem::file_size(path, error);
    if (error)
        return std::unexpected(problem::unreadable);
    if (size > k_max_bytes)
        return std::unexpected(problem::too_large);
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return std::unexpected(problem::unreadable);
    std::string bytes(static_cast<std::size_t>(size), '\0');
    if (!input.read(bytes.data(), static_cast<std::streamsize>(bytes.size())))
        return std::unexpected(problem::unreadable);
    return parse(bytes, accept_legacy);
}

std::string serialize(const std::string_view name, const nlohmann::json& settings, const std::int64_t saved_at)
{
    nlohmann::ordered_json document;
    document["vesta"] = "config";
    document["format"] = k_format;
    document["name"] = std::string(name);
    document["saved_at"] = saved_at;
    document["settings"] = settings;
    return document.dump(4);
}

std::string clean_name(const std::string_view name)
{
    std::string result{};
    bool pending_space = false;
    for (std::size_t index = 0; index < name.size();)
    {
        const auto lead = static_cast<unsigned char>(name[index]);
        auto length = sequence_length(lead);
        if (length == 0 || index + length > name.size())
        {
            ++index;
            continue;
        }
        bool well_formed = true;
        for (std::size_t next = 1; next < length; ++next)
            well_formed &= (static_cast<unsigned char>(name[index + next]) & 0xc0) == 0x80;
        if (!well_formed)
        {
            ++index;
            continue;
        }
        const auto piece = name.substr(index, length);
        index += length;
        if (length == 1 && (lead < 0x20 || lead == 0x7f))
            continue;
        if (length == 1 && lead == ' ')
        {
            pending_space = !result.empty();
            continue;
        }
        const auto needed = piece.size() + (pending_space ? 1 : 0);
        if (result.size() + needed > k_max_name_bytes)
            break;
        if (pending_space)
            result += ' ';
        pending_space = false;
        result += piece;
    }
    return result;
}

bool valid_name(const std::string_view name)
{
    return !name.empty() && clean_name(name) == name;
}

std::wstring file_stem_for(const std::string_view name)
{
    auto stem = std::filesystem::u8path(clean_name(name)).wstring();
    for (auto& c : stem)
        if (c < 0x20 || std::wstring_view{L"<>:\"/\\|?*"}.find(c) != std::wstring_view::npos)
            c = L'_';
    while (!stem.empty() && (stem.back() == L'.' || stem.back() == L' '))
        stem.pop_back();
    if (stem.empty())
        stem = L"config";
    static constexpr std::array<std::wstring_view, 22> reserved{
        L"CON",  L"PRN",  L"AUX",  L"NUL",  L"COM1", L"COM2", L"COM3", L"COM4", L"COM5", L"COM6", L"COM7",
        L"COM8", L"COM9", L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9"};
    const auto base = stem.substr(0, stem.find(L'.'));
    for (const auto word : reserved)
        if (base.size() == word.size() && ::_wcsnicmp(base.c_str(), word.data(), word.size()) == 0)
            return L"_" + stem;
    return stem;
}
} // namespace config::file
