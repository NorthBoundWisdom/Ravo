#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "gpu_adapter.h"
#include "ravo/engine/engine.h"
#include "ravo/recipe/recipe.h"

namespace ravo
{

[[nodiscard]] Result<std::optional<std::vector<GpuRgbPass>>>
gpu_preview_rgb_passes(const LinearWorkingBuffer &working, const Recipe &recipe,
                       const CancellationToken &cancellation);
[[nodiscard]] Result<LinearWorkingBuffer>
apply_gpu_preview_rgb(const LinearWorkingBuffer &working, std::span<const GpuRgbPass> passes,
                      const GpuAdapter &gpu, const CancellationToken &cancellation,
                      GpuRgbApplyOptions options = {});
[[nodiscard]] Result<LinearWorkingBuffer>
apply_preview_rgb(LinearWorkingBuffer working, const Recipe &recipe, const GpuAdapter *gpu,
                  std::string *gpu_backend, const CancellationToken &cancellation,
                  bool need_cpu_pixels = true, std::uint32_t display_slot = 0,
                  bool prefer_retained_source = false);

} // namespace ravo
