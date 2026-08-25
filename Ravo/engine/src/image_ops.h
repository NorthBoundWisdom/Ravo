#pragma once

#include <cstdint>
#include <vector>

#include "ravo/engine/engine.h"
#include "raw_pipeline.h"

namespace ravo
{

using WorkingImage = LinearWorkingBuffer;

[[nodiscard]] Result<WorkingImage> working_from_raw(const DecodedRaw &raw, std::uint32_t width,
                                                    std::uint32_t height,
                                                    const CancellationToken &cancellation);
[[nodiscard]] Result<WorkingImage> working_from_srgb8(const RasterBuffer &raster);
[[nodiscard]] Result<WorkingImage> apply_recipe_ops(WorkingImage image, const Recipe &recipe,
                                                    const CancellationToken &cancellation);
[[nodiscard]] RenderedImage encode_working_srgb(const WorkingImage &image);
[[nodiscard]] Result<std::vector<std::uint8_t>> encode_png_bytes(const RenderedImage &image);
[[nodiscard]] Result<RasterBuffer> decode_png_bytes(const std::vector<std::uint8_t> &bytes);

} // namespace ravo
