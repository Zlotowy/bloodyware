#pragma once

#include <array>
#include <memory>

namespace foundation
{

// Single writer. Only reuse a buffer when no published frame or reader owns it.
template<class T>
class snapshot_pool
{
public:
    [[nodiscard]] std::shared_ptr<T> acquire()
    {
        for (auto& buffer : m_buffers)
        {
            if (!buffer)
                buffer = std::make_shared<T>();
            if (buffer.use_count() == 1)
                return buffer;
        }
        return std::make_shared<T>();
    }

    void clear() { m_buffers = {}; }

private:
    std::array<std::shared_ptr<T>, 2> m_buffers;
};

} // namespace foundation
