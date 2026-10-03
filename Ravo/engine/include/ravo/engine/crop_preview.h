#pragma once

#include "ravo/recipe/recipe.h"
#include "ravo/foundation/cancellation.h"

namespace ravo
{
// Compensate geometry's loss of display pixels toward max_edge of the final
// photo. The prepared source never exceeds native dimensions; small native
// crops are never upsampled. Expanding canvas/frame geometry retains its size.
struct PreviewSourceSize
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t output_width = 0;
    std::uint32_t output_height = 0;
};

[[nodiscard]] Result<PreviewSourceSize>
plan_preview_source_size(const Recipe &recipe, std::uint32_t source_width,
                         std::uint32_t source_height, std::uint32_t max_edge,
                         const CancellationToken &cancellation = {});

// Preview-only projection. Recipe crop coordinates remain relative to the
// canonical constrained frame; the backdrop contains the full source quad.
struct CropPreviewPlan
{
    Recipe recipe;
    double x = 0.0;
    double y = 0.0;
    double width = 1.0;
    double height = 1.0;
    double width_scale = 1.0;
    double height_scale = 1.0;
    std::uint32_t coverage_width = 0;
    std::uint32_t coverage_height = 0;
    std::vector<std::uint8_t> source_coverage;
};

struct RenderedImage;
// Workspace surround only; source black pixels are never classified by colour.
[[nodiscard]] Result<void> fill_crop_preview_exterior(RenderedImage &image,
                                                      const CropPreviewPlan &plan,
                                                      const CancellationToken &cancellation);

[[nodiscard]] Result<CropPreviewPlan> plan_crop_preview(const Recipe &recipe,
                                                        std::uint32_t source_width,
                                                        std::uint32_t source_height,
                                                        const CancellationToken &cancellation = {});
} // namespace ravo
