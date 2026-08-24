#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ravo/engine/engine.h"

namespace ravo
{

struct DecodedRaw
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t cfa_width = 0;
    std::uint32_t cfa_height = 0;
    std::int32_t black_level = 0;
    std::uint32_t white_level = 65535;
    std::string make;
    std::string model;
    std::array<float, 3> white_balance{1.0F, 1.0F, 1.0F};
    std::array<float, 9> camera_to_srgb{1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F};
    std::vector<std::uint8_t> cfa_channels;
    std::vector<std::uint16_t> pixels;
};

struct RenderedImage
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgb;
};

[[nodiscard]] Result<InspectionResult> identify_raw(std::string_view input_uri);
[[nodiscard]] Result<EmbeddedPreview>
extract_libraw_preview(std::string_view input_uri, const CancellationToken &cancellation);
[[nodiscard]] Result<DecodedRaw> decode_raw(std::string_view input_uri);
[[nodiscard]] Result<RenderedImage> render_raw(const DecodedRaw &raw, const RenderRequest &request);
[[nodiscard]] Result<void> write_png_atomically(std::string_view output_uri,
                                                const RenderedImage &image);

} // namespace ravo
