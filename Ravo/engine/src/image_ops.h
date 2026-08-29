#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "ravo/engine/engine.h"
#include "raw_pipeline.h"

namespace ravo
{

using WorkingImage = LinearWorkingBuffer;
struct ColorBalanceParams;

[[nodiscard]] Result<WorkingImage> working_from_raw(const DecodedRaw &raw, std::uint32_t width,
                                                    std::uint32_t height,
                                                    const std::array<float, 4> &white_balance,
                                                    const CancellationToken &cancellation);
[[nodiscard]] Result<WorkingImage> working_from_encoded_rgb8(const RasterBuffer &raster);
// The input is borrowed and never mutated. A successful result owns its pixels and
// retains the exact declared RGB working-profile state.
[[nodiscard]] Result<WorkingImage> apply_exposure(const WorkingImage &input,
                                                  const ExposureParams &params,
                                                  const CancellationToken &cancellation);
[[nodiscard]] Result<WorkingImage> apply_exposure(const WorkingImage &input,
                                                  const OperationInstance &operation,
                                                  const CancellationToken &cancellation);
// Frozen legacy colorbalance.c v4. The borrowed input is never mutated; successful
// publication owns its pixels/profile and preserves the immutable RAW analysis snapshot.
[[nodiscard]] Result<WorkingImage> apply_color_balance(const WorkingImage &input,
                                                       const ColorBalanceParams &params,
                                                       const CancellationToken &cancellation);
[[nodiscard]] Result<WorkingImage> apply_color_balance(const WorkingImage &input,
                                                       const OperationInstance &operation,
                                                       const CancellationToken &cancellation);
[[nodiscard]] Result<WorkingImage> apply_recipe_ops(WorkingImage image, const Recipe &recipe,
                                                    const CancellationToken &cancellation);
[[nodiscard]] Result<std::vector<std::uint8_t>> encode_png_bytes(const RenderedImage &image,
                                                                 bool fast = false);

} // namespace ravo
