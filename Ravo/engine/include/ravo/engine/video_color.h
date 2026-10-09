#pragma once
#include <span>
#include <string_view>
#include <vector>
#include "ravo/foundation/cancellation.h"
namespace ravo
{
// CPU reference: source nonlinear RGB -> SDR sRGB. PQ uses absolute luminance;
// HLG uses the declared nominal/mastering peak. Output white is 100 cd/m².
[[nodiscard]] Result<std::vector<std::uint8_t>>
video_rgb_to_sdr(std::span<const float> rgb, std::string_view transfer, std::string_view primaries,
                 double peak_nits, const CancellationToken &cancellation = {});
} // namespace ravo
