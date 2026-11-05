#include <diagnostics/report.hpp>
#include <diagnostics/build_identity.hpp>
#include <fstream>
#include <iterator>
#include <string>
#include <shlobj.h>
#include <windows.h>

namespace diagnostics
{
namespace
{
constexpr std::size_t per_file_limit = 128 * 1024;
constexpr std::size_t total_limit = 1024 * 1024;

std::filesystem::path known_folder(const KNOWNFOLDERID& id)
{
    PWSTR folder{};
    const auto result = ::SHGetKnownFolderPath(id, 0, nullptr, &folder);
    std::filesystem::path path;
    if (SUCCEEDED(result))
        path = folder;
    ::CoTaskMemFree(folder);
    return path;
}

// A backslash or a drive-letter prefix means a file path may have leaked into a line.
bool looks_like_path(std::string_view line) noexcept
{
    if (line.find('\\') != std::string_view::npos)
        return true;
    for (std::size_t i = 1; i + 1 < line.size(); ++i)
        if (line[i] == ':' && line[i + 1] == '/' && ((line[i - 1] | 0x20) >= 'a' && (line[i - 1] | 0x20) <= 'z'))
            return true;
    return false;
}

void append_section(std::string& out, const std::filesystem::path& file, std::string_view title)
{
    std::ifstream stream(file, std::ios::binary);
    if (!stream)
        return;
    std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    if (text.size() > per_file_limit)
        text.erase(0, text.size() - per_file_limit); // keep the newest part
    out += "--- ";
    out += title;
    out += '\n';
    std::size_t start = 0;
    while (start < text.size())
    {
        auto end = text.find('\n', start);
        if (end == std::string::npos)
            end = text.size();
        const std::string_view line(text.data() + start, end - start);
        if (looks_like_path(line))
            out += "[line withheld: possible file path]\n";
        else
        {
            out.append(line);
            out += '\n';
        }
        start = end + 1;
    }
}
} // namespace

std::filesystem::path desktop_directory()
{
    return known_folder(FOLDERID_Desktop);
}

report_result export_report(const std::filesystem::path& logs_directory,
                            const std::filesystem::path& destination_directory, std::string_view summary) noexcept
{
    report_result result;
    try
    {
        if (destination_directory.empty())
        {
            result.error = ERROR_PATH_NOT_FOUND;
            return result;
        }
        std::string out;
        out += "vesta-diagnostic-report schema=1 version=" VESTA_JOURNAL_VERSION " build=" VESTA_JOURNAL_REVISION "\n";
        if (!summary.empty())
        {
            out += "summary ";
            out.append(summary);
            out += '\n';
        }
        if (!logs_directory.empty())
        {
            append_section(out, logs_directory / L"crash.previous.log", "crash.previous.log");
            append_section(out, logs_directory / L"crash.log", "crash.log");
            // Oldest rotation first so the report reads chronologically.
            for (int i = 3; i >= 1; --i)
                append_section(out, logs_directory / (L"journal." + std::to_wstring(i) + L".log"),
                               "journal." + std::to_string(i) + ".log");
            append_section(out, logs_directory / L"journal.log", "journal.log");
        }
        if (out.size() > total_limit)
            out.erase(0, out.size() - total_limit);
        std::error_code failure;
        std::filesystem::create_directories(destination_directory, failure);
        if (failure)
        {
            result.error = static_cast<std::uint32_t>(failure.value());
            return result;
        }
        result.path = destination_directory / std::filesystem::path(report_file_name);
        std::ofstream file(result.path, std::ios::binary | std::ios::trunc);
        if (!file)
        {
            result.error = ::GetLastError() ? ::GetLastError() : ERROR_WRITE_FAULT;
            return result;
        }
        file.write(out.data(), static_cast<std::streamsize>(out.size()));
        file.close();
        if (!file)
        {
            result.error = ERROR_WRITE_FAULT;
            return result;
        }
        result.ok = true;
    }
    catch (...)
    {
        result.error = ERROR_OUTOFMEMORY;
        result.ok = false;
    }
    return result;
}
} // namespace diagnostics
