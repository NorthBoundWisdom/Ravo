#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/error.h"

namespace ravo::detail
{

// This is engine-private S2.2 infrastructure.  It intentionally has no
// public include path or ABI: C14 is its first consumer, while tests use this
// header only through the engine source include directory.
enum class RecursiveGaussianCheckpoint : std::uint8_t
{
    kBeforeValidation,
    kBeforeAllocation,
    kVerticalColumn,
    kVerticalRow,
    kHorizontalRow,
    kHorizontalColumn,
    kBeforePublication,
};

using RecursiveGaussianCheckpointCallback = void (*)(void *context,
                                                     RecursiveGaussianCheckpoint checkpoint,
                                                     std::uint32_t progress) noexcept;

struct RecursiveGaussianControl
{
    void *context = nullptr;
    RecursiveGaussianCheckpointCallback checkpoint_callback = nullptr;
};

// Consumes a two-channel interleaved correction signal and returns an owned
// two-channel interleaved result.  The recurrence, its source-order
// expressions, and its +/- 1e9 per-read clamp reproduce the frozen
// DT_IOP_GAUSSIAN_ZERO helper used by colorharmonizer.
[[nodiscard]] Result<std::vector<float>>
recursive_gaussian_zero_2c(std::vector<float> signal, std::uint32_t width, std::uint32_t height,
                           float sigma, const CancellationToken &cancellation,
                           RecursiveGaussianControl control = {});

// Saturating workspace accounting for the mutable signal plus the recurrence
// scratch.  The caller owns any adjacent JCH/output buffers separately.
[[nodiscard]] std::uint64_t recursive_gaussian_zero_2c_bytes(std::uint32_t width,
                                                             std::uint32_t height) noexcept;

} // namespace ravo::detail
