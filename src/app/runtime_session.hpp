#pragma once

#include <foundation/scope_exit.hpp>
#include <foundation/thread_group.hpp>
#include <platform/windows/worker_control.hpp>

namespace app
{
struct context_t;

// Main-thread owner; workers may request shutdown but never join themselves.
class runtime_session final
{
public:
    explicit runtime_session(context_t& context);
    ~runtime_session();
    runtime_session(const runtime_session&) = delete;
    runtime_session& operator=(const runtime_session&) = delete;

    void start_workers();
    [[nodiscard]] bool initialize_scripting();
    void initialize_trace();
    void quiesce() noexcept;
    [[nodiscard]] foundation::shutdown_action quiesce_action() noexcept;
    [[nodiscard]] bool failed() const noexcept;

private:
    static void worker_failed(void* owner, foundation::thread_group::worker_id id,
                              foundation::thread_group::worker_error error) noexcept;
    context_t& m_context;
    std::stop_source m_source;
    platform::windows::stop_signal m_stop;
    // Destroy/join workers before closing their shared wake event.
    foundation::thread_group m_workers;
    bool m_quiesced{};
    bool m_scripting_constructed{};
    bool m_trace_started{};
};
} // namespace app
