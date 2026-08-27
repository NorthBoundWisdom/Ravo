#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "ravo/domain/types.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/error.h"

namespace ravo
{

class RasterDecoder
{
public:
    virtual ~RasterDecoder() = default;

    [[nodiscard]] virtual Result<RasterInfo> probe(std::string_view path) const = 0;
    [[nodiscard]] virtual Result<DecodedRaster>
    decode(std::string_view path, std::uint32_t max_edge,
           const CancellationToken &cancellation) const = 0;
    [[nodiscard]] virtual Result<DecodedRaster>
    decode_memory(const std::vector<std::uint8_t> &encoded, std::uint32_t max_edge,
                  const CancellationToken &cancellation, int rotate_quarters = 0) const = 0;
    // Pixel, profile, format-option, and cancellation inputs are borrowed only
    // for this synchronous call. Success returns an owned encoded byte buffer.
    [[nodiscard]] virtual Result<std::vector<std::uint8_t>>
    encode(std::uint32_t width, std::uint32_t height, const std::vector<std::uint8_t> &rgb,
           const ColorProfileState &color_profile, ExportFormat format,
           const JpegExportOptions &jpeg_options, const CancellationToken &cancellation,
           const PngExportOptions &png_options = {}) const = 0;
    // Source compatibility for existing non-TIFF test doubles: default TIFF
    // options retain the prior encode path, while explicit TIFF options fail
    // closed until the concrete encoder overrides this overload.
    [[nodiscard]] virtual Result<std::vector<std::uint8_t>>
    encode(std::uint32_t width, std::uint32_t height, const std::vector<std::uint8_t> &rgb,
           const ColorProfileState &color_profile, ExportFormat format,
           const JpegExportOptions &jpeg_options, const CancellationToken &cancellation,
           const PngExportOptions &png_options, const TiffExportOptions &tiff_options) const
    {
        if (format == ExportFormat::kTiff && tiff_options != TiffExportOptions{})
        {
            return make_error(ErrorCode::kUnsupported,
                              "Raster encoder does not own explicit TIFF options",
                              {{"format", "tiff"}, {"reason", "unsupported_tiff_options_owner"}});
        }
        return encode(width, height, rgb, color_profile, format, jpeg_options, cancellation,
                      png_options);
    }
};

} // namespace ravo
