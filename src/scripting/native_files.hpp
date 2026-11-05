#pragma once
struct lua_State;
namespace scripting::native_files
{
// Stock Lua API/stack contract, native Windows UTF-8 filename conversion.
// nullptr retains stock stdin behavior; parser/allocator remain upstream Lua.
int loadfile(lua_State* state, const char* filename, const char* mode = nullptr);
// Installs native source searcher/searchpath/loadfile/dofile/io.open/input/output/lines adapters.
// Keeps existing preload, loaded-module cache, and native-library policy.
void install(lua_State* state);
} // namespace scripting::native_files
