#include "image_ops.h"

#include <limits>
#include <new>

#include "mask_evaluator.h"
#include "ravo/recipe/local_adjustment.h"

namespace ravo
{
Result<WorkingImage> apply_local_adjustment(WorkingImage image, const Recipe &recipe,
                                            const OperationInstance &operation,
                                            const CancellationToken &cancellation,
                                            AlphaPlane *const alpha_output)
try
{
    auto active = cancellation.check();
    if (!active)
        return active.error();
    if (!operation.mask_id || image.width == 0 || image.height == 0 ||
        image.width > std::numeric_limits<std::uint32_t>::max() / 3U ||
        operation.children.size() > kLocalAdjustmentMaxOperations)
        return make_error(ErrorCode::kValidation, "Invalid local adjustment input",
                          {{"reason", "invalid_local_adjustment_input"}});
    for (const auto &child : operation.children)
        if (!local_adjustment_operation_allowed(child.id) || child.mask_id ||
            !child.children.empty())
            return make_error(
                ErrorCode::kUnsupported, "Unsupported local adjustment child",
                {{"reason", "unsupported_local_adjustment_operation"}, {"operation_id", child.id}});
    Recipe nested;
    nested.operations = operation.children;
    auto adjusted = apply_recipe_ops(image, nested, cancellation, true);
    if (!adjusted)
        return adjusted.error();
    if (image.width != adjusted.value().width || image.height != adjusted.value().height ||
        image.rgb.size() != adjusted.value().rgb.size() ||
        image.rgb.size() != static_cast<std::uint64_t>(image.width) * image.height * 3U)
        return make_error(ErrorCode::kValidation, "Local adjustment changed its pixel frame",
                          {{"reason", "local_adjustment_frame_changed"}});
    const auto stride = image.width * 3U;
    MaskEvaluationRequest request{
        .full_width = image.width,
        .full_height = image.height,
        .roi_x = 0,
        .roi_y = 0,
        .roi_width = image.width,
        .roi_height = image.height,
        .input = MaskRgbPlaneView{image.rgb, stride},
        .operation_output = MaskRgbPlaneView{adjusted.value().rgb, stride},
        .attached_frame = image.mask_attached_frame,
        .cancellation = cancellation,
    };
    auto alpha = evaluate_canonical_mask(recipe.masks, *operation.mask_id, request);
    if (!alpha)
        return alpha.error();
    auto mixed = normal_mask_mix(image.rgb, adjusted.value().rgb, alpha.value(), cancellation);
    if (!mixed)
        return mixed.error();
    if (alpha_output)
        *alpha_output = std::move(alpha).value();
    return adjusted;
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Local adjustment allocation failed",
                      {{"reason", "allocation_failed"}});
}
} // namespace ravo
