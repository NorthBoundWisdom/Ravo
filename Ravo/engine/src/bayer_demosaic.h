#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "image_ops.h"

namespace ravo
{

class GpuAdapter;

enum class BayerDemosaicMode
{
    kRcd,
    kPpg,
};

inline constexpr std::string_view kBayerDemosaicModeRcd = "rcd";
inline constexpr std::string_view kBayerDemosaicModePpg = "ppg";

[[nodiscard]] Result<BayerDemosaicMode> parse_bayer_demosaic_mode(std::string_view mode);

// The decoded RAW is borrowed and never mutated. The returned image owns its
// camera-RGB pixels. Preview-size mosaics use bounded same-CFA area reduction;
// a full-size request consumes every source sample exactly once.
[[nodiscard]] Result<WorkingImage> demosaic_bayer(const DecodedRaw &raw, std::uint32_t width,
                                                  std::uint32_t height,
                                                  const std::array<float, 4> &white_balance,
                                                  BayerDemosaicMode mode,
                                                  const CancellationToken &cancellation);

// 1:1 CFA window. `origin` is in unrotated sensor pixels. The decoded RAW is
// borrowed. The result owns camera-RGB pixels of exactly width x height.
// RCD expands onto the absolute full-frame tile grid (+ apron) so owned pixels
// match full-frame demosaic then crop; PPG uses a fixed apron only.
[[nodiscard]] Result<WorkingImage>
demosaic_bayer_window(const DecodedRaw &raw, std::uint32_t origin_x, std::uint32_t origin_y,
                      std::uint32_t width, std::uint32_t height,
                      const std::array<float, 4> &white_balance, BayerDemosaicMode mode,
                      const CancellationToken &cancellation, const GpuAdapter *gpu = nullptr);

[[nodiscard]] std::uint64_t estimate_bayer_demosaic_memory(std::uint32_t width,
                                                           std::uint32_t height,
                                                           BayerDemosaicMode mode) noexcept;

} // namespace ravo
