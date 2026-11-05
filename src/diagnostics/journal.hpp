#pragma once
#include <diagnostics/events.hpp>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <thread>
#include <windows.h>

namespace diagnostics
{
struct journal_options
{
    // Empty selects %TEMP%\vesta\logs on the writer.
    std::filesystem::path directory;
    std::size_t file_bytes{64 * 1024};
    std::wstring writer_gate{L"Local\\VestaDiagnosticJournal-v1"};
};
enum class storage_stage : std::uint32_t
{
    none,
    initialize,
    gate,
    open,
    seek,
    write,
    rotate,
    writer
};
struct journal_stats
{
    std::uint64_t submitted{}, accepted{}, dropped{}, suppressed{}, storage_lost{}, storage_retries{};
    std::uint32_t storage_error{};
    storage_stage storage_operation{};
};
class journal final
{
public:
    static constexpr std::size_t queue_capacity = 512;
    static constexpr std::size_t error_reserve = 64;
    static constexpr std::size_t backup_count = 3;
    journal() noexcept = default;
    ~journal() { stop(); }
    journal(const journal&) = delete;
    journal& operator=(const journal&) = delete;

    // Start/stop only on the owner; the object outlives all producer calls.
    // Producers never wait for queue space, a lock or filesystem work.
    [[nodiscard]] bool start() noexcept;
    [[nodiscard]] bool start(journal_options options) noexcept;
    void stop() noexcept;
    [[nodiscard]] bool submit(event id, severity level = severity::information, code_domain domain = code_domain::none,
                              std::uint32_t code = 0, subject owner = subject::application) noexcept;
    [[nodiscard]] journal_stats stats() const noexcept;

private:
    struct record
    {
        std::uint64_t sequence{}, elapsed_ms{};
        event id{};
        severity level{};
        code_domain domain{};
        subject owner{};
        std::uint32_t code{};
    };
    void writer(journal_options options) noexcept;
    void lock_owner() noexcept;
    std::array<record, queue_capacity> m_queue{};
    std::atomic_flag m_queue_lock = ATOMIC_FLAG_INIT;
    std::atomic<bool> m_accepting{};
    std::size_t m_head{}, m_count{};
    std::uint64_t m_sequence{}, m_origin_ms{};
    HANDLE m_wake{};
    std::thread m_thread;
    std::atomic<std::uint64_t> m_submitted{}, m_accepted{}, m_dropped{}, m_pending_dropped{}, m_suppressed{},
        m_storage_lost{}, m_storage_retries{};
    std::atomic<std::uint32_t> m_storage_error{}, m_storage_operation{};
};
[[nodiscard]] journal& process_journal() noexcept;
void emit(event id, severity level = severity::information, code_domain domain = code_domain::none,
          std::uint32_t code = 0, subject owner = subject::application) noexcept;
} // namespace diagnostics
