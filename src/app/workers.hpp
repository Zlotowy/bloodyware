#pragma once

#include <platform/windows/worker_control.hpp>

namespace app::workers
{

using control = platform::windows::worker_control;
void game(control);
void movement(control);
void combat(control);
void nade_helper(control);
void seed_trigger(control);
void watchdog(control);

} // namespace app::workers
