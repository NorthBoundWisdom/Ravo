#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/error.h"
#include "ravo/recipe/mask.h"

namespace ravo
{

// This is engine-private input. `samples[0]` is the first RGB channel at the
// requested ROI's top-left pixel, not necessarily the full frame's origin.
// Its lifetime is the evaluator call; stride is expressed in float samples,
// not pixels. Callers evaluating a sub-ROI must select the corresponding
// source subspan while retaining the source plane's row stride.
struct MaskRgbPlaneView
{
    std::span<const float> samples;
    std::uint32_t row_stride_samples = 0;
};

struct MaskEvaluationRequest
{
    std::uint32_t full_width = 0;
    std::uint32_t full_height = 0;
    std::uint32_t roi_x = 0;
    std::uint32_t roi_y = 0;
    std::uint32_t roi_width = 0;
    std::uint32_t roi_height = 0;
    MaskRgbPlaneView input;
    // Required only by an operation-output parametric node.  The caller
    // deliberately decides whether an operation output exists; there is no
    // fallback to input samples.
    std::optional<MaskRgbPlaneView> operation_output;
    CancellationToken cancellation;
};

struct AlphaPlane
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<float> alpha;
};

struct MaskEvaluatorMemoryEstimate
{
    std::uint64_t alpha_plane_bytes = 0;
    // The evaluator is depth-first: this is the peak of current group
    // accumulators/child planes, excluding the returned alpha plane.
    std::uint64_t evaluator_scratch_bytes = 0;
};

[[nodiscard]] Result<AlphaPlane> evaluate_canonical_mask(const std::vector<Mask> &masks,
                                                         std::string_view root_mask_id,
                                                         const MaskEvaluationRequest &request);

// Writes only the owned operation output plane.  Exact 0 and 1 alpha values
// select the source/output samples verbatim so all/identity paths retain their
// expected bit behavior.
[[nodiscard]] Result<void> normal_mask_mix(std::span<const float> input_rgb,
                                           std::span<float> operation_output_rgb,
                                           const AlphaPlane &alpha,
                                           const CancellationToken &cancellation);

// Valid graphs produce a no-throw, saturating peak estimate.  Unknown roots
// return zero; validation is the owner of structured graph diagnostics.
[[nodiscard]] MaskEvaluatorMemoryEstimate
estimate_mask_evaluator_memory(const std::vector<Mask> &masks, std::string_view root_mask_id,
                               std::uint32_t width, std::uint32_t height) noexcept;

namespace detail
{

// Engine-source test seam. It is passed by value into one evaluator call and
// never becomes scheduler or global state.
enum class MaskEvaluatorCheckpoint : std::uint8_t
{
    kBeforeAllocation,
    kBeforeNode,
    kEvaluateRow,
};

using MaskEvaluatorCheckpointCallback = void (*)(void *context, MaskEvaluatorCheckpoint checkpoint,
                                                 std::uint32_t progress) noexcept;

struct MaskEvaluatorControl
{
    void *context = nullptr;
    MaskEvaluatorCheckpointCallback checkpoint_callback = nullptr;
};

[[nodiscard]] Result<AlphaPlane>
evaluate_canonical_mask_controlled(const std::vector<Mask> &masks, std::string_view root_mask_id,
                                   const MaskEvaluationRequest &request,
                                   MaskEvaluatorControl control);

} // namespace detail

} // namespace ravo
