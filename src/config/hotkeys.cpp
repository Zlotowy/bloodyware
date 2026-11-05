#include <config/hotkeys.hpp>
#include <diagnostics/log.hpp>
#include <platform/windows/storage.hpp>
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>

namespace config
{

namespace
{

[[nodiscard]] std::string trim(std::string value)
{
    const auto whitespace = [](const unsigned char character) { return std::isspace(character) != 0; };
    while (!value.empty() && whitespace(value.front()))
        value.erase(value.begin());
    while (!value.empty() && whitespace(value.back()))
        value.pop_back();
    return value;
}

[[nodiscard]] std::uint16_t parse_key_name(std::string name)
{
    name = trim(std::move(name));
    if (name.size() >= 2 &&
        ((name.front() == '"' && name.back() == '"') || (name.front() == '\'' && name.back() == '\'')))
    {
        name = name.substr(1, name.size() - 2);
    }
    std::ranges::transform(name, name.begin(),
                           [](const unsigned char value) { return static_cast<char>(std::toupper(value)); });
    name.erase(std::remove_if(name.begin(), name.end(),
                              [](const char value) { return value == ' ' || value == '_' || value == '-'; }),
               name.end());

    if (name.size() == 1)
    {
        const auto value = name.front();
        if ((value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9'))
            return static_cast<std::uint16_t>(value);
    }
    if (name.size() >= 2 && name.front() == 'F')
    {
        int number{};
        for (auto index = std::size_t{1}; index < name.size(); ++index)
        {
            if (name[index] < '0' || name[index] > '9')
            {
                number = 0;
                break;
            }
            number = number * 10 + name[index] - '0';
        }
        if (number >= 1 && number <= 24)
            return static_cast<std::uint16_t>(VK_F1 + number - 1);
    }

    static constexpr std::array names{
        std::pair{std::string_view{"INSERT"}, std::uint16_t{VK_INSERT}},
        std::pair{std::string_view{"INS"}, std::uint16_t{VK_INSERT}},
        std::pair{std::string_view{"DELETE"}, std::uint16_t{VK_DELETE}},
        std::pair{std::string_view{"DEL"}, std::uint16_t{VK_DELETE}},
        std::pair{std::string_view{"HOME"}, std::uint16_t{VK_HOME}},
        std::pair{std::string_view{"END"}, std::uint16_t{VK_END}},
        std::pair{std::string_view{"PAGEUP"}, std::uint16_t{VK_PRIOR}},
        std::pair{std::string_view{"PGUP"}, std::uint16_t{VK_PRIOR}},
        std::pair{std::string_view{"PAGEDOWN"}, std::uint16_t{VK_NEXT}},
        std::pair{std::string_view{"PGDN"}, std::uint16_t{VK_NEXT}},
        std::pair{std::string_view{"PAUSE"}, std::uint16_t{VK_PAUSE}},
        std::pair{std::string_view{"SCROLLLOCK"}, std::uint16_t{VK_SCROLL}},
        std::pair{std::string_view{"NUMLOCK"}, std::uint16_t{VK_NUMLOCK}},
        std::pair{std::string_view{"BACKQUOTE"}, std::uint16_t{VK_OEM_3}},
        std::pair{std::string_view{"TILDE"}, std::uint16_t{VK_OEM_3}},
    };
    for (const auto& [key_name, virtual_key] : names)
    {
        if (name == key_name)
            return virtual_key;
    }
    return 0;
}

// Keys offered by the in-game guide; the file may still contain any name parse_key_name understands.
constexpr std::array<lifecycle_key_choice, 21> choices{{
    {VK_INSERT, "INSERT"},   {VK_DELETE, "DELETE"}, {VK_HOME, "HOME"},   {VK_END, "END"},
    {VK_PRIOR, "PGUP"},      {VK_NEXT, "PGDN"},     {VK_PAUSE, "PAUSE"}, {VK_SCROLL, "SCROLLLOCK"},
    {VK_F1, "F1"},           {VK_F2, "F2"},         {VK_F3, "F3"},       {VK_F4, "F4"},
    {VK_F5, "F5"},           {VK_F6, "F6"},         {VK_F7, "F7"},       {VK_F8, "F8"},
    {VK_F9, "F9"},           {VK_F10, "F10"},       {VK_F11, "F11"},     {VK_F12, "F12"},
    {VK_NUMLOCK, "NUMLOCK"},
}};

[[nodiscard]] std::filesystem::path hotkeys_path()
{
    const auto& configs = platform::windows::runtime_storage::paths().configs;
    return configs.empty() ? std::filesystem::path{} : configs / L"hotkeys.cfg";
}

[[nodiscard]] std::string key_text(const std::uint16_t virtual_key)
{
    for (const auto& choice : choices)
        if (choice.virtual_key == virtual_key)
            return choice.name;
    if (virtual_key >= VK_F1 && virtual_key <= VK_F24)
        return "F" + std::to_string(virtual_key - VK_F1 + 1);
    return {};
}

[[nodiscard]] std::string file_text(const lifecycle_key_bindings& keys)
{
    return std::string{"# Vesta lifecycle hotkeys / Системные клавиши Vesta\n"
                       "# EN: These keys can also be changed in the in-game guide (Misc > Configs). You may edit the "
                       "value after '=' by hand.\n"
                       "# RU: Эти клавиши можно менять и в игровом гайде (Misc > Configs). Либо измените значение "
                       "после '=' вручную.\n"
                       "# Supported examples / Примеры: INSERT, DELETE, HOME, END, PGUP, PGDN, F1...F24.\n\n"} +
           "menu = " + key_text(keys.menu) + "\n" + "exit = " + key_text(keys.exit) + "\n";
}

[[nodiscard]] bool save_keys(const std::filesystem::path& path, const lifecycle_key_bindings& keys)
{
    if (path.empty())
    {
        diagnostics::process_log().warning("[hotkeys] no configuration folder; hotkeys are not saved.");
        return false;
    }
    if (const auto error = platform::windows::write_file_atomically(path, file_text(keys)))
    {
        diagnostics::process_log().warning("[hotkeys] cannot save hotkeys.cfg: {}", error.message());
        return false;
    }
    return true;
}

void create_default_file(const std::filesystem::path& path)
{
    if (path.empty())
        return;
    std::error_code error{};
    if (std::filesystem::exists(path, error))
        return;
    static_cast<void>(save_keys(path, lifecycle_key_bindings{VK_INSERT, VK_END}));
}

[[nodiscard]] lifecycle_key_bindings load_keys()
{
    lifecycle_key_bindings result{VK_INSERT, VK_END};
    const auto path = hotkeys_path();
    create_default_file(path);
    std::ifstream input(path, std::ios::binary);
    std::string line{};
    while (std::getline(input, line))
    {
        if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xef &&
            static_cast<unsigned char>(line[1]) == 0xbb && static_cast<unsigned char>(line[2]) == 0xbf)
        {
            line.erase(0, 3);
        }
        line = trim(std::move(line));
        if (line.empty() || line.front() == '#' || line.front() == ';')
            continue;
        const auto separator = line.find('=');
        if (separator == std::string::npos)
            continue;
        auto name = trim(line.substr(0, separator));
        std::ranges::transform(name, name.begin(),
                               [](const unsigned char value) { return static_cast<char>(std::tolower(value)); });
        auto value = line.substr(separator + 1);
        if (const auto comment = value.find_first_of("#;"); comment != std::string::npos)
            value.resize(comment);
        const auto key = parse_key_name(std::move(value));
        if (!key)
            continue;
        if (name == "menu" || name == "menu_key")
            result.menu = key;
        else if (name == "exit" || name == "exit_key")
            result.exit = key;
    }
    if (result.menu == result.exit)
        result.exit = result.menu == VK_END ? VK_HOME : VK_END;
    return result;
}

struct live_keys
{
    std::atomic<std::uint16_t> menu{}, exit{};
    explicit live_keys(const lifecycle_key_bindings& loaded)
        : menu(loaded.menu)
        , exit(loaded.exit)
    {
    }
};

// Loaded on first use, then updated by set_lifecycle_keys. Reads never take a lock.
[[nodiscard]] live_keys& keys_state()
{
    static live_keys state{load_keys()};
    return state;
}

} // namespace

lifecycle_key_bindings lifecycle_keys()
{
    const auto& state = keys_state();
    return {state.menu.load(std::memory_order_relaxed), state.exit.load(std::memory_order_relaxed)};
}

std::span<const lifecycle_key_choice> lifecycle_key_choices()
{
    return choices;
}

const char* lifecycle_key_name(const std::uint16_t virtual_key)
{
    for (const auto& choice : choices)
        if (choice.virtual_key == virtual_key)
            return choice.name;
    return "?";
}

key_change_result set_lifecycle_keys(const std::uint16_t menu, const std::uint16_t exit)
{
    const auto allowed = [](const std::uint16_t key)
    { return std::ranges::any_of(choices, [key](const auto& choice) { return choice.virtual_key == key; }); };
    if (!allowed(menu) || !allowed(exit))
        return key_change_result::rejected_invalid;
    if (menu == exit)
        return key_change_result::rejected_duplicate;
    auto& state = keys_state();
    state.menu.store(menu, std::memory_order_relaxed);
    state.exit.store(exit, std::memory_order_relaxed);
    // The new keys already work; a failed write only means they are not remembered next time.
    return save_keys(hotkeys_path(), {menu, exit}) ? key_change_result::saved : key_change_result::write_failed;
}

bool is_lifecycle_key(const std::uint16_t virtual_key)
{
    const auto keys = lifecycle_keys();
    return virtual_key == VK_ESCAPE || virtual_key == keys.menu || virtual_key == keys.exit;
}

} // namespace config
