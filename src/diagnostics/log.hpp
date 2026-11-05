#pragma once

#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace diagnostics
{

// Developer-facing diagnostics.
//
// Console builds (VESTA_ENABLE_CONSOLE) print every message. In every build, warnings are also recorded in the process
// journal as diagnostic.warning with code_domain::message: the code is the FNV-1a identity of the format pattern, so
// the journal names the call site without carrying any message text. Console lines end with the same code.
class logger
{
public:
    [[nodiscard]] bool open(std::string_view title);

    template<typename... args_t>
    void info(std::format_string<args_t...> pattern, args_t&&... args) const
    {
        print(message_level::information, pattern, std::forward<args_t>(args)...);
    }

    template<typename... args_t>
    void success(std::format_string<args_t...> pattern, args_t&&... args) const
    {
        print(message_level::success, pattern, std::forward<args_t>(args)...);
    }

    template<typename... args_t>
    void warning(std::format_string<args_t...> pattern, args_t&&... args) const
    {
        record(message_level::warning, pattern.get());
        print(message_level::warning, pattern, std::forward<args_t>(args)...);
    }

    // The journal code a message with this format pattern is recorded under.
    [[nodiscard]] static std::uint32_t message_code(std::string_view pattern) noexcept;

private:
    enum class message_level : std::uint8_t
    {
        information,
        warning,
        success,
    };

    template<typename... args_t>
    void print([[maybe_unused]] message_level level, [[maybe_unused]] std::format_string<args_t...> pattern,
               [[maybe_unused]] args_t&&... args) const
    {
#if defined(VESTA_ENABLE_CONSOLE) && VESTA_ENABLE_CONSOLE
        write(level, message_code(pattern.get()), std::format(pattern, std::forward<args_t>(args)...));
#endif
    }

    static void record(message_level level, std::string_view pattern) noexcept;
    void write(message_level level, std::uint32_t code, std::string_view message) const;

    void* m_output{};
};

// The process-wide sink for code that has no injected one (configuration, assets, scripting).
[[nodiscard]] const logger& process_log() noexcept;

// What the exception currently being handled says about itself; call only inside a catch block.
[[nodiscard]] std::string current_exception_message() noexcept;

} // namespace diagnostics
