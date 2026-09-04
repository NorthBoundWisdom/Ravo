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

} // namespace ravo
