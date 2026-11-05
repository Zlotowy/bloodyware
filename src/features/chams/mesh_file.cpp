#include <features/chams/mesh_file.hpp>
#include <platform/windows/data_paths.hpp>
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>

namespace features::chams::mesh_file
{
namespace
{
constexpr std::uint32_t magic = 0x4D414843;
constexpr std::uint32_t version = 6;
constexpr std::uint32_t maximum_string = 1u << 20;

// The legacy writer stores these fields individually without padding. Prove
// that bulk IO has exactly the same byte representation on this platform.
static_assert(std::endian::native == std::endian::little);
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
static_assert(sizeof(int) == 4);
static_assert(std::is_trivially_copyable_v<skinned_vertex>);
static_assert(sizeof(skinned_vertex) == 56);
static_assert(offsetof(skinned_vertex, normal) == 12);
static_assert(offsetof(skinned_vertex, tangent) == 24);
static_assert(offsetof(skinned_vertex, uv) == 40);
static_assert(offsetof(skinned_vertex, bone_indices) == 48);
static_assert(offsetof(skinned_vertex, bone_weights) == 52);
static_assert(std::is_nothrow_move_assignable_v<skinned_mesh>);

class reader
{
public:
    explicit reader(const std::filesystem::path& path)
        : m_stream(path, std::ios::binary)
    {
        if (!m_stream)
            return;
        m_stream.seekg(0, std::ios::end);
        const auto end = m_stream.tellg();
        if (end < 0)
            return;
        m_remaining = static_cast<std::uint64_t>(end);
        m_stream.seekg(0);
        m_ready = static_cast<bool>(m_stream);
    }
    [[nodiscard]] bool ready() const noexcept { return m_ready; }
    [[nodiscard]] std::uint64_t remaining() const noexcept { return m_remaining; }
    bool bytes(void* destination, std::uint64_t length)
    {
        if (length > m_remaining || length > static_cast<std::uint64_t>((std::numeric_limits<std::streamsize>::max)()))
            return false;
        if (length)
            m_stream.read(static_cast<char*>(destination), static_cast<std::streamsize>(length));
        if (!m_stream)
            return false;
        m_remaining -= length;
        return true;
    }
    template<class T>
    bool pod(T& value)
    {
        return bytes(&value, sizeof(value));
    }
    bool string(std::string& value)
    {
        std::uint32_t length{};
        if (!pod(length) || length > maximum_string || length > m_remaining)
            return false;
        value.resize(length);
        return bytes(value.data(), length);
    }

private:
    std::ifstream m_stream;
    std::uint64_t m_remaining{};
    bool m_ready{};
};

bool valid_geometry(const skinned_mesh& mesh) noexcept
{
    if (mesh.vertices.empty() || mesh.indices.empty() || mesh.draw_calls.empty())
        return false;
    for (const auto index : mesh.indices)
        if (index >= mesh.vertices.size())
            return false;
    for (const auto& draw : mesh.draw_calls)
        if (draw.index_offset > mesh.indices.size() || draw.index_count > mesh.indices.size() - draw.index_offset)
            return false;
    for (const auto& bone : mesh.bones)
        if (bone.parent < -1 || (bone.parent >= 0 && static_cast<std::size_t>(bone.parent) >= mesh.bones.size()))
            return false;
    return true;
}

bool encodable(const skinned_mesh& mesh) noexcept
{
    constexpr auto maximum_count = (std::numeric_limits<std::uint32_t>::max)();
    if (!mesh.valid || mesh.vertices.size() > maximum_count || mesh.indices.size() > maximum_count ||
        mesh.draw_calls.size() > maximum_count || mesh.bones.size() > maximum_count || !valid_geometry(mesh))
        return false;
    for (const auto& draw : mesh.draw_calls)
        if (draw.material.size() > maximum_string)
            return false;
    for (const auto& bone : mesh.bones)
        if (bone.name.size() > maximum_string)
            return false;
    return true;
}

class staged_writer
{
public:
    staged_writer() = default;
    staged_writer(const staged_writer&) = delete;
    staged_writer& operator=(const staged_writer&) = delete;
    ~staged_writer()
    {
        if (m_file)
            std::fclose(m_file);
        if (m_owned)
            ::DeleteFileW(m_pending.c_str());
    }
    bool open(const std::filesystem::path& target)
    {
        static std::atomic<std::uint64_t> sequence{};
        for (unsigned attempt = 0; attempt < 16; ++attempt)
        {
            m_pending = target;
            m_pending +=
                L".pending-" + std::to_wstring(::GetCurrentProcessId()) + L"-" + std::to_wstring(sequence.fetch_add(1));
            const auto handle =
                ::CreateFileW(m_pending.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle == INVALID_HANDLE_VALUE)
            {
                if (::GetLastError() == ERROR_FILE_EXISTS || ::GetLastError() == ERROR_ALREADY_EXISTS)
                    continue;
                return false;
            }
            m_owned = true;
            const int descriptor =
                ::_open_osfhandle(reinterpret_cast<std::intptr_t>(handle), _O_WRONLY | _O_BINARY | _O_NOINHERIT);
            if (descriptor == -1)
            {
                ::CloseHandle(handle);
                return false;
            }
            m_file = ::_fdopen(descriptor, "wb");
            if (!m_file)
            {
                ::_close(descriptor);
                return false;
            }
            (void)std::setvbuf(m_file, m_buffer.data(), _IOFBF, m_buffer.size());
            return true;
        }
        return false;
    }
    bool bytes(const void* source, std::size_t length)
    {
        return length == 0 || std::fwrite(source, 1, length, m_file) == length;
    }
    template<class T>
    bool pod(const T& value)
    {
        return bytes(&value, sizeof(value));
    }
    bool string(const std::string& value)
    {
        const auto length = static_cast<std::uint32_t>(value.size());
        return pod(length) && bytes(value.data(), length);
    }
    bool publish(const std::filesystem::path& target)
    {
        if (std::fflush(m_file) != 0)
            return false;
        const auto handle = reinterpret_cast<HANDLE>(::_get_osfhandle(::_fileno(m_file)));
        if (!::FlushFileBuffers(handle))
            return false;
        const int closed = std::fclose(m_file);
        m_file = nullptr;
        if (closed != 0 ||
            !::MoveFileExW(m_pending.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return false;
        m_owned = false;
        return true;
    }

private:
    std::filesystem::path m_pending;
    std::array<char, 64 * 1024> m_buffer;
    FILE* m_file{};
    bool m_owned{};
};
} // namespace

result load(const std::filesystem::path& path, skinned_mesh& out) noexcept
{
    try
    {
        reader input(platform::windows::data_storage::extended_path(path));
        if (!input.ready())
            return result::io_error;
        std::array<std::uint32_t, 6> header{};
        if (!input.bytes(header.data(), sizeof(header)) || header[0] != magic || header[1] != version)
            return result::invalid_data;
        const auto [vertices, indices, draws, bones] = std::array{header[2], header[3], header[4], header[5]};
        const std::uint64_t minimum = std::uint64_t{vertices} * 56 + std::uint64_t{indices} * 4 +
                                      std::uint64_t{draws} * 12 + std::uint64_t{bones} * 56;
        if (!vertices || !indices || !draws || minimum > input.remaining())
            return result::invalid_data;
        skinned_mesh decoded;
        decoded.vertices.resize(vertices);
        decoded.indices.resize(indices);
        decoded.draw_calls.resize(draws);
        decoded.bones.resize(bones);
        if (!input.bytes(decoded.vertices.data(), std::uint64_t{vertices} * sizeof(skinned_vertex)) ||
            !input.bytes(decoded.indices.data(), std::uint64_t{indices} * sizeof(std::uint32_t)))
            return result::invalid_data;
        for (auto& draw : decoded.draw_calls)
            if (!input.pod(draw.index_offset) || !input.pod(draw.index_count) || !input.string(draw.material))
                return result::invalid_data;
        for (auto& bone : decoded.bones)
            if (!input.string(bone.name) || !input.pod(bone.parent) || !input.pod(bone.inverse_bind.m))
                return result::invalid_data;
        if (!valid_geometry(decoded))
            return result::invalid_data;
        decoded.valid = true;
        out = std::move(decoded);
        return result::success;
    }
    catch (const std::bad_alloc&)
    {
        return result::allocation_failure;
    }
    catch (...)
    {
        return result::io_error;
    }
}

result save(const std::filesystem::path& path, const skinned_mesh& mesh) noexcept
{
    try
    {
        if (!encodable(mesh))
            return result::invalid_data;
        const auto target = platform::windows::data_storage::extended_path(path);
        if (target.empty())
            return result::io_error;
        staged_writer output;
        if (!output.open(target))
            return result::io_error;
        const std::array<std::uint32_t, 6> header{magic,
                                                  version,
                                                  static_cast<std::uint32_t>(mesh.vertices.size()),
                                                  static_cast<std::uint32_t>(mesh.indices.size()),
                                                  static_cast<std::uint32_t>(mesh.draw_calls.size()),
                                                  static_cast<std::uint32_t>(mesh.bones.size())};
        if (!output.bytes(header.data(), sizeof(header)) ||
            !output.bytes(mesh.vertices.data(), mesh.vertices.size() * sizeof(skinned_vertex)) ||
            !output.bytes(mesh.indices.data(), mesh.indices.size() * sizeof(std::uint32_t)))
            return result::io_error;
        for (const auto& draw : mesh.draw_calls)
            if (!output.pod(draw.index_offset) || !output.pod(draw.index_count) || !output.string(draw.material))
                return result::io_error;
        for (const auto& bone : mesh.bones)
            if (!output.string(bone.name) || !output.pod(bone.parent) || !output.pod(bone.inverse_bind.m))
                return result::io_error;
        return output.publish(target) ? result::success : result::io_error;
    }
    catch (const std::bad_alloc&)
    {
        return result::allocation_failure;
    }
    catch (...)
    {
        return result::io_error;
    }
}
} // namespace features::chams::mesh_file
