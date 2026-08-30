#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "image_ops.h"
#include "ravo/recipe/lut3d.h"

namespace ravo
{

inline constexpr std::uint32_t kCubeLutMinimumSize = 2U;
inline constexpr std::uint32_t kCubeLutMaximumSize = 65U;
inline constexpr std::uint64_t kCubeLutMaximumFileBytes = 64U * 1024U * 1024U;
inline constexpr std::size_t kCubeLutMaximumLineBytes = 4096U;

struct CubeLut
{
    std::string canonical_path;
    std::string fingerprint;
    std::string title;
    std::uint32_t size = 0U;
    std::array<float, 3> domain_min{};
    std::array<float, 3> domain_max{1.0F, 1.0F, 1.0F};
    // .cube order: red changes fastest, followed by green and blue.
    std::vector<std::array<float, 3>> values;
};

class Lut3dCache final
{
public:
    [[nodiscard]] Result<std::shared_ptr<const CubeLut>>
    load(std::string_view path, const CancellationToken &cancellation);

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
    std::once_flag initialize_once_;
};

[[nodiscard]] Lut3dCache &process_lut3d_cache();
[[nodiscard]] Result<WorkingImage> apply_lut3d(WorkingImage input, const Lut3dParams &params,
                                               Lut3dCache &cache,
                                               const CancellationToken &cancellation);
[[nodiscard]] Result<WorkingImage> apply_lut3d(WorkingImage input,
                                               const OperationInstance &operation,
                                               Lut3dCache &cache,
                                               const CancellationToken &cancellation);
[[nodiscard]] Result<std::string>
lut3d_recipe_cache_fingerprint(const Recipe &recipe, Lut3dCache &cache,
                               const CancellationToken &cancellation);

} // namespace ravo
