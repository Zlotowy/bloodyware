#pragma once

#include <features/chams/mesh.hpp>
#include <filesystem>

namespace features::chams::mesh_file
{
enum class result
{
    success,
    invalid_data,
    io_error,
    allocation_failure
};

// Version-6 compatibility. A failed read leaves out unchanged. Counts are
// checked against the opened file before allocation; strings retain the 1MiB
// format limit. These functions never propagate errors into extraction futures.
[[nodiscard]] result load(const std::filesystem::path& path, skinned_mesh& out) noexcept;

// Stage, flush and replace on the same volume. Failed publication preserves
// the previous cache. The cache is rebuildable; this is not power-loss proof.
[[nodiscard]] result save(const std::filesystem::path& path, const skinned_mesh& mesh) noexcept;
} // namespace features::chams::mesh_file
