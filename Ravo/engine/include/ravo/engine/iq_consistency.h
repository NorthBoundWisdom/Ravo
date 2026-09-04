#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ravo/engine/engine.h"
#include "ravo/foundation/error.h"

namespace ravo
{

// ADR-0151: IQ-00 CPU gold / GPU consistency contract (first Ready).
inline constexpr std::string_view kIqConsistencyContractVersion = "ravo.iq.consistency/v1";
inline constexpr std::int64_t kIqConsistencySchemaVersion = 1;

// Per-channel absolute tolerance for admitted interactive GPU RGB batches
// versus CPU gold linear working (Exposure / light / Lab USM / Sigmoid).
inline constexpr float kIqGpuCpuWorkingAbsTolerance = 2.0e-3F;

[[nodiscard]] inline bool is_cpu_gold_backend(const std::string_view gpu_backend) noexcept
{
    return gpu_backend.empty();
}

[[nodiscard]] inline Result<void> require_cpu_gold_backend(const std::string_view gpu_backend,
                                                           const std::string_view path)
{
    if (is_cpu_gold_backend(gpu_backend))
        return {};
    return make_error(ErrorCode::kValidation, "Persist/export path left the CPU gold backend",
                      {{"reason", "iq_cpu_gold_backend_required"},
                       {"path", std::string(path)},
                       {"gpu_backend", std::string(gpu_backend)}});
}

[[nodiscard]] inline bool rgb8_buffers_equal(const std::vector<std::uint8_t> &left,
                                             const std::vector<std::uint8_t> &right) noexcept
{
    return left == right;
}

[[nodiscard]] inline bool working_rgb_within_abs_tolerance(const std::vector<float> &left,
                                                           const std::vector<float> &right,
                                                           const float abs_tolerance) noexcept
{
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index)
    {
        if (!(std::fabs(left[index] - right[index]) <= abs_tolerance))
            return false;
    }
    return true;
}

// Max abs channel delta on packed RGB8 for interactive GPU (or GPU ROI live)
// versus CPU-gold export/persist when sizes match. One code unit beyond bit-
// exact; used by IQ-00 macOS contract probes, not a Win/Linux claim.
inline constexpr int kIqGpuCpuPackedRgb8AbsDelta = 1;

[[nodiscard]] inline bool packed_rgb8_within_abs_delta(const std::vector<std::uint8_t> &left,
                                                       const std::vector<std::uint8_t> &right,
                                                       const int abs_delta) noexcept
{
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index)
    {
        const int delta = static_cast<int>(left[index]) - static_cast<int>(right[index]);
        if (std::abs(delta) > abs_delta)
            return false;
    }
    return true;
}

[[nodiscard]] inline Result<std::vector<std::uint8_t>>
crop_packed_rgb8(const std::vector<std::uint8_t> &rgb, const std::uint32_t width,
                 const std::uint32_t height, const std::uint32_t x, const std::uint32_t y,
                 const std::uint32_t crop_w, const std::uint32_t crop_h)
{
    if (width == 0U || height == 0U || crop_w == 0U || crop_h == 0U || x + crop_w > width ||
        y + crop_h > height || rgb.size() < static_cast<std::size_t>(width) * height * 3U)
    {
        return make_error(ErrorCode::kInvalidArgument, "Packed RGB8 crop is out of bounds",
                          {{"reason", "iq_rgb8_crop_out_of_bounds"}});
    }
    std::vector<std::uint8_t> out;
    out.resize(static_cast<std::size_t>(crop_w) * crop_h * 3U);
    for (std::uint32_t row = 0; row < crop_h; ++row)
    {
        const auto src = (static_cast<std::size_t>(y + row) * width + x) * 3U;
        const auto dst = static_cast<std::size_t>(row) * crop_w * 3U;
        for (std::uint32_t col = 0; col < crop_w * 3U; ++col)
            out[dst + col] = rgb[src + col];
    }
    return out;
}

// Host evidence scope for IQ-00 contract probes on this tree (honest residual).
inline constexpr std::string_view kIqConsistencyHostScope = "macos_debug_release_contract";
inline constexpr std::string_view kIqConsistencyGpuLiveResidual =
    "interactive_develop_preview_and_raw_roi_may_use_gpu; persist_export_reopen_cpu_gold";

} // namespace ravo
