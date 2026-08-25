#pragma once

#include "ravo/domain/raster_decoder.h"

namespace ravo
{

class QtRasterDecoder final : public RasterDecoder
{
public:
    [[nodiscard]] Result<RasterInfo> probe(std::string_view path) const override;
    [[nodiscard]] Result<EncodedPng> decode(std::string_view path, std::uint32_t max_edge,
                                            const CancellationToken &cancellation) const override;
    [[nodiscard]] Result<EncodedPng>
    decode_memory(const std::vector<std::uint8_t> &encoded, std::uint32_t max_edge,
                  const CancellationToken &cancellation) const override;
    [[nodiscard]] Result<std::vector<std::uint8_t>>
    encode(std::uint32_t width, std::uint32_t height, const std::vector<std::uint8_t> &rgb,
           ExportFormat format, int jpeg_quality,
           const CancellationToken &cancellation) const override;
};

} // namespace ravo
