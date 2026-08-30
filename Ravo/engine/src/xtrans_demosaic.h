#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "image_ops.h"

namespace ravo
{

enum class XTransDemosaicMode
{
    kMarkesteijn1,
    kMarkesteijn3,
};

inline constexpr std::string_view kXTransDemosaicModeMarkesteijn1 = "markesteijn1";
inline constexpr std::string_view kXTransDemosaicModeMarkesteijn3 = "markesteijn3";

[[nodiscard]] Result<XTransDemosaicMode> parse_xtrans_demosaic_mode(std::string_view mode);

// The decoded RAW is borrowed and never mutated. The returned image owns its
// camera-RGB pixels. Preview-size mosaics use bounded same-CFA area reduction;
// a full-size request consumes every source sample exactly once.
[[nodiscard]] Result<WorkingImage> demosaic_xtrans(const DecodedRaw &raw, std::uint32_t width,
                                                   std::uint32_t height,
                                                   const std::array<float, 4> &white_balance,
                                                   XTransDemosaicMode mode,
                                                   const CancellationToken &cancellation);

[[nodiscard]] std::uint64_t estimate_xtrans_demosaic_memory(std::uint32_t width,
                                                            std::uint32_t height,
                                                            XTransDemosaicMode mode) noexcept;

} // namespace ravo
