#include <stdafx.hpp>
#include <foundation/identity.hpp>
#include <game/state/constants.hpp>
#include <config/settings.hpp>
#include <config/misc.hpp>
#include <game/state/pose.hpp>
#include <game/memory/bound_services.hpp>
#include <game/state/services.hpp>
#include <app/diagnostic_reports.hpp>
#include <features/misc/misc.hpp>
#include <game/input/bindings.hpp>
#include <game/memory/compatibility.hpp>
#include <game/memory/remote_pointer.hpp>
#include <simulation/ballistics.hpp>
#include <simulation/seed_diagnostics.hpp>

#include <fstream>
#include <string_view>
#include <app/context.hpp>

namespace app
{
namespace
{
// A report runs against the live game like the product does: attach, find the modules, read the schema and the
// globals, and bind the memory adapters. Each step's outcome is part of the report rather than a reason to stop.
[[nodiscard]] game::attach_status attach_for_report()
{
    auto& context = app::context();
    game::attach_status status{};
    status.process = context.process.attach(L"cs2.exe") && context.modules.discover(context.process);
    if (!status.process)
        return status;
    status.schema = game::fields().initialize(context.memory);
    status.addresses = context.addresses.initialize(context.memory);
    game::bind_memory(context.memory, context.addresses);
    return status;
}

// Signature probes, the radar layout, the jump binding, console variables and a live spread sample.
int compatibility_report(std::ostream& out, const game::attach_status& attached)
{
    out << std::unitbuf;
    out << "compatibility-report v1\n";
    if (!attached.process)
    {
        out << "process=unavailable\n";
        return 2;
    }
    bool ok = true;
    const auto check = [&](std::string_view name, bool passed)
    {
        out << name << '=' << (passed ? "PASS" : "FAIL") << '\n';
        ok &= passed;
    };
    check("schema", attached.schema);
    out << "schema.fields=" << game::fields().field_count() << '\n';
    check("globals", attached.addresses);
    out << "spread.cache.signature=" << (game::compatibility::spread_patterns() ? "PASS" : "UNAVAILABLE (optional)")
        << '\n';
    const auto layout = game::compatibility::radar();
    check("radar.transform", layout.has_value());
    if (layout)
        out << "radar.origin=0x" << std::hex << layout->origin << " alternate=0x" << layout->alternate_scale << std::dec
            << '\n';
    const auto object = game::compatibility::hud_element("CCSGO_HudRadar");
    out << "radar.hud=" << (object ? "present" : "not-loaded") << '\n';
    if (object && layout)
    {
        const auto origin = game::bound_memory().process.load<foundation::vec2>(object + layout->origin);
        const auto scale = game::bound_memory().process.load<float>(object + layout->origin + 0x20);
        out << "radar.origin.xy=" << origin.x << ',' << origin.y << " inverse_scale=" << scale << '\n';
        bool found{};
        for (std::size_t off = 0x220; off < 0x440; off += 8)
        {
            const auto wrapper = game::bound_memory().process.load<std::uintptr_t>(object + off);
            if (!game::is_user_pointer(wrapper))
                continue;
            const auto ui = game::bound_memory().process.load<std::uintptr_t>(wrapper + 8);
            const auto p = game::compatibility::panel(ui);
            if (!p)
                continue;
            const auto w = game::bound_memory().process.load<float>(ui + p.width);
            if (!std::isfinite(w) || w < 200 || w > 1000)
                continue;
            out << "panorama.width=0x" << std::hex << p.width << " x=0x" << p.x << std::dec << " value=" << w << '\n';
            found = true;
            break;
        }
        check("panorama.getters", found);
    }
    const auto jump_bindings = game::input_bindings().candidates(game::input_action::jump);
    check("controls.jump", !jump_bindings.empty());
    out << "controls.jump.count=" << jump_bindings.size() << '\n';
    for (const auto& binding : jump_bindings)
        out << "controls.jump.name=" << binding.name << '\n';
    float forced{};
    check("weapon_accuracy_forcespread",
          game::variables().try_get(game::variables().find(foundation::identity::of("weapon_accuracy_forcespread")),
                                    forced));
    out << "forcespread=" << forced << '\n';
    config::publish_runtime_snapshot();
    game::local_player().update();
    for (int attempt = 0; attempt < 10; ++attempt)
    {
        simulation::ballistics().tick();
        if (simulation::ballistics().ctx().valid)
            break;
        ::Sleep(2);
    }
    const auto ctx = simulation::ballistics().ctx();
    out << "weapon.context=" << (ctx.valid ? "valid" : "not-ready") << " item=" << ctx.item_def_idx
        << " inaccuracy=" << ctx.inaccuracy << " spread=" << ctx.spread << '\n';
    if (ctx.valid && game::rules::is_firearm(ctx.weapon_type))
    {
        const auto sample = simulation::ballistics().sample_spread_offset(
            1337, ctx.inaccuracy, ctx.spread, ctx.recoil_index, ctx.item_def_idx, ctx.fire_mode, ctx.num_bullets, 0,
            ctx.pattern_seed);
        check("spread.live-sample", std::isfinite(sample.x) && std::isfinite(sample.y));
        out << "spread.xy=" << sample.x << ',' << sample.y << '\n';
    }
    out << "result=" << (ok ? "PASS" : "FAIL") << '\n';
    return ok ? 0 : 2;
}
} // namespace

std::optional<int> run_diagnostic_report(const int argc, char** argv)
{
    if (argc != 3)
        return std::nullopt;
    using writer = int (*)(std::ostream&, const game::attach_status&);
    const std::string_view mode{argv[1]};
    writer write{};
    if (mode == "--compatibility-report")
        write = &compatibility_report;
    else if (mode == "--seed-snapshot-report")
        write = &simulation::seed_diagnostics::report;
    else if (mode == "--auto-accept-report")
        write = &features::misc::auto_accept_report;
    else
        return std::nullopt;

    std::ofstream out(argv[2], std::ios::trunc);
    if (!out)
        return 3;
    return write(out, attach_for_report());
}
} // namespace app
