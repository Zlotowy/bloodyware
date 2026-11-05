#pragma once

#include <type_traits>
#include <utility>

namespace foundation
{

template<class Function>
class scope_exit final
{
public:
    explicit scope_exit(Function function) noexcept(std::is_nothrow_move_constructible_v<Function>)
        : m_function(std::move(function))
    {
        static_assert(std::is_nothrow_invocable_v<Function>);
    }
    ~scope_exit() noexcept { m_function(); }
    scope_exit(const scope_exit&) = delete;
    scope_exit& operator=(const scope_exit&) = delete;

private:
    Function m_function;
};

// The owner outlives the synchronous operation accepting this action.
struct shutdown_action
{
    void (*invoke)(void*) noexcept {};
    void* owner{};

    void operator()() const noexcept
    {
        if (invoke)
            invoke(owner);
    }
};

} // namespace foundation
