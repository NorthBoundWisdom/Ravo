#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "perspective_transform.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/perspective.h"

namespace ravo
{
namespace
{

[[nodiscard]] LinearWorkingBuffer grid_image(const std::uint32_t width = 96U,
                                              const std::uint32_t height = 72U)
{
    LinearWorkingBuffer image;
    image.width = width;
    image.height = height;
    image.color_profile.kind = ColorProfileKind::kBuiltin;
    image.color_profile.model = ColorModel::kRgb;
    image.color_profile.identifier = "linear_rec709";
    image.color_profile.has_matrix = true;
    image.rgb.resize(static_cast<std::size_t>(width) * height * 3U);
    for (std::uint32_t y = 0U; y < height; ++y)
    {
        for (std::uint32_t x = 0U; x < width; ++x)
        {
            const float grid = x % 12U == 0U || y % 12U == 0U ? 0.92F : 0.18F;
            const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 3U;
            image.rgb[offset] = grid;
            image.rgb[offset + 1U] =
                grid * 0.8F + static_cast<float>(x) / static_cast<float>(width) * 0.1F;
            image.rgb[offset + 2U] =
                grid * 0.6F + static_cast<float>(y) / static_cast<float>(height) * 0.2F;
        }
    }
    image.canonical_roi_scale =
        CanonicalRoiScale::from_scaled_dimensions(width, height, width, height);
    return image;
}

[[nodiscard]] std::uint64_t quantized_hash(const LinearWorkingBuffer &image)
{
    std::uint64_t hash = 1469598103934665603ULL;
    for (const float sample : image.rgb)
    {
        const auto quantized = static_cast<std::int32_t>(
            std::lround(std::clamp(sample, -4.0F, 4.0F) * 16384.0F));
        for (std::uint32_t byte = 0U; byte < 4U; ++byte)
        {
            hash ^= static_cast<std::uint8_t>(static_cast<std::uint32_t>(quantized) >> (byte * 8U));
            hash *= 1099511628211ULL;
        }
    }
    return hash;
}

[[nodiscard]] PerspectivePoint map_point(const std::array<double, 9> &matrix, const double x,
                                         const double y)
{
    const double denominator = matrix[6] * x + matrix[7] * y + matrix[8];
    return {(matrix[0] * x + matrix[1] * y + matrix[2]) / denominator,
            (matrix[3] * x + matrix[4] * y + matrix[5]) / denominator};
}

[[nodiscard]] RasterBuffer axis_grid_raster()
{
    RasterBuffer raster;
    raster.width = 320U;
    raster.height = 240U;
    raster.source_width = raster.width;
    raster.source_height = raster.height;
    raster.srgb.assign(static_cast<std::size_t>(raster.width) * raster.height * 3U, 24U);
    const auto paint = [&](const std::uint32_t x, const std::uint32_t y)
    {
        if (x >= raster.width || y >= raster.height)
            return;
        const std::size_t offset = (static_cast<std::size_t>(y) * raster.width + x) * 3U;
        raster.srgb[offset] = raster.srgb[offset + 1U] = raster.srgb[offset + 2U] = 235U;
    };
    for (const std::uint32_t x : {55U, 155U, 265U})
        for (std::uint32_t y = 18U; y < 222U; ++y)
            for (std::int32_t dx = -1; dx <= 1; ++dx)
                paint(static_cast<std::uint32_t>(static_cast<std::int32_t>(x) + dx), y);
    for (const std::uint32_t y : {45U, 118U, 198U})
        for (std::uint32_t x = 18U; x < 302U; ++x)
            for (std::int32_t dy = -1; dy <= 1; ++dy)
                paint(x, static_cast<std::uint32_t>(static_cast<std::int32_t>(y) + dy));
    return raster;
}

TEST(PerspectiveRecipeTest, CanonicalDevelopRoundTripAndStrictSchema)
{
    DevelopParams params;
    params.straighten_degrees = 2.5;
    params.perspective_vertical = 0.18;
    params.perspective_horizontal = -0.09;
    params.perspective_shear = 0.035;
    params.perspective_constrain_crop = false;
    params.perspective_interpolation_index = 1;
    auto recipe = recipe_from_develop({"asset", "file:///grid.png", std::nullopt}, params);
    ASSERT_TRUE(recipe) << recipe.error().message;
    const auto found = std::find_if(recipe.value().operations.begin(), recipe.value().operations.end(),
                                    [](const OperationInstance &operation)
                                    { return operation.id == kPerspectiveOperationId; });
    ASSERT_NE(found, recipe.value().operations.end());
    EXPECT_EQ(found->schema_version, kPerspectiveOperationSchemaVersion);
    EXPECT_EQ(found->parameters.size(), 8U);
    EXPECT_EQ(std::get<std::string>(found->parameters.at("interpolation").value),
              kPerspectiveInterpolationLanczos2);
    auto restored = develop_from_recipe(recipe.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_DOUBLE_EQ(restored.value().straighten_degrees, params.straighten_degrees);
    EXPECT_DOUBLE_EQ(restored.value().perspective_vertical, params.perspective_vertical);
    EXPECT_DOUBLE_EQ(restored.value().perspective_horizontal, params.perspective_horizontal);
    EXPECT_DOUBLE_EQ(restored.value().perspective_shear, params.perspective_shear);
    EXPECT_EQ(restored.value().perspective_constrain_crop, params.perspective_constrain_crop);
    EXPECT_EQ(restored.value().perspective_interpolation_index,
              params.perspective_interpolation_index);

    auto malformed = found->parameters;
    malformed.emplace("unknown", ParameterValue{0.0});
    auto rejected = perspective_from_parameters(malformed);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "invalid_perspective_parameters");
    malformed = found->parameters;
    malformed["vertical_shift"] = ParameterValue{std::numeric_limits<double>::infinity()};
    EXPECT_FALSE(perspective_from_parameters(malformed));
}

TEST(PerspectiveTest, IdentityIsPixelExactAndDoesNotMutateSource)
{
    const auto input = grid_image();
    const auto original = input.rgb;
    auto layout = compute_perspective_layout(input.width, input.height, PerspectiveParams{});
    ASSERT_TRUE(layout) << layout.error().message;
    EXPECT_EQ(layout.value().output_width, input.width);
    EXPECT_EQ(layout.value().output_height, input.height);
    auto result = apply_perspective(input, PerspectiveParams{}, CancellationToken{});
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result.value().rgb, original);
    EXPECT_EQ(input.rgb, original);
}

TEST(PerspectiveTest, SafeCropContainsOnlyMappedFinitePixelsAndPreservesSource)
{
    auto input = grid_image(120U, 80U);
    std::fill(input.rgb.begin(), input.rgb.end(), 0.375F);
    const auto original = input.rgb;
    PerspectiveParams params;
    params.rotation_degrees = 7.0;
    params.vertical_shift = 0.24;
    params.horizontal_shift = -0.13;
    params.shear = 0.04;
    params.constrain_crop = true;
    for (const auto interpolation : {kPerspectiveInterpolationBilinear,
                                     kPerspectiveInterpolationLanczos2,
                                     kPerspectiveInterpolationLanczos3})
    {
        params.interpolation = std::string(interpolation);
        auto output = apply_perspective(input, params, CancellationToken{});
        ASSERT_TRUE(output) << output.error().message;
        EXPECT_LT(output.value().width, 120U * 4U);
        EXPECT_LT(output.value().height, 80U * 4U);
        EXPECT_TRUE(std::all_of(output.value().rgb.begin(), output.value().rgb.end(),
                                [](const float sample)
                                { return std::isfinite(sample) && sample > 0.35F; }));
    }
    EXPECT_EQ(input.rgb, original);
}

TEST(PerspectiveTest, InterpolatorsHaveIndependentDeterministicGridGoldens)
{
    const auto input = grid_image();
    PerspectiveParams params;
    params.rotation_degrees = -3.25;
    params.vertical_shift = 0.17;
    params.horizontal_shift = 0.11;
    params.shear = -0.025;
    params.constrain_crop = false;
    std::vector<std::uint64_t> hashes;
    for (const auto interpolation : {kPerspectiveInterpolationBilinear,
                                     kPerspectiveInterpolationLanczos2,
                                     kPerspectiveInterpolationLanczos3})
    {
        params.interpolation = std::string(interpolation);
        auto first = apply_perspective(input, params, CancellationToken{});
        auto second = apply_perspective(input, params, CancellationToken{});
        ASSERT_TRUE(first) << first.error().message;
        ASSERT_TRUE(second) << second.error().message;
        EXPECT_EQ(first.value().rgb, second.value().rgb);
        hashes.push_back(quantized_hash(first.value()));
    }
    EXPECT_NE(hashes[0], hashes[1]);
    EXPECT_NE(hashes[1], hashes[2]);
    EXPECT_EQ(hashes, (std::vector<std::uint64_t>{16313785851501956540ULL,
                                                  7650514657445475949ULL,
                                                  13807960239381974773ULL}));
}

TEST(PerspectiveTest, InvalidInputAndCancellationFailWithoutPublication)
{
    auto invalid = grid_image();
    invalid.rgb[17] = std::numeric_limits<float>::quiet_NaN();
    auto nonfinite = apply_perspective(invalid, PerspectiveParams{}, CancellationToken{});
    ASSERT_FALSE(nonfinite);
    EXPECT_EQ(nonfinite.error().context.at("reason"), "nonfinite_perspective_input");

    CancellationSource source;
    ASSERT_TRUE(source.cancel("perspective_test_cancelled"));
    PerspectiveParams params;
    params.rotation_degrees = 1.0;
    auto cancelled = apply_perspective(grid_image(), params, source.token());
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, ErrorCode::kCancelled);
}

TEST(PerspectiveAutoTest, RobustGuideFitRecoversKnownHomography)
{
    constexpr std::uint32_t width = 400U;
    constexpr std::uint32_t height = 300U;
    PerspectiveParams known;
    known.rotation_degrees = 4.0;
    known.vertical_shift = 0.24;
    known.horizontal_shift = -0.16;
    known.shear = 0.025;
    known.constrain_crop = false;
    auto layout = compute_perspective_layout(width, height, known);
    ASSERT_TRUE(layout) << layout.error().message;
    const auto safe = layout.value().safe_crop;
    std::vector<PerspectiveGuideLine> lines;
    for (const double fraction : {0.2, 0.5, 0.8})
    {
        const double x = safe.left + safe.width() * fraction;
        const auto first = map_point(layout.value().inverse, x, safe.top);
        const auto second = map_point(layout.value().inverse, x, safe.bottom);
        lines.push_back({first.x, first.y, second.x, second.y, 100.0,
                         PerspectiveGuideOrientation::kVertical});
    }
    for (const double fraction : {0.2, 0.5, 0.8})
    {
        const double y = safe.top + safe.height() * fraction;
        const auto first = map_point(layout.value().inverse, safe.left, y);
        const auto second = map_point(layout.value().inverse, safe.right, y);
        lines.push_back({first.x, first.y, second.x, second.y, 100.0,
                         PerspectiveGuideOrientation::kHorizontal});
    }
    // One high-weight diagonal outlier must not own the solution.
    lines.push_back({20.0, 30.0, 360.0, 260.0, 1000.0,
                     PerspectiveGuideOrientation::kHorizontal});
    auto fitted = fit_perspective_guides(width, height, lines, PerspectiveAnalysisMode::kFull,
                                         CancellationToken{});
    ASSERT_TRUE(fitted) << fitted.error().message;
    EXPECT_NEAR(fitted.value().params.rotation_degrees, known.rotation_degrees, 0.2);
    EXPECT_NEAR(fitted.value().params.vertical_shift, known.vertical_shift, 0.02);
    EXPECT_NEAR(fitted.value().params.horizontal_shift, known.horizontal_shift, 0.02);
    EXPECT_NEAR(fitted.value().params.shear, known.shear, 0.01);
    EXPECT_LT(fitted.value().residual_degrees, 0.3);
}

TEST(PerspectiveAutoTest, DetectorFindsBothAxisFamiliesAndReportsNoSolution)
{
    auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    const auto raster = axis_grid_raster();
    auto analysis = engine.value().analyze_perspective(
        raster, PerspectiveAnalysisMode::kFull, CancellationToken{});
    ASSERT_TRUE(analysis) << analysis.error().message;
    EXPECT_GE(analysis.value().vertical_line_count, 2U);
    EXPECT_GE(analysis.value().horizontal_line_count, 2U);
    EXPECT_NEAR(analysis.value().params.rotation_degrees, 0.0, 0.2);
    EXPECT_NEAR(analysis.value().params.vertical_shift, 0.0, 0.02);
    EXPECT_NEAR(analysis.value().params.horizontal_shift, 0.0, 0.02);

    RasterBuffer blank = raster;
    std::fill(blank.srgb.begin(), blank.srgb.end(), 64U);
    auto missing = engine.value().analyze_perspective(
        blank, PerspectiveAnalysisMode::kFull, CancellationToken{});
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, ErrorCode::kNotFound);
    EXPECT_EQ(missing.error().context.at("reason"), "no_perspective_lines");
}

} // namespace
} // namespace ravo
