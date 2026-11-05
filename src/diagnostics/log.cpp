#include <stdafx.hpp>
#include <foundation/identity.hpp>
#include <foundation/color.hpp>
#include <diagnostics/log.hpp>
#include <diagnostics/journal.hpp>

#include <cstdio>

namespace diagnostics
{

bool logger::open([[maybe_unused]] std::string_view title)
{
#if defined(VESTA_ENABLE_CONSOLE) && VESTA_ENABLE_CONSOLE
    m_output = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (!m_output || m_output == INVALID_HANDLE_VALUE)
        return false;

    DWORD mode{};
    if (::GetConsoleMode(m_output, &mode))
        ::SetConsoleMode(m_output, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    const std::string owned_title{title};
    ::SetConsoleTitleA(owned_title.c_str());
    info("diagnostics ready");
#endif
    return true;
}

std::uint32_t logger::message_code(const std::string_view pattern) noexcept
{
    return foundation::identity::of(pattern);
}

void logger::record(const message_level level, const std::string_view pattern) noexcept
{
    if (level == message_level::warning)
        emit(event::diagnostic_warning, severity::warning, code_domain::message, message_code(pattern));
}

void logger::write([[maybe_unused]] message_level level, [[maybe_unused]] std::uint32_t code,
                   [[maybe_unused]] std::string_view message) const
{
#if defined(VESTA_ENABLE_CONSOLE) && VESTA_ENABLE_CONSOLE
    const auto marker = [level]() -> std::string_view
    {
        switch (level)
        {
            case message_level::warning:
                return "\x1b[38;2;238;185;90mwarn";
            case message_level::success:
                return "\x1b[38;2;92;205;145mok";
            case message_level::information:
                break;
        }
        return "\x1b[38;2;135;150;235minfo";
    }();
    std::printf("  [%.*s\x1b[0m] %.*s  #%08x\n", static_cast<int>(marker.size()), marker.data(),
                static_cast<int>(message.size()), message.data(), static_cast<unsigned>(code));
    std::fflush(stdout);
#endif
}

const logger& process_log() noexcept
{
    static const logger sink{};
    return sink;
}

std::string current_exception_message() noexcept
{
    try
    {
        throw;
    }
    catch (const std::exception& exception)
    {
        try
        {
            return exception.what();
        }
        catch (...)
        {
            return {};
        }
    }
    catch (...)
    {
        return "non-standard exception";
    }
}

} // namespace diagnostics
