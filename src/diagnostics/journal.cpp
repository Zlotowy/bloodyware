#include <diagnostics/journal.hpp>
#include <diagnostics/build_identity.hpp>
#include <diagnostics/log_directory.hpp>
#include <foundation/unique_handle.hpp>
#include <foundation/scope_exit.hpp>
#include <shlobj.h>
#include <bcrypt.h>
#include <algorithm>
#include <cstdio>
#include <utility>

namespace diagnostics
{
namespace
{
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
constexpr const char* build_configuration = VESTA_JOURNAL_CONFIGURATION;

// Only the sleeping writer resolves paths, formats lines, rotates or touches disk.
class file_sink final
{
public:
    file_sink(const journal_options& options, std::atomic<std::uint32_t>& error, std::atomic<std::uint32_t>& operation,
              std::atomic<std::uint64_t>& retries)
        : m_error(error)
        , m_operation(operation)
        , m_retries(retries)
        , m_limit(options.file_bytes)
        , m_gate(::CreateMutexW(nullptr, FALSE, options.writer_gate.c_str()))
    {
        if (!m_gate.get())
        {
            fail(::GetLastError());
            return;
        }
        auto directory = options.directory;
        if (directory.empty())
        {
            directory = default_logs_directory();
            if (directory.empty())
            {
                fail(ERROR_PATH_NOT_FOUND);
                return;
            }
        }
        std::error_code failure;
        std::filesystem::create_directories(directory, failure);
        if (failure)
        {
            fail(static_cast<std::uint32_t>(failure.value()));
            return;
        }
        m_path = directory / L"journal.log";
        for (std::size_t i = 0; i < m_backups.size(); ++i)
            m_backups[i] = directory / (L"journal." + std::to_wstring(i + 1) + L".log");
        if (::BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&m_session), sizeof(m_session),
                              BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        {
            fail(ERROR_GEN_FAILURE);
            return;
        }
        m_ready = true;
    }
    ~file_sink() { end(); }
    [[nodiscard]] std::uint64_t session() const noexcept { return m_session; }
    [[nodiscard]] bool begin() noexcept
    {
        if (!m_ready)
            return false;
        const auto waited = ::WaitForSingleObject(m_gate.get(), 200);
        if (waited != WAIT_OBJECT_0 && waited != WAIT_ABANDONED)
        {
            fail(waited == WAIT_TIMEOUT ? ERROR_TIMEOUT : ::GetLastError(), storage_stage::gate);
            return false;
        }
        m_locked = true;
        m_rotation_failed = false;
        return true;
    }
    void end() noexcept
    {
        close_file();
        if (m_locked)
        {
            ::ReleaseMutex(m_gate.get());
            m_locked = false;
        }
    }
    [[nodiscard]] bool line(std::string_view text) noexcept
    {
        if (!m_locked || m_rotation_failed)
            return false;
        char header[256]{};
        const auto header_count = std::snprintf(header, sizeof(header), "schema=1 version=%s build=%s config=%s\n",
                                                VESTA_JOURNAL_VERSION, VESTA_JOURNAL_REVISION, build_configuration);
        if (header_count <= 0 || static_cast<std::size_t>(header_count) >= sizeof(header))
        {
            fail(ERROR_BUFFER_OVERFLOW);
            return false;
        }
        if (!open_file())
            return false;
        const auto header_bytes = m_size == 0 ? static_cast<std::size_t>(header_count) : 0;
        if (m_size + header_bytes + text.size() > m_limit)
        {
            close_file();
            if (!rotate())
            {
                m_rotation_failed = true;
                return false;
            }
            if (!open_file())
                return false;
        }
        if (m_size == 0 && !write(std::string_view{header, static_cast<std::size_t>(header_count)}))
            return false;
        return write(text);
    }

private:
    void fail(std::uint32_t code, storage_stage operation = storage_stage::initialize) noexcept
    {
        m_operation.store(static_cast<std::uint32_t>(operation), std::memory_order_relaxed);
        m_error.store(code, std::memory_order_release);
    }
    void close_file() noexcept
    {
        if (m_file != INVALID_HANDLE_VALUE)
        {
            ::CloseHandle(m_file);
            m_file = INVALID_HANDLE_VALUE;
        }
        m_size = 0;
    }
    [[nodiscard]] bool open_file() noexcept
    {
        if (m_file != INVALID_HANDLE_VALUE)
            return true;
        m_file = ::CreateFileW(m_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (m_file == INVALID_HANDLE_VALUE)
        {
            fail(::GetLastError(), storage_stage::open);
            return false;
        }
        LARGE_INTEGER size{};
        if (!::GetFileSizeEx(m_file, &size) || !::SetFilePointerEx(m_file, {}, nullptr, FILE_END))
        {
            fail(::GetLastError(), storage_stage::seek);
            close_file();
            return false;
        }
        m_size = static_cast<std::uint64_t>(size.QuadPart);
        return true;
    }
    [[nodiscard]] bool write(std::string_view text) noexcept
    {
        DWORD written{};
        if (!::WriteFile(m_file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr))
        {
            fail(::GetLastError(), storage_stage::write);
            return false;
        }
        if (written != text.size())
        {
            fail(ERROR_WRITE_FAULT, storage_stage::write);
            return false;
        }
        m_size += written;
        return true;
    }
    [[nodiscard]] bool rotate() noexcept
    {
        for (std::size_t i = m_backups.size(); i > 0; --i)
        {
            const auto& source = i == 1 ? m_path : m_backups[i - 2];
            // Retry this move only: earlier generations may already have moved.
            // Delays are writer-only; persistent failure aborts the whole batch.
            for (unsigned attempt = 0;; ++attempt)
            {
                if (::MoveFileExW(source.c_str(), m_backups[i - 1].c_str(), MOVEFILE_REPLACE_EXISTING))
                    break;
                const auto code = ::GetLastError();
                if (code == ERROR_FILE_NOT_FOUND)
                    break;
                const bool transient =
                    code == ERROR_ACCESS_DENIED || code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION;
                if (!transient || attempt == 9)
                {
                    fail(code, storage_stage::rotate);
                    return false;
                }
                m_retries.fetch_add(1, std::memory_order_relaxed);
                ::Sleep(10);
            }
        }
        return true;
    }
    std::atomic<std::uint32_t>& m_error;
    std::atomic<std::uint32_t>& m_operation;
    std::atomic<std::uint64_t>& m_retries;
    std::size_t m_limit{};
    foundation::unique_handle m_gate;
    std::filesystem::path m_path;
    std::array<std::filesystem::path, journal::backup_count> m_backups;
    HANDLE m_file{INVALID_HANDLE_VALUE};
    std::uint64_t m_size{}, m_session{};
    bool m_ready{}, m_locked{}, m_rotation_failed{};
};
} // namespace

void journal::lock_owner() noexcept
{
    while (m_queue_lock.test_and_set(std::memory_order_acquire))
        std::this_thread::yield();
}

bool journal::start() noexcept
{
    try
    {
        return start(journal_options{});
    }
    catch (...)
    {
        m_storage_error = ERROR_NOT_ENOUGH_MEMORY;
        return false;
    }
}

bool journal::start(journal_options options) noexcept
{
    if (m_thread.joinable())
        return false;
    if (options.file_bytes < 4096 || options.file_bytes > 64 * 1024 || options.writer_gate.empty())
    {
        m_storage_error = ERROR_INVALID_PARAMETER;
        return false;
    }
    m_wake = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_wake)
    {
        m_storage_error = ::GetLastError();
        return false;
    }
    m_head = m_count = 0;
    m_sequence = 0;
    m_origin_ms = ::GetTickCount64();
    m_submitted = 0;
    m_accepted = 0;
    m_dropped = 0;
    m_pending_dropped = 0;
    m_suppressed = 0;
    m_storage_lost = 0;
    m_storage_retries = 0;
    m_storage_error = 0;
    m_storage_operation = 0;
    m_accepting.store(true, std::memory_order_release);
    try
    {
        m_thread = std::thread([this, options = std::move(options)]() mutable { writer(std::move(options)); });
    }
    catch (...)
    {
        m_accepting.store(false, std::memory_order_release);
        ::CloseHandle(m_wake);
        m_wake = nullptr;
        m_storage_error = ERROR_NOT_ENOUGH_MEMORY;
        return false;
    }
    return true;
}

void journal::stop() noexcept
{
    if (!m_thread.joinable())
        return;
    lock_owner();
    m_accepting.store(false, std::memory_order_release);
    ::SetEvent(m_wake);
    m_queue_lock.clear(std::memory_order_release);
    m_thread.join();
    ::CloseHandle(m_wake);
    m_wake = nullptr;
}

bool journal::submit(event id, severity level, code_domain domain, std::uint32_t code, subject owner) noexcept
{
    if (static_cast<std::size_t>(id) >= event_count || level > severity::error || domain > code_domain::last ||
        owner > subject::last || !m_accepting.load(std::memory_order_acquire))
        return false;
    m_submitted.fetch_add(1, std::memory_order_relaxed);
    const auto drop = [this]() noexcept
    {
        m_dropped.fetch_add(1, std::memory_order_relaxed);
        m_pending_dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    };
    if (m_queue_lock.test_and_set(std::memory_order_acquire))
        return drop();
    const foundation::scope_exit unlock([this]() noexcept { m_queue_lock.clear(std::memory_order_release); });
    const auto capacity = level == severity::error ? queue_capacity : queue_capacity - error_reserve;
    if (!m_accepting.load(std::memory_order_acquire) || m_count >= capacity)
        return drop();
    m_queue[(m_head + m_count) % queue_capacity] = {
        ++m_sequence, ::GetTickCount64() - m_origin_ms,      id, level, domain,
        owner,        domain == code_domain::none ? 0 : code};
    ++m_count;
    m_accepted.fetch_add(1, std::memory_order_relaxed);
    ::SetEvent(m_wake);
    return true;
}

journal_stats journal::stats() const noexcept
{
    return {m_submitted.load(std::memory_order_relaxed),
            m_accepted.load(std::memory_order_relaxed),
            m_dropped.load(std::memory_order_relaxed),
            m_suppressed.load(std::memory_order_relaxed),
            m_storage_lost.load(std::memory_order_relaxed),
            m_storage_retries.load(std::memory_order_relaxed),
            m_storage_error.load(std::memory_order_acquire),
            static_cast<storage_stage>(m_storage_operation.load(std::memory_order_relaxed))};
}

void journal::writer(journal_options options) noexcept
{
    const foundation::scope_exit close_admission(
        [this]() noexcept
        {
            lock_owner();
            m_accepting.store(false, std::memory_order_release);
            m_storage_lost.fetch_add(m_count, std::memory_order_relaxed);
            m_count = 0;
            m_queue_lock.clear(std::memory_order_release);
        });
    try
    {
        file_sink output(options, m_storage_error, m_storage_operation, m_storage_retries);
        struct recent
        {
            record last{};
            std::uint64_t repeats{};
            bool present{};
        };
        std::array<recent, event_count> history{};
        std::array<record, 64> batch{};
        std::uint64_t output_sequence{};
        const auto write_record = [&](const record& item, std::uint64_t repeats) noexcept
        {
            char line[512]{};
            const auto label = name(item.id);
            const auto count = std::snprintf(
                line, sizeof(line),
                "session=%016llx seq=%llu event_seq=%llu t_ms=%llu event=%.*s level=%u domain=%u subject=%u code=%lu "
                "repeat=%llu version=%s build=%s config=%s\n",
                output.session(), ++output_sequence, item.sequence, item.elapsed_ms, static_cast<int>(label.size()),
                label.data(), static_cast<unsigned>(item.level), static_cast<unsigned>(item.domain),
                static_cast<unsigned>(item.owner), static_cast<unsigned long>(item.code), repeats,
                VESTA_JOURNAL_VERSION, VESTA_JOURNAL_REVISION, build_configuration);
            if (count <= 0 || static_cast<std::size_t>(count) >= sizeof(line) ||
                !output.line(std::string_view{line, static_cast<std::size_t>(count)}))
                m_storage_lost.fetch_add(repeats ? repeats : 1, std::memory_order_relaxed);
        };
        for (;;)
        {
            if (::WaitForSingleObject(m_wake, INFINITE) != WAIT_OBJECT_0)
            {
                m_storage_error = ::GetLastError();
                return;
            }
            bool finished{};
            for (;;)
            {
                lock_owner();
                const auto size = std::min(m_count, batch.size());
                for (std::size_t i = 0; i < size; ++i)
                    batch[i] = m_queue[(m_head + i) % queue_capacity];
                m_head = (m_head + size) % queue_capacity;
                m_count -= size;
                finished = !m_accepting.load(std::memory_order_relaxed) && m_count == 0;
                const auto lost = m_pending_dropped.exchange(0, std::memory_order_relaxed);
                m_queue_lock.clear(std::memory_order_release);
                if (size == 0 && lost == 0)
                    break;
                const auto opened = output.begin();
                const foundation::scope_exit close([&]() noexcept { output.end(); });
                if (!opened)
                {
                    m_storage_lost.fetch_add(size, std::memory_order_relaxed);
                    continue;
                }
                if (lost)
                {
                    char line[256]{};
                    const auto n = std::snprintf(
                        line, sizeof(line), "session=%016llx seq=%llu dropped=%llu version=%s build=%s config=%s\n",
                        output.session(), ++output_sequence, lost, VESTA_JOURNAL_VERSION, VESTA_JOURNAL_REVISION,
                        build_configuration);
                    if (n > 0 && static_cast<std::size_t>(n) < sizeof(line))
                        (void)output.line(std::string_view{line, static_cast<std::size_t>(n)});
                }
                for (std::size_t i = 0; i < size; ++i)
                {
                    const auto& item = batch[i];
                    auto& previous = history[static_cast<std::size_t>(item.id)];
                    if (previous.present && previous.last.level == item.level && previous.last.domain == item.domain &&
                        previous.last.owner == item.owner && previous.last.code == item.code &&
                        item.elapsed_ms - previous.last.elapsed_ms < 1000)
                    {
                        ++previous.repeats;
                        m_suppressed.fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }
                    if (previous.repeats)
                        write_record(previous.last, previous.repeats);
                    write_record(item, 0);
                    previous = {item, 0, true};
                }
            }
            if (finished)
                break;
        }
        const auto opened = output.begin();
        const foundation::scope_exit close([&]() noexcept { output.end(); });
        for (const auto& item : history)
            if (item.repeats)
            {
                if (opened)
                    write_record(item.last, item.repeats);
                else
                    m_storage_lost.fetch_add(item.repeats, std::memory_order_relaxed);
            }
    }
    catch (...)
    {
        // A diagnostic failure must not terminate the product or a producer.
        m_storage_error = ERROR_UNHANDLED_EXCEPTION;
        // Keep draining impossible after writer failure; producers stop accepting.
        lock_owner();
        m_accepting.store(false, std::memory_order_release);
        m_storage_lost.fetch_add(m_count, std::memory_order_relaxed);
        m_count = 0;
        m_queue_lock.clear(std::memory_order_release);
    }
}

journal& process_journal() noexcept
{
    static journal instance;
    return instance;
}
void emit(event id, severity level, code_domain domain, std::uint32_t code, subject owner) noexcept
{
    (void)process_journal().submit(id, level, domain, code, owner);
}
} // namespace diagnostics
