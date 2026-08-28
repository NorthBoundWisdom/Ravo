#include "mask_evaluator.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <new>
#include <string>
#include <utility>

namespace ravo
{
namespace
{

constexpr float kPi = 3.14159265358979323846F;

[[nodiscard]] std::uint64_t saturating_add(const std::uint64_t left,
                                           const std::uint64_t right) noexcept
{
    return left > std::numeric_limits<std::uint64_t>::max() - right ?
               std::numeric_limits<std::uint64_t>::max() :
               left + right;
}

[[nodiscard]] std::uint64_t saturating_multiply(const std::uint64_t left,
                                                const std::uint64_t right) noexcept
{
    if (left == 0U || right == 0U)
    {
        return 0U;
    }
    return left > std::numeric_limits<std::uint64_t>::max() / right ?
               std::numeric_limits<std::uint64_t>::max() :
               left * right;
}

[[nodiscard]] TaskError evaluator_error(const std::string_view message,
                                        const std::string_view reason,
                                        const std::string_view mask_id = {})
{
    std::map<std::string, std::string, std::less<>> context{{"reason", std::string(reason)}};
    if (!mask_id.empty())
    {
        context.emplace("mask_id", std::string(mask_id));
    }
    return make_error(ErrorCode::kValidation, std::string(message), std::move(context));
}

[[nodiscard]] Result<std::size_t> pixel_count(const MaskEvaluationRequest &request)
{
    if (request.full_width == 0U || request.full_height == 0U || request.roi_width == 0U ||
        request.roi_height == 0U)
    {
        return evaluator_error("Mask evaluator dimensions must be non-zero", "invalid_mask_roi");
    }
    const std::uint64_t right = static_cast<std::uint64_t>(request.roi_x) + request.roi_width;
    const std::uint64_t bottom = static_cast<std::uint64_t>(request.roi_y) + request.roi_height;
    if (right > request.full_width || bottom > request.full_height)
    {
        return evaluator_error("Mask ROI is outside the attached operation input frame",
                               "invalid_mask_roi");
    }
    const std::uint64_t pixels = static_cast<std::uint64_t>(request.roi_width) * request.roi_height;
    if (pixels > std::numeric_limits<std::size_t>::max() / sizeof(float))
    {
        return evaluator_error("Mask ROI exceeds the evaluator allocation limit",
                               "mask_roi_overflow");
    }
    return static_cast<std::size_t>(pixels);
}

[[nodiscard]] Result<void> validate_plane(const MaskRgbPlaneView plane,
                                          const MaskEvaluationRequest &request,
                                          const std::string_view name)
{
    const std::uint64_t minimum_stride = static_cast<std::uint64_t>(request.roi_width) * 3U;
    if (plane.row_stride_samples < minimum_stride)
    {
        return evaluator_error("Mask RGB row stride is too small", "invalid_mask_stride");
    }
    const std::uint64_t required =
        static_cast<std::uint64_t>(request.roi_height - 1U) * plane.row_stride_samples +
        minimum_stride;
    if (required > std::numeric_limits<std::size_t>::max() || plane.samples.size() < required)
    {
        return evaluator_error("Mask RGB samples do not cover the requested ROI",
                               "invalid_mask_samples");
    }
    for (std::uint32_t row = 0; row < request.roi_height; ++row)
    {
        auto cancelled = request.cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
        const std::size_t begin = static_cast<std::size_t>(row) * plane.row_stride_samples;
        const std::size_t end = begin + static_cast<std::size_t>(request.roi_width) * 3U;
        for (std::size_t index = begin; index < end; ++index)
        {
            if (!std::isfinite(plane.samples[index]))
            {
                return make_error(ErrorCode::kValidation, "Mask RGB input contains NaN or infinity",
                                  {{"reason", "non_finite_mask_samples"},
                                   {"plane", std::string(name)},
                                   {"sample_index", std::to_string(index)}});
            }
        }
    }
    return {};
}

[[nodiscard]] float unit_math(const float value) noexcept
{
    return std::clamp(value, 0.0F, 1.0F);
}

[[nodiscard]] Result<void> apply_common(std::vector<float> &alpha, const MaskCommon &common,
                                        const MaskEvaluationRequest &request,
                                        const std::string_view mask_id)
{
    for (std::uint32_t row = 0; row < request.roi_height; ++row)
    {
        auto cancelled = request.cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
        const std::size_t begin = static_cast<std::size_t>(row) * request.roi_width;
        const std::size_t end = begin + request.roi_width;
        for (std::size_t index = begin; index < end; ++index)
        {
            const float source = alpha[index];
            const float inverted = common.inverted ? 1.0F - source : source;
            const float value = static_cast<float>(common.opacity) * inverted;
            if (!std::isfinite(value))
            {
                return evaluator_error("Mask math produced a non-finite alpha",
                                       "non_finite_mask_output", mask_id);
            }
            alpha[index] = unit_math(value);
        }
    }
    return {};
}

[[nodiscard]] float parametric_factor(const float value,
                                      const std::array<double, 4> &thresholds) noexcept
{
    // The branch/order is the frozen `_blendif_compute_factor` contract.  Equal
    // adjacent keys deliberately fall through to a hard edge rather than a
    // repaired denominator.
    const float lower0 = static_cast<float>(thresholds[0]);
    const float lower1 = static_cast<float>(thresholds[1]);
    const float upper0 = static_cast<float>(thresholds[2]);
    const float upper1 = static_cast<float>(thresholds[3]);
    const float lower_slope = 1.0F / std::fmax(0.001F, lower1 - lower0);
    const float upper_slope = 1.0F / std::fmax(0.001F, upper1 - upper0);
    if (value <= lower0)
    {
        return 0.0F;
    }
    if (value < lower1)
    {
        return (value - lower0) * lower_slope;
    }
    if (value <= upper0)
    {
        return 1.0F;
    }
    if (value < upper1)
    {
        return 1.0F - (value - upper0) * upper_slope;
    }
    return 0.0F;
}

[[nodiscard]] float read_parametric_channel(const MaskRgbPlaneView plane, const std::uint32_t row,
                                            const std::uint32_t column,
                                            const ParametricMaskChannel channel) noexcept
{
    const std::size_t index = static_cast<std::size_t>(row) * plane.row_stride_samples +
                              static_cast<std::size_t>(column) * 3U;
    const float red = plane.samples[index];
    const float green = plane.samples[index + 1U];
    const float blue = plane.samples[index + 2U];
    switch (channel)
    {
    case ParametricMaskChannel::kLuminance:
        // The frozen RGB branch asks its declared profile for luminance. The
        // canonical evaluator runs after the engine's explicit linear-Rec709
        // bridge, whose fixed XYZ-D50 matrix Y row is reproduced here.
        return 0.2225045F * red + 0.7168786F * green + 0.0606169F * blue;
    case ParametricMaskChannel::kRed:
        return red;
    case ParametricMaskChannel::kGreen:
        return green;
    case ParametricMaskChannel::kBlue:
        return blue;
    }
    return red;
}

[[nodiscard]] std::size_t find_mask_index(const std::vector<Mask> &masks,
                                          const std::string_view id) noexcept
{
    for (std::size_t index = 0; index < masks.size(); ++index)
    {
        if (masks[index].id == id)
        {
            return index;
        }
    }
    return masks.size();
}

[[nodiscard]] bool root_needs_operation_output(const std::vector<Mask> &masks,
                                               const std::size_t root) noexcept
{
    const auto needs = [&](auto &&self, const std::size_t index) noexcept -> bool
    {
        if (const auto *parametric = std::get_if<ParametricMask>(&masks[index].payload);
            parametric != nullptr)
        {
            return parametric->source == ParametricMaskSource::kOperationOutput;
        }
        const auto *group = std::get_if<MaskGroup>(&masks[index].payload);
        if (group == nullptr)
        {
            return false;
        }
        for (const auto &child : group->children)
        {
            const std::size_t child_index = find_mask_index(masks, child.mask_id);
            if (child_index != masks.size() && self(self, child_index))
            {
                return true;
            }
        }
        return false;
    };
    return needs(needs, root);
}

[[nodiscard]] Result<void> apply_group_edge(std::vector<float> &child, const MaskGroupChild &edge,
                                            const MaskEvaluationRequest &request,
                                            const std::string_view group_id)
{
    for (std::uint32_t row = 0; row < request.roi_height; ++row)
    {
        auto cancelled = request.cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
        const std::size_t begin = static_cast<std::size_t>(row) * request.roi_width;
        const std::size_t end = begin + request.roi_width;
        for (std::size_t index = begin; index < end; ++index)
        {
            const float input = child[index];
            const float inverted = edge.inverted ? 1.0F - input : input;
            const float value = static_cast<float>(edge.opacity) * inverted;
            if (!std::isfinite(value))
            {
                return evaluator_error("Mask group edge produced a non-finite alpha",
                                       "non_finite_mask_output", group_id);
            }
            child[index] = unit_math(value);
        }
    }
    return {};
}

[[nodiscard]] Result<void> compose_group(std::vector<float> &left, const std::vector<float> &right,
                                         const MaskGroupOperator operation,
                                         const MaskEvaluationRequest &request,
                                         const std::string_view group_id)
{
    for (std::uint32_t row = 0; row < request.roi_height; ++row)
    {
        auto cancelled = request.cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
        const std::size_t begin = static_cast<std::size_t>(row) * request.roi_width;
        const std::size_t end = begin + request.roi_width;
        for (std::size_t index = begin; index < end; ++index)
        {
            const float lhs = left[index];
            const float rhs = right[index];
            float combined = 0.0F;
            switch (operation)
            {
            case MaskGroupOperator::kReplace:
                return evaluator_error("Only the first group child may replace the accumulator",
                                       "invalid_mask_group_order", group_id);
            case MaskGroupOperator::kUnion:
                combined = std::max(lhs, rhs);
                break;
            case MaskGroupOperator::kIntersection:
                combined = lhs > 0.0F && rhs > 0.0F ? std::min(lhs, rhs) : 0.0F;
                break;
            case MaskGroupOperator::kDifference:
                // Frozen group ROI helper: outside an overlap the accumulator
                // survives unchanged; within it use lhs * (1 - rhs).
                combined = lhs > 0.0F && rhs > 0.0F ? lhs * (1.0F - rhs) : lhs;
                break;
            case MaskGroupOperator::kExclusion:
                // Frozen group ROI helper, including the non-overlap max path.
                combined = lhs > 0.0F && rhs > 0.0F ?
                               std::max((1.0F - lhs) * rhs, lhs * (1.0F - rhs)) :
                               std::max(lhs, rhs);
                break;
            }
            if (!std::isfinite(combined))
            {
                return evaluator_error("Mask group produced a non-finite alpha",
                                       "non_finite_mask_output", group_id);
            }
            left[index] = unit_math(combined);
        }
    }
    return {};
}

} // namespace

Result<AlphaPlane> detail::evaluate_canonical_mask_controlled(
    const std::vector<Mask> &masks, const std::string_view root_mask_id,
    const MaskEvaluationRequest &request, const detail::MaskEvaluatorControl control)
try
{
    auto cancelled = request.cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    auto valid_graph = validate_mask_graph(masks);
    if (!valid_graph)
    {
        return valid_graph.error();
    }
    const std::size_t root = find_mask_index(masks, root_mask_id);
    if (root == masks.size())
    {
        return make_error(
            ErrorCode::kNotFound, "Mask root is not present in the canonical graph",
            {{"reason", "unknown_mask_root"}, {"mask_id", std::string(root_mask_id)}});
    }
    auto count = pixel_count(request);
    if (!count)
    {
        return count.error();
    }
    auto input = validate_plane(request.input, request, "input");
    if (!input)
    {
        return input.error();
    }
    if (request.operation_output.has_value())
    {
        auto output = validate_plane(*request.operation_output, request, "operation_output");
        if (!output)
        {
            return output.error();
        }
    }
    if (root_needs_operation_output(masks, root) && !request.operation_output.has_value())
    {
        return make_error(ErrorCode::kUnsupported,
                          "Parametric operation-output mask source is unavailable",
                          {{"reason", "mask_parametric_operation_output_unavailable"},
                           {"mask_id", std::string(root_mask_id)}});
    }
    cancelled = request.cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    const auto checkpoint = [&](const detail::MaskEvaluatorCheckpoint point,
                                const std::uint32_t progress) -> Result<void>
    {
        if (control.checkpoint_callback != nullptr)
        {
            control.checkpoint_callback(control.context, point, progress);
        }
        return request.cancellation.check();
    };
    auto before_allocation = checkpoint(detail::MaskEvaluatorCheckpoint::kBeforeAllocation, 0U);
    if (!before_allocation)
    {
        return before_allocation.error();
    }

    std::function<Result<std::vector<float>>(std::size_t)> evaluate;
    evaluate = [&](const std::size_t index) -> Result<std::vector<float>>
    {
        auto active = checkpoint(detail::MaskEvaluatorCheckpoint::kBeforeNode,
                                 static_cast<std::uint32_t>(index));
        if (!active)
        {
            return active.error();
        }
        const Mask &mask = masks[index];
        std::vector<float> result;
        if (const auto *group = std::get_if<MaskGroup>(&mask.payload); group != nullptr)
        {
            bool first = true;
            for (const auto &child_edge : group->children)
            {
                const std::size_t child_index = find_mask_index(masks, child_edge.mask_id);
                // Graph validation established this already. Keep a fail-closed
                // guard here because this private owner may be called directly.
                if (child_index == masks.size())
                {
                    return make_error(ErrorCode::kValidation,
                                      "Mask group references a missing mask",
                                      {{"reason", "mask_graph_dangling_reference"},
                                       {"mask_id", mask.id},
                                       {"referenced_mask_id", child_edge.mask_id}});
                }
                auto child = evaluate(child_index);
                if (!child)
                {
                    return child.error();
                }
                auto edge = apply_group_edge(child.value(), child_edge, request, mask.id);
                if (!edge)
                {
                    return edge.error();
                }
                if (first)
                {
                    result = std::move(child).value();
                    first = false;
                }
                else
                {
                    auto combined = compose_group(result, child.value(), child_edge.operation,
                                                  request, mask.id);
                    if (!combined)
                    {
                        return combined.error();
                    }
                }
            }
        }
        else
        {
            active = request.cancellation.check();
            if (!active)
            {
                return active.error();
            }
            result.assign(count.value(), 0.0F);
            for (std::uint32_t row = 0; row < request.roi_height; ++row)
            {
                active = checkpoint(detail::MaskEvaluatorCheckpoint::kEvaluateRow, row);
                if (!active)
                {
                    return active.error();
                }
                for (std::uint32_t column = 0; column < request.roi_width; ++column)
                {
                    const std::size_t output_index =
                        static_cast<std::size_t>(row) * request.roi_width + column;
                    float value = 0.0F;
                    const float px = static_cast<float>(request.roi_x + column) + 0.5F;
                    const float py = static_cast<float>(request.roi_y + row) + 0.5F;
                    if (std::holds_alternative<AllMask>(mask.payload))
                    {
                        value = 1.0F;
                    }
                    else if (const auto *gradient = std::get_if<LinearGradientMask>(&mask.payload);
                             gradient != nullptr)
                    {
                        const float full_width = static_cast<float>(request.full_width);
                        const float full_height = static_cast<float>(request.full_height);
                        const float hwscale = 1.0F / std::hypot(full_width, full_height);
                        const float v =
                            -static_cast<float>(gradient->rotation_degrees) * kPi / 180.0F;
                        const float sine = std::sin(v);
                        const float cosine = std::cos(v);
                        const float yoffset =
                            sine * static_cast<float>(gradient->anchor_x) * full_width -
                            cosine * static_cast<float>(gradient->anchor_y) * full_height;
                        const float y0 = (sine * px - cosine * py - yoffset) * hwscale;
                        const float transition = static_cast<float>(gradient->transition);
                        const float inverse_transition = 1.0F / std::fmax(transition, 0.001F);
                        value = transition == 0.0F ?
                                    (y0 >= 0.0F ? 1.0F : 0.0F) :
                                    unit_math(0.5F + 0.5F * (inverse_transition * y0));
                    }
                    else if (const auto *circle = std::get_if<CircleMask>(&mask.payload);
                             circle != nullptr)
                    {
                        const float min_dimension =
                            static_cast<float>(std::min(request.full_width, request.full_height));
                        const float center_x = static_cast<float>(circle->center_x) *
                                               static_cast<float>(request.full_width);
                        const float center_y = static_cast<float>(circle->center_y) *
                                               static_cast<float>(request.full_height);
                        const float normalized_radius = static_cast<float>(circle->radius);
                        const float normalized_feather = static_cast<float>(circle->feather);
                        const float outer =
                            (normalized_radius + normalized_feather) * min_dimension;
                        const float dx = px - center_x;
                        const float dy = py - center_y;
                        const float inner2 =
                            normalized_radius * min_dimension * normalized_radius * min_dimension;
                        const float distance2 = dx * dx + dy * dy;
                        if (circle->feather == 0.0)
                        {
                            value = distance2 <= inner2 ? 1.0F : 0.0F;
                        }
                        else
                        {
                            const float outer2 = outer * outer;
                            const float ratio = (outer2 - distance2) / (outer2 - inner2);
                            const float clamped = unit_math(ratio);
                            value = clamped * clamped;
                        }
                    }
                    else if (const auto *ellipse = std::get_if<EllipseMask>(&mask.payload);
                             ellipse != nullptr)
                    {
                        const float min_dimension =
                            static_cast<float>(std::min(request.full_width, request.full_height));
                        const float center_x = static_cast<float>(ellipse->center_x) *
                                               static_cast<float>(request.full_width);
                        const float center_y = static_cast<float>(ellipse->center_y) *
                                               static_cast<float>(request.full_height);
                        const float normalized_a = static_cast<float>(ellipse->radius_x);
                        const float normalized_b = static_cast<float>(ellipse->radius_y);
                        const float normalized_feather = static_cast<float>(ellipse->feather);
                        const float a = normalized_a * min_dimension;
                        const float b = normalized_b * min_dimension;
                        const float outer_a = (normalized_a + normalized_feather) * min_dimension;
                        const float outer_b = (normalized_b + normalized_feather) * min_dimension;
                        const float dx = px - center_x;
                        const float dy = py - center_y;
                        const float length2 = dx * dx + dy * dy;
                        const float length = std::sqrt(length2);
                        const float xnorm = length != 0.0F ? dx / length : 0.0F;
                        const float ynorm = length != 0.0F ? dy / length : 1.0F;
                        const float angle =
                            static_cast<float>(ellipse->rotation_degrees) * kPi / 180.0F;
                        const float cosine = std::cos(angle);
                        const float sine = std::sin(angle);
                        const float xrot = xnorm * cosine + ynorm * sine;
                        const float yrot = -xnorm * sine + ynorm * cosine;
                        const float cos2 = xrot * xrot;
                        const float sin2 = yrot * yrot;
                        const float a2 = a * a;
                        const float b2 = b * b;
                        const float radius2 = a2 * b2 / (a2 * sin2 + b2 * cos2);
                        if (ellipse->feather == 0.0)
                        {
                            value = length2 <= radius2 ? 1.0F : 0.0F;
                        }
                        else
                        {
                            const float outer_a2 = outer_a * outer_a;
                            const float outer_b2 = outer_b * outer_b;
                            const float outer2 =
                                outer_a2 * outer_b2 / (outer_a2 * sin2 + outer_b2 * cos2);
                            const float ratio = (outer2 - length2) / (outer2 - radius2);
                            const float clamped = unit_math(ratio);
                            value = clamped * clamped;
                        }
                    }
                    else if (const auto *parametric = std::get_if<ParametricMask>(&mask.payload);
                             parametric != nullptr)
                    {
                        const MaskRgbPlaneView &source =
                            parametric->source == ParametricMaskSource::kInput ?
                                request.input :
                                *request.operation_output;
                        const float channel =
                            read_parametric_channel(source, row, column, parametric->channel);
                        value = parametric_factor(channel, parametric->thresholds);
                    }
                    else
                    {
                        return evaluator_error("Mask payload has no evaluator",
                                               "unsupported_mask_evaluator", mask.id);
                    }
                    if (!std::isfinite(value))
                    {
                        return evaluator_error("Mask math produced a non-finite alpha",
                                               "non_finite_mask_output", mask.id);
                    }
                    result[output_index] = unit_math(value);
                }
            }
        }
        auto common = apply_common(result, mask.common, request, mask.id);
        if (!common)
        {
            return common.error();
        }
        return result;
    };

    auto alpha = evaluate(root);
    if (!alpha)
    {
        return alpha.error();
    }
    cancelled = request.cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    return AlphaPlane{request.roi_width, request.roi_height, std::move(alpha).value()};
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Mask evaluator allocation failed",
                      {{"reason", "allocation_failed"}});
}

Result<AlphaPlane> evaluate_canonical_mask(const std::vector<Mask> &masks,
                                           const std::string_view root_mask_id,
                                           const MaskEvaluationRequest &request)
{
    return detail::evaluate_canonical_mask_controlled(masks, root_mask_id, request, {});
}

Result<void> normal_mask_mix(const std::span<const float> input_rgb,
                             const std::span<float> operation_output_rgb, const AlphaPlane &alpha,
                             const CancellationToken &cancellation)
{
    auto active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    const std::uint64_t pixels = static_cast<std::uint64_t>(alpha.width) * alpha.height;
    if (alpha.width == 0U || alpha.height == 0U ||
        pixels > std::numeric_limits<std::size_t>::max() / 3U || alpha.alpha.size() != pixels ||
        input_rgb.size() != pixels * 3U || operation_output_rgb.size() != pixels * 3U)
    {
        return evaluator_error("Normal mask mix dimensions or samples are invalid",
                               "invalid_mask_mix");
    }
    for (std::uint32_t row = 0; row < alpha.height; ++row)
    {
        active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
        for (std::uint32_t column = 0; column < alpha.width; ++column)
        {
            const std::size_t pixel = static_cast<std::size_t>(row) * alpha.width + column;
            const float opacity = alpha.alpha[pixel];
            if (!std::isfinite(opacity) || opacity < 0.0F || opacity > 1.0F)
            {
                return evaluator_error("Normal mask mix alpha is invalid", "invalid_mask_alpha");
            }
            const std::size_t channel = pixel * 3U;
            for (std::size_t offset = 0; offset < 3U; ++offset)
            {
                const float input = input_rgb[channel + offset];
                const float output = operation_output_rgb[channel + offset];
                if (!std::isfinite(input) || !std::isfinite(output))
                {
                    return evaluator_error("Normal mask mix input contains NaN or infinity",
                                           "non_finite_mask_mix_samples");
                }
                if (opacity == 0.0F)
                {
                    operation_output_rgb[channel + offset] = input;
                }
                else if (opacity != 1.0F)
                {
                    const float mixed = input + opacity * (output - input);
                    if (!std::isfinite(mixed))
                    {
                        return evaluator_error("Normal mask mix produced NaN or infinity",
                                               "non_finite_mask_mix_output");
                    }
                    operation_output_rgb[channel + offset] = mixed;
                }
            }
        }
    }
    return {};
}

MaskEvaluatorMemoryEstimate estimate_mask_evaluator_memory(const std::vector<Mask> &masks,
                                                           const std::string_view root_mask_id,
                                                           const std::uint32_t width,
                                                           const std::uint32_t height) noexcept
{
    if (width == 0U || height == 0U)
    {
        return {};
    }
    const std::size_t root = find_mask_index(masks, root_mask_id);
    if (root == masks.size())
    {
        return {};
    }
    const std::uint64_t plane = saturating_multiply(
        saturating_multiply(static_cast<std::uint64_t>(width), height), sizeof(float));
    const auto peak_planes = [&](auto &&self, const std::size_t index) noexcept -> std::uint64_t
    {
        const auto *group = std::get_if<MaskGroup>(&masks[index].payload);
        if (group == nullptr || group->children.empty())
        {
            return 1U;
        }
        std::uint64_t peak = 1U;
        bool first = true;
        for (const auto &child : group->children)
        {
            const std::size_t child_index = find_mask_index(masks, child.mask_id);
            if (child_index == masks.size())
            {
                continue;
            }
            const std::uint64_t child_peak = self(self, child_index);
            peak = std::max(peak, first ? child_peak : saturating_add(1U, child_peak));
            first = false;
        }
        return peak;
    };
    const std::uint64_t planes = peak_planes(peak_planes, root);
    MaskEvaluatorMemoryEstimate result;
    result.alpha_plane_bytes = plane;
    result.evaluator_scratch_bytes = saturating_multiply(planes > 0U ? planes - 1U : 0U, plane);
    // Vector control blocks coexist with the planes at peak; account for them
    // conservatively without turning the graph estimate into node-count planes.
    result.evaluator_scratch_bytes = saturating_add(
        result.evaluator_scratch_bytes, saturating_multiply(planes, sizeof(std::vector<float>)));
    return result;
}

} // namespace ravo
