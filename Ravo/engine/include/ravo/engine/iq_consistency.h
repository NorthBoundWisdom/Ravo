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
inline constexpr std::int64_t kIqConsistencySchemaVersion = 3;

// Per-channel absolute tolerance for admitted interactive GPU RGB batches
// versus CPU gold linear working (Exposure / light / Lab USM / display tone).
inline constexpr float kIqGpuCpuWorkingAbsTolerance = 2.0e-3F;

// Interactive Metal GPU edit-math stages admitted under the packed-RGB8
// contract on macOS Debug/Release. Non-listed ops stay on the interactive
// CPU hybrid (not a silent algorithm downgrade; persist/export remain CPU
// gold). Masked instances, non-linear-Rec709 sharpen, and non-linear-sRGB
// sigmoid working spaces are also CPU on the interactive path.
inline constexpr std::string_view kIqGpuAdmittedInteractiveStages[] = {
    "ravo.core.exposure",
    "ravo.core.highlights",
    "ravo.core.shadows",
    "ravo.core.whites",
    "ravo.core.blacks",
    "ravo.detail.sharpen",
    "ravo.display.sigmoid",
    "ravo.core.rapidraw-tone-controls",
    "ravo.display.rapidraw-basic",
};

// Machine-visible residual after admitted-stage packed contracts: full
// corpus / Win-Linux / proof-monitor / multi-instance GPU remain open; RAW
// ROI vs export owned packed-delta + same-scale scaled-export compare are
// documented below. Non-admitted interactive ops (e.g. contrast) run CPU.
inline constexpr std::string_view kIqGpuInteractiveNonAdmittedPolicy =
    "non_admitted_ops_run_cpu_on_interactive_hybrid; masked_ops_cpu; "
    "non_linear_rec709_sharpen_cpu; non_linear_srgb_sigmoid_working_space_cpu; "
    "gpu_batch_fail_closes_on_pipeline_error";

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

// Max observed abs channel delta helper for packed RGB8 diagnostics.
[[nodiscard]] inline int max_packed_rgb8_abs_delta(const std::vector<std::uint8_t> &left,
                                                   const std::vector<std::uint8_t> &right) noexcept
{
    if (left.size() != right.size())
        return -1;
    int max_delta = 0;
    for (std::size_t index = 0; index < left.size(); ++index)
    {
        const int delta = std::abs(static_cast<int>(left[index]) - static_cast<int>(right[index]));
        if (delta > max_delta)
            max_delta = delta;
    }
    return max_delta;
}

// Host evidence scope for IQ-00 contract probes on this tree (honest residual).
inline constexpr std::string_view kIqConsistencyHostScope = "macos_debug_release_contract";
// Admitted interactive stages carry packed-RGB8 abs-delta contracts on macOS
// Metal; RAW viewport ROI may still report GPU; persist/export/reopen stay CPU.
inline constexpr std::string_view kIqConsistencyGpuLiveResidual =
    "admitted_interactive_develop_stages_packed_delta_contracted_macos_metal; "
    "raw_viewport_roi_may_use_gpu; non_admitted_interactive_ops_cpu_hybrid; "
    "persist_export_reopen_cpu_gold; win_linux_and_full_corpus_open";

// RAW viewport ROI versus CPU-gold export (macOS contract probe):
// full-resolution export crop size-matches the owned ROI window; packed RGB8 is
// compared as owned pixels. RCD window demosaic aligns to the full-frame tile
// grid on CPU; ROI processing expands by a spatial apron (sharpen / RapidRAW
// tone radius) then crops to owned pixels. Size-matched packed channels stay
// within interactive abs-delta (±1) on the macOS contract probe (observed).
// Metal ROI display publish crops the same owned window after the apron so
// gpu_display_* matches owned packed size when admitted stages publish.
// Scaled max_edge export is compared fairly at the same scale: crop the
// settled preview and the scaled export with the same norm rect (owned packed
// bit-exact when dims match). Cross-scale 1:1 ROI vs scaled-export crop is an
// explicit non-compare probe (dims mismatch; no resample that weakens CPU-gold).
// RAW ROI may report GPU for admitted edit stages. Win/Linux / full corpus /
// Bayer-RCD matrix stay open.
inline constexpr std::string_view kIqRawRoiVersusExportResidual =
    "full_export_crop_size_matched_macos_contract; "
    "owned_packed_rgb8_within_interactive_abs_delta_rcd_tile_aligned_cpu_demosaic_spatial_apron; "
    "gpu_native_roi_apron_owned_surface_publish_macos_metal; "
    "scaled_export_same_scale_settled_preview_crop_packed_bit_exact; "
    "cross_scale_roi_vs_scaled_export_explicit_non_compare; "
    "raw_viewport_roi_may_report_gpu; "
    "persist_export_reopen_cpu_gold; win_linux_and_full_corpus_open";

} // namespace ravo
