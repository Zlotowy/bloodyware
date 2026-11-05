#include <platform/windows/data_paths.hpp>
#include <platform/windows/worker_control.hpp>
#include <foundation/scope_exit.hpp>
#include <array>
#include <algorithm>
#include <string_view>

namespace platform::windows::data_storage
{
namespace
{
constexpr std::string_view marker_text = "vesta-data-migration=1\n";
// Lua projects and caches are shallow; the bound keeps recursion and stack use
// independent of hostile or corrupted trees. Deeper trees fail before publish.
constexpr unsigned maximum_tree_depth = 32;
class migration final
{
    enum class marker_state
    {
        missing,
        complete,
        failed
    };

public:
    migration(const locations& target, const std::filesystem::path* source, std::stop_token stop)
        : m_target(target)
        , m_explicit_source(source)
        , m_stop(stop)
    {
    }
    migration_result run()
    {
        if (!validate_destination())
            return m_result;
        // The committed marker is the only authority for an already migrated
        // destination. No legacy metadata/resolver/lock is needed on this path.
        const auto marker = inspect_marker();
        if (marker != marker_state::missing)
        {
            if (marker == marker_state::complete)
                (void)ensure_areas();
            return m_result;
        }
        if (!resolve_source() || !validate_source() || !ensure_areas())
            return m_result;
        const auto lock_path = m_target.root / L".migration-v1.lock";
        foundation::unique_handle gate(::CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                                     OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                                                     nullptr));
        if (!valid_handle(gate.get()))
        {
            fail(migration_stage::lock, ::GetLastError());
            return m_result;
        }
        if (!regular(gate.get(), migration_stage::lock))
            return m_result;
        // Another preparer may have published the marker since the first read.
        if (inspect_marker() != marker_state::missing)
            return m_result;
        const auto marker_path = m_target.root / L".migration-v1.complete";
        // One reserved same-volume staging file, protected by the exclusive gate.
        // A previous interrupted staging file is validated before reuse.
        m_pending = m_target.root / L".migration-v1.pending";
        if (!copy_file(m_source / L"hotkeys.cfg", m_target.configs / L"hotkeys.cfg"))
            return m_result;
        if (!tree(m_source / L"config_cache", m_target.configs, false, 0) ||
            !tree(m_source / L"chams_cache", m_target.mesh_cache, false, 0) ||
            !tree(m_source / L"lua", m_target.lua, true, 0))
            return m_result;
        if (cancelled())
            return m_result;
        if (!publish_marker(marker_path))
            return m_result;
        return m_result;
    }

private:
    static bool valid_handle(HANDLE handle) noexcept { return handle && handle != INVALID_HANDLE_VALUE; }
    static bool missing() noexcept
    {
        const auto error = ::GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }
    void fail(migration_stage stage, DWORD error) noexcept
    {
        m_result.stage = stage;
        m_result.error = error ? error : ERROR_GEN_FAILURE;
    }
    bool cancelled() noexcept
    {
        if (!m_stop.stop_requested())
            return false;
        fail(migration_stage::cancelled, ERROR_CANCELLED);
        return true;
    }
    bool regular(HANDLE handle, migration_stage stage) noexcept
    {
        BY_HANDLE_FILE_INFORMATION info{};
        if (!::GetFileInformationByHandle(handle, &info))
        {
            fail(stage, ::GetLastError());
            return false;
        }
        if (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
        {
            fail(stage, ERROR_INVALID_DATA);
            return false;
        }
        return true;
    }
    bool no_reparse(const std::filesystem::path& path)
    {
        auto prefix = path.root_path();
        for (const auto& component : path.relative_path())
        {
            prefix /= component;
            const auto attributes = ::GetFileAttributesW(prefix.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES)
            {
                if (missing())
                    continue;
                fail(migration_stage::validate_paths, ::GetLastError());
                return false;
            }
            if (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
            {
                fail(migration_stage::validate_paths, ERROR_REPARSE_TAG_INVALID);
                return false;
            }
        }
        return true;
    }
    static bool inside(const std::filesystem::path& child, const std::filesystem::path& parent)
    {
        auto component = child.begin();
        for (const auto& expected : parent)
        {
            if (component == child.end())
                return false;
            const auto& actual = component->native();
            const auto& prefix = expected.native();
            if (::CompareStringOrdinal(actual.c_str(), static_cast<int>(actual.size()), prefix.c_str(),
                                       static_cast<int>(prefix.size()), TRUE) != CSTR_EQUAL)
                return false;
            ++component;
        }
        return true;
    }
    bool validate_destination()
    {
        if (cancelled())
            return false;
        if (m_target.root.empty() || !m_target.root.is_absolute())
        {
            fail(migration_stage::validate_paths, ERROR_INVALID_PARAMETER);
            return false;
        }
        const auto expected = locations::under(m_target.root);
        if (m_target.configs != expected.configs || m_target.cache != expected.cache ||
            m_target.logs != expected.logs || m_target.lua != expected.lua ||
            m_target.mesh_cache != expected.mesh_cache || m_target.research_logs != expected.research_logs)
        {
            fail(migration_stage::validate_paths, ERROR_INVALID_PARAMETER);
            return false;
        }
        return no_reparse(m_target.root);
    }
    // Complete, absent and invalid/unreadable markers are distinct states.
    marker_state inspect_marker()
    {
        const auto marker = m_target.root / L".migration-v1.complete";
        foundation::unique_handle file(::CreateFileW(marker.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                                     OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!valid_handle(file.get()))
        {
            if (missing())
                return marker_state::missing;
            fail(migration_stage::marker, ::GetLastError());
            return marker_state::failed;
        }
        if (!regular(file.get(), migration_stage::marker))
            return marker_state::failed;
        std::array<char, 64> bytes{};
        DWORD read{};
        if (!::ReadFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr))
        {
            fail(migration_stage::marker, ::GetLastError());
            return marker_state::failed;
        }
        if (std::string_view(bytes.data(), read) != marker_text)
        {
            fail(migration_stage::marker, ERROR_INVALID_DATA);
            return marker_state::failed;
        }
        m_result.already_complete = true;
        return marker_state::complete;
    }
    bool ensure_areas()
    {
        for (const auto& folder : {m_target.root, m_target.configs, m_target.cache, m_target.logs, m_target.lua,
                                   m_target.mesh_cache, m_target.research_logs})
            if (!directory(folder))
                return false;
        return true;
    }
    bool resolve_source()
    {
        if (cancelled())
            return false;
        if (m_explicit_source)
        {
            m_source = extended_path(*m_explicit_source);
            return true;
        }
        std::error_code error;
        const auto temporary = std::filesystem::temp_directory_path(error);
        if (error)
        {
            fail(migration_stage::validate_paths, static_cast<DWORD>(error.value()));
            return false;
        }
        const auto parent = std::filesystem::weakly_canonical(temporary, error);
        if (error)
        {
            fail(migration_stage::validate_paths, static_cast<DWORD>(error.value()));
            return false;
        }
        m_source = extended_path(parent / L"vesta");
        return true;
    }
    bool validate_source()
    {
        if (m_source.empty() || !m_source.is_absolute())
        {
            fail(migration_stage::validate_paths, ERROR_INVALID_PARAMETER);
            return false;
        }
        std::error_code error;
        const auto destination = std::filesystem::weakly_canonical(m_target.root, error);
        if (error)
        {
            fail(migration_stage::validate_paths, static_cast<DWORD>(error.value()));
            return false;
        }
        const auto legacy = std::filesystem::weakly_canonical(m_source, error);
        if (error)
        {
            fail(migration_stage::validate_paths, static_cast<DWORD>(error.value()));
            return false;
        }
        // Equal roots mean the new typed folders live inside the old data directory (%TEMP%\vesta): the old
        // files are imported in place. Any other overlap could copy a tree into itself and is refused.
        m_in_place = inside(destination, legacy) && inside(legacy, destination);
        if (!m_in_place && (inside(destination, legacy) || inside(legacy, destination)))
        {
            fail(migration_stage::validate_paths, ERROR_INVALID_PARAMETER);
            return false;
        }
        if (!no_reparse(m_source))
            return false;
        const auto attributes = ::GetFileAttributesW(m_source.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            if (missing())
                return true;
            fail(migration_stage::validate_paths, ::GetLastError());
            return false;
        }
        if (!(attributes & FILE_ATTRIBUTE_DIRECTORY))
        {
            fail(migration_stage::validate_paths, ERROR_DIRECTORY);
            return false;
        }
        return true;
    }
    bool directory(const std::filesystem::path& path)
    {
        if (cancelled() || !no_reparse(path))
            return false;
        std::error_code error;
        std::filesystem::create_directories(path, error);
        if (error)
        {
            fail(migration_stage::create_directories, static_cast<DWORD>(error.value()));
            return false;
        }
        return no_reparse(path);
    }
    bool tree(const std::filesystem::path& from, const std::filesystem::path& to, bool lua, unsigned depth)
    {
        if (cancelled() || !no_reparse(from))
            return false;
        if (m_in_place && inside(extended_path(from), extended_path(to)) &&
            inside(extended_path(to), extended_path(from)))
            return true;
        if (depth > maximum_tree_depth)
        {
            fail(migration_stage::enumerate, ERROR_FILENAME_EXCED_RANGE);
            return false;
        }
        const auto attributes = ::GetFileAttributesW(from.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            if (missing())
                return true;
            fail(migration_stage::enumerate, ::GetLastError());
            return false;
        }
        if (!(attributes & FILE_ATTRIBUTE_DIRECTORY))
        {
            fail(migration_stage::enumerate, ERROR_DIRECTORY);
            return false;
        }
        std::error_code error;
        std::filesystem::directory_iterator it(from, error), end;
        if (error)
        {
            fail(migration_stage::enumerate, static_cast<DWORD>(error.value()));
            return false;
        }
        for (; it != end; it.increment(error))
        {
            if (error)
            {
                fail(migration_stage::enumerate, static_cast<DWORD>(error.value()));
                return false;
            }
            if (cancelled())
                return false;
            const auto filename = it->path().filename();
            // Only concrete pending names written by the existing product are
            // temporary. Arbitrary Lua/user files ending in .tmp remain data.
            const bool config_pending = from == m_source / L"config_cache" && filename == L"current.cfg.tmp";
            const bool lua_pending = lua && ((from == m_source / L"lua" && filename == L"runtime.json.tmp") ||
                                             (inside(from, m_source / L"lua/data") && filename == L"store.json.tmp"));
            const bool lua_log = lua && from == m_source / L"lua" &&
                                 ::CompareStringOrdinal(filename.c_str(), -1, L"logs", -1, TRUE) == CSTR_EQUAL;
            if (config_pending || lua_pending || lua_log)
                continue;
            const auto child_attributes = ::GetFileAttributesW(it->path().c_str());
            if (child_attributes == INVALID_FILE_ATTRIBUTES)
            {
                fail(migration_stage::enumerate, ::GetLastError());
                return false;
            }
            if (child_attributes & FILE_ATTRIBUTE_REPARSE_POINT)
            {
                fail(migration_stage::enumerate, ERROR_REPARSE_TAG_INVALID);
                return false;
            }
            if (child_attributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                if (!directory(to / filename) || !tree(it->path(), to / filename, lua, depth + 1))
                    return false;
            }
            else if (!copy_file(it->path(), to / filename))
                return false;
        }
        if (error)
        {
            fail(migration_stage::enumerate, static_cast<DWORD>(error.value()));
            return false;
        }
        return true;
    }
    bool prepare_pending(HANDLE handle)
    {
        if (!regular(handle, migration_stage::copy))
            return false;
        if (!::SetFilePointerEx(handle, {}, nullptr, FILE_BEGIN) || !::SetEndOfFile(handle))
        {
            fail(migration_stage::copy, ::GetLastError());
            return false;
        }
        return true;
    }
    bool copy_file(const std::filesystem::path& from, const std::filesystem::path& to)
    {
        if (cancelled() || !no_reparse(from) || !no_reparse(to))
            return false;
        const auto source_attributes = ::GetFileAttributesW(from.c_str());
        if (source_attributes == INVALID_FILE_ATTRIBUTES)
        {
            if (missing())
                return true;
            fail(migration_stage::open_source, ::GetLastError());
            return false;
        }
        if (source_attributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            fail(migration_stage::open_source, ERROR_DIRECTORY);
            return false;
        }
        const auto existing = ::GetFileAttributesW(to.c_str());
        if (existing != INVALID_FILE_ATTRIBUTES)
        {
            if (existing & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
            {
                fail(migration_stage::publish, ERROR_INVALID_DATA);
                return false;
            }
            ++m_result.retained;
            return true;
        }
        if (!missing())
        {
            fail(migration_stage::publish, ::GetLastError());
            return false;
        }
        // An existing destination needs no access to the old file contents.
        // Only missing destinations acquire an immutable read of the source.
        foundation::unique_handle source(
            ::CreateFileW(from.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                          FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (!valid_handle(source.get()))
        {
            fail(migration_stage::open_source, ::GetLastError());
            return false;
        }
        if (!regular(source.get(), migration_stage::open_source))
            return false;
        {
            foundation::unique_handle output(
                ::CreateFileW(m_pending.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (!valid_handle(output.get()))
            {
                fail(migration_stage::copy, ::GetLastError());
                return false;
            }
            if (!prepare_pending(output.get()))
                return false;
            std::array<char, 64 * 1024> bytes{};
            for (;;)
            {
                if (cancelled())
                    return false;
                DWORD read{}, written{};
                if (!::ReadFile(source.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr))
                {
                    fail(migration_stage::copy, ::GetLastError());
                    return false;
                }
                if (!read)
                    break;
                if (!::WriteFile(output.get(), bytes.data(), read, &written, nullptr))
                {
                    fail(migration_stage::copy, ::GetLastError());
                    return false;
                }
                if (written != read)
                {
                    fail(migration_stage::copy, ERROR_WRITE_FAULT);
                    return false;
                }
            }
            if (!::FlushFileBuffers(output.get()) || !::SetFilePointerEx(source.get(), {}, nullptr, FILE_BEGIN) ||
                !::SetFilePointerEx(output.get(), {}, nullptr, FILE_BEGIN))
            {
                fail(migration_stage::verify, ::GetLastError());
                return false;
            }
            std::array<char, 64 * 1024> copied{};
            for (;;)
            {
                if (cancelled())
                    return false;
                DWORD a{}, b{};
                if (!::ReadFile(source.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &a, nullptr) ||
                    !::ReadFile(output.get(), copied.data(), static_cast<DWORD>(copied.size()), &b, nullptr))
                {
                    fail(migration_stage::verify, ::GetLastError());
                    return false;
                }
                if (a != b || !std::equal(bytes.begin(), bytes.begin() + a, copied.begin()))
                {
                    fail(migration_stage::verify, ERROR_CRC);
                    return false;
                }
                if (!a)
                    break;
            }
        }
        if (cancelled())
            return false;
        // No REPLACE_EXISTING: even a concurrent destination writer wins.
        if (!::MoveFileExW(m_pending.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH))
        {
            const auto error = ::GetLastError();
            if (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS)
            {
                ++m_result.retained;
                return true;
            }
            fail(migration_stage::publish, error);
            return false;
        }
        ++m_result.copied;
        return true;
    }
    bool publish_marker(const std::filesystem::path& marker)
    {
        {
            foundation::unique_handle file(
                ::CreateFileW(m_pending.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (!valid_handle(file.get()))
            {
                fail(migration_stage::marker, ::GetLastError());
                return false;
            }
            if (!prepare_pending(file.get()))
                return false;
            DWORD written{};
            if (!::WriteFile(file.get(), marker_text.data(), static_cast<DWORD>(marker_text.size()), &written, nullptr))
            {
                fail(migration_stage::marker, ::GetLastError());
                return false;
            }
            if (written != marker_text.size())
            {
                fail(migration_stage::marker, ERROR_WRITE_FAULT);
                return false;
            }
            if (!::FlushFileBuffers(file.get()))
            {
                fail(migration_stage::marker, ::GetLastError());
                return false;
            }
        }
        if (cancelled())
            return false;
        if (!::MoveFileExW(m_pending.c_str(), marker.c_str(), MOVEFILE_WRITE_THROUGH))
        {
            fail(migration_stage::marker, ::GetLastError());
            return false;
        }
        return true;
    }
    const locations& m_target;
    const std::filesystem::path* m_explicit_source;
    std::filesystem::path m_source;
    bool m_in_place{};
    std::filesystem::path m_pending;
    std::stop_token m_stop;
    migration_result m_result;
};
} // namespace
migration_result prepare(const locations& destination, std::stop_token stop) noexcept
{
    try
    {
        return migration(destination, nullptr, stop).run();
    }
    catch (...)
    {
        return {migration_stage::copy, ERROR_NOT_ENOUGH_MEMORY};
    }
}
migration_result migrate(const locations& destination, const std::filesystem::path& legacy,
                         std::stop_token stop) noexcept
{
    try
    {
        return migration(destination, &legacy, stop).run();
    }
    catch (...)
    {
        return {migration_stage::copy, ERROR_NOT_ENOUGH_MEMORY};
    }
}
} // namespace platform::windows::data_storage
