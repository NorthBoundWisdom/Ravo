#pragma once

#include "ravo/domain/raster_decoder.h"

namespace ravo
{

class QtRasterDecoder final : public RasterDecoder
{
public:
    [[nodiscard]] Result<RasterInfo> probe(std::string_view path) const override;
    [[nodiscard]] Result<DecodedRaster>
    decode(std::string_view path, std::uint32_t max_edge,
           const CancellationToken &cancellation) const override;
    [[nodiscard]] Result<DecodedRaster> decode_memory(const std::vector<std::uint8_t> &encoded,
                                                      std::uint32_t max_edge,
                                                      const CancellationToken &cancellation,
                                                      int rotate_quarters = 0) const override;
    [[nodiscard]] Result<std::vector<std::uint8_t>>
    encode(std::uint32_t width, std::uint32_t height, const std::vector<std::uint8_t> &rgb,
           const ColorProfileState &color_profile, ExportFormat format,
           const JpegExportOptions &jpeg_options, const CancellationToken &cancellation,
           const PngExportOptions &png_options = {}) const override;
    [[nodiscard]] Result<std::vector<std::uint8_t>>
    encode(std::uint32_t width, std::uint32_t height, const std::vector<std::uint8_t> &rgb,
           const ColorProfileState &color_profile, ExportFormat format,
           const JpegExportOptions &jpeg_options, const CancellationToken &cancellation,
           const PngExportOptions &png_options,
           const TiffExportOptions &tiff_options) const override;
};

} // namespace ravo
