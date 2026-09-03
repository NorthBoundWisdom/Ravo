#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ravo/recipe/color_harmonizer.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/develop_mask.h"
#include "ravo/recipe/operation.h"
#include "ravo/recipe/recipe.h"

#include "capability_ops.h"
#include "color_harmonizer.h"
#include "image_ops.h"
#include "mask_evaluator.h"
#include "raw_pipeline.h"

namespace ravo
{
namespace
{

[[nodiscard]] Mask all_mask(std::string id, const double opacity = 1.0, const bool inverted = false)
{
    Mask result{std::move(id), kCanonicalMaskSchemaVersion, MaskKind::kAll};
    result.common = {opacity, inverted};
    return result;
}

[[nodiscard]] std::vector<float> rgb_grid(const std::uint32_t width, const std::uint32_t height)
{
    std::vector<float> result(static_cast<std::size_t>(width) * height * 3U);
    for (std::uint32_t row = 0; row < height; ++row)
    {
        for (std::uint32_t column = 0; column < width; ++column)
        {
            const std::size_t index = (static_cast<std::size_t>(row) * width + column) * 3U;
            result[index] = static_cast<float>(column) / static_cast<float>(width);
            result[index + 1U] = static_cast<float>(row) / static_cast<float>(height);
            result[index + 2U] = 0.5F;
        }
    }
    return result;
}

[[nodiscard]] MaskEvaluationRequest
full_request(const std::uint32_t width, const std::uint32_t height, const std::vector<float> &input,
             const std::optional<std::vector<float>> &output = std::nullopt)
{
    MaskEvaluationRequest request;
    request.full_width = width;
    request.full_height = height;
    request.roi_width = width;
    request.roi_height = height;
    request.input = {input, width * 3U};
    if (output.has_value())
    {
        request.operation_output = MaskRgbPlaneView{*output, width * 3U};
    }
    return request;
}

[[nodiscard]] OperationInstance *find_operation(Recipe &recipe, const std::string_view id)
{
    const auto found =
        std::find_if(recipe.operations.begin(), recipe.operations.end(),
                     [id](const OperationInstance &operation) { return operation.id == id; });
    return found == recipe.operations.end() ? nullptr : &*found;
}

struct MaskCancellationFixture
{
    CancellationSource *source = nullptr;
    detail::MaskEvaluatorCheckpoint target = detail::MaskEvaluatorCheckpoint::kBeforeNode;
    bool fired = false;

    static void checkpoint(void *context, const detail::MaskEvaluatorCheckpoint point,
                           const std::uint32_t) noexcept
    {
        auto &fixture = *static_cast<MaskCancellationFixture *>(context);
        if (!fixture.fired && fixture.source != nullptr && point == fixture.target)
        {
            fixture.fired = fixture.source->cancel("mask-controlled-cancel");
        }
    }
};

TEST(MaskGraphRecipeTest, UpgradesLegacyAllAndRejectsUnknownOrInvalidGraphState)
{
    const auto parsed = parse_recipe_json(
        R"({"asset":{"id":"asset-1","input_uri":"file:///fixture.raw"},"masks":[{"id":"all","kind":"all","schema_version":1}],"operations":[],"schema_version":3})");
    ASSERT_TRUE(parsed) << parsed.error().message;
    ASSERT_EQ(parsed.value().masks.size(), 1U);
    EXPECT_EQ(parsed.value().masks.front().schema_version, kCanonicalMaskSchemaVersion);
    const auto serialized = serialize_recipe(parsed.value());
    ASSERT_TRUE(serialized) << serialized.error().message;
    EXPECT_NE(serialized.value().find(R"("schema_version":2)"), std::string::npos);
    EXPECT_NE(serialized.value().find(R"("opacity":1)"), std::string::npos);
    EXPECT_NE(serialized.value().find(R"("inverted":false)"), std::string::npos);

    const auto unknown = parse_recipe_json(
        R"({"asset":{"id":"asset-1","input_uri":"file:///fixture.raw"},"masks":[{"id":"all","inverted":false,"kind":"all","opacity":1,"schema_version":2,"unexpected":true}],"operations":[],"schema_version":3})");
    ASSERT_FALSE(unknown);
    EXPECT_EQ(unknown.error().context.at("path"), "recipe.masks[0].unexpected");

    const auto unknown_kind = parse_recipe_json(
        R"({"asset":{"id":"asset-1","input_uri":"file:///fixture.raw"},"masks":[{"id":"future","inverted":false,"kind":"future","opacity":1,"schema_version":2}],"operations":[],"schema_version":3})");
    ASSERT_FALSE(unknown_kind);
    EXPECT_EQ(unknown_kind.error().context.at("reason"), "unsupported_mask_kind");

    const auto unknown_version = parse_recipe_json(
        R"({"asset":{"id":"asset-1","input_uri":"file:///fixture.raw"},"masks":[{"id":"future","inverted":false,"kind":"all","opacity":1,"schema_version":99}],"operations":[],"schema_version":3})");
    ASSERT_FALSE(unknown_version);
    EXPECT_EQ(unknown_version.error().context.at("reason"), "unsupported_mask_schema");

    const auto wrong_type = parse_recipe_json(
        R"({"asset":{"id":"asset-1","input_uri":"file:///fixture.raw"},"masks":[{"id":"all","inverted":false,"kind":"all","opacity":"one","schema_version":2}],"operations":[],"schema_version":3})");
    ASSERT_FALSE(wrong_type);
    EXPECT_EQ(wrong_type.error().context.at("path"), "recipe.masks[0].opacity");

    Mask group{"group", kCanonicalMaskSchemaVersion, MaskKind::kGroup};
    group.payload = MaskGroup{{{"missing", MaskGroupOperator::kReplace, 1.0, false}}};
    auto dangling = validate_mask_graph({group});
    ASSERT_FALSE(dangling);
    EXPECT_EQ(dangling.error().context.at("reason"), "mask_graph_dangling_reference");

    auto duplicate = validate_mask_graph({all_mask("duplicate"), all_mask("duplicate")});
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().context.at("reason"), "duplicate_mask_id");

    Mask left{"left", kCanonicalMaskSchemaVersion, MaskKind::kGroup};
    left.payload = MaskGroup{{{"right", MaskGroupOperator::kReplace, 1.0, false}}};
    Mask right{"right", kCanonicalMaskSchemaVersion, MaskKind::kGroup};
    right.payload = MaskGroup{{{"left", MaskGroupOperator::kReplace, 1.0, false}}};
    auto cycle = validate_mask_graph({left, right});
    ASSERT_FALSE(cycle);
    EXPECT_EQ(cycle.error().context.at("reason"), "mask_graph_cycle");

    Mask invalid = all_mask("invalid");
    invalid.common.opacity = std::numeric_limits<double>::infinity();
    auto nonfinite = validate_mask_graph({invalid});
    ASSERT_FALSE(nonfinite);
    EXPECT_EQ(nonfinite.error().context.at("reason"), "invalid_mask_opacity");

    Mask invalid_circle{"invalid-circle", kCanonicalMaskSchemaVersion, MaskKind::kCircle};
    invalid_circle.payload = CircleMask{0.5, 0.5, 0.0, 0.1};
    auto bounded = validate_mask_graph({invalid_circle});
    ASSERT_FALSE(bounded);
    EXPECT_EQ(bounded.error().context.at("reason"), "invalid_circle");
    Mask invalid_thresholds{"invalid-thresholds", kCanonicalMaskSchemaVersion,
                            MaskKind::kParametric};
    invalid_thresholds.payload = ParametricMask{
        ParametricMaskSource::kInput, ParametricMaskChannel::kRed, {0.0, 0.8, 0.7, 1.0}};
    bounded = validate_mask_graph({invalid_thresholds});
    ASSERT_FALSE(bounded);
    EXPECT_EQ(bounded.error().context.at("reason"), "invalid_parametric_thresholds");

    Mask legacy = all_mask("legacy", 0.5);
    legacy.schema_version = 1;
    std::vector<Mask> legacy_graph{legacy};
    auto legacy_upgrade = upgrade_mask_graph(legacy_graph);
    ASSERT_FALSE(legacy_upgrade);
    EXPECT_EQ(legacy_upgrade.error().context.at("reason"), "unsupported_mask_v1_state");
    const auto legacy_json = canonical_mask_to_json(legacy);
    ASSERT_FALSE(legacy_json);
    EXPECT_EQ(legacy_json.error().context.at("reason"), "invalid_mask_v1_common");

    std::vector<Mask> deep;
    const auto one_child_group = [](std::string id, std::string child)
    {
        Mask result{std::move(id), kCanonicalMaskSchemaVersion, MaskKind::kGroup};
        result.payload = MaskGroup{{{std::move(child), MaskGroupOperator::kReplace, 1.0, false}}};
        return result;
    };
    // Put a shallow parent first. It reaches and completes the shared deep
    // tail before a later parent chain reaches that tail at depth 33.
    deep.push_back(one_child_group("shallow", "tail20"));
    deep.push_back(all_mask("leaf"));
    for (int index = 1; index <= 20; ++index)
    {
        deep.push_back(one_child_group("tail" + std::to_string(index),
                                       index == 1 ? "leaf" : "tail" + std::to_string(index - 1)));
    }
    for (int index = 1; index <= 12; ++index)
    {
        deep.push_back(
            one_child_group("parent" + std::to_string(index),
                            index == 1 ? "tail20" : "parent" + std::to_string(index - 1)));
    }
    auto too_deep = validate_mask_graph(deep);
    ASSERT_FALSE(too_deep);
    EXPECT_EQ(too_deep.error().context.at("reason"), "mask_graph_too_deep");

    std::vector<Mask> too_many(kCanonicalMaskMaxNodes + 1U);
    for (std::size_t index = 0; index < too_many.size(); ++index)
    {
        too_many[index] = all_mask("mask" + std::to_string(index));
    }
    auto node_limit = validate_mask_graph(too_many);
    ASSERT_FALSE(node_limit);
    EXPECT_EQ(node_limit.error().context.at("reason"), "mask_graph_too_large");

    Mask children{"children", kCanonicalMaskSchemaVersion, MaskKind::kGroup};
    MaskGroup oversized;
    for (std::size_t index = 0; index <= kCanonicalMaskMaxGroupChildren; ++index)
    {
        oversized.children.push_back(
            {"mask" + std::to_string(index),
             index == 0U ? MaskGroupOperator::kReplace : MaskGroupOperator::kUnion, 1.0, false});
    }
    children.payload = std::move(oversized);
    auto child_limit = validate_mask_graph({children});
    ASSERT_FALSE(child_limit);
    EXPECT_EQ(child_limit.error().context.at("reason"), "invalid_mask_group_size");

    Mask invalid_selector{"invalid-selector", kCanonicalMaskSchemaVersion, MaskKind::kParametric};
    invalid_selector.payload = ParametricMask{static_cast<ParametricMaskSource>(99),
                                              ParametricMaskChannel::kLuminance,
                                              {0.0, 0.0, 1.0, 1.0}};
    auto selector = validate_mask_graph({invalid_selector});
    ASSERT_FALSE(selector);
    EXPECT_EQ(selector.error().context.at("reason"), "invalid_parametric_selector");
    invalid_selector.payload = ParametricMask{
        ParametricMaskSource::kInput, static_cast<ParametricMaskChannel>(99), {0.0, 0.0, 1.0, 1.0}};
    selector = validate_mask_graph({invalid_selector});
    ASSERT_FALSE(selector);
    EXPECT_EQ(selector.error().context.at("reason"), "invalid_parametric_selector");

    Mask invalid_operator{"invalid-operator", kCanonicalMaskSchemaVersion, MaskKind::kGroup};
    invalid_operator.payload =
        MaskGroup{{{"first", MaskGroupOperator::kReplace, 1.0, false},
                   {"second", static_cast<MaskGroupOperator>(99), 1.0, false}}};
    auto group_operator =
        validate_mask_graph({all_mask("first"), all_mask("second"), invalid_operator});
    ASSERT_FALSE(group_operator);
    EXPECT_EQ(group_operator.error().context.at("reason"), "invalid_mask_group_operator");

    std::vector<Mask> expanding{all_mask("expansion-leaf")};
    std::string previous = "expansion-leaf";
    for (int index = 1; index <= 8; ++index)
    {
        Mask parent{"expansion-" + std::to_string(index), kCanonicalMaskSchemaVersion,
                    MaskKind::kGroup};
        parent.payload = MaskGroup{{{previous, MaskGroupOperator::kReplace, 1.0, false},
                                    {previous, MaskGroupOperator::kUnion, 1.0, false}}};
        previous = parent.id;
        expanding.push_back(std::move(parent));
    }
    auto expansion_limit = validate_mask_graph(expanding);
    ASSERT_FALSE(expansion_limit);
    EXPECT_EQ(expansion_limit.error().context.at("reason"), "mask_graph_expansion_too_large");
}

TEST(MaskGraphEvaluatorTest, UsesPixelCentersSourceOrderGroupsAndParametricSources)
{
    const auto input = rgb_grid(4U, 4U);
    Mask gradient{"gradient", kCanonicalMaskSchemaVersion, MaskKind::kLinearGradient};
    gradient.payload = LinearGradientMask{0.5, 0.5, 0.0, 0.0};
    auto alpha = evaluate_canonical_mask({gradient}, "gradient", full_request(4U, 4U, input));
    ASSERT_TRUE(alpha) << alpha.error().message;
    ASSERT_EQ(alpha.value().alpha.size(), 16U);
    EXPECT_FLOAT_EQ(alpha.value().alpha[0], 1.0F);
    EXPECT_FLOAT_EQ(alpha.value().alpha[4], 1.0F);
    EXPECT_FLOAT_EQ(alpha.value().alpha[8], 0.0F);
    EXPECT_FLOAT_EQ(alpha.value().alpha[12], 0.0F);

    Mask circle{"circle", kCanonicalMaskSchemaVersion, MaskKind::kCircle};
    circle.payload = CircleMask{0.5, 0.5, 0.25, 0.0};
    alpha = evaluate_canonical_mask({circle}, "circle", full_request(4U, 4U, input));
    ASSERT_TRUE(alpha) << alpha.error().message;
    EXPECT_FLOAT_EQ(alpha.value().alpha[5], 1.0F);
    EXPECT_FLOAT_EQ(alpha.value().alpha[10], 1.0F);
    EXPECT_FLOAT_EQ(alpha.value().alpha[0], 0.0F);

    circle.payload = CircleMask{0.5, 0.5, 0.25, 0.25};
    alpha = evaluate_canonical_mask({circle}, "circle", full_request(4U, 4U, input));
    ASSERT_TRUE(alpha) << alpha.error().message;
    EXPECT_FLOAT_EQ(alpha.value().alpha[4], 0.25F);

    Mask ellipse{"ellipse", kCanonicalMaskSchemaVersion, MaskKind::kEllipse};
    ellipse.payload = EllipseMask{0.5, 0.5, 0.25, 0.125, 0.0, 0.125};
    alpha = evaluate_canonical_mask({ellipse}, "ellipse", full_request(4U, 4U, input));
    ASSERT_TRUE(alpha) << alpha.error().message;
    EXPECT_GT(alpha.value().alpha[5], 0.0F);
    EXPECT_LT(alpha.value().alpha[0], alpha.value().alpha[5]);

    Mask lhs = all_mask("lhs", 0.6);
    Mask rhs = all_mask("rhs", 0.5);
    Mask difference{"difference", kCanonicalMaskSchemaVersion, MaskKind::kGroup};
    difference.payload = MaskGroup{{{"lhs", MaskGroupOperator::kReplace, 1.0, false},
                                    {"rhs", MaskGroupOperator::kDifference, 0.5, false}}};
    alpha = evaluate_canonical_mask({lhs, rhs, difference}, "difference",
                                    full_request(1U, 1U, {0.2F, 0.2F, 0.2F}));
    ASSERT_TRUE(alpha) << alpha.error().message;
    EXPECT_FLOAT_EQ(alpha.value().alpha.front(), 0.45F);

    Mask union_group{"union", kCanonicalMaskSchemaVersion, MaskKind::kGroup};
    union_group.payload = MaskGroup{{{"lhs", MaskGroupOperator::kReplace, 1.0, false},
                                     {"rhs", MaskGroupOperator::kUnion, 0.5, true}}};
    alpha = evaluate_canonical_mask({lhs, rhs, union_group}, "union",
                                    full_request(1U, 1U, {0.2F, 0.2F, 0.2F}));
    ASSERT_TRUE(alpha) << alpha.error().message;
    EXPECT_FLOAT_EQ(alpha.value().alpha.front(), 0.6F);

    Mask intersection{"intersection", kCanonicalMaskSchemaVersion, MaskKind::kGroup};
    intersection.payload = MaskGroup{{{"lhs", MaskGroupOperator::kReplace, 1.0, false},
                                      {"rhs", MaskGroupOperator::kIntersection, 0.5, false}}};
    alpha = evaluate_canonical_mask({lhs, rhs, intersection}, "intersection",
                                    full_request(1U, 1U, {0.2F, 0.2F, 0.2F}));
    ASSERT_TRUE(alpha) << alpha.error().message;
    EXPECT_FLOAT_EQ(alpha.value().alpha.front(), 0.25F);

    Mask exclusion{"exclusion", kCanonicalMaskSchemaVersion, MaskKind::kGroup};
    exclusion.payload = MaskGroup{{{"lhs", MaskGroupOperator::kReplace, 1.0, false},
                                   {"rhs", MaskGroupOperator::kExclusion, 0.5, false}}};
    alpha = evaluate_canonical_mask({lhs, rhs, exclusion}, "exclusion",
                                    full_request(1U, 1U, {0.2F, 0.2F, 0.2F}));
    ASSERT_TRUE(alpha) << alpha.error().message;
    EXPECT_FLOAT_EQ(alpha.value().alpha.front(), 0.45F);

    Mask parametric{"parametric", kCanonicalMaskSchemaVersion, MaskKind::kParametric};
    parametric.payload = ParametricMask{
        ParametricMaskSource::kInput, ParametricMaskChannel::kRed, {0.25, 0.5, 0.5, 0.75}};
    const std::vector<float> ramp{0.25F, 0.0F,   0.0F, 0.375F, 0.0F,  0.0F, 0.5F, 0.0F,
                                  0.0F,  0.625F, 0.0F, 0.0F,   0.75F, 0.0F, 0.0F};
    alpha = evaluate_canonical_mask({parametric}, "parametric", full_request(5U, 1U, ramp));
    ASSERT_TRUE(alpha) << alpha.error().message;
    EXPECT_FLOAT_EQ(alpha.value().alpha[0], 0.0F);
    EXPECT_FLOAT_EQ(alpha.value().alpha[1], 0.5F);
    EXPECT_FLOAT_EQ(alpha.value().alpha[2], 1.0F);
    EXPECT_FLOAT_EQ(alpha.value().alpha[3], 0.5F);
    EXPECT_FLOAT_EQ(alpha.value().alpha[4], 0.0F);

    parametric.payload = ParametricMask{
        ParametricMaskSource::kInput, ParametricMaskChannel::kRed, {0.25, 0.2505, 0.75, 0.7505}};
    alpha = evaluate_canonical_mask({parametric}, "parametric",
                                    full_request(1U, 1U, {0.25025F, 0.0F, 0.0F}));
    ASSERT_TRUE(alpha) << alpha.error().message;
    const float expected_narrow_slope =
        (0.25025F - 0.25F) * (1.0F / std::fmax(0.001F, 0.2505F - 0.25F));
    EXPECT_FLOAT_EQ(alpha.value().alpha.front(), expected_narrow_slope);

    parametric.payload = ParametricMask{
        ParametricMaskSource::kInput, ParametricMaskChannel::kLuminance, {0.22, 0.22, 0.23, 0.23}};
    alpha = evaluate_canonical_mask({parametric}, "parametric",
                                    full_request(1U, 1U, {1.0F, 0.0F, 0.0F}));
    ASSERT_TRUE(alpha) << alpha.error().message;
    EXPECT_FLOAT_EQ(alpha.value().alpha.front(), 1.0F);

    parametric.payload = ParametricMask{ParametricMaskSource::kOperationOutput,
                                        ParametricMaskChannel::kGreen,
                                        {0.0, 0.0, 1.0, 1.0}};
    auto missing_output = evaluate_canonical_mask({parametric}, "parametric",
                                                  full_request(1U, 1U, {0.0F, 0.0F, 0.0F}));
    ASSERT_FALSE(missing_output);
    EXPECT_EQ(missing_output.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(missing_output.error().context.at("reason"),
              "mask_parametric_operation_output_unavailable");

    const std::vector<float> operation_output{0.0F, 0.5F, 0.0F};
    alpha = evaluate_canonical_mask({parametric}, "parametric",
                                    full_request(1U, 1U, {0.0F, 0.0F, 0.0F}, operation_output));
    ASSERT_TRUE(alpha) << alpha.error().message;
    EXPECT_FLOAT_EQ(alpha.value().alpha.front(), 1.0F);
}

TEST(MaskGraphEvaluatorTest, RoiTilesExactlyMatchWholeFrameAndCancellationFailsBeforeAllocation)
{
    const auto input = rgb_grid(4U, 4U);
    Mask gradient{"gradient", kCanonicalMaskSchemaVersion, MaskKind::kLinearGradient};
    gradient.payload = LinearGradientMask{0.5, 0.5, 30.0, 0.35};
    const auto whole = evaluate_canonical_mask({gradient}, "gradient", full_request(4U, 4U, input));
    ASSERT_TRUE(whole) << whole.error().message;

    for (std::uint32_t tile_y = 0; tile_y < 4U; tile_y += 2U)
    {
        MaskEvaluationRequest tile;
        tile.full_width = 4U;
        tile.full_height = 4U;
        tile.roi_y = tile_y;
        tile.roi_width = 4U;
        tile.roi_height = 2U;
        std::vector<float> tile_input(input.begin() + static_cast<std::ptrdiff_t>(tile_y * 4U * 3U),
                                      input.begin() +
                                          static_cast<std::ptrdiff_t>((tile_y + 2U) * 4U * 3U));
        tile.input = {tile_input, 12U};
        const auto partial = evaluate_canonical_mask({gradient}, "gradient", tile);
        ASSERT_TRUE(partial) << partial.error().message;
        for (std::size_t index = 0; index < partial.value().alpha.size(); ++index)
        {
            EXPECT_FLOAT_EQ(partial.value().alpha[index], whole.value().alpha[tile_y * 4U + index]);
        }
    }

    MaskEvaluationRequest inset;
    inset.full_width = 4U;
    inset.full_height = 4U;
    inset.roi_x = 1U;
    inset.roi_y = 1U;
    inset.roi_width = 2U;
    inset.roi_height = 2U;
    const std::size_t inset_offset = (1U * 4U + 1U) * 3U;
    inset.input = {std::span<const float>(input).subspan(inset_offset), 12U};
    const auto inset_alpha = evaluate_canonical_mask({gradient}, "gradient", inset);
    ASSERT_TRUE(inset_alpha) << inset_alpha.error().message;
    ASSERT_EQ(inset_alpha.value().alpha.size(), 4U);
    EXPECT_FLOAT_EQ(inset_alpha.value().alpha[0], whole.value().alpha[5]);
    EXPECT_FLOAT_EQ(inset_alpha.value().alpha[1], whole.value().alpha[6]);
    EXPECT_FLOAT_EQ(inset_alpha.value().alpha[2], whole.value().alpha[9]);
    EXPECT_FLOAT_EQ(inset_alpha.value().alpha[3], whole.value().alpha[10]);

    CancellationSource source;
    ASSERT_TRUE(source.cancel("mask-pre-cancel"));
    auto cancelled = full_request(4U, 4U, input);
    cancelled.cancellation = source.token();
    const auto rejected = evaluate_canonical_mask({gradient}, "gradient", cancelled);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kCancelled);

    CancellationSource allocation_source;
    MaskCancellationFixture allocation_fixture{&allocation_source,
                                               detail::MaskEvaluatorCheckpoint::kBeforeAllocation};
    auto allocation_controlled = full_request(4U, 4U, input);
    allocation_controlled.cancellation = allocation_source.token();
    const auto allocation_cancelled = detail::evaluate_canonical_mask_controlled(
        {gradient}, "gradient", allocation_controlled,
        {.context = &allocation_fixture,
         .checkpoint_callback = &MaskCancellationFixture::checkpoint});
    ASSERT_FALSE(allocation_cancelled);
    EXPECT_TRUE(allocation_fixture.fired);
    EXPECT_EQ(allocation_cancelled.error().code, ErrorCode::kCancelled);

    const auto estimate = estimate_mask_evaluator_memory({gradient}, "gradient", 4U, 4U);
    EXPECT_EQ(estimate.alpha_plane_bytes, 16U * sizeof(float));
    EXPECT_LE(estimate.evaluator_scratch_bytes, sizeof(std::vector<float>));

    auto bad_stride = full_request(4U, 4U, input);
    bad_stride.input.row_stride_samples = 2U;
    const auto rejected_stride = evaluate_canonical_mask({gradient}, "gradient", bad_stride);
    ASSERT_FALSE(rejected_stride);
    EXPECT_EQ(rejected_stride.error().context.at("reason"), "invalid_mask_stride");

    auto bad_roi = full_request(4U, 4U, input);
    bad_roi.roi_x = 4U;
    const auto rejected_roi = evaluate_canonical_mask({gradient}, "gradient", bad_roi);
    ASSERT_FALSE(rejected_roi);
    EXPECT_EQ(rejected_roi.error().context.at("reason"), "invalid_mask_roi");

    auto nonfinite = input;
    nonfinite.front() = std::numeric_limits<float>::quiet_NaN();
    const auto rejected_samples =
        evaluate_canonical_mask({gradient}, "gradient", full_request(4U, 4U, nonfinite));
    ASSERT_FALSE(rejected_samples);
    EXPECT_EQ(rejected_samples.error().context.at("reason"), "non_finite_mask_samples");

    CancellationSource row_source;
    MaskCancellationFixture fixture{&row_source, detail::MaskEvaluatorCheckpoint::kEvaluateRow};
    auto controlled = full_request(4U, 4U, input);
    controlled.cancellation = row_source.token();
    const auto row_cancelled = detail::evaluate_canonical_mask_controlled(
        {gradient}, "gradient", controlled,
        {.context = &fixture, .checkpoint_callback = &MaskCancellationFixture::checkpoint});
    ASSERT_FALSE(row_cancelled);
    EXPECT_TRUE(fixture.fired);
    EXPECT_EQ(row_cancelled.error().code, ErrorCode::kCancelled);

    CancellationSource node_source;
    MaskCancellationFixture node_fixture{&node_source,
                                         detail::MaskEvaluatorCheckpoint::kBeforeNode};
    auto node_controlled = full_request(4U, 4U, input);
    node_controlled.cancellation = node_source.token();
    const auto node_cancelled = detail::evaluate_canonical_mask_controlled(
        {gradient}, "gradient", node_controlled,
        {.context = &node_fixture, .checkpoint_callback = &MaskCancellationFixture::checkpoint});
    ASSERT_FALSE(node_cancelled);
    EXPECT_TRUE(node_fixture.fired);
    EXPECT_EQ(node_cancelled.error().code, ErrorCode::kCancelled);
}

TEST(MaskGraphEngineTest, NormalMixAndOnlySupportedOperationDispatchUseTheGraph)
{
    const auto registry = make_phase1_registry();
    ASSERT_TRUE(registry) << registry.error().message;
    EXPECT_TRUE(registry.value().find(kColorHarmonizerOperationId)->supports_mask);
    EXPECT_TRUE(registry.value().find("ravo.effect.graduatednd")->supports_mask);
    EXPECT_TRUE(registry.value().find("ravo.color.colorbalancergb")->supports_mask);
    EXPECT_TRUE(registry.value().find(kExposureOperationId)->supports_mask);
    EXPECT_TRUE(registry.value().find("ravo.color.rgbcurve")->supports_mask);
    EXPECT_TRUE(registry.value().find("ravo.core.tonecurve")->supports_mask);
    EXPECT_TRUE(registry.value().find("ravo.core.highlights")->supports_mask);
    EXPECT_TRUE(registry.value().find("ravo.core.shadows")->supports_mask);
    EXPECT_TRUE(registry.value().find("ravo.core.whites")->supports_mask);
    EXPECT_TRUE(registry.value().find("ravo.core.blacks")->supports_mask);
    EXPECT_FALSE(registry.value().find("ravo.core.gamma")->supports_mask);

    WorkingImage input;
    input.width = 2U;
    input.height = 2U;
    input.rgb.assign(12U, 0.5F);
    OperationInstance graduated{"ravo.effect.graduatednd",
                                1,
                                "graduated-1",
                                true,
                                {{"density_ev", ParameterValue{1.0}},
                                 {"hardness", ParameterValue{0.5}},
                                 {"rotation_deg", ParameterValue{0.0}},
                                 {"offset", ParameterValue{0.0}}},
                                std::nullopt};
    WorkingImage expected = input;
    ASSERT_TRUE(apply_graduated_nd(expected, graduated, CancellationToken{}));

    Recipe all_recipe;
    all_recipe.asset = {"asset-1", "file:///fixture.raw", std::nullopt};
    all_recipe.masks.push_back(all_mask("all"));
    graduated.mask_id = "all";
    all_recipe.operations.push_back(graduated);
    const auto all = apply_recipe_ops(input, all_recipe, CancellationToken{});
    ASSERT_TRUE(all) << all.error().message;
    EXPECT_EQ(all.value().rgb, expected.rgb);

    Recipe zero_recipe = all_recipe;
    zero_recipe.masks.front().common.opacity = 0.0;
    const auto zero = apply_recipe_ops(input, zero_recipe, CancellationToken{});
    ASSERT_TRUE(zero) << zero.error().message;
    EXPECT_EQ(zero.value().rgb, input.rgb);

    Recipe unsupported = all_recipe;
    unsupported.operations.front().id = "ravo.core.gamma";
    unsupported.operations.front().parameters = {{"gamma", ParameterValue{2.0}}};
    const auto rejected = apply_recipe_ops(input, unsupported, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "unsupported_operation_mask");

    WorkingImage harmonizer_input;
    harmonizer_input.width = 4U;
    harmonizer_input.height = 2U;
    harmonizer_input.rgb = {0.03F, 0.18F, 0.72F, 0.91F, 0.42F, 0.07F, 0.25F, 0.50F,
                            0.70F, 0.20F, 0.30F, 0.40F, 0.03F, 0.18F, 0.72F, 0.91F,
                            0.42F, 0.07F, 0.25F, 0.50F, 0.70F, 0.20F, 0.30F, 0.40F};
    harmonizer_input.color_profile.kind = ColorProfileKind::kIcc;
    harmonizer_input.color_profile.model = ColorModel::kRgb;
    harmonizer_input.color_profile.identifier = std::string(kInputProfileLinearRec709);
    harmonizer_input.color_profile.icc_bytes = {1U, 2U, 3U, 4U};
    harmonizer_input.color_profile.matrix_to_xyz_d50 = {0.4360747F, 0.3850649F, 0.1430804F,
                                                        0.2225045F, 0.7168786F, 0.0606169F,
                                                        0.0139322F, 0.0971045F, 0.7141733F};
    harmonizer_input.color_profile.has_matrix = true;
    auto harmonizer_parameters = color_harmonizer_to_parameters(ColorHarmonizerParams{});
    ASSERT_TRUE(harmonizer_parameters) << harmonizer_parameters.error().message;
    OperationInstance harmonizer{std::string(kColorHarmonizerOperationId),
                                 kColorHarmonizerOperationSchemaVersion,
                                 "harmonizer-1",
                                 true,
                                 harmonizer_parameters.value(),
                                 std::nullopt};
    const auto direct_harmonizer =
        apply_color_harmonizer(harmonizer_input, harmonizer, CancellationToken{});
    ASSERT_TRUE(direct_harmonizer) << direct_harmonizer.error().message;
    Recipe harmonizer_all;
    harmonizer_all.asset = {"asset-1", "file:///fixture.raw", std::nullopt};
    harmonizer_all.masks.push_back(all_mask("all"));
    harmonizer.mask_id = "all";
    harmonizer_all.operations.push_back(harmonizer);
    const auto masked_all = apply_recipe_ops(harmonizer_input, harmonizer_all, CancellationToken{});
    ASSERT_TRUE(masked_all) << masked_all.error().message;
    EXPECT_EQ(masked_all.value().rgb, direct_harmonizer.value().rgb);

    Recipe harmonizer_zero = harmonizer_all;
    harmonizer_zero.masks.front().common.opacity = 0.0;
    const auto masked_zero =
        apply_recipe_ops(harmonizer_input, harmonizer_zero, CancellationToken{});
    ASSERT_TRUE(masked_zero) << masked_zero.error().message;
    EXPECT_EQ(masked_zero.value().rgb, harmonizer_input.rgb);

    Mask spatial{"spatial", kCanonicalMaskSchemaVersion, MaskKind::kLinearGradient};
    spatial.payload = LinearGradientMask{0.5, 0.5, 0.0, 0.0};
    Recipe harmonizer_spatial = harmonizer_all;
    harmonizer_spatial.masks = {spatial};
    harmonizer_spatial.operations.front().mask_id = "spatial";
    const auto masked_spatial =
        apply_recipe_ops(harmonizer_input, harmonizer_spatial, CancellationToken{});
    ASSERT_TRUE(masked_spatial) << masked_spatial.error().message;
    EXPECT_EQ(masked_spatial.value().rgb[0], direct_harmonizer.value().rgb[0]);
    EXPECT_EQ(masked_spatial.value().rgb[12], harmonizer_input.rgb[12]);

    DecodedRaw raw;
    raw.width = 2U;
    raw.height = 2U;
    raw.pixels.assign(4U, 128U);
    Recipe raw_baseline;
    raw_baseline.asset = {"asset-1", "file:///fixture.raw", std::nullopt};
    const auto baseline_bytes = estimate_raw_render_memory(raw, raw_baseline, 2U, 2U);
    Recipe raw_masked = harmonizer_all;
    const auto masked_bytes = estimate_raw_render_memory(raw, raw_masked, 2U, 2U);
    const std::uint64_t float_rgb_bytes = 2U * 2U * 3U * sizeof(float);
    EXPECT_GE(masked_bytes - baseline_bytes, 2U * float_rgb_bytes + 4U * sizeof(float));
}

TEST(MaskGraphDevelopTest, PreservesTypedGraphAndDisabledOrIdentityAttachments)
{
    DevelopParams params;
    params.masks.push_back(all_mask("all"));
    params.graduated_present = true;
    params.graduated_enabled = true;
    params.graduated_mask_id = "all";
    params.graduated_density = 0.0;
    params.color_harmonizer_present = true;
    params.color_harmonizer_enabled = false;
    params.color_harmonizer_mask_id = "all";
    EXPECT_FALSE(params.is_identity());

    auto recipe = recipe_from_develop({"asset-1", "file:///fixture.raw", std::nullopt}, params);
    ASSERT_TRUE(recipe) << recipe.error().message;
    ASSERT_EQ(recipe.value().masks, params.masks);
    const auto *graduated = find_operation(recipe.value(), "ravo.effect.graduatednd");
    ASSERT_NE(graduated, nullptr);
    EXPECT_TRUE(graduated->enabled);
    EXPECT_EQ(graduated->mask_id, std::optional<std::string>{"all"});
    const auto *harmonizer = find_operation(recipe.value(), kColorHarmonizerOperationId);
    ASSERT_NE(harmonizer, nullptr);
    EXPECT_FALSE(harmonizer->enabled);
    EXPECT_EQ(harmonizer->mask_id, std::optional<std::string>{"all"});

    const auto restored = develop_from_recipe(recipe.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_EQ(restored.value().masks, params.masks);
    EXPECT_TRUE(restored.value().graduated_present);
    EXPECT_TRUE(restored.value().graduated_enabled);
    EXPECT_EQ(restored.value().graduated_mask_id, std::optional<std::string>{"all"});
    EXPECT_TRUE(restored.value().color_harmonizer_present);
    EXPECT_FALSE(restored.value().color_harmonizer_enabled);
    EXPECT_EQ(restored.value().color_harmonizer_mask_id, std::optional<std::string>{"all"});

    auto edited = restored.value();
    ASSERT_TRUE(reset_develop_section(edited, "effects"));
    EXPECT_EQ(edited.masks, params.masks);
    EXPECT_EQ(edited.graduated_mask_id, std::optional<std::string>{"all"});
    EXPECT_FALSE(edited.is_identity());

    DevelopParams disabled = params;
    disabled.graduated_enabled = false;
    disabled.graduated_density = 0.75;
    auto disabled_recipe =
        recipe_from_develop({"asset-2", "file:///fixture.raw", std::nullopt}, disabled);
    ASSERT_TRUE(disabled_recipe) << disabled_recipe.error().message;
    const auto *disabled_graduated =
        find_operation(disabled_recipe.value(), "ravo.effect.graduatednd");
    ASSERT_NE(disabled_graduated, nullptr);
    EXPECT_FALSE(disabled_graduated->enabled);
    auto disabled_restored = develop_from_recipe(disabled_recipe.value());
    ASSERT_TRUE(disabled_restored) << disabled_restored.error().message;
    EXPECT_TRUE(disabled_restored.value().graduated_present);
    EXPECT_FALSE(disabled_restored.value().graduated_enabled);
    EXPECT_DOUBLE_EQ(disabled_restored.value().graduated_density, 0.75);
}

TEST(DevelopMaskAuthoringTest, CreatesTypedStudioOwnedLeavesAndRoundTripsOrdinaryEdits)
{
    DevelopParams params;
    auto initial = develop_mask_editor_state(params, DevelopMaskTarget::kColorHarmonizer);
    EXPECT_FALSE(initial.attached);
    EXPECT_TRUE(initial.editable);
    EXPECT_EQ(initial.kind_index, 0);

    ASSERT_TRUE(apply_develop_mask_field_strict(params, "colorHarmonizerMaskKind", 3.0));
    ASSERT_TRUE(apply_develop_mask_field_strict(params, "colorHarmonizerMaskCenterX", 0.25));
    ASSERT_TRUE(apply_develop_mask_field_strict(params, "colorHarmonizerMaskRadius", 0.4));
    ASSERT_TRUE(apply_develop_mask_field_strict(params, "colorHarmonizerMaskOpacity", 0.6));
    ASSERT_TRUE(apply_develop_mask_field_strict(params, "graduatedMaskKind", 5.0));
    ASSERT_TRUE(apply_develop_mask_field_strict(params, "graduatedMaskSource", 1.0));
    ASSERT_TRUE(apply_develop_mask_field_strict(params, "graduatedMaskThreshold1", 0.2));
    ASSERT_TRUE(apply_develop_mask_field_strict(params, "graduatedMaskThreshold2", 0.8));

    ASSERT_TRUE(params.color_harmonizer_mask_id);
    ASSERT_TRUE(params.graduated_mask_id);
    EXPECT_EQ(*params.color_harmonizer_mask_id, "ravo.studio.mask.color_harmonizer.1");
    EXPECT_EQ(*params.graduated_mask_id, "ravo.studio.mask.graduatednd.1");
    EXPECT_TRUE(params.color_harmonizer_present);
    EXPECT_TRUE(params.color_harmonizer_enabled);
    EXPECT_TRUE(params.graduated_present);
    EXPECT_TRUE(params.graduated_enabled);

    const auto color_state = develop_mask_editor_state(params, DevelopMaskTarget::kColorHarmonizer);
    EXPECT_TRUE(color_state.attached);
    EXPECT_TRUE(color_state.editable);
    EXPECT_EQ(color_state.status, DevelopMaskAttachmentStatus::kEditable);
    EXPECT_EQ(color_state.kind_name, "circle");
    EXPECT_DOUBLE_EQ(color_state.center_x, 0.25);
    EXPECT_DOUBLE_EQ(color_state.radius, 0.4);
    EXPECT_DOUBLE_EQ(color_state.opacity, 0.6);
    const auto before_same_kind = params;
    ASSERT_TRUE(apply_develop_mask_field_strict(params, "colorHarmonizerMaskKind", 3.0));
    EXPECT_EQ(params, before_same_kind);
    const auto graduated_state = develop_mask_editor_state(params, DevelopMaskTarget::kGraduatedNd);
    EXPECT_EQ(graduated_state.kind_name, "parametric");
    EXPECT_EQ(graduated_state.source_index, 1);
    EXPECT_DOUBLE_EQ(graduated_state.threshold1, 0.2);
    EXPECT_DOUBLE_EQ(graduated_state.threshold2, 0.8);

    auto recipe = recipe_from_develop({"asset-1", "file:///fixture.raw", std::nullopt}, params);
    ASSERT_TRUE(recipe) << recipe.error().message;
    auto restored = develop_from_recipe(recipe.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_EQ(restored.value(), params);

    auto ordinary_edit = restored.value();
    ASSERT_TRUE(apply_develop_field_strict(ordinary_edit, "exposure", 0.25));
    EXPECT_EQ(ordinary_edit.masks, params.masks);
    EXPECT_EQ(ordinary_edit.color_harmonizer_mask_id, params.color_harmonizer_mask_id);
    EXPECT_EQ(ordinary_edit.graduated_mask_id, params.graduated_mask_id);
}

TEST(DevelopMaskAuthoringTest, AuthorizedParametricHistogramAssistAuthorsThresholds)
{
    EXPECT_TRUE(develop_mask_parametric_assist_allowed(DevelopMaskTarget::kColorBalanceRgb));
    EXPECT_TRUE(develop_mask_parametric_assist_allowed(DevelopMaskTarget::kExposure));
    EXPECT_TRUE(develop_mask_parametric_assist_allowed(DevelopMaskTarget::kRgbCurve));
    EXPECT_TRUE(develop_mask_parametric_assist_allowed(DevelopMaskTarget::kToneCurve));
    EXPECT_TRUE(develop_mask_parametric_assist_allowed(DevelopMaskTarget::kHighlights));
    EXPECT_TRUE(develop_mask_parametric_assist_allowed(DevelopMaskTarget::kShadows));
    EXPECT_TRUE(develop_mask_parametric_assist_allowed(DevelopMaskTarget::kWhites));
    EXPECT_TRUE(develop_mask_parametric_assist_allowed(DevelopMaskTarget::kBlacks));
    EXPECT_FALSE(develop_mask_parametric_assist_allowed(DevelopMaskTarget::kColorHarmonizer));
    EXPECT_FALSE(develop_mask_parametric_assist_allowed(DevelopMaskTarget::kGraduatedNd));

    struct Case
    {
        DevelopMaskTarget target;
        const char *kind_field;
        const char *threshold0;
        const char *threshold1;
        const char *threshold2;
        const char *threshold3;
        const char *asset_id;
    };
    const Case cases[] = {
        {DevelopMaskTarget::kColorBalanceRgb, "colorBalanceRgbMaskKind",
         "colorBalanceRgbMaskThreshold0", "colorBalanceRgbMaskThreshold1",
         "colorBalanceRgbMaskThreshold2", "colorBalanceRgbMaskThreshold3", "asset-cbrgb-assist"},
        {DevelopMaskTarget::kExposure, "exposureMaskKind", "exposureMaskThreshold0",
         "exposureMaskThreshold1", "exposureMaskThreshold2", "exposureMaskThreshold3",
         "asset-exposure-assist"},
        {DevelopMaskTarget::kRgbCurve, "rgbCurveMaskKind", "rgbCurveMaskThreshold0",
         "rgbCurveMaskThreshold1", "rgbCurveMaskThreshold2", "rgbCurveMaskThreshold3",
         "asset-rgbcurve-assist"},
        {DevelopMaskTarget::kToneCurve, "toneCurveMaskKind", "toneCurveMaskThreshold0",
         "toneCurveMaskThreshold1", "toneCurveMaskThreshold2", "toneCurveMaskThreshold3",
         "asset-tonecurve-assist"},
        {DevelopMaskTarget::kHighlights, "highlightsMaskKind", "highlightsMaskThreshold0",
         "highlightsMaskThreshold1", "highlightsMaskThreshold2", "highlightsMaskThreshold3",
         "asset-highlights-assist"},
        {DevelopMaskTarget::kShadows, "shadowsMaskKind", "shadowsMaskThreshold0",
         "shadowsMaskThreshold1", "shadowsMaskThreshold2", "shadowsMaskThreshold3",
         "asset-shadows-assist"},
        {DevelopMaskTarget::kWhites, "whitesMaskKind", "whitesMaskThreshold0",
         "whitesMaskThreshold1", "whitesMaskThreshold2", "whitesMaskThreshold3",
         "asset-whites-assist"},
        {DevelopMaskTarget::kBlacks, "blacksMaskKind", "blacksMaskThreshold0",
         "blacksMaskThreshold1", "blacksMaskThreshold2", "blacksMaskThreshold3",
         "asset-blacks-assist"},
    };

    for (const auto &entry : cases)
    {
        DevelopParams params;
        ASSERT_TRUE(apply_develop_mask_field_strict(params, entry.kind_field, 5.0))
            << entry.kind_field;
        auto state = develop_mask_editor_state(params, entry.target);
        EXPECT_EQ(state.kind_name, "parametric") << entry.kind_field;
        EXPECT_TRUE(develop_mask_parametric_assist_allowed(entry.target)) << entry.kind_field;

        const double sample = normalized_display_mask_channel(128, 64, 32, state.channel_index);
        auto thresholds = parametric_thresholds_from_histogram_assist(sample, nullptr);
        ASSERT_TRUE(thresholds) << thresholds.error().message << " " << entry.kind_field;
        ASSERT_TRUE(
            apply_develop_mask_field_strict(params, entry.threshold2, thresholds.value()[2]))
            << entry.threshold2;
        ASSERT_TRUE(
            apply_develop_mask_field_strict(params, entry.threshold1, thresholds.value()[1]))
            << entry.threshold1;
        ASSERT_TRUE(
            apply_develop_mask_field_strict(params, entry.threshold0, thresholds.value()[0]))
            << entry.threshold0;
        ASSERT_TRUE(
            apply_develop_mask_field_strict(params, entry.threshold3, thresholds.value()[3]))
            << entry.threshold3;

        state = develop_mask_editor_state(params, entry.target);
        EXPECT_NEAR(state.threshold0, thresholds.value()[0], 1e-12) << entry.kind_field;
        EXPECT_NEAR(state.threshold1, thresholds.value()[1], 1e-12) << entry.kind_field;
        EXPECT_NEAR(state.threshold2, thresholds.value()[2], 1e-12) << entry.kind_field;
        EXPECT_NEAR(state.threshold3, thresholds.value()[3], 1e-12) << entry.kind_field;

        auto recipe =
            recipe_from_develop({entry.asset_id, "file:///fixture.raw", std::nullopt}, params);
        ASSERT_TRUE(recipe) << recipe.error().message << " " << entry.kind_field;
        auto restored = develop_from_recipe(recipe.value());
        ASSERT_TRUE(restored) << restored.error().message << " " << entry.kind_field;
        auto restored_state = develop_mask_editor_state(restored.value(), entry.target);
        EXPECT_NEAR(restored_state.threshold0, thresholds.value()[0], 1e-12) << entry.kind_field;
        EXPECT_NEAR(restored_state.threshold3, thresholds.value()[3], 1e-12) << entry.kind_field;
    }

    auto rejected = parametric_thresholds_from_histogram_assist(2.0, nullptr);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "invalid_parametric_assist_sample");
}

TEST(DevelopMaskAuthoringTest, ColorBalanceRgbOwnsCircleAndBrushMasksAndRoundTrips)
{
    DevelopParams params;
    params.color_balance_rgb.saturation_global = 0.4;
    ASSERT_TRUE(apply_develop_mask_field_strict(params, "colorBalanceRgbMaskKind", 3.0));
    ASSERT_TRUE(apply_develop_mask_field_strict(params, "colorBalanceRgbMaskCenterX", 0.3));
    ASSERT_TRUE(apply_develop_mask_field_strict(params, "colorBalanceRgbMaskRadius", 0.35));
    ASSERT_TRUE(params.color_balance_rgb_mask_id);
    EXPECT_EQ(*params.color_balance_rgb_mask_id, "ravo.studio.mask.color_balance_rgb.1");
    auto circle_state = develop_mask_editor_state(params, DevelopMaskTarget::kColorBalanceRgb);
    EXPECT_EQ(circle_state.kind_name, "circle");
    EXPECT_DOUBLE_EQ(circle_state.center_x, 0.3);
    EXPECT_DOUBLE_EQ(circle_state.radius, 0.35);

    ASSERT_TRUE(apply_develop_mask_field_strict(params, "colorBalanceRgbMaskKind", 8.0));
    auto brush_state = develop_mask_editor_state(params, DevelopMaskTarget::kColorBalanceRgb);
    EXPECT_EQ(brush_state.kind_name, "brush");
    EXPECT_EQ(*params.color_balance_rgb_mask_id, "ravo.studio.mask.color_balance_rgb.1");

    auto recipe = recipe_from_develop({"asset-1", "file:///fixture.raw", std::nullopt}, params);
    ASSERT_TRUE(recipe) << recipe.error().message;
    const auto *operation = find_operation(recipe.value(), "ravo.color.colorbalancergb");
    ASSERT_NE(operation, nullptr);
    EXPECT_EQ(operation->mask_id, params.color_balance_rgb_mask_id);
    auto restored = develop_from_recipe(recipe.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_EQ(restored.value().color_balance_rgb_mask_id, params.color_balance_rgb_mask_id);
    EXPECT_EQ(restored.value().masks, params.masks);
    EXPECT_NEAR(restored.value().color_balance_rgb.saturation_global, 0.4, 1e-12);
}

TEST(MaskGraphEngineTest, ColorBalanceRgbNormalMixMatchesUnmaskedAndZeroOpacityInput)
{
    ColorBalanceRgbParams grade;
    grade.saturation_global = 0.5;
    auto parameters = color_balance_rgb_to_parameters(grade);
    OperationInstance operation{"ravo.color.colorbalancergb", 1,
                                "colorbalancergb-1",          true,
                                std::move(parameters),        std::nullopt};
    WorkingImage input;
    input.width = 2U;
    input.height = 2U;
    input.rgb = {0.12F, 0.40F, 0.70F, 0.80F, 0.22F, 0.18F,
                 0.33F, 0.55F, 0.41F, 0.60F, 0.10F, 0.25F};
    WorkingImage expected = input;
    ASSERT_TRUE(apply_color_balance_rgb(expected, operation, CancellationToken{}));

    Recipe all_recipe;
    all_recipe.asset = {"asset-1", "file:///fixture.raw", std::nullopt};
    all_recipe.masks.push_back(all_mask("all"));
    operation.mask_id = "all";
    all_recipe.operations.push_back(operation);
    const auto masked_all = apply_recipe_ops(input, all_recipe, CancellationToken{});
    ASSERT_TRUE(masked_all) << masked_all.error().message;
    EXPECT_EQ(masked_all.value().rgb, expected.rgb);

    Recipe zero_recipe = all_recipe;
    zero_recipe.masks.front().common.opacity = 0.0;
    const auto masked_zero = apply_recipe_ops(input, zero_recipe, CancellationToken{});
    ASSERT_TRUE(masked_zero) << masked_zero.error().message;
    EXPECT_EQ(masked_zero.value().rgb, input.rgb);
}

TEST(DevelopMaskAuthoringTest, ExposureOwnsCircleAndBrushMasksAndRoundTrips)
{
    DevelopParams params;
    params.exposure_ev = 0.75;
    ASSERT_TRUE(apply_develop_mask_field_strict(params, "exposureMaskKind", 3.0));
    ASSERT_TRUE(apply_develop_mask_field_strict(params, "exposureMaskCenterX", 0.3));
    ASSERT_TRUE(apply_develop_mask_field_strict(params, "exposureMaskRadius", 0.35));
    ASSERT_TRUE(params.exposure_mask_id);
    EXPECT_EQ(*params.exposure_mask_id, "ravo.studio.mask.exposure.1");
    auto circle_state = develop_mask_editor_state(params, DevelopMaskTarget::kExposure);
    EXPECT_EQ(circle_state.kind_name, "circle");
    EXPECT_DOUBLE_EQ(circle_state.center_x, 0.3);
    EXPECT_DOUBLE_EQ(circle_state.radius, 0.35);

    ASSERT_TRUE(apply_develop_mask_field_strict(params, "exposureMaskKind", 8.0));
    auto brush_state = develop_mask_editor_state(params, DevelopMaskTarget::kExposure);
    EXPECT_EQ(brush_state.kind_name, "brush");
    EXPECT_EQ(*params.exposure_mask_id, "ravo.studio.mask.exposure.1");

    auto recipe = recipe_from_develop({"asset-1", "file:///fixture.raw", std::nullopt}, params);
    ASSERT_TRUE(recipe) << recipe.error().message;
    const auto *operation = find_operation(recipe.value(), kExposureOperationId);
    ASSERT_NE(operation, nullptr);
    EXPECT_EQ(operation->mask_id, params.exposure_mask_id);
    auto restored = develop_from_recipe(recipe.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_EQ(restored.value().exposure_mask_id, params.exposure_mask_id);
    EXPECT_EQ(restored.value().masks, params.masks);
    EXPECT_NEAR(restored.value().exposure_ev, 0.75, 1e-12);

    DevelopParams identity_mask;
    ASSERT_TRUE(apply_develop_mask_field_strict(identity_mask, "exposureMaskKind", 3.0));
    EXPECT_TRUE(identity_mask.exposure_ev == 0.0);
    auto identity_recipe =
        recipe_from_develop({"asset-2", "file:///fixture.raw", std::nullopt}, identity_mask);
    ASSERT_TRUE(identity_recipe) << identity_recipe.error().message;
    const auto *identity_operation = find_operation(identity_recipe.value(), kExposureOperationId);
    ASSERT_NE(identity_operation, nullptr);
    EXPECT_EQ(identity_operation->mask_id, identity_mask.exposure_mask_id);

    auto section_reset = restored.value();
    ASSERT_TRUE(reset_develop_section(section_reset, "light"));
    EXPECT_EQ(section_reset.exposure_mask_id, params.exposure_mask_id);
    EXPECT_EQ(section_reset.masks, params.masks);
}

TEST(MaskGraphEngineTest, ExposureNormalMixMatchesUnmaskedAndZeroOpacityInput)
{
    ExposureParams grade;
    grade.exposure_ev = 1.0;
    OperationInstance operation{std::string(kExposureOperationId),
                                kExposureOperationSchemaVersion,
                                "exposure-1",
                                true,
                                exposure_to_parameters(grade),
                                std::nullopt};
    WorkingImage input;
    input.width = 2U;
    input.height = 2U;
    input.rgb = {0.12F, 0.40F, 0.70F, 0.80F, 0.22F, 0.18F,
                 0.33F, 0.55F, 0.41F, 0.60F, 0.10F, 0.25F};
    input.color_profile.kind = ColorProfileKind::kMatrix;
    input.color_profile.model = ColorModel::kRgb;
    const auto expected = apply_exposure(input, operation, CancellationToken{});
    ASSERT_TRUE(expected) << expected.error().message;

    Recipe all_recipe;
    all_recipe.asset = {"asset-1", "file:///fixture.raw", std::nullopt};
    all_recipe.masks.push_back(all_mask("all"));
    operation.mask_id = "all";
    all_recipe.operations.push_back(operation);
    const auto masked_all = apply_recipe_ops(input, all_recipe, CancellationToken{});
    ASSERT_TRUE(masked_all) << masked_all.error().message;
    EXPECT_EQ(masked_all.value().rgb, expected.value().rgb);

    Recipe zero_recipe = all_recipe;
    zero_recipe.masks.front().common.opacity = 0.0;
    const auto masked_zero = apply_recipe_ops(input, zero_recipe, CancellationToken{});
    ASSERT_TRUE(masked_zero) << masked_zero.error().message;
    EXPECT_EQ(masked_zero.value().rgb, input.rgb);

    OperationInstance identity{std::string(kExposureOperationId),
                               kExposureOperationSchemaVersion,
                               "exposure-identity",
                               true,
                               exposure_to_parameters(ExposureParams{}),
                               "all"};
    Recipe identity_recipe = all_recipe;
    identity_recipe.operations = {identity};
    const auto masked_identity = apply_recipe_ops(input, identity_recipe, CancellationToken{});
    ASSERT_TRUE(masked_identity) << masked_identity.error().message;
    EXPECT_EQ(masked_identity.value().rgb, input.rgb);
}

TEST(DevelopMaskAuthoringTest, RgbCurveOwnsCircleAndBrushMasksAndRoundTrips)
{
    DevelopParams params;
    params.rgb_curve.channels[0] = {{0.0, 0.0}, {0.5, 0.75}, {1.0, 1.0}};
    ASSERT_TRUE(apply_develop_mask_field_strict(params, "rgbCurveMaskKind", 3.0));
    ASSERT_TRUE(apply_develop_mask_field_strict(params, "rgbCurveMaskCenterX", 0.3));
    ASSERT_TRUE(apply_develop_mask_field_strict(params, "rgbCurveMaskRadius", 0.35));
    ASSERT_TRUE(params.rgb_curve_mask_id);
    EXPECT_EQ(*params.rgb_curve_mask_id, "ravo.studio.mask.rgb_curve.1");
    auto circle_state = develop_mask_editor_state(params, DevelopMaskTarget::kRgbCurve);
    EXPECT_EQ(circle_state.kind_name, "circle");
    EXPECT_DOUBLE_EQ(circle_state.center_x, 0.3);
    EXPECT_DOUBLE_EQ(circle_state.radius, 0.35);

    ASSERT_TRUE(apply_develop_mask_field_strict(params, "rgbCurveMaskKind", 8.0));
    auto brush_state = develop_mask_editor_state(params, DevelopMaskTarget::kRgbCurve);
    EXPECT_EQ(brush_state.kind_name, "brush");
    EXPECT_EQ(*params.rgb_curve_mask_id, "ravo.studio.mask.rgb_curve.1");

    auto recipe = recipe_from_develop({"asset-1", "file:///fixture.raw", std::nullopt}, params);
    ASSERT_TRUE(recipe) << recipe.error().message;
    const auto *operation = find_operation(recipe.value(), "ravo.color.rgbcurve");
    ASSERT_NE(operation, nullptr);
    EXPECT_EQ(operation->mask_id, params.rgb_curve_mask_id);
    auto restored = develop_from_recipe(recipe.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_EQ(restored.value().rgb_curve_mask_id, params.rgb_curve_mask_id);
    EXPECT_EQ(restored.value().masks, params.masks);

    DevelopParams identity_mask;
    ASSERT_TRUE(apply_develop_mask_field_strict(identity_mask, "rgbCurveMaskKind", 3.0));
    auto identity_recipe =
        recipe_from_develop({"asset-2", "file:///fixture.raw", std::nullopt}, identity_mask);
    ASSERT_TRUE(identity_recipe) << identity_recipe.error().message;
    const auto *identity_operation = find_operation(identity_recipe.value(), "ravo.color.rgbcurve");
    ASSERT_NE(identity_operation, nullptr);
    EXPECT_EQ(identity_operation->mask_id, identity_mask.rgb_curve_mask_id);

    auto section_reset = restored.value();
    ASSERT_TRUE(reset_develop_section(section_reset, "curves"));
    EXPECT_EQ(section_reset.rgb_curve_mask_id, params.rgb_curve_mask_id);
    EXPECT_EQ(section_reset.masks, params.masks);
}

TEST(MaskGraphEngineTest, RgbCurveNormalMixMatchesUnmaskedAndZeroOpacityInput)
{
    RgbCurveParams grade;
    grade.preserve_colors = std::string(kToneCurvePreserveColorsNone);
    grade.channels[0] = {{0.0, 0.0}, {0.5, 0.75}, {1.0, 1.0}};
    OperationInstance operation{"ravo.color.rgbcurve",          1,           "rgbcurve-1", true,
                                rgb_curve_to_parameters(grade), std::nullopt};
    WorkingImage input;
    input.width = 2U;
    input.height = 2U;
    input.rgb = {0.12F, 0.40F, 0.70F, 0.80F, 0.22F, 0.18F,
                 0.33F, 0.55F, 0.41F, 0.60F, 0.10F, 0.25F};
    input.color_profile.kind = ColorProfileKind::kMatrix;
    input.color_profile.model = ColorModel::kRgb;
    Recipe expected_recipe;
    expected_recipe.asset = {"asset-1", "file:///fixture.raw", std::nullopt};
    expected_recipe.operations.push_back(operation);
    const auto expected = apply_recipe_ops(input, expected_recipe, CancellationToken{});
    ASSERT_TRUE(expected) << expected.error().message;

    Recipe all_recipe = expected_recipe;
    all_recipe.masks.push_back(all_mask("all"));
    all_recipe.operations.front().mask_id = "all";
    const auto masked_all = apply_recipe_ops(input, all_recipe, CancellationToken{});
    ASSERT_TRUE(masked_all) << masked_all.error().message;
    EXPECT_EQ(masked_all.value().rgb, expected.value().rgb);

    Recipe zero_recipe = all_recipe;
    zero_recipe.masks.front().common.opacity = 0.0;
    const auto masked_zero = apply_recipe_ops(input, zero_recipe, CancellationToken{});
    ASSERT_TRUE(masked_zero) << masked_zero.error().message;
    EXPECT_EQ(masked_zero.value().rgb, input.rgb);

    OperationInstance identity{"ravo.color.rgbcurve",
                               1,
                               "rgbcurve-identity",
                               true,
                               rgb_curve_to_parameters(RgbCurveParams{}),
                               std::nullopt};
    Recipe identity_unmasked;
    identity_unmasked.asset = all_recipe.asset;
    identity_unmasked.operations = {identity};
    const auto unmasked_identity = apply_recipe_ops(input, identity_unmasked, CancellationToken{});
    ASSERT_TRUE(unmasked_identity) << unmasked_identity.error().message;
    identity.mask_id = "all";
    Recipe identity_recipe = all_recipe;
    identity_recipe.operations = {identity};
    const auto masked_identity = apply_recipe_ops(input, identity_recipe, CancellationToken{});
    ASSERT_TRUE(masked_identity) << masked_identity.error().message;
    EXPECT_EQ(masked_identity.value().rgb, unmasked_identity.value().rgb);
}

TEST(DevelopMaskAuthoringTest, ToneCurveOwnsCircleAndBrushMasksAndRoundTrips)
{
    DevelopParams params;
    params.tone_curve = {{0.0, 0.0}, {0.5, 0.75}, {1.0, 1.0}};
    ASSERT_TRUE(apply_develop_mask_field_strict(params, "toneCurveMaskKind", 3.0));
    ASSERT_TRUE(apply_develop_mask_field_strict(params, "toneCurveMaskCenterX", 0.3));
    ASSERT_TRUE(apply_develop_mask_field_strict(params, "toneCurveMaskRadius", 0.35));
    ASSERT_TRUE(params.tone_curve_mask_id);
    EXPECT_EQ(*params.tone_curve_mask_id, "ravo.studio.mask.tone_curve.1");
    auto circle_state = develop_mask_editor_state(params, DevelopMaskTarget::kToneCurve);
    EXPECT_EQ(circle_state.kind_name, "circle");
    EXPECT_DOUBLE_EQ(circle_state.center_x, 0.3);
    EXPECT_DOUBLE_EQ(circle_state.radius, 0.35);

    ASSERT_TRUE(apply_develop_mask_field_strict(params, "toneCurveMaskKind", 8.0));
    auto brush_state = develop_mask_editor_state(params, DevelopMaskTarget::kToneCurve);
    EXPECT_EQ(brush_state.kind_name, "brush");
    EXPECT_EQ(*params.tone_curve_mask_id, "ravo.studio.mask.tone_curve.1");

    auto recipe = recipe_from_develop({"asset-1", "file:///fixture.raw", std::nullopt}, params);
    ASSERT_TRUE(recipe) << recipe.error().message;
    const auto *operation = find_operation(recipe.value(), "ravo.core.tonecurve");
    ASSERT_NE(operation, nullptr);
    EXPECT_EQ(operation->mask_id, params.tone_curve_mask_id);
    auto restored = develop_from_recipe(recipe.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_EQ(restored.value().tone_curve_mask_id, params.tone_curve_mask_id);
    EXPECT_EQ(restored.value().masks, params.masks);

    DevelopParams identity_mask;
    ASSERT_TRUE(apply_develop_mask_field_strict(identity_mask, "toneCurveMaskKind", 3.0));
    auto identity_recipe =
        recipe_from_develop({"asset-2", "file:///fixture.raw", std::nullopt}, identity_mask);
    ASSERT_TRUE(identity_recipe) << identity_recipe.error().message;
    const auto *identity_operation = find_operation(identity_recipe.value(), "ravo.core.tonecurve");
    ASSERT_NE(identity_operation, nullptr);
    EXPECT_EQ(identity_operation->mask_id, identity_mask.tone_curve_mask_id);

    auto section_reset = restored.value();
    ASSERT_TRUE(reset_develop_section(section_reset, "curves"));
    EXPECT_EQ(section_reset.tone_curve_mask_id, params.tone_curve_mask_id);
    EXPECT_EQ(section_reset.masks, params.masks);
}

TEST(MaskGraphEngineTest, ToneCurveNormalMixMatchesUnmaskedAndZeroOpacityInput)
{
    std::map<std::string, ParameterValue, std::less<>> parameters{
        {"working_space", ParameterValue{std::string(kToneCurveWorkingSpaceSrgb)}},
        {"interpolation", ParameterValue{std::string(kToneCurveInterpolationMonotoneHermite)}},
        {"channel_mode", ParameterValue{std::string(kToneCurveChannelModeRgb)}},
        {"preserve_colors", ParameterValue{std::string(kToneCurvePreserveColorsAverage)}},
        {"points", tone_curve_points_to_parameter({{0.0, 0.0}, {0.5, 0.75}, {1.0, 1.0}})}};
    OperationInstance operation{"ravo.core.tonecurve", 1, "tonecurve-1", true, parameters,
                                std::nullopt};
    WorkingImage input;
    input.width = 2U;
    input.height = 2U;
    input.rgb = {0.12F, 0.40F, 0.70F, 0.80F, 0.22F, 0.18F,
                 0.33F, 0.55F, 0.41F, 0.60F, 0.10F, 0.25F};
    input.color_profile.kind = ColorProfileKind::kMatrix;
    input.color_profile.model = ColorModel::kRgb;
    Recipe expected_recipe;
    expected_recipe.asset = {"asset-1", "file:///fixture.raw", std::nullopt};
    expected_recipe.operations.push_back(operation);
    const auto expected = apply_recipe_ops(input, expected_recipe, CancellationToken{});
    ASSERT_TRUE(expected) << expected.error().message;

    Recipe all_recipe = expected_recipe;
    all_recipe.masks.push_back(all_mask("all"));
    all_recipe.operations.front().mask_id = "all";
    const auto masked_all = apply_recipe_ops(input, all_recipe, CancellationToken{});
    ASSERT_TRUE(masked_all) << masked_all.error().message;
    EXPECT_EQ(masked_all.value().rgb, expected.value().rgb);

    Recipe zero_recipe = all_recipe;
    zero_recipe.masks.front().common.opacity = 0.0;
    const auto masked_zero = apply_recipe_ops(input, zero_recipe, CancellationToken{});
    ASSERT_TRUE(masked_zero) << masked_zero.error().message;
    EXPECT_EQ(masked_zero.value().rgb, input.rgb);

    std::map<std::string, ParameterValue, std::less<>> identity_parameters{
        {"working_space", ParameterValue{std::string(kToneCurveWorkingSpaceSrgb)}},
        {"interpolation", ParameterValue{std::string(kToneCurveInterpolationMonotoneHermite)}},
        {"channel_mode", ParameterValue{std::string(kToneCurveChannelModeRgb)}},
        {"preserve_colors", ParameterValue{std::string(kToneCurvePreserveColorsAverage)}},
        {"points", tone_curve_points_to_parameter({{0.0, 0.0}, {1.0, 1.0}})}};
    OperationInstance identity{"ravo.core.tonecurve", 1,           "tonecurve-identity", true,
                               identity_parameters,   std::nullopt};
    Recipe identity_unmasked;
    identity_unmasked.asset = all_recipe.asset;
    identity_unmasked.operations = {identity};
    const auto unmasked_identity = apply_recipe_ops(input, identity_unmasked, CancellationToken{});
    ASSERT_TRUE(unmasked_identity) << unmasked_identity.error().message;
    identity.mask_id = "all";
    Recipe identity_recipe = all_recipe;
    identity_recipe.operations = {identity};
    const auto masked_identity = apply_recipe_ops(input, identity_recipe, CancellationToken{});
    ASSERT_TRUE(masked_identity) << masked_identity.error().message;
    EXPECT_EQ(masked_identity.value().rgb, unmasked_identity.value().rgb);
}

TEST(DevelopMaskAuthoringTest, EllipseCenterAndFeatherFieldsApplyToEllipseLeaves)
{
    DevelopParams params;
    ASSERT_TRUE(apply_develop_mask_field_strict(params, "exposureMaskKind", 4.0));
    ASSERT_TRUE(apply_develop_mask_field_strict(params, "exposureMaskCenterX", 0.22));
    ASSERT_TRUE(apply_develop_mask_field_strict(params, "exposureMaskCenterY", 0.64));
    ASSERT_TRUE(apply_develop_mask_field_strict(params, "exposureMaskFeather", 0.12));
    auto state = develop_mask_editor_state(params, DevelopMaskTarget::kExposure);
    EXPECT_EQ(state.kind_name, "ellipse");
    EXPECT_DOUBLE_EQ(state.center_x, 0.22);
    EXPECT_DOUBLE_EQ(state.center_y, 0.64);
    EXPECT_DOUBLE_EQ(state.feather, 0.12);
}

TEST(DevelopMaskAuthoringTest, LightControlsOwnCircleAndBrushMasksAndRoundTrip)
{
    struct Case
    {
        const char *kind_field;
        const char *center_field;
        const char *radius_field;
        DevelopMaskTarget target;
        const char *operation_id;
        std::optional<std::string> DevelopParams::*attachment;
        const char *studio_id;
    };
    const std::array cases{
        Case{"highlightsMaskKind", "highlightsMaskCenterX", "highlightsMaskRadius",
             DevelopMaskTarget::kHighlights, "ravo.core.highlights",
             &DevelopParams::highlights_mask_id, "ravo.studio.mask.highlights.1"},
        Case{"shadowsMaskKind", "shadowsMaskCenterX", "shadowsMaskRadius",
             DevelopMaskTarget::kShadows, "ravo.core.shadows", &DevelopParams::shadows_mask_id,
             "ravo.studio.mask.shadows.1"},
        Case{"whitesMaskKind", "whitesMaskCenterX", "whitesMaskRadius", DevelopMaskTarget::kWhites,
             "ravo.core.whites", &DevelopParams::whites_mask_id, "ravo.studio.mask.whites.1"},
        Case{"blacksMaskKind", "blacksMaskCenterX", "blacksMaskRadius", DevelopMaskTarget::kBlacks,
             "ravo.core.blacks", &DevelopParams::blacks_mask_id, "ravo.studio.mask.blacks.1"},
    };
    for (const auto &test : cases)
    {
        DevelopParams params;
        params.highlights = 0.4;
        params.shadows = -0.3;
        params.whites = 0.2;
        params.blacks = -0.1;
        ASSERT_TRUE(apply_develop_mask_field_strict(params, test.kind_field, 3.0));
        ASSERT_TRUE(apply_develop_mask_field_strict(params, test.center_field, 0.3));
        ASSERT_TRUE(apply_develop_mask_field_strict(params, test.radius_field, 0.35));
        ASSERT_TRUE(params.*(test.attachment));
        EXPECT_EQ(*(params.*(test.attachment)), test.studio_id);
        auto circle_state = develop_mask_editor_state(params, test.target);
        EXPECT_EQ(circle_state.kind_name, "circle");
        EXPECT_DOUBLE_EQ(circle_state.center_x, 0.3);
        EXPECT_DOUBLE_EQ(circle_state.radius, 0.35);

        ASSERT_TRUE(apply_develop_mask_field_strict(params, test.kind_field, 8.0));
        EXPECT_EQ(develop_mask_editor_state(params, test.target).kind_name, "brush");
        EXPECT_EQ(*(params.*(test.attachment)), test.studio_id);

        auto recipe = recipe_from_develop({"asset-1", "file:///fixture.raw", std::nullopt}, params);
        ASSERT_TRUE(recipe) << recipe.error().message;
        const auto *operation = find_operation(recipe.value(), test.operation_id);
        ASSERT_NE(operation, nullptr);
        EXPECT_EQ(operation->mask_id, params.*(test.attachment));
        auto restored = develop_from_recipe(recipe.value());
        ASSERT_TRUE(restored) << restored.error().message;
        EXPECT_EQ(restored.value().*(test.attachment), params.*(test.attachment));

        DevelopParams identity_mask;
        ASSERT_TRUE(apply_develop_mask_field_strict(identity_mask, test.kind_field, 3.0));
        auto identity_recipe =
            recipe_from_develop({"asset-2", "file:///fixture.raw", std::nullopt}, identity_mask);
        ASSERT_TRUE(identity_recipe) << identity_recipe.error().message;
        const auto *identity_operation = find_operation(identity_recipe.value(), test.operation_id);
        ASSERT_NE(identity_operation, nullptr);
        EXPECT_EQ(identity_operation->mask_id, identity_mask.*(test.attachment));

        auto section_reset = restored.value();
        ASSERT_TRUE(reset_develop_section(section_reset, "light"));
        EXPECT_EQ(section_reset.*(test.attachment), params.*(test.attachment));
    }
}

TEST(MaskGraphEngineTest, HighlightsNormalMixMatchesUnmaskedAndZeroOpacityInput)
{
    OperationInstance operation{"ravo.core.highlights",
                                1,
                                "highlights-1",
                                true,
                                {{"amount", ParameterValue{0.6}}},
                                std::nullopt};
    WorkingImage input;
    input.width = 2U;
    input.height = 2U;
    input.rgb = {0.12F, 0.40F, 0.70F, 0.80F, 0.22F, 0.18F,
                 0.33F, 0.55F, 0.41F, 0.60F, 0.10F, 0.25F};
    input.color_profile.kind = ColorProfileKind::kMatrix;
    input.color_profile.model = ColorModel::kRgb;
    Recipe expected_recipe;
    expected_recipe.asset = {"asset-1", "file:///fixture.raw", std::nullopt};
    expected_recipe.operations.push_back(operation);
    const auto expected = apply_recipe_ops(input, expected_recipe, CancellationToken{});
    ASSERT_TRUE(expected) << expected.error().message;

    Recipe all_recipe = expected_recipe;
    all_recipe.masks.push_back(all_mask("all"));
    all_recipe.operations.front().mask_id = "all";
    const auto masked_all = apply_recipe_ops(input, all_recipe, CancellationToken{});
    ASSERT_TRUE(masked_all) << masked_all.error().message;
    EXPECT_EQ(masked_all.value().rgb, expected.value().rgb);

    Recipe zero_recipe = all_recipe;
    zero_recipe.masks.front().common.opacity = 0.0;
    const auto masked_zero = apply_recipe_ops(input, zero_recipe, CancellationToken{});
    ASSERT_TRUE(masked_zero) << masked_zero.error().message;
    EXPECT_EQ(masked_zero.value().rgb, input.rgb);

    OperationInstance identity{"ravo.core.highlights",
                               1,
                               "highlights-identity",
                               true,
                               {{"amount", ParameterValue{0.0}}},
                               std::nullopt};
    Recipe identity_unmasked;
    identity_unmasked.asset = all_recipe.asset;
    identity_unmasked.operations = {identity};
    const auto unmasked_identity = apply_recipe_ops(input, identity_unmasked, CancellationToken{});
    ASSERT_TRUE(unmasked_identity) << unmasked_identity.error().message;
    identity.mask_id = "all";
    Recipe identity_recipe = all_recipe;
    identity_recipe.operations = {identity};
    const auto masked_identity = apply_recipe_ops(input, identity_recipe, CancellationToken{});
    ASSERT_TRUE(masked_identity) << masked_identity.error().message;
    EXPECT_EQ(masked_identity.value().rgb, unmasked_identity.value().rgb);
}

TEST(MaskGraphEngineTest, MaskedHighlightsDoesNotFuseWithFollowingShadows)
{
    WorkingImage input;
    input.width = 2U;
    input.height = 2U;
    input.rgb = {0.12F, 0.40F, 0.70F, 0.80F, 0.22F, 0.18F,
                 0.33F, 0.55F, 0.41F, 0.60F, 0.10F, 0.25F};
    input.color_profile.kind = ColorProfileKind::kMatrix;
    input.color_profile.model = ColorModel::kRgb;
    OperationInstance highlights{"ravo.core.highlights",
                                 1,
                                 "highlights-1",
                                 true,
                                 {{"amount", ParameterValue{0.6}}},
                                 std::nullopt};
    OperationInstance shadows{
        "ravo.core.shadows", 1, "shadows-1", true, {{"amount", ParameterValue{-0.5}}},
        std::nullopt};
    Recipe sequential_highlights;
    sequential_highlights.asset = {"asset-1", "file:///fixture.raw", std::nullopt};
    sequential_highlights.operations = {highlights};
    const auto after_highlights =
        apply_recipe_ops(input, sequential_highlights, CancellationToken{});
    ASSERT_TRUE(after_highlights) << after_highlights.error().message;
    Recipe sequential_shadows;
    sequential_shadows.asset = sequential_highlights.asset;
    sequential_shadows.operations = {shadows};
    const auto sequential =
        apply_recipe_ops(after_highlights.value(), sequential_shadows, CancellationToken{});
    ASSERT_TRUE(sequential) << sequential.error().message;

    Recipe masked;
    masked.asset = sequential_highlights.asset;
    masked.masks.push_back(all_mask("all"));
    highlights.mask_id = "all";
    masked.operations = {highlights, shadows};
    const auto masked_result = apply_recipe_ops(input, masked, CancellationToken{});
    ASSERT_TRUE(masked_result) << masked_result.error().message;
    EXPECT_EQ(masked_result.value().rgb, sequential.value().rgb);
}

TEST(DevelopMaskAuthoringTest, StrictlyValidatesSelectorsBoundsAndParametricOrderWithoutMutation)
{
    DevelopParams params;
    ASSERT_TRUE(apply_develop_mask_field_strict(params, "colorHarmonizerMaskKind", 2.0));
    const auto before = params;
    auto invalid = apply_develop_mask_field_strict(params, "colorHarmonizerMaskKind", 9.0);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().context.at("reason"), "invalid_develop_mask_kind");
    EXPECT_EQ(params, before);
    invalid = apply_develop_mask_field_strict(params, "colorHarmonizerMaskKind", 2.5);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(params, before);
    invalid = apply_develop_mask_field_strict(params, "colorHarmonizerMaskAnchorX",
                                              std::numeric_limits<double>::quiet_NaN());
    ASSERT_FALSE(invalid);
    EXPECT_EQ(params, before);
    invalid = apply_develop_mask_field_strict(params, "colorHarmonizerMaskRotationDegrees", 181.0);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(params, before);

    ASSERT_TRUE(apply_develop_mask_field_strict(params, "graduatedMaskKind", 5.0));
    ASSERT_TRUE(apply_develop_mask_field_strict(params, "graduatedMaskThreshold1", 0.3));
    const auto parametric_before = params;
    invalid = apply_develop_mask_field_strict(params, "graduatedMaskThreshold0", 0.4);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().context.at("reason"), "invalid_parametric_thresholds");
    EXPECT_EQ(params, parametric_before);
    invalid = apply_develop_mask_field_strict(params, "graduatedMaskSource", 0.5);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().context.at("reason"), "invalid_develop_mask_source");
    EXPECT_EQ(params, parametric_before);
    invalid = apply_develop_mask_field_strict(params, "graduatedMaskFuture", 1.0);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().context.at("reason"), "unknown_develop_mask_field");
    EXPECT_EQ(params, parametric_before);
    ASSERT_TRUE(reset_develop_mask_field(params, "graduatedMaskThreshold1"));
    const auto reset_parametric =
        develop_mask_editor_state(params, DevelopMaskTarget::kGraduatedNd);
    EXPECT_DOUBLE_EQ(reset_parametric.threshold0, 0.0);
    EXPECT_DOUBLE_EQ(reset_parametric.threshold1, 0.0);
    EXPECT_DOUBLE_EQ(reset_parametric.threshold2, 1.0);
    EXPECT_DOUBLE_EQ(reset_parametric.threshold3, 1.0);

    ASSERT_TRUE(apply_develop_mask_field_strict(params, "graduatedMaskSource", 1.0));
    ASSERT_TRUE(apply_develop_mask_field_strict(params, "graduatedMaskChannel", 2.0));
    ASSERT_TRUE(apply_develop_mask_field_strict(params, "graduatedMaskThreshold1", 0.2));
    ASSERT_TRUE(apply_develop_mask_field_strict(params, "graduatedMaskThreshold2", 0.8));
    ASSERT_TRUE(reset_develop_mask_field(params, "graduatedMaskSource"));
    auto independently_reset = develop_mask_editor_state(params, DevelopMaskTarget::kGraduatedNd);
    EXPECT_EQ(independently_reset.source_index, 0);
    EXPECT_EQ(independently_reset.channel_index, 2);
    EXPECT_DOUBLE_EQ(independently_reset.threshold1, 0.2);
    EXPECT_DOUBLE_EQ(independently_reset.threshold2, 0.8);
    ASSERT_TRUE(reset_develop_mask_field(params, "graduatedMaskChannel"));
    independently_reset = develop_mask_editor_state(params, DevelopMaskTarget::kGraduatedNd);
    EXPECT_EQ(independently_reset.channel_index, 0);
    EXPECT_DOUBLE_EQ(independently_reset.threshold1, 0.2);
    EXPECT_DOUBLE_EQ(independently_reset.threshold2, 0.8);
    ASSERT_TRUE(reset_develop_mask_field(params, "graduatedMaskThreshold1"));
    independently_reset = develop_mask_editor_state(params, DevelopMaskTarget::kGraduatedNd);
    EXPECT_EQ(independently_reset.source_index, 0);
    EXPECT_EQ(independently_reset.channel_index, 0);
    EXPECT_DOUBLE_EQ(independently_reset.threshold0, 0.0);
    EXPECT_DOUBLE_EQ(independently_reset.threshold1, 0.0);
    EXPECT_DOUBLE_EQ(independently_reset.threshold2, 1.0);
    EXPECT_DOUBLE_EQ(independently_reset.threshold3, 1.0);
}

TEST(DevelopMaskAuthoringTest, ResetKeepsOwnedAttachmentAndDetachDoesNotRewriteOperationPresence)
{
    DevelopParams params;
    ASSERT_TRUE(apply_develop_mask_field_strict(params, "colorHarmonizerMaskKind", 4.0));
    ASSERT_TRUE(apply_develop_mask_field_strict(params, "colorHarmonizerMaskRadiusX", 0.7));
    ASSERT_TRUE(apply_develop_mask_field_strict(params, "colorHarmonizerMaskInverted", 1.0));
    const auto id = params.color_harmonizer_mask_id;
    ASSERT_TRUE(id);
    ASSERT_TRUE(reset_develop_mask_field(params, "colorHarmonizerMaskRadiusX"));
    EXPECT_EQ(params.color_harmonizer_mask_id, id);
    const auto after_field_reset =
        develop_mask_editor_state(params, DevelopMaskTarget::kColorHarmonizer);
    EXPECT_DOUBLE_EQ(after_field_reset.radius_x, 0.25);
    ASSERT_TRUE(reset_develop_mask_field(params, "colorHarmonizerMaskInverted"));
    EXPECT_FALSE(develop_mask_editor_state(params, DevelopMaskTarget::kColorHarmonizer).inverted);
    EXPECT_EQ(params.color_harmonizer_mask_id, id);

    ASSERT_TRUE(reset_develop_mask_field(params, "colorHarmonizerMaskKind"));
    EXPECT_EQ(params.color_harmonizer_mask_id, id);
    EXPECT_EQ(develop_mask_editor_state(params, DevelopMaskTarget::kColorHarmonizer).kind_name,
              "all");
    ASSERT_TRUE(reset_develop_mask_field(params, "colorHarmonizerMask"));
    EXPECT_FALSE(params.color_harmonizer_mask_id);
    EXPECT_TRUE(params.masks.empty());
    // Attachment lifecycle has no durable provenance. Detach deliberately
    // leaves the explicit operation state intact rather than guessing whether
    // it was user-authored before Studio created the leaf.
    EXPECT_TRUE(params.color_harmonizer_present);
    EXPECT_TRUE(params.color_harmonizer_enabled);
}

TEST(DevelopMaskAuthoringTest, ExternalSharedAndGroupAttachmentsAreReadOnlyButDetachable)
{
    DevelopParams invalid;
    invalid.color_harmonizer_mask_id = "missing";
    const auto invalid_state =
        develop_mask_editor_state(invalid, DevelopMaskTarget::kColorHarmonizer);
    EXPECT_TRUE(invalid_state.attached);
    EXPECT_FALSE(invalid_state.editable);
    EXPECT_FALSE(invalid_state.can_detach);
    EXPECT_EQ(invalid_state.status, DevelopMaskAttachmentStatus::kInvalid);
    auto rejected = apply_develop_mask_field_strict(invalid, "colorHarmonizerMaskKind", 0.0);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "missing_develop_mask_attachment");

    DevelopParams external;
    external.masks.push_back(all_mask("external-mask"));
    external.color_harmonizer_mask_id = "external-mask";
    const auto external_before = external;
    rejected = apply_develop_mask_field_strict(external, "colorHarmonizerMaskOpacity", 0.5);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(rejected.error().context.at("reason"), "external_read_only");
    EXPECT_EQ(external, external_before);
    ASSERT_TRUE(apply_develop_mask_field_strict(external, "colorHarmonizerMaskKind", 0.0));
    EXPECT_FALSE(external.color_harmonizer_mask_id);
    ASSERT_EQ(external.masks.size(), 1U);
    EXPECT_EQ(external.masks.front().id, "external-mask");

    DevelopParams shared;
    shared.masks.push_back(all_mask("ravo.studio.mask.color_harmonizer.1"));
    shared.color_harmonizer_mask_id = shared.masks.front().id;
    shared.graduated_mask_id = shared.masks.front().id;
    rejected = apply_develop_mask_field_strict(shared, "colorHarmonizerMaskOpacity", 0.5);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "shared_read_only");
    ASSERT_TRUE(reset_develop_mask_field(shared, "colorHarmonizerMask"));
    EXPECT_FALSE(shared.color_harmonizer_mask_id);
    EXPECT_TRUE(shared.graduated_mask_id);
    ASSERT_EQ(shared.masks.size(), 1U);

    DevelopParams grouped;
    Mask group{"external-group", kCanonicalMaskSchemaVersion, MaskKind::kGroup};
    group.payload = MaskGroup{
        {{"ravo.studio.mask.color_harmonizer.1", MaskGroupOperator::kReplace, 1.0, false}}};
    grouped.masks = {all_mask("ravo.studio.mask.color_harmonizer.1"), group};
    grouped.color_harmonizer_mask_id = "ravo.studio.mask.color_harmonizer.1";
    rejected = apply_develop_mask_field_strict(grouped, "colorHarmonizerMaskKind", 3.0);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "shared_read_only");
    ASSERT_TRUE(reset_develop_mask_field(grouped, "colorHarmonizerMask"));
    ASSERT_EQ(grouped.masks.size(), 2U);

    DevelopParams group_root;
    Mask root_group{"external-group", kCanonicalMaskSchemaVersion, MaskKind::kGroup};
    root_group.payload = MaskGroup{{{"child", MaskGroupOperator::kReplace, 1.0, false}}};
    group_root.masks = {all_mask("child"), root_group};
    group_root.color_harmonizer_mask_id = "external-group";
    rejected = apply_develop_mask_field_strict(group_root, "colorHarmonizerMaskOpacity", 0.5);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "group_read_only");
    ASSERT_TRUE(reset_develop_mask_field(group_root, "colorHarmonizerMask"));
    ASSERT_EQ(group_root.masks.size(), 2U);
}

TEST(DevelopMaskAuthoringTest, AllocatesTargetSpecificCollisionSafeStudioIds)
{
    DevelopParams params;
    params.masks.push_back(all_mask("ravo.studio.mask.color_harmonizer.1"));
    ASSERT_TRUE(apply_develop_mask_field_strict(params, "colorHarmonizerMaskKind", 1.0));
    ASSERT_TRUE(params.color_harmonizer_mask_id);
    EXPECT_EQ(*params.color_harmonizer_mask_id, "ravo.studio.mask.color_harmonizer.2");
}

TEST(DevelopMaskAuthoringTest, AuthorsOwnedGroupsPathsAndBrushes)
{
    DevelopParams params;
    ASSERT_TRUE(apply_develop_mask_field_strict(params, "colorHarmonizerMaskKind", 6.0));
    const auto group_state = develop_mask_editor_state(params, DevelopMaskTarget::kColorHarmonizer);
    EXPECT_TRUE(group_state.editable);
    EXPECT_EQ(group_state.kind_name, "group");
    EXPECT_EQ(group_state.child_count, 1);
    ASSERT_TRUE(apply_develop_mask_field_strict(params, "colorHarmonizerMaskAddChild", 3.0));
    EXPECT_EQ(develop_mask_editor_state(params, DevelopMaskTarget::kColorHarmonizer).child_count,
              2);
    ASSERT_TRUE(apply_develop_mask_field_strict(params, "colorHarmonizerMaskChildOperator", 1.0));
    ASSERT_TRUE(apply_develop_mask_field_strict(params, "colorHarmonizerMaskCenterX", 0.4));
    ASSERT_TRUE(apply_develop_mask_field_strict(params, "colorHarmonizerMaskKind", 7.0));
    auto path_state = develop_mask_editor_state(params, DevelopMaskTarget::kColorHarmonizer);
    EXPECT_EQ(path_state.kind_name, "path");
    EXPECT_GE(path_state.point_count, 3);
    ASSERT_TRUE(apply_develop_mask_field_strict(params, "colorHarmonizerMaskAddPoint", 1.0));
    EXPECT_EQ(develop_mask_editor_state(params, DevelopMaskTarget::kColorHarmonizer).point_count,
              path_state.point_count + 1);
    ASSERT_TRUE(apply_develop_mask_field_strict(params, "colorHarmonizerMaskKind", 8.0));
    EXPECT_EQ(develop_mask_editor_state(params, DevelopMaskTarget::kColorHarmonizer).kind_name,
              "brush");
}

TEST(CanonicalMaskGraphTest, PathAndBrushEvaluateClosedAndOpenGeometry)
{
    PathMask square;
    square.points = {{0.2, 0.2, 0.2, 0.2, 0.2, 0.2},
                     {0.8, 0.2, 0.8, 0.2, 0.8, 0.2},
                     {0.8, 0.8, 0.8, 0.8, 0.8, 0.8},
                     {0.2, 0.8, 0.2, 0.8, 0.2, 0.8}};
    Mask path{"path", kCanonicalMaskSchemaVersion, MaskKind::kPath};
    path.payload = square;
    auto json = canonical_mask_to_json(path);
    ASSERT_TRUE(json) << json.error().message;
    auto parsed = parse_canonical_mask(json.value(), "path");
    ASSERT_TRUE(parsed) << parsed.error().message;
    EXPECT_EQ(parsed.value(), path);

    const auto input = rgb_grid(32U, 32U);
    auto alpha = evaluate_canonical_mask({path}, "path", full_request(32U, 32U, input));
    ASSERT_TRUE(alpha) << alpha.error().message;
    EXPECT_GT(alpha.value().alpha[16U * 32U + 16U], 0.5F);
    EXPECT_LT(alpha.value().alpha[1U], 0.5F);

    BrushMask stroke;
    stroke.points = {{0.2, 0.5, 0.2, 0.5, 0.35, 0.5, 0.08, 0.4, 1.0},
                     {0.8, 0.5, 0.65, 0.5, 0.8, 0.5, 0.08, 0.4, 1.0}};
    Mask brush{"brush", kCanonicalMaskSchemaVersion, MaskKind::kBrush};
    brush.payload = stroke;
    alpha = evaluate_canonical_mask({brush}, "brush", full_request(32U, 32U, input));
    ASSERT_TRUE(alpha) << alpha.error().message;
    EXPECT_GT(alpha.value().alpha[16U * 32U + 16U], 0.0F);
}

TEST(CanonicalMaskGraphTest, OverlayCompositePreservesExactZeroAlpha)
{
    std::vector<std::uint8_t> rgb{10, 20, 30, 40, 50, 60};
    AlphaPlane alpha;
    alpha.width = 2;
    alpha.height = 1;
    alpha.alpha = {0.0F, 1.0F};
    ASSERT_TRUE(composite_mask_overlay_rgb8(rgb, alpha, {}));
    EXPECT_EQ(rgb[0], 10);
    EXPECT_EQ(rgb[1], 20);
    EXPECT_EQ(rgb[2], 30);
    EXPECT_NE(rgb[3], 40);
}

} // namespace
} // namespace ravo
