#pragma once

#include <atomic>
#include <memory>
#include <string>

namespace game
{
// The level the game has loaded ("de_mirage"), empty when it is in none. The game-state worker is the only writer;
// any thread may read.
class level_state
{
public:
    [[nodiscard]] std::shared_ptr<const std::string> name() const noexcept
    {
        return m_name.load(std::memory_order_acquire);
    }
    void publish(std::string name)
    {
        m_name.store(std::make_shared<const std::string>(std::move(name)), std::memory_order_release);
    }

private:
    std::atomic<std::shared_ptr<const std::string>> m_name{std::make_shared<const std::string>()};
};

[[nodiscard]] inline level_state& level() noexcept
{
    static level_state value{};
    return value;
}
} // namespace game
