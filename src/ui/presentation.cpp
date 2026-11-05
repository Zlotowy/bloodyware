#include <ui/presentation.hpp>

#include <render/presentation.hpp>
#include <ui/menu/menu.hpp>
#include <ui/overlay/overlay.hpp>

#include <atomic>

namespace ui
{
namespace
{
std::atomic<overlay_t*> g_overlay{};
std::atomic<menu_t*> g_menu{};

// The UI as the layers below see it.
class overlay_presentation final : public render::presentation
{
public:
    [[nodiscard]] const render::font_set& fonts() const noexcept override { return bound_overlay().fonts(); }
    [[nodiscard]] bool menu_open() const noexcept override { return bound_menu().is_open(); }
    [[nodiscard]] bool combat_input_ready() const noexcept override { return bound_overlay().combat_input_ready(); }
    [[nodiscard]] HWND window() const noexcept override { return bound_overlay().hwnd(); }
    [[nodiscard]] ID3D11Device* device() const noexcept override { return bound_overlay().device(); }
};

const overlay_presentation g_presentation{};
} // namespace

void bind_presentation(overlay_t& overlay, menu_t& menu) noexcept
{
    g_overlay.store(&overlay, std::memory_order_release);
    g_menu.store(&menu, std::memory_order_release);
    render::bind_presentation(g_presentation);
}

overlay_t& bound_overlay() noexcept
{
    static overlay_t idle{};
    auto* bound = g_overlay.load(std::memory_order_acquire);
    return bound ? *bound : idle;
}

menu_t& bound_menu() noexcept
{
    static menu_t closed{};
    auto* bound = g_menu.load(std::memory_order_acquire);
    return bound ? *bound : closed;
}
} // namespace ui
