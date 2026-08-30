#pragma once

#include <array>
#include <cstdint>

#include "ravo/engine/engine.h"
#include "ravo/recipe/perspective.h"

namespace ravo
{

struct PerspectivePoint
{
    double x = 0.0;
    double y = 0.0;

    [[nodiscard]] bool operator==(const PerspectivePoint &) const noexcept = default;
};

struct PerspectiveRect
{
    double left = 0.0;
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;

    [[nodiscard]] double width() const noexcept { return right - left; }
    [[nodiscard]] double height() const noexcept { return bottom - top; }
    [[nodiscard]] bool operator==(const PerspectiveRect &) const noexcept = default;
};

struct PerspectiveLayout
{
    std::array<double, 9> forward{};
    std::array<double, 9> inverse{};
    std::array<PerspectivePoint, 4> source_quad{};
    PerspectiveRect safe_crop{};
    std::uint32_t full_width = 0U;
    std::uint32_t full_height = 0U;
    std::uint32_t output_width = 0U;
    std::uint32_t output_height = 0U;
    std::uint32_t output_left = 0U;
    std::uint32_t output_top = 0U;
};

[[nodiscard]] Result<PerspectiveLayout>
compute_perspective_layout(std::uint32_t width, std::uint32_t height,
                           const PerspectiveParams &params);
[[nodiscard]] Result<LinearWorkingBuffer>
apply_perspective(const LinearWorkingBuffer &input, const PerspectiveParams &params,
                  const CancellationToken &cancellation);
[[nodiscard]] Result<PerspectiveAnalysis>
fit_perspective_guides(std::uint32_t width, std::uint32_t height,
                       const std::vector<PerspectiveGuideLine> &lines,
                       PerspectiveAnalysisMode mode, const CancellationToken &cancellation);
[[nodiscard]] Result<PerspectiveAnalysis>
analyze_perspective_raster(const RasterBuffer &raster, PerspectiveAnalysisMode mode,
                           const CancellationToken &cancellation);

} // namespace ravo
