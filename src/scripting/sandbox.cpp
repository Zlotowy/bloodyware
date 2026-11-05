#include <stdafx.hpp>
#include <scripting/sandbox.hpp>
#include <scripting/native_files.hpp>
#include <scripting/trusted_scripts.hpp>

#include <bcrypt.h>

#include <array>
#include <cstring>
#include <fstream>

extern "C"
{
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

// Lua reports errors with longjmp: functions called from Lua below keep no C++ object with a destructor alive across
// a Lua call that can raise. Path work happens in separate functions that return plain buffers.

namespace scripting::sandbox
{
namespace
{
constexpr std::uintmax_t k_max_script_bytes = 8u << 20;
constexpr std::size_t k_max_module_name = 64;
constexpr const char* k_loaded_key = "vesta.sandbox.loaded";

[[nodiscard]] bool sha256_hex(const std::string_view bytes, std::string& out)
{
    std::array<unsigned char, 32> digest{};
    if (!BCRYPT_SUCCESS(::BCryptHash(
            BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
            static_cast<ULONG>(bytes.size()), digest.data(), static_cast<ULONG>(digest.size()))))
        return false;
    static constexpr char k_hex[] = "0123456789abcdef";
    out.clear();
    out.reserve(digest.size() * 2);
    for (const auto byte : digest)
    {
        out.push_back(k_hex[byte >> 4]);
        out.push_back(k_hex[byte & 0x0f]);
    }
    return true;
}

// `module` is a dotted name such as "lib.util"; the file is <root>/lib/util.lua and must stay inside root.
[[nodiscard]] bool resolve_module(const char* root_utf8, const char* module, char* out,
                                  const std::size_t out_size) noexcept
{
    try
    {
        const std::string_view name{module};
        if (name.empty() || name.size() > k_max_module_name || name.front() == '.' || name.back() == '.' ||
            name.find("..") != std::string_view::npos)
            return false;
        for (const char c : name)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '.')
                return false;
        std::string relative{name};
        std::ranges::replace(relative, '.', '/');
        const auto root = std::filesystem::u8path(root_utf8);
        std::error_code error{};
        const auto canonical_root = std::filesystem::weakly_canonical(root, error);
        if (error)
            return false;
        const auto candidate =
            std::filesystem::weakly_canonical(root / std::filesystem::u8path(relative + ".lua"), error);
        if (error || !std::filesystem::is_regular_file(candidate, error))
            return false;
        const auto inside = std::filesystem::relative(candidate, canonical_root, error);
        if (error || inside.empty() || inside.native().starts_with(L".."))
            return false;
        if (std::filesystem::file_size(candidate, error) > k_max_script_bytes || error)
            return false;
        const auto encoded = candidate.u8string();
        if (encoded.size() + 1 > out_size)
            return false;
        std::memcpy(out, encoded.data(), encoded.size());
        out[encoded.size()] = '\0';
        return true;
    }
    catch (...)
    {
        return false;
    }
}

int restricted_require(lua_State* state)
{
    const char* name = luaL_checkstring(state, 1);
    lua_getfield(state, LUA_REGISTRYINDEX, k_loaded_key);
    const int loaded = lua_gettop(state);
    lua_getfield(state, loaded, name);
    if (lua_isstring(state, -1) && std::strcmp(lua_tostring(state, -1), "\x01loading") == 0)
        return luaL_error(state, "module '%s' requires itself", name);
    if (!lua_isnil(state, -1))
        return 1;
    lua_pop(state, 1);

    char path[4096 * 3]{};
    if (!resolve_module(lua_tostring(state, lua_upvalueindex(1)), name, path, sizeof(path)) &&
        !resolve_module(lua_tostring(state, lua_upvalueindex(2)), name, path, sizeof(path)))
        return luaL_error(state, "module '%s' was not found in the script's folder or the modules folder", name);
    lua_pushliteral(state, "\x01loading");
    lua_setfield(state, loaded, name);
    if (native_files::loadfile(state, path, "t") != LUA_OK)
    {
        lua_pushnil(state);
        lua_setfield(state, loaded, name);
        return lua_error(state);
    }
    lua_pushstring(state, name);
    lua_call(state, 1, 1);
    if (lua_isnil(state, -1))
    {
        lua_pop(state, 1);
        lua_pushboolean(state, 1);
    }
    lua_pushvalue(state, -1);
    lua_setfield(state, loaded, name);
    return 1;
}

// load(chunk [, chunkname [, mode [, env]]]) with the mode forced to text: precompiled chunks are refused.
int text_only_load(lua_State* state)
{
    const int count = lua_gettop(state);
    if (count < 3)
    {
        lua_settop(state, 2);
        lua_pushliteral(state, "t");
    }
    else
    {
        lua_pushliteral(state, "t");
        lua_replace(state, 3);
    }
    const int arguments = lua_gettop(state);
    lua_pushvalue(state, lua_upvalueindex(1));
    lua_insert(state, 1);
    lua_call(state, arguments, LUA_MULTRET);
    return lua_gettop(state);
}

// setmetatable without finalizers: a __gc metamethod would run Lua code inside any Vesta API call that allocates,
// re-entering the runtime while it is in the middle of that call.
int setmetatable_without_finalizers(lua_State* state)
{
    if (lua_type(state, 2) == LUA_TTABLE)
    {
        lua_pushliteral(state, "__gc");
        const auto finalizer = lua_rawget(state, 2);
        lua_pop(state, 1);
        if (finalizer != LUA_TNIL)
            return luaL_error(state, "finalizers (__gc) are not available to scripts");
    }
    lua_pushvalue(state, lua_upvalueindex(1));
    lua_insert(state, 1);
    lua_call(state, lua_gettop(state) - 1, LUA_MULTRET);
    return lua_gettop(state);
}

void open_library(lua_State* state, const char* name, const lua_CFunction open)
{
    luaL_requiref(state, name, open, 1);
    lua_pop(state, 1);
}
} // namespace

std::string script_digest(const std::filesystem::path& file)
{
    std::error_code error{};
    const auto size = std::filesystem::file_size(file, error);
    if (error || size == 0 || size > k_max_script_bytes)
        return {};
    std::ifstream input(file, std::ios::binary);
    std::string text(static_cast<std::size_t>(size), '\0');
    if (!input.read(text.data(), static_cast<std::streamsize>(text.size())))
        return {};
    std::erase(text, '\r');
    std::string digest{};
    return sha256_hex(text, digest) ? digest : std::string{};
}

bool trusted(const std::filesystem::path& file)
{
    const auto digest = script_digest(file);
    return !digest.empty() && std::ranges::find(trusted::k_sha256, digest) != trusted::k_sha256.end();
}

void open_restricted(lua_State* state, const std::filesystem::path& script_folder,
                     const std::filesystem::path& modules_folder)
{
    open_library(state, LUA_GNAME, luaopen_base);
    open_library(state, LUA_COLIBNAME, luaopen_coroutine);
    open_library(state, LUA_TABLIBNAME, luaopen_table);
    open_library(state, LUA_STRLIBNAME, luaopen_string);
    open_library(state, LUA_MATHLIBNAME, luaopen_math);
    open_library(state, LUA_UTF8LIBNAME, luaopen_utf8);

    // Base: no file loaders, text-only load.
    lua_pushnil(state);
    lua_setglobal(state, "dofile");
    lua_pushnil(state);
    lua_setglobal(state, "loadfile");
    lua_getglobal(state, "load");
    lua_pushcclosure(state, text_only_load, 1);
    lua_setglobal(state, "load");
    lua_getglobal(state, "setmetatable");
    lua_pushcclosure(state, setmetatable_without_finalizers, 1);
    lua_setglobal(state, "setmetatable");

    // string.dump would hand out bytecode.
    lua_getglobal(state, LUA_STRLIBNAME);
    lua_pushnil(state);
    lua_setfield(state, -2, "dump");
    lua_pop(state, 1);

    // os: time only.
    luaL_requiref(state, "vesta.full_os", luaopen_os, 0);
    lua_createtable(state, 0, 4);
    for (const char* name : {"clock", "time", "date", "difftime"})
    {
        lua_getfield(state, -2, name);
        lua_setfield(state, -2, name);
    }
    lua_setglobal(state, LUA_OSLIBNAME);
    lua_pop(state, 1);

    // The restricted require keeps its own table of loaded modules; without the package library the standard one is
    // out of a script's reach.
    lua_newtable(state);
    lua_setfield(state, LUA_REGISTRYINDEX, k_loaded_key);
    for (const auto* folder : {&script_folder, &modules_folder})
    {
        const auto text = folder->u8string();
        lua_pushlstring(state, reinterpret_cast<const char*>(text.data()), text.size());
    }
    lua_pushcclosure(state, restricted_require, 2);
    lua_setglobal(state, "require");
}
} // namespace scripting::sandbox
