#include <stdafx.hpp>
#include <diagnostics/log.hpp>
#include <diagnostics/performance.hpp>
#include <platform/windows/storage.hpp>
#include <diagnostics/health.hpp>
#include <config/settings.hpp>
#include <game/state/services.hpp>
#include <ui/presentation.hpp>
#include <simulation/collision.hpp>
#include <diagnostics/journal.hpp>
#include <diagnostics/crash_record.hpp>
#include <cwchar>
#include <app/context.hpp>
#include <app/startup_guards.hpp>
#include <app/startup_failure.hpp>
#include <config/first_run.hpp>
#include <config/hotkeys.hpp>
#include <app/runtime_session.hpp>
#include <app/diagnostic_reports.hpp>
#include <simulation/shot_trace.hpp>
#include <app/trace_session.hpp>
#include <scripting/runtime.hpp>

namespace
{
// Fatal pre-window failure: say what happened and what to do; the journal has the detail.
int fail_startup(app::startup_failure failure)
{
    const auto text = app::describe(failure);
    std::wstring message{text.what};
    message += L"\n\n";
    message += text.action;
    message += L"\n\nDetails: %TEMP%\\vesta\\logs";
    ::MessageBoxW(nullptr, message.c_str(), L"Vesta", MB_OK | MB_ICONERROR);
    return 1;
}

int run(int argc, char** argv)
{
#if defined(VESTA_SHOT_TRACE_ENABLED) && VESTA_SHOT_TRACE_ENABLED
    if (const auto report = app::run_diagnostic_report(argc, argv))
        return *report;
#else
    static_cast<void>(argc);
    static_cast<void>(argv);
#endif
    // Ordinary user process: no elevation, no token changes, no relaunch.
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    diagnostics::emit(diagnostics::event::startup_instance_begin);
    const app::single_instance_guard instance{};
    if (instance.state() == app::instance_state::already_running)
    {
        diagnostics::emit(diagnostics::event::startup_instance_duplicate);
        return 0;
    }
    if (instance.state() == app::instance_state::failed)
    {
        diagnostics::emit(diagnostics::event::startup_instance_failed, diagnostics::severity::error,
                          diagnostics::code_domain::win32, instance.error());
        const auto message =
            std::format(L"Vesta could not initialize its single-instance guard (Win32 {}).", instance.error());
        ::MessageBoxW(nullptr, message.c_str(), L"Vesta startup", MB_OK | MB_ICONERROR);
        return 1;
    }

    {
        if (!app::context().diagnostics.open(" :> "))
        {
            diagnostics::emit(diagnostics::event::startup_console_failed, diagnostics::severity::error);
            return 1;
        }
    }

    diagnostics::emit(diagnostics::event::startup_storage_begin);
    const auto storage = platform::windows::runtime_storage::initialize();
    if (!storage)
    {
        diagnostics::emit(diagnostics::event::startup_storage_failed, diagnostics::severity::error,
                          diagnostics::code_domain::system, storage.error);
        diagnostics::emit(diagnostics::event::startup_storage_stage, diagnostics::severity::error,
                          diagnostics::code_domain::mode, static_cast<std::uint32_t>(storage.stage));
        wchar_t message[256]{};
        std::swprintf(message, std::size(message),
                      L"Vesta user data initialization failed (stage %u, code %lu). Original files were preserved.",
                      static_cast<unsigned>(storage.stage), static_cast<unsigned long>(storage.error));
        ::MessageBoxW(nullptr, message, L"Vesta user data", MB_OK | MB_ICONERROR);
        return 1;
    }
    diagnostics::emit(diagnostics::event::startup_storage_ready);
    diagnostics::performance::set_output_directory(platform::windows::runtime_storage::paths().research_logs);
    if (storage.retained)
        diagnostics::emit(diagnostics::event::startup_storage_retained, diagnostics::severity::warning);
    const app::timer_period_guard timer_period{};
    ui::bind_presentation(app::context().overlay, app::context().menu);
    app::runtime_session session(app::context());

    {
        diagnostics::emit(diagnostics::event::startup_attach_begin);
        if (!app::context().process.attach(L"cs2.exe"))
        {
            diagnostics::emit(diagnostics::event::startup_attach_failed, diagnostics::severity::error);
            return fail_startup(app::startup_failure::game_connection);
        }
        diagnostics::emit(diagnostics::event::startup_attach_ready);

        diagnostics::emit(diagnostics::event::startup_input_begin);
        if (!app::context().input.connect())
        {
            diagnostics::emit(diagnostics::event::startup_input_failed, diagnostics::severity::error);
            return fail_startup(app::startup_failure::input_unavailable);
        }
        app::context().input.set_protected_keys(&config::is_lifecycle_key);
        platform::windows::bind_input(app::context().input);
        diagnostics::emit(diagnostics::event::startup_input_ready);
    }

    {
        diagnostics::emit(diagnostics::event::startup_modules_begin);
        if (!app::context().modules.discover(app::context().process))
        {
            diagnostics::emit(diagnostics::event::startup_modules_failed, diagnostics::severity::error);
            return fail_startup(app::startup_failure::modules_not_found);
        }
        diagnostics::emit(diagnostics::event::startup_modules_ready);

        diagnostics::emit(diagnostics::event::startup_addresses_begin);
        if (!app::context().addresses.initialize(app::context().memory))
        {
            diagnostics::emit(diagnostics::event::startup_addresses_failed, diagnostics::severity::error);
            return fail_startup(app::startup_failure::addresses_failed);
        }
        diagnostics::emit(diagnostics::event::startup_addresses_ready);

        diagnostics::emit(diagnostics::event::startup_schema_begin);
        if (!game::fields().initialize(app::context().memory))
        {
            diagnostics::emit(diagnostics::event::startup_schema_failed, diagnostics::severity::error);
            return fail_startup(app::startup_failure::schema_failed);
        }
        diagnostics::emit(diagnostics::event::startup_schema_ready);
        // Connect the game-memory adapters to the attached process before anything looks anything up.
        game::bind_memory(app::context().memory, app::context().addresses);
        game::world().set_line_of_sight(
            [](const foundation::vec3& from, const foundation::vec3& to)
            { return simulation::collision().valid() && !simulation::collision().trace_ray(from, to).hit; });
    }

    {
        // Start from the curated legit profile, then overlay the user's last live
        // cache.  Clean installs and missing/new keys therefore inherit legit.cfg.
        diagnostics::emit(diagnostics::event::startup_config_begin);
        config::apply_default_config();
        const bool profile_existed = config::storage.read_cache();
        if (!profile_existed)
        {
            diagnostics::emit(diagnostics::event::startup_cache_unavailable, diagnostics::severity::warning);
            config::storage.write_cache();
        }
        // A fresh installation shows the in-game guide once; a returning user is not greeted again.
        config::first_run::evaluate(profile_existed, platform::windows::runtime_storage::paths().configs);
        diagnostics::emit(diagnostics::event::startup_scripting_begin);
        if (!session.initialize_scripting())
        {
            app::context().diagnostics.warning("Lua runtime storage could not be initialized; scripting is disabled.");
            diagnostics::emit(diagnostics::event::startup_scripting_disabled, diagnostics::severity::warning);
            diagnostics::health().scripting.publish(1, diagnostics::readiness::state::error,
                                                    diagnostics::readiness::reason::scripting_storage_failed);
        }

        else
        {
            diagnostics::health().scripting.publish(1, diagnostics::readiness::state::ready);
            diagnostics::emit(diagnostics::event::startup_scripting_ready);
        }
        config::publish_runtime_snapshot();
        session.initialize_trace();

        diagnostics::emit(diagnostics::event::startup_workers_begin);
        session.start_workers();
        diagnostics::emit(diagnostics::event::startup_workers_ready);
        const auto completed = app::context().overlay.launch(session.quiesce_action());
        session.quiesce();
        return completed && !session.failed() ? EXIT_SUCCESS : EXIT_FAILURE;
    }
}
} // namespace

int main(int argc, char** argv)
{
    // DLLs load from System32 only, never from the folder vesta.exe was started from: a downloaded executable often
    // sits next to files nobody checked. The imports resolved before main are covered by /DEPENDENTLOADFLAG.
    ::SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto& journal = diagnostics::process_journal();
    if (!journal.start())
        diagnostics::process_log().warning("[startup] the journal cannot write its files; events are not recorded");
    const bool previous_crash = diagnostics::take_previous_crash();
    if (!diagnostics::install_crash_recorder())
        diagnostics::process_log().warning("[startup] crash recorder unavailable; a crash will leave no record");
    diagnostics::emit(diagnostics::event::bootstrap_begin);
    if (previous_crash)
    {
        diagnostics::health().previous_session_crashed.store(true, std::memory_order_release);
        diagnostics::emit(diagnostics::event::startup_previous_crash, diagnostics::severity::warning);
    }
    auto result = EXIT_FAILURE;
    const foundation::scope_exit finish(
        [&]() noexcept
        {
            diagnostics::emit(diagnostics::event::bootstrap_exit,
                              result == EXIT_SUCCESS ? diagnostics::severity::information
                                                     : diagnostics::severity::error,
                              diagnostics::code_domain::system, static_cast<std::uint32_t>(result));
            journal.stop();
        });
    try
    {
        result = run(argc, argv);
        return result;
    }
    catch (const std::system_error& error)
    {
        diagnostics::emit(diagnostics::event::bootstrap_exception, diagnostics::severity::error,
                          diagnostics::code_domain::system, static_cast<std::uint32_t>(error.code().value()));
        wchar_t message[160]{};
        std::swprintf(message, std::size(message), L"Vesta initialization failed (system error %d).",
                      error.code().value());
        ::MessageBoxW(nullptr, message, L"Vesta startup", MB_OK | MB_ICONERROR);
    }
    catch (...)
    {
        diagnostics::emit(diagnostics::event::bootstrap_exception, diagnostics::severity::error);
        ::MessageBoxW(nullptr, L"Vesta initialization failed. Resources were released.", L"Vesta startup",
                      MB_OK | MB_ICONERROR);
    }
    return result;
}
