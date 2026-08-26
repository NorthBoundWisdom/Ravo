#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "ravo/engine/engine.h"
#include "raw_pipeline.h"

namespace ravo
{

using WorkingImage = LinearWorkingBuffer;

[[nodiscard]] Result<WorkingImage> working_from_raw(const DecodedRaw &raw, std::uint32_t width,
                                                    std::uint32_t height,
                                                    const std::array<float, 4> &white_balance,
                                                    const CancellationToken &cancellation);
[[nodiscard]] Result<WorkingImage> working_from_encoded_rgb8(const RasterBuffer &raster);
[[nodiscard]] Result<WorkingImage> apply_recipe_ops(WorkingImage image, const Recipe &recipe,
                                                    const CancellationToken &cancellation);
[[nodiscard]] Result<std::vector<std::uint8_t>> encode_png_bytes(const RenderedImage &image);

} // namespace ravo
