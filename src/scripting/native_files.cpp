#include <scripting/native_files.hpp>
#include <platform/windows/data_paths.hpp>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <new>
extern "C"
{
#include <lua.h>
#include <lauxlib.h>
}

namespace scripting::native_files
{
namespace
{
// C++ work finishes before any error-raising Lua API call. Lua callbacks below
// keep only POD locals; native FILEs close before stack allocation/error paths.
FILE* open_utf8(const char* filename, const char* mode) noexcept
{
    constexpr std::size_t maximum_utf8_path = 32767u * 4u;
    std::size_t length{};
    while (length <= maximum_utf8_path && filename[length])
        ++length;
    if (length > maximum_utf8_path)
    {
        errno = ENAMETOOLONG;
        return nullptr;
    }
    try
    {
        auto path = platform::windows::data_storage::extended_path(std::filesystem::u8path(filename));
        path.make_preferred();
        // Modes were validated by the Lua-facing caller. Collapse repeated
        // binary flags instead of allocating a script-controlled mode string.
        std::array<wchar_t, 4> wide_mode{};
        wide_mode[0] = static_cast<wchar_t>(mode[0]);
        std::size_t source = 1, destination = 1;
        if (mode[source] == '+')
        {
            wide_mode[destination++] = L'+';
            ++source;
        }
        if (mode[source] == 'b')
            wide_mode[destination] = L'b';
        return ::_wfopen(path.c_str(), wide_mode.data());
    }
    catch (const std::bad_alloc&)
    {
        errno = ENOMEM;
    }
    catch (...)
    {
        errno = EILSEQ;
    }
    return nullptr;
}
bool readable(const char* filename) noexcept
{
    auto* file = open_utf8(filename, "r");
    if (!file)
        return false;
    std::fclose(file);
    return true;
}
struct source_reader
{
    FILE* file{};
    std::array<char, 32 * 1024> bytes;
    std::array<char, 2> first{};
    std::size_t first_size{};
};
int first_character(FILE* file, bool& header) noexcept
{
    int c = std::fgetc(file);
    if (c == 0xef)
    {
        const int b = std::fgetc(file), d = std::fgetc(file);
        if (b == 0xbb && d == 0xbf)
            c = std::fgetc(file);
        else
        {
            if (d != EOF)
                std::ungetc(d, file);
            if (b != EOF)
                std::ungetc(b, file);
        }
    }
    header = c == '#';
    if (header)
    {
        do
        {
            c = std::fgetc(file);
        } while (c != EOF && c != '\n');
        c = std::fgetc(file);
    }
    return c;
}
const char* read_source(lua_State*, void* context, std::size_t* size) noexcept
{
    auto& reader = *static_cast<source_reader*>(context);
    if (reader.first_size)
    {
        *size = reader.first_size;
        reader.first_size = 0;
        return reader.first.data();
    }
    *size = std::fread(reader.bytes.data(), 1, reader.bytes.size(), reader.file);
    return *size ? reader.bytes.data() : nullptr;
}
int file_error(lua_State* state, int filename_index, const char* operation, int error)
{
    const auto* filename = lua_tostring(state, filename_index) + 1;
    lua_pushfstring(state, "cannot %s %s: %s", operation, filename, std::strerror(error));
    lua_remove(state, filename_index);
    return LUA_ERRFILE;
}
bool find_source(lua_State* state, const char* name, const char* path, const char* separator,
                 const char* directory_separator)
{
    if (*separator)
        name = luaL_gsub(state, name, separator, directory_separator);
    luaL_Buffer names;
    luaL_buffinit(state, &names);
    // The '?' in a leading Windows namespace is not a module placeholder.
    // Template payloads and the aggregate buffer remain Lua-allocator-owned.
    for (const char* start = path;;)
    {
        const auto length = std::strcspn(start, ";");
        const bool prefixed = length >= 4 && (start[0] == '\\' || start[0] == '/') && start[1] == start[0] &&
                              start[2] == '?' && (start[3] == '\\' || start[3] == '/');
        const std::size_t prefix = prefixed ? 4 : 0;
        if (prefix)
            luaL_addlstring(&names, start, prefix);
        lua_pushlstring(state, start + prefix, length - prefix);
        luaL_gsub(state, lua_tostring(state, -1), "?", name);
        lua_remove(state, -2);
        luaL_addvalue(&names);
        start += length;
        if (!*start)
            break;
        luaL_addchar(&names, ';');
        ++start;
    }
    luaL_pushresult(&names);
    const auto* list = lua_tostring(state, -1);
    const auto* next = list;
    for (;;)
    {
        const auto length = std::strcspn(next, ";");
        lua_pushlstring(state, next, length);
        if (readable(lua_tostring(state, -1)))
            return true;
        lua_pop(state, 1);
        next += length;
        if (!*next)
            break;
        ++next;
    }
    luaL_Buffer error;
    luaL_buffinit(state, &error);
    luaL_addstring(&error, "no file '");
    luaL_addgsub(&error, list, ";", "'\n\tno file '");
    luaL_addstring(&error, "'");
    luaL_pushresult(&error);
    return false;
}
int searchpath(lua_State* state)
{
    const bool found = find_source(state, luaL_checkstring(state, 1), luaL_checkstring(state, 2),
                                   luaL_optstring(state, 3, "."), luaL_optstring(state, 4, "\\"));
    if (found)
        return 1;
    lua_pushnil(state);
    lua_insert(state, -2);
    return 2;
}
int source_searcher(lua_State* state)
{
    const auto* name = luaL_checkstring(state, 1);
    lua_getfield(state, lua_upvalueindex(1), "path");
    const auto* path = lua_tostring(state, -1);
    if (!path)
        return luaL_error(state, "'package.path' must be a string");
    if (!find_source(state, name, path, ".", "\\"))
        return 1;
    const int filename_index = lua_gettop(state);
    const auto* filename = lua_tostring(state, filename_index);
    if (loadfile(state, filename) != LUA_OK)
        return luaL_error(state, "error loading module '%s' from file '%s':\n\t%s", name, filename,
                          lua_tostring(state, -1));
    lua_pushvalue(state, filename_index);
    return 2;
}
int script_loadfile(lua_State* state)
{
    const auto* filename = luaL_optstring(state, 1, nullptr);
    const auto* mode = luaL_optstring(state, 2, nullptr);
    const bool environment = !lua_isnone(state, 3);
    const int status = loadfile(state, filename, mode);
    if (status == LUA_OK)
    {
        if (environment)
        {
            lua_pushvalue(state, 3);
            lua_setupvalue(state, -2, 1);
        }
        return 1;
    }
    lua_pushnil(state);
    lua_insert(state, -2);
    return 2;
}
int dofile_result(lua_State* state, int, lua_KContext)
{
    return lua_gettop(state) - 1;
}
int script_dofile(lua_State* state)
{
    const auto* filename = luaL_optstring(state, 1, nullptr);
    lua_settop(state, 1);
    if (loadfile(state, filename) != LUA_OK)
        return lua_error(state);
    lua_callk(state, 0, LUA_MULTRET, 0, dofile_result);
    return dofile_result(state, 0, 0);
}
bool valid_mode(const char* mode) noexcept
{
    if (!*mode || !std::strchr("rwa", *mode))
        return false;
    ++mode;
    if (*mode == '+')
        ++mode;
    return std::strspn(mode, "b") == std::strlen(mode);
}
int close_file(lua_State* state)
{
    auto* stream = static_cast<luaL_Stream*>(luaL_checkudata(state, 1, LUA_FILEHANDLE));
    const int result = std::fclose(stream->f);
    const int error = errno;
    stream->f = nullptr;
    errno = error;
    return luaL_fileresult(state, result == 0, nullptr);
}
luaL_Stream* open_stream(lua_State* state, const char* filename, const char* mode)
{
    auto* stream = static_cast<luaL_Stream*>(lua_newuserdatauv(state, sizeof(luaL_Stream), 0));
    stream->f = nullptr;
    stream->closef = nullptr;
    luaL_setmetatable(state, LUA_FILEHANDLE);
    stream->f = open_utf8(filename, mode);
    if (stream->f)
        stream->closef = close_file;
    return stream;
}
int io_open(lua_State* state)
{
    const auto* filename = luaL_checkstring(state, 1);
    const auto* mode = luaL_optstring(state, 2, "r");
    luaL_argcheck(state, valid_mode(mode), 2, "invalid mode");
    if (!open_stream(state, filename, mode)->f)
        return luaL_fileresult(state, 0, filename);
    return 1;
}
int default_file(lua_State* state)
{
    if (!lua_isstring(state, 1))
    {
        const int arguments = lua_gettop(state);
        lua_pushvalue(state, lua_upvalueindex(1));
        lua_insert(state, 1);
        lua_call(state, arguments, LUA_MULTRET);
        return lua_gettop(state);
    }
    const auto* filename = lua_tostring(state, 1);
    const auto* mode = lua_tostring(state, lua_upvalueindex(2));
    if (!open_stream(state, filename, mode)->f)
        return luaL_argerror(state, 1,
                             lua_pushfstring(state, "cannot open file '%s' (%s)", filename, std::strerror(errno)));
    lua_pushvalue(state, lua_upvalueindex(1));
    lua_insert(state, -2);
    lua_call(state, 1, 1);
    return 1;
}
int file_lines(lua_State* state)
{
    auto* stream = static_cast<luaL_Stream*>(lua_touserdata(state, lua_upvalueindex(1)));
    if (!stream->closef)
        return luaL_error(state, "file is already closed");
    lua_settop(state, 0);
    lua_pushvalue(state, lua_upvalueindex(3));
    lua_pushvalue(state, lua_upvalueindex(1));
    const int count = static_cast<int>(lua_tointeger(state, lua_upvalueindex(4)));
    for (int i = 1; i <= count; ++i)
        lua_rawgeti(state, lua_upvalueindex(2), i);
    lua_call(state, count + 1, LUA_MULTRET);
    const int results = lua_gettop(state);
    if (results && lua_toboolean(state, 1))
        return results;
    if (results > 1)
        return luaL_error(state, "%s", lua_tostring(state, 2));
    lua_settop(state, 0);
    lua_pushvalue(state, lua_upvalueindex(1));
    const auto close = stream->closef;
    stream->closef = nullptr;
    (void)close(state);
    return 0;
}
int io_lines(lua_State* state)
{
    if (lua_isnoneornil(state, 1))
    {
        const int count = lua_gettop(state);
        lua_pushvalue(state, lua_upvalueindex(1));
        lua_insert(state, 1);
        lua_call(state, count, LUA_MULTRET);
        return lua_gettop(state);
    }
    const auto* filename = luaL_checkstring(state, 1);
    const int count = lua_gettop(state);
    luaL_argcheck(state, count - 1 <= 250, 252, "too many arguments");
    if (!open_stream(state, filename, "r")->f)
        return luaL_argerror(state, 1,
                             lua_pushfstring(state, "cannot open file '%s' (%s)", filename, std::strerror(errno)));
    const int file_index = lua_gettop(state);
    lua_pushvalue(state, file_index);
    lua_createtable(state, count - 1, 0);
    for (int i = 2; i <= count; ++i)
    {
        lua_pushvalue(state, i);
        lua_rawseti(state, -2, i - 1);
    }
    lua_pushvalue(state, lua_upvalueindex(2));
    lua_pushinteger(state, count - 1);
    lua_pushcclosure(state, file_lines, 4);
    lua_pushnil(state);
    lua_pushnil(state);
    lua_pushvalue(state, file_index);
    return 4; // Lua 5.4 generic-for closes the file on early break/error.
}

} // namespace

int loadfile(lua_State* state, const char* filename, const char* mode)
{
    if (!filename)
        return luaL_loadfilex(state, nullptr, mode);
    const int filename_index = lua_gettop(state) + 1;
    lua_pushfstring(state, "@%s", filename); // Lua-owned name; no native file yet.
    source_reader reader;
    reader.file = open_utf8(filename, "r");
    if (!reader.file)
        return file_error(state, filename_index, "open", errno);
    bool header{};
    int c = first_character(reader.file, header);
    if (c == static_cast<unsigned char>(LUA_SIGNATURE[0]))
    {
        std::fclose(reader.file);
        reader.file = open_utf8(filename, "rb");
        if (!reader.file)
            return file_error(state, filename_index, "reopen", errno);
        c = first_character(reader.file, header);
        header = false;
    }
    if (header)
        reader.first[reader.first_size++] = '\n';
    if (c != EOF)
        reader.first[reader.first_size++] = static_cast<char>(c);
    errno = 0;
    const int status = lua_load(state, read_source, &reader, lua_tostring(state, filename_index), mode);
    const bool failed = std::ferror(reader.file) != 0;
    const int error = errno;
    std::fclose(reader.file);
    if (failed)
    {
        lua_settop(state, filename_index);
        return file_error(state, filename_index, "read", error ? error : EIO);
    }
    lua_remove(state, filename_index);
    return status;
}
void install(lua_State* state)
{
    const int top = lua_gettop(state);
    lua_pushcfunction(state, script_loadfile);
    lua_setglobal(state, "loadfile");
    lua_pushcfunction(state, script_dofile);
    lua_setglobal(state, "dofile");
    lua_getglobal(state, "io");
    if (lua_istable(state, -1))
    {
        lua_pushcfunction(state, io_open);
        lua_setfield(state, -2, "open");
        lua_getfield(state, -1, "input");
        lua_pushliteral(state, "r");
        lua_pushcclosure(state, default_file, 2);
        lua_setfield(state, -2, "input");
        lua_getfield(state, -1, "output");
        lua_pushliteral(state, "w");
        lua_pushcclosure(state, default_file, 2);
        lua_setfield(state, -2, "output");
        lua_getfield(state, -1, "lines");
        luaL_getmetatable(state, LUA_FILEHANDLE);
        lua_getfield(state, -1, "__index");
        lua_getfield(state, -1, "read");
        lua_remove(state, -2);
        lua_remove(state, -2);
        lua_pushcclosure(state, io_lines, 2);
        lua_setfield(state, -2, "lines");
    }
    lua_pop(state, 1);
    lua_getglobal(state, "package");
    if (lua_istable(state, -1))
    {
        const int package_index = lua_gettop(state);
        lua_pushcfunction(state, searchpath);
        lua_setfield(state, -2, "searchpath");
        lua_getfield(state, -1, "searchers");
        if (lua_istable(state, -1))
        {
            lua_pushvalue(state, package_index);
            lua_pushcclosure(state, source_searcher, 1);
            lua_rawseti(state, -2, 2);
        }
    }
    lua_settop(state, top);
}
} // namespace scripting::native_files
