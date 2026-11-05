#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace assets::kv3
{

class object; // forward decl, values may nest objects/arrays

using array = std::vector<object>;
using dict = std::map<std::string, object>; // insertion order not required for lookup use

// A KV3 value. NULL_VALUE is distinct from "key absent" -- callers doing
// dict lookups should check both.
class object
{
public:
    using value_t = std::variant<std::monostate, // NULL_VALUE
                                 bool, std::int64_t, std::uint64_t, double, std::string,
                                 std::vector<std::uint8_t>, // BINARY_BLOB
                                 array, dict>;

    object() = default;
    object(value_t v)
        : m_value(std::move(v))
    {
    }

    [[nodiscard]] bool is_dict() const { return std::holds_alternative<dict>(m_value); }
    [[nodiscard]] bool is_array() const { return std::holds_alternative<array>(m_value); }
    [[nodiscard]] bool is_null() const { return std::holds_alternative<std::monostate>(m_value); }

    [[nodiscard]] const object* find(const std::string& key) const;
    [[nodiscard]] const object* at(std::size_t index) const;
    // Lookups that tolerate a missing key or index: they yield a null value instead of nullptr, so a chain such as
    // value.get("a").get(0).as_int() reads 0 on unexpected data instead of dereferencing nullptr.
    [[nodiscard]] const object& get(const std::string& key) const;
    [[nodiscard]] const object& get(std::size_t index) const;
    [[nodiscard]] std::size_t size() const;

    [[nodiscard]] std::string as_string() const;
    [[nodiscard]] double as_double() const;
    [[nodiscard]] std::int64_t as_int() const;
    [[nodiscard]] bool as_bool() const;

    [[nodiscard]] bool as_float3(float out[3]) const;
    [[nodiscard]] bool as_float4(float out[4]) const;

    value_t m_value{};
};

struct document
{
    std::array<std::uint8_t, 16> format_guid{};
    object root{};
};

// Why a block could not be read. The values are journaled, so they never change meaning; new ones go at the end.
enum class failure : std::uint8_t
{
    truncated = 1,        // a read ran past the block or one of its sections
    not_kv3,              // the block does not start with a KV3 signature
    unsupported_version,  // a KV3 version this reader does not implement
    unknown_compression,  // compression method other than none/LZ4/ZSTD
    invalid_header,       // negative or impossible sizes in the header
    section_out_of_range, // a pool or stream does not fit in its decoded buffer
    string_table,         // unterminated string
    decompression,        // LZ4/ZSTD reported an error or a different size
    missing_trailer,      // 0xFFEEDD00 marker not where the layout puts it
    unknown_node_type,    // value type byte this reader does not know
    blob_table,           // binary blob bookkeeping does not match the blobs
};

class decode_error : public std::runtime_error
{
public:
    decode_error(const failure reason, const std::string& message)
        : std::runtime_error(message)
        , m_reason(reason)
    {
    }

    [[nodiscard]] failure reason() const noexcept { return m_reason; }
    // Set by decode() once the header is known, so the journal can tell the formats apart.
    void describe_block(const int version, const std::uint32_t compression) noexcept
    {
        m_version = static_cast<std::uint8_t>(version);
        m_compression = static_cast<std::uint8_t>(compression);
    }
    // reason | version << 8 | (compression + 1) << 16; a zero byte means "not known yet". No file data or names.
    [[nodiscard]] std::uint32_t code() const noexcept
    {
        return static_cast<std::uint32_t>(m_reason) | static_cast<std::uint32_t>(m_version) << 8 |
               static_cast<std::uint32_t>(m_compression == 0xFF ? 0 : m_compression + 1) << 16;
    }

private:
    failure m_reason{};
    std::uint8_t m_version{};
    std::uint8_t m_compression{0xFF};
};

// Throws decode_error for any block it cannot read completely.
[[nodiscard]] document decode(const std::uint8_t* data, std::size_t size);

inline document decode(const std::vector<std::uint8_t>& data)
{
    return decode(data.data(), data.size());
}

} // namespace assets::kv3
