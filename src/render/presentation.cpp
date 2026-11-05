#include <render/presentation.hpp>

#include <atomic>

namespace render
{
namespace
{
class absent_presentation final : public presentation
{
public:
    [[nodiscard]] const font_set& fonts() const noexcept override { return m_fonts; }
    [[nodiscard]] bool menu_open() const noexcept override { return false; }
    [[nodiscard]] bool combat_input_ready() const noexcept override { return false; }
    [[nodiscard]] HWND window() const noexcept override { return nullptr; }
    [[nodiscard]] ID3D11Device* device() const noexcept override { return nullptr; }

private:
    font_set m_fonts{};
};

const absent_presentation g_absent{};
std::atomic<const presentation*> g_bound{};
} // namespace

void bind_presentation(const presentation& implementation) noexcept
{
    g_bound.store(&implementation, std::memory_order_release);
}

const presentation& bound_presentation() noexcept
{
    const auto* bound = g_bound.load(std::memory_order_acquire);
    return bound ? *bound : g_absent;
}
} // namespace render
