#pragma once

#include <vector>

#include "mask_evaluator.h"
#include "ravo/recipe/mask.h"

namespace ravo
{

[[nodiscard]] Result<std::vector<float>> evaluate_path_mask_alpha(const PathMask &path,
                                                                  const MaskEvaluationRequest &request);

[[nodiscard]] Result<std::vector<float>>
evaluate_brush_mask_alpha(const BrushMask &brush, const MaskEvaluationRequest &request);

} // namespace ravo
