#pragma once

#include <platform/windows/process.hpp>

#include <cstdint>
#include <string>

namespace game
{
// A scene node's model state names its model either directly or through a handle to the CModel, whose name is read
// instead. The handle is only read when there is no direct name.
struct model_name_source
{
    std::uintptr_t scene_node{};
    std::uintptr_t direct_name{};
    std::uintptr_t model_handle{};

    // Changes whenever the node switches to another model; a cache keyed by node can compare it.
    [[nodiscard]] std::uintptr_t identity() const noexcept { return direct_name ? direct_name : model_handle; }
};

namespace detail
{
constexpr std::uintptr_t k_model_state{0x160};
} // namespace detail

[[nodiscard]] inline model_name_source read_model_name_source(const platform::windows::process_session& process,
                                                              const std::uintptr_t scene_node)
{
    model_name_source source{scene_node};
    source.direct_name = process.load<std::uintptr_t>(scene_node + detail::k_model_state + 0x88);
    if (!source.direct_name)
        source.model_handle = process.load<std::uintptr_t>(scene_node + detail::k_model_state + 0x80);
    return source;
}

// Compiled model path ("characters/.../model.vmdl_c") of the model a scene node renders, or empty.
[[nodiscard]] inline std::string read_model_path(const platform::windows::process_session& process,
                                                 const model_name_source& source)
{
    auto path = source.direct_name ? process.load_text(source.direct_name, 256) : std::string{};
    if (path.empty())
    {
        const auto handle = source.model_handle
                                ? source.model_handle
                                : process.load<std::uintptr_t>(source.scene_node + detail::k_model_state + 0x80);
        const auto model = handle ? process.load<std::uintptr_t>(handle) : 0;
        const auto name = model ? process.load<std::uintptr_t>(model + 0x08) : 0;
        if (name)
            path = process.load_text(name, 256);
    }
    if (path.ends_with(".vmdl"))
        path += "_c";
    return path;
}

[[nodiscard]] inline std::string read_model_path(const platform::windows::process_session& process,
                                                 const std::uintptr_t scene_node)
{
    return scene_node ? read_model_path(process, read_model_name_source(process, scene_node)) : std::string{};
}
} // namespace game
