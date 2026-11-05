#include <stdafx.hpp>
#include <assets/resource.hpp>

namespace assets
{

bool resource::parse(std::vector<std::uint8_t> file_data)
{
    m_blocks.clear();
    m_valid = false;
    m_data = std::move(file_data);

    // header: u32 fileSize, u16 headerVersion, u16 version, u32 blockOffset, u32 blockCount
    constexpr std::size_t k_header_size{16};
    if (m_data.size() < k_header_size)
    {
        return false;
    }

    std::uint32_t file_size{}, block_offset{}, block_count{};
    std::uint16_t header_version{}, version{};

    std::memcpy(&file_size, m_data.data() + 0, 4);
    std::memcpy(&header_version, m_data.data() + 4, 2);
    std::memcpy(&version, m_data.data() + 6, 2);
    std::memcpy(&block_offset, m_data.data() + 8, 4);
    std::memcpy(&block_count, m_data.data() + 12, 4);

    if (header_version != 12 || block_count == 0 || block_count > 256)
    {
        return false;
    }

    // blockOffset is relative to its own field (file offset 8).
    const std::size_t table = 8u + block_offset;
    if (table + static_cast<std::size_t>(block_count) * 12u > m_data.size())
    {
        return false;
    }

    m_blocks.reserve(block_count);

    for (std::uint32_t i = 0; i < block_count; ++i)
    {
        const auto entry = table + static_cast<std::size_t>(i) * 12u;

        block b{};
        std::memcpy(b.type, m_data.data() + entry, 4);

        std::uint32_t rel_offset{}, size{};
        std::memcpy(&rel_offset, m_data.data() + entry + 4, 4);
        std::memcpy(&size, m_data.data() + entry + 8, 4);

        // Each block's offset is likewise relative to the field holding it.
        const std::size_t absolute = entry + 4u + rel_offset;
        if (absolute + size > m_data.size())
        {
            return false;
        }

        b.offset = static_cast<std::uint32_t>(absolute);
        b.size = size;
        m_blocks.push_back(b);
    }

    m_valid = true;
    return true;
}

const resource::block* resource::find(const char* type) const
{
    for (const auto& b : m_blocks)
    {
        if (b.is(type))
        {
            return &b;
        }
    }

    return nullptr;
}

std::vector<const resource::block*> resource::find_all(const char* type) const
{
    std::vector<const block*> result{};
    for (const auto& b : m_blocks)
    {
        if (b.is(type))
        {
            result.push_back(&b);
        }
    }

    return result;
}

} // namespace assets
