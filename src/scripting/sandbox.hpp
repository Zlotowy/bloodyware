#pragma once

#include <filesystem>
#include <string>

struct lua_State;

namespace scripting::sandbox
{
// SHA-256 of a script's text with CRLF folded to LF, as lowercase hex; empty when the file cannot be read.
[[nodiscard]] std::string script_digest(const std::filesystem::path& file);

// True only for a script whose digest is one of the trusted scripts built into Vesta (the web radar). Such a script
// gets the full standard library and the helper launcher; every other script runs restricted.
[[nodiscard]] bool trusted(const std::filesystem::path& file);

// The standard library a restricted script sees: base without dofile/loadfile and with text-only `load`,
// coroutine, table, string without `dump`, math, utf8, os with clock/time/date/difftime only, and a `require` that
// loads Lua source from the script's own folder or the shared modules folder and nowhere else. No io, no package,
// no debug.
void open_restricted(lua_State* state, const std::filesystem::path& script_folder,
                     const std::filesystem::path& modules_folder);
} // namespace scripting::sandbox
