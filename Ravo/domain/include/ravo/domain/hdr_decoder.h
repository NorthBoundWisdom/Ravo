#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/error.h"

namespace ravo
{

enum class HdrPixelFormat : std::uint8_t
{
    kLinearRgbF32 = 0,
};

enum class HdrAlphaMode : std::uint8_t
{
    kOpaque = 0,
};

struct RadianceRgbeMetadata
{
    std::string program_type;
    float gamma = 1.0F;
    float exposure = 1.0F;
    bool has_gamma = false;
    bool has_exposure = false;
    bool has_custom_primaries = false;
    std::array<float, 8> primaries_xy{0.640F, 0.330F, 0.290F, 0.600F,
                                      0.150F, 0.060F, 0.333F, 0.333F};
    // The legacy owner transposed its row-primary matrix before inversion.
    // Keep both directions explicit so this non-D50 Radiance state cannot be
    // confused with ColorProfileState::matrix_to_xyz_d50.
    std::array<float, 9> rgb_to_xyz{};
    std::array<float, 9> xyz_to_rgb{};

    [[nodiscard]] bool operator==(const RadianceRgbeMetadata &) const noexcept = default;
};

struct DecodedHdrRaster
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<float> rgb;
    RadianceRgbeMetadata radiance;
    HdrPixelFormat pixel_format = HdrPixelFormat::kLinearRgbF32;
    HdrAlphaMode alpha_mode = HdrAlphaMode::kOpaque;
};

class HdrDecoder
{
public:
    virtual ~HdrDecoder() = default;

    // Both inputs are borrowed only for the duration of their synchronous
    // call. Implementations must not retain the path view or encoded bytes.
    [[nodiscard]] virtual Result<DecodedHdrRaster>
    decode(std::string_view path, const CancellationToken &cancellation) const = 0;
    [[nodiscard]] virtual Result<DecodedHdrRaster>
    decode_memory(const std::vector<std::uint8_t> &encoded,
                  const CancellationToken &cancellation) const = 0;
};

} // namespace ravo
