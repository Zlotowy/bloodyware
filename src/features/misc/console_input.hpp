#pragma once

#include <string>

namespace features::misc
{

// Runs one console command: open the console, enter the line, Enter, close the console.
//  paste = true : the line goes in through the clipboard (one Ctrl+V), nearly instant, layout independent. The
//                 previous clipboard text is put back afterwards.
//  paste = false: the line is typed key by key (US layout, ASCII only), slower.
// Only the console open and the wait before closing use delay_ms. An open console swallows game input; the walk bot
// already stands back while the console is up. ; and " are dropped, they would end or break the command.
void run_console_command(const std::string& line, int console_key, int delay_ms, bool paste = true);

} // namespace features::misc
