#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "ravo/adapters/legacy_xmp.h"
#include "ravo/engine/engine.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/color.h"
#include "ravo/recipe/color_input.h"
#include "ravo/recipe/color_output.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/primaries.h"

#include "image_ops.h"
#include "input_color.h"
#include "primaries.h"

namespace ravo
{
namespace
{

using Matrix3 = std::array<double, 9>;

struct Chromaticity
{
    double x = 0.0;
    double y = 0.0;
};

struct WorkingGeometry
{
    std::array<Chromaticity, 3> primaries{};
    Chromaticity whitepoint{};
};

constexpr std::array<float, 9> kSrgbToXyzD50{0.4360747F, 0.3850649F, 0.1430804F,
                                             0.2225045F, 0.7168786F, 0.0606169F,
                                             0.0139322F, 0.0971045F, 0.7141733F};

[[nodiscard]] Matrix3 to_double_matrix(const std::array<float, 9> &matrix)
{
    Matrix3 result{};
    for (std::size_t index = 0; index < matrix.size(); ++index)
    {
        result[index] = matrix[index];
    }
    return result;
}

[[nodiscard]] std::array<double, 3> apply_matrix(const Matrix3 &matrix,
                                                 const std::array<double, 3> &value)
{
    return {matrix[0] * value[0] + matrix[1] * value[1] + matrix[2] * value[2],
            matrix[3] * value[0] + matrix[4] * value[1] + matrix[5] * value[2],
            matrix[6] * value[0] + matrix[7] * value[1] + matrix[8] * value[2]};
}

[[nodiscard]] Chromaticity xy_from_xyz(const std::array<double, 3> &xyz)
{
    const double sum = xyz[0] + xyz[1] + xyz[2];
    return {xyz[0] / sum, xyz[1] / sum};
}

[[nodiscard]] Chromaticity subtract(const Chromaticity left, const Chromaticity right)
{
    return {left.x - right.x, left.y - right.y};
}

[[nodiscard]] double cross(const Chromaticity left, const Chromaticity right)
{
    return left.x * right.y - left.y * right.x;
}

[[nodiscard]] WorkingGeometry geometry_from_matrix(const Matrix3 &matrix)
{
    WorkingGeometry result;
    for (std::size_t primary = 0; primary < result.primaries.size(); ++primary)
    {
        result.primaries[primary] =
            xy_from_xyz({matrix[primary], matrix[3U + primary], matrix[6U + primary]});
    }
    result.whitepoint =
        xy_from_xyz({matrix[0] + matrix[1] + matrix[2], matrix[3] + matrix[4] + matrix[5],
                     matrix[6] + matrix[7] + matrix[8]});
    return result;
}

[[nodiscard]] std::optional<Matrix3> invert_matrix(const Matrix3 &matrix)
{
    const double a = matrix[0];
    const double b = matrix[1];
    const double c = matrix[2];
    const double d = matrix[3];
    const double e = matrix[4];
    const double f = matrix[5];
    const double g = matrix[6];
    const double h = matrix[7];
    const double i = matrix[8];
    const double determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!std::isfinite(determinant) || std::abs(determinant) < 1.0e-12)
    {
        return std::nullopt;
    }
    const double inverse = 1.0 / determinant;
    return Matrix3{(e * i - f * h) * inverse, (c * h - b * i) * inverse, (b * f - c * e) * inverse,
                   (f * g - d * i) * inverse, (a * i - c * g) * inverse, (c * d - a * f) * inverse,
                   (d * h - e * g) * inverse, (b * g - a * h) * inverse, (a * e - b * d) * inverse};
}

[[nodiscard]] std::optional<Matrix3>
matrix_from_primaries_and_whitepoint(const std::array<Chromaticity, 3> &primaries,
                                     const Chromaticity whitepoint)
{
    Matrix3 primary_matrix{};
    for (std::size_t primary = 0; primary < primaries.size(); ++primary)
    {
        primary_matrix[primary] = primaries[primary].x / primaries[primary].y;
        primary_matrix[3U + primary] = 1.0;
        primary_matrix[6U + primary] =
            (1.0 - primaries[primary].x - primaries[primary].y) / primaries[primary].y;
    }
    const auto inverse = invert_matrix(primary_matrix);
    if (!inverse)
    {
        return std::nullopt;
    }
    const std::array<double, 3> white_xyz{whitepoint.x / whitepoint.y, 1.0,
                                          (1.0 - whitepoint.x - whitepoint.y) / whitepoint.y};
    const std::array<double, 3> scale = apply_matrix(*inverse, white_xyz);
    Matrix3 result{};
    for (std::size_t row = 0; row < 3U; ++row)
    {
        for (std::size_t column = 0; column < 3U; ++column)
        {
            result[row * 3U + column] = primary_matrix[row * 3U + column] * scale[column];
        }
    }
    return result;
}

[[nodiscard]] double ray_distance(const WorkingGeometry &geometry, const double angle)
{
    const Chromaticity direction{std::cos(angle), std::sin(angle)};
    double nearest = std::numeric_limits<double>::infinity();
    for (std::size_t edge = 0; edge < geometry.primaries.size(); ++edge)
    {
        const Chromaticity start = geometry.primaries[edge];
        const Chromaticity end = geometry.primaries[(edge + 1U) % 3U];
        const Chromaticity edge_direction = subtract(end, start);
        const Chromaticity origin_to_edge = subtract(start, geometry.whitepoint);
        const double denominator = cross(direction, edge_direction);
        if (std::abs(denominator) < 1.0e-12)
        {
            continue;
        }
        const double distance = cross(origin_to_edge, edge_direction) / denominator;
        if (distance > 1.0e-12)
        {
            nearest = std::min(nearest, distance);
        }
    }
    return nearest;
}

[[nodiscard]] Chromaticity expected_rotated_primary(const WorkingGeometry &geometry,
                                                    const std::size_t primary, const double purity,
                                                    const double hue)
{
    const Chromaticity offset = subtract(geometry.primaries[primary], geometry.whitepoint);
    const double angle = std::atan2(offset.y, offset.x) + hue;
    const double distance = ray_distance(geometry, angle);
    return {geometry.whitepoint.x + purity * distance * std::cos(angle),
            geometry.whitepoint.y + purity * distance * std::sin(angle)};
}

[[nodiscard]] ColorProfileState make_profile(const std::array<float, 9> &matrix,
                                             const std::string identifier)
{
    ColorProfileState profile;
    profile.kind = ColorProfileKind::kMatrix;
    profile.model = ColorModel::kRgb;
    profile.identifier = identifier;
    profile.matrix_to_xyz_d50 = matrix;
    profile.has_matrix = true;
    return profile;
}

[[nodiscard]] ColorProfileState make_profile(const Matrix3 &matrix, const std::string identifier)
{
    std::array<float, 9> converted{};
    for (std::size_t index = 0; index < converted.size(); ++index)
    {
        converted[index] = static_cast<float>(matrix[index]);
    }
    return make_profile(converted, identifier);
}

[[nodiscard]] WorkingImage make_image(const ColorProfileState &profile,
                                      const std::vector<float> &pixels, const std::uint32_t width,
                                      const std::uint32_t height)
{
    WorkingImage image;
    image.width = width;
    image.height = height;
    image.rgb = pixels;
    image.color_profile = profile;
    return image;
}

[[nodiscard]] OperationInstance primaries_operation(const PrimariesParams &params)
{
    return {std::string(kPrimariesOperationId), 1,           "primaries-test", true,
            primaries_to_parameters(params),    std::nullopt};
}

[[nodiscard]] std::array<std::uint64_t, 3> channel_sums(const RenderedImage &image)
{
    std::array<std::uint64_t, 3> result{};
    for (std::size_t index = 0; index + 2U < image.rgb.size(); index += 3U)
    {
        for (std::size_t channel = 0; channel < result.size(); ++channel)
        {
            result[channel] += image.rgb[index + channel];
        }
    }
    return result;
}

[[nodiscard]] std::string mire1_path()
{
    const auto path =
        std::filesystem::path(RAVO_REPOSITORY_ROOT) / "Ravo" / "tests" / "fixtures" / "frozen" / "images" / "mire1.cr2";
    const auto utf8 = path.generic_u8string();
    return {utf8.begin(), utf8.end()};
}

[[nodiscard]] OperationInstance sigmoid_operation()
{
    return {"ravo.display.sigmoid",
            1,
            "sigmoid-test",
            true,
            {{"working_space", ParameterValue{std::string(kSigmoidWorkingSpaceLinearSrgb)}},
             {"color_processing", ParameterValue{std::string(kSigmoidColorProcessingPerChannel)}},
             {"middle_grey_contrast", ParameterValue{kSigmoidContrastDefault}},
             {"contrast_skewness", ParameterValue{kSigmoidSkewDefault}},
             {"display_white_target", ParameterValue{kSigmoidDisplayWhiteDefault}},
             {"display_black_target", ParameterValue{kSigmoidDisplayBlackDefault}},
             {"hue_preservation", ParameterValue{kSigmoidHuePreservationDefault}}},
            std::nullopt};
}

[[nodiscard]] Recipe recipe_with_primaries(const PrimariesParams &params)
{
    Recipe recipe;
    recipe.asset = {"mire1", mire1_path(), std::nullopt};
    recipe.operations = {
        {"ravo.color.input", 1, "input-test", true, input_color_to_parameters(InputColorParams{}),
         std::nullopt},
        primaries_operation(params),
        sigmoid_operation(),
        {"ravo.color.output", 1, "output-test", true,
         output_color_to_parameters(OutputColorParams{}), std::nullopt},
    };
    return recipe;
}

TEST(PrimariesTest, DefaultParametersAreAnExactIdentityAndKeepTheWorkingProfile)
{
    const auto profile = make_profile(kSrgbToXyzD50, "synthetic-srgb-d50");
    const auto input = make_image(profile, {0.15F, 0.20F, 0.30F, 0.75F, 0.50F, 0.25F}, 2U, 1U);

    const auto matrix = primaries_adjustment_matrix(input.color_profile, PrimariesParams{});
    ASSERT_TRUE(matrix) << matrix.error().message;
    EXPECT_EQ(matrix.value(), (Matrix3{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0}));

    const auto output = apply_primaries(input, PrimariesParams{}, CancellationToken{});
    ASSERT_TRUE(output) << output.error().message;
    EXPECT_EQ(output.value().rgb, input.rgb);
    EXPECT_EQ(output.value().color_profile, input.color_profile);
}

TEST(PrimariesTest, EachPrimaryHueAndPurityFollowTheWorkingTriangleBoundary)
{
    const auto profile = make_profile(kSrgbToXyzD50, "synthetic-srgb-d50");
    const Matrix3 matrix = to_double_matrix(profile.matrix_to_xyz_d50);
    const WorkingGeometry geometry = geometry_from_matrix(matrix);
    const std::array<double, 3> hues{0.21, -0.17, 0.29};
    const std::array<double, 3> purities{0.63, 0.71, 0.82};

    for (std::size_t primary = 0; primary < 3U; ++primary)
    {
        PrimariesParams params;
        switch (primary)
        {
        case 0U:
            params.red_hue = hues[primary];
            params.red_purity = purities[primary];
            break;
        case 1U:
            params.green_hue = hues[primary];
            params.green_purity = purities[primary];
            break;
        case 2U:
            params.blue_hue = hues[primary];
            params.blue_purity = purities[primary];
            break;
        default:
            break;
        }
        std::vector<float> basis(3U, 0.0F);
        basis[primary] = 1.0F;
        const auto input = make_image(profile, basis, 1U, 1U);
        const auto output = apply_primaries(input, params, CancellationToken{});
        ASSERT_TRUE(output) << output.error().message;
        const Chromaticity actual = xy_from_xyz(apply_matrix(
            matrix, {output.value().rgb[0], output.value().rgb[1], output.value().rgb[2]}));
        const Chromaticity expected =
            expected_rotated_primary(geometry, primary, purities[primary], hues[primary]);
        EXPECT_NEAR(actual.x, expected.x, 2.0e-6) << "primary=" << primary;
        EXPECT_NEAR(actual.y, expected.y, 2.0e-6) << "primary=" << primary;
    }
}

TEST(PrimariesTest, AchromaticTintUsesTheRedAnchoredTriangleRay)
{
    const auto profile = make_profile(kSrgbToXyzD50, "synthetic-srgb-d50");
    const Matrix3 matrix = to_double_matrix(profile.matrix_to_xyz_d50);
    const WorkingGeometry geometry = geometry_from_matrix(matrix);
    PrimariesParams params;
    params.achromatic_tint_hue = -0.26;
    params.achromatic_tint_purity = 0.38;
    const auto input = make_image(profile, {1.0F, 1.0F, 1.0F}, 1U, 1U);

    const auto output = apply_primaries(input, params, CancellationToken{});
    ASSERT_TRUE(output) << output.error().message;
    const Chromaticity actual = xy_from_xyz(apply_matrix(
        matrix, {output.value().rgb[0], output.value().rgb[1], output.value().rgb[2]}));
    const Chromaticity expected = expected_rotated_primary(
        geometry, 0U, params.achromatic_tint_purity, params.achromatic_tint_hue);
    EXPECT_NEAR(actual.x, expected.x, 2.0e-6);
    EXPECT_NEAR(actual.y, expected.y, 2.0e-6);
}

TEST(PrimariesTest, CustomRgbToXyzMatrixUsesAndRetainsAnAlternativeWorkingProfile)
{
    const std::array<Chromaticity, 3> primaries{
        Chromaticity{0.708, 0.292}, Chromaticity{0.170, 0.797}, Chromaticity{0.131, 0.046}};
    const auto alternative_matrix =
        matrix_from_primaries_and_whitepoint(primaries, {0.3457, 0.3585});
    ASSERT_TRUE(alternative_matrix.has_value());
    const auto profile = make_profile(*alternative_matrix, "synthetic-rec2020-d50");
    const Matrix3 matrix = to_double_matrix(profile.matrix_to_xyz_d50);
    const WorkingGeometry geometry = geometry_from_matrix(matrix);
    PrimariesParams params;
    params.red_hue = 0.18;
    params.red_purity = 1.30;
    const auto input = make_image(profile, {1.0F, 0.0F, 0.0F}, 1U, 1U);

    const auto adjustment = primaries_adjustment_matrix(profile, params);
    ASSERT_TRUE(adjustment) << adjustment.error().message;
    EXPECT_NE(adjustment.value(), (Matrix3{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0}));
    const auto output = apply_primaries(input, params, CancellationToken{});
    ASSERT_TRUE(output) << output.error().message;
    EXPECT_EQ(output.value().color_profile, profile);
    const Chromaticity actual = xy_from_xyz(apply_matrix(
        matrix, {output.value().rgb[0], output.value().rgb[1], output.value().rgb[2]}));
    const Chromaticity expected =
        expected_rotated_primary(geometry, 0U, params.red_purity, params.red_hue);
    EXPECT_NEAR(actual.x, expected.x, 2.0e-6);
    EXPECT_NEAR(actual.y, expected.y, 2.0e-6);
}

TEST(PrimariesTest, InvalidStateAndParametersFailWithoutMutatingTheInput)
{
    const auto profile = make_profile(kSrgbToXyzD50, "synthetic-srgb-d50");
    const auto input = make_image(profile, {0.1F, 0.3F, 0.7F}, 1U, 1U);
    const auto original = input.rgb;

    auto missing_profile = input;
    missing_profile.color_profile.has_matrix = false;
    const auto missing = apply_primaries(missing_profile, PrimariesParams{}, CancellationToken{});
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, ErrorCode::kValidation);
    EXPECT_EQ(missing_profile.rgb, original);

    auto nonfinite_profile = input;
    nonfinite_profile.color_profile.matrix_to_xyz_d50[0] = std::numeric_limits<float>::quiet_NaN();
    const auto nonfinite =
        apply_primaries(nonfinite_profile, PrimariesParams{}, CancellationToken{});
    ASSERT_FALSE(nonfinite);
    EXPECT_EQ(nonfinite.error().code, ErrorCode::kValidation);
    EXPECT_EQ(nonfinite_profile.rgb, original);

    auto singular_profile = input;
    singular_profile.color_profile.matrix_to_xyz_d50.fill(0.0F);
    const auto singular = apply_primaries(singular_profile, PrimariesParams{}, CancellationToken{});
    ASSERT_FALSE(singular);
    EXPECT_EQ(singular.error().code, ErrorCode::kValidation);
    EXPECT_EQ(singular_profile.rgb, original);

    auto invalid_dimensions = input;
    invalid_dimensions.width = 2U;
    const auto dimensions =
        apply_primaries(invalid_dimensions, PrimariesParams{}, CancellationToken{});
    ASSERT_FALSE(dimensions);
    EXPECT_EQ(dimensions.error().code, ErrorCode::kValidation);
    EXPECT_EQ(invalid_dimensions.rgb, original);

    auto nonfinite_input = input;
    nonfinite_input.rgb[1] = std::numeric_limits<float>::infinity();
    const auto input_failure =
        apply_primaries(nonfinite_input, PrimariesParams{}, CancellationToken{});
    ASSERT_FALSE(input_failure);
    EXPECT_EQ(input_failure.error().code, ErrorCode::kValidation);
    EXPECT_TRUE(std::isinf(nonfinite_input.rgb[1]));

    PrimariesParams out_of_range;
    out_of_range.red_purity = 5.1;
    const auto parameters = apply_primaries(input, out_of_range, CancellationToken{});
    ASSERT_FALSE(parameters);
    EXPECT_EQ(parameters.error().code, ErrorCode::kValidation);
    EXPECT_EQ(input.rgb, original);
}

TEST(PrimariesTest, BackwardsAndSingularCustomGeometryFailStructurally)
{
    const std::array<Chromaticity, 3> outside_primaries{
        Chromaticity{0.70, 0.20}, Chromaticity{0.10, 0.80}, Chromaticity{0.10, 0.10}};
    const auto backwards_matrix =
        matrix_from_primaries_and_whitepoint(outside_primaries, {0.05, 0.05});
    ASSERT_TRUE(backwards_matrix.has_value());
    const auto backwards_profile = make_profile(*backwards_matrix, "white-outside-triangle");
    const auto backwards_input = make_image(backwards_profile, {1.0F, 0.0F, 0.0F}, 1U, 1U);
    PrimariesParams backwards_params;
    backwards_params.red_hue = std::numbers::pi;
    const auto backwards = apply_primaries(backwards_input, backwards_params, CancellationToken{});
    ASSERT_FALSE(backwards);
    EXPECT_EQ(backwards.error().code, ErrorCode::kValidation);
    EXPECT_EQ(backwards_input.rgb, (std::vector<float>{1.0F, 0.0F, 0.0F}));

    const auto standard_profile = make_profile(kSrgbToXyzD50, "synthetic-srgb-d50");
    const WorkingGeometry geometry = geometry_from_matrix(to_double_matrix(kSrgbToXyzD50));
    const double common_angle = 0.0;
    PrimariesParams singular_params;
    singular_params.red_hue =
        common_angle - std::atan2(geometry.primaries[0].y - geometry.whitepoint.y,
                                  geometry.primaries[0].x - geometry.whitepoint.x);
    singular_params.green_hue =
        common_angle - std::atan2(geometry.primaries[1].y - geometry.whitepoint.y,
                                  geometry.primaries[1].x - geometry.whitepoint.x);
    singular_params.blue_hue =
        common_angle - std::atan2(geometry.primaries[2].y - geometry.whitepoint.y,
                                  geometry.primaries[2].x - geometry.whitepoint.x);
    const auto singular_custom = primaries_adjustment_matrix(standard_profile, singular_params);
    ASSERT_FALSE(singular_custom);
    EXPECT_EQ(singular_custom.error().code, ErrorCode::kValidation);
}

TEST(PrimariesTest, FloatOverflowCancellationAndOperationDispatchDoNotPublishPartialPixels)
{
    const auto profile = make_profile(kSrgbToXyzD50, "synthetic-srgb-d50");
    PrimariesParams params;
    params.red_purity = kPrimariesPrimaryPurityMax;
    const auto adjustment = primaries_adjustment_matrix(profile, params);
    ASSERT_TRUE(adjustment) << adjustment.error().message;
    std::size_t strongest_column = 0U;
    double strongest = 0.0;
    for (std::size_t index = 0; index < adjustment.value().size(); ++index)
    {
        if (std::abs(adjustment.value()[index]) > strongest)
        {
            strongest = std::abs(adjustment.value()[index]);
            strongest_column = index % 3U;
        }
    }
    ASSERT_GT(strongest, 1.0);
    std::vector<float> large_pixels(3U, 0.0F);
    large_pixels[strongest_column] = std::numeric_limits<float>::max();
    const auto overflow_input = make_image(profile, large_pixels, 1U, 1U);
    const auto overflow = apply_primaries(overflow_input, params, CancellationToken{});
    ASSERT_FALSE(overflow);
    EXPECT_EQ(overflow.error().code, ErrorCode::kValidation);
    EXPECT_EQ(overflow_input.rgb, large_pixels);

    const auto input = make_image(profile, {0.1F, 0.2F, 0.3F, 0.4F, 0.5F, 0.6F}, 1U, 2U);
    CancellationSource cancellation;
    ASSERT_TRUE(cancellation.cancel("primaries-test-cancel"));
    const auto cancelled = apply_primaries(input, params, cancellation.token());
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, ErrorCode::kCancelled);
    EXPECT_EQ(input.rgb, (std::vector<float>{0.1F, 0.2F, 0.3F, 0.4F, 0.5F, 0.6F}));

    auto large_image = make_image(profile, {}, 1024U, 4096U);
    large_image.rgb.assign(static_cast<std::size_t>(large_image.width) * large_image.height * 3U,
                           0.5F);
    const auto large_original = large_image.rgb;
    const auto deadline = CancellationSource::with_deadline(std::chrono::steady_clock::now() +
                                                            std::chrono::milliseconds{1});
    const auto row_cancelled = apply_primaries(large_image, params, deadline.token());
    ASSERT_FALSE(row_cancelled);
    EXPECT_EQ(row_cancelled.error().code, ErrorCode::kCancelled);
    EXPECT_EQ(large_image.rgb, large_original);

    Recipe recipe;
    recipe.operations.push_back(primaries_operation(params));
    const auto dispatched = apply_recipe_ops(input, recipe, CancellationToken{});
    ASSERT_TRUE(dispatched) << dispatched.error().message;
    EXPECT_NE(dispatched.value().rgb, input.rgb);
    EXPECT_EQ(dispatched.value().color_profile, input.color_profile);
}

TEST(PrimariesTest, FacadeAppliesPrimariesBeforeBridgingAnAlternativeWorkingProfile)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;

    RasterBuffer raster;
    raster.width = 2U;
    raster.height = 2U;
    raster.srgb = {220U, 25U, 20U, 25U, 205U, 45U, 20U, 50U, 220U, 180U, 150U, 30U};
    raster.color_profile.kind = ColorProfileKind::kBuiltin;
    raster.color_profile.model = ColorModel::kRgb;
    raster.color_profile.identifier = std::string(kInputProfileSrgb);

    PrimariesParams params;
    params.red_hue = 0.37;
    params.red_purity = 1.45;
    params.green_hue = -0.22;
    params.green_purity = 0.78;
    params.blue_hue = 0.16;
    params.blue_purity = 1.31;

    InputColorParams input_color;
    input_color.working_profile = std::string(kInputProfileLinearRec2020);
    Recipe recipe;
    recipe.asset = {"raster", "memory:primaries-scheduling", std::nullopt};
    recipe.operations = {
        {"ravo.color.input", 1, "input-scheduling", true, input_color_to_parameters(input_color),
         std::nullopt},
        primaries_operation(params),
        {"ravo.color.output", 1, "output-scheduling", true,
         output_color_to_parameters(OutputColorParams{}), std::nullopt},
    };

    RenderRequest request;
    request.asset = recipe.asset;
    request.recipe = recipe;
    const auto facade_result = engine.value().render_to_image(request, &raster);
    ASSERT_TRUE(facade_result) << facade_result.error().message;

    const auto working =
        engine.value().linear_working_from_raster(raster, recipe, CancellationToken{});
    ASSERT_TRUE(working) << working.error().message;
    EXPECT_EQ(working.value().color_profile.identifier, kInputProfileLinearRec2020);
    const auto pre_bridge =
        apply_primaries(working.value(), recipe.operations[1], CancellationToken{});
    ASSERT_TRUE(pre_bridge) << pre_bridge.error().message;
    EXPECT_EQ(pre_bridge.value().color_profile.identifier, kInputProfileLinearRec2020);
    const auto expected_working =
        convert_working_profile(pre_bridge.value(), kInputProfileLinearRec709, CancellationToken{});
    ASSERT_TRUE(expected_working) << expected_working.error().message;
    Recipe remaining = recipe;
    remaining.operations[1].enabled = false;
    const auto expected = engine.value().render_linear_working(expected_working.value(), remaining,
                                                               CancellationToken{});
    ASSERT_TRUE(expected) << expected.error().message;
    EXPECT_EQ(facade_result.value().rgb, expected.value().rgb);

    const auto bridge_first =
        convert_working_profile(working.value(), kInputProfileLinearRec709, CancellationToken{});
    ASSERT_TRUE(bridge_first) << bridge_first.error().message;
    const auto incorrectly_post_bridge =
        apply_primaries(bridge_first.value(), recipe.operations[1], CancellationToken{});
    ASSERT_TRUE(incorrectly_post_bridge) << incorrectly_post_bridge.error().message;
    const auto wrong = engine.value().render_linear_working(incorrectly_post_bridge.value(),
                                                            remaining, CancellationToken{});
    ASSERT_TRUE(wrong) << wrong.error().message;
    EXPECT_NE(facade_result.value().rgb, wrong.value().rgb);
}

TEST(PrimariesTest, Frozen0152PayloadHasAMire1ChannelSumReference)
{
    constexpr std::string_view kFrozen0152Payload =
        "cc56143f4c37093e22c9293ed9ce573f448d12be7b149e3f1e206a3e85eb513f";
    const auto params = decode_legacy_primaries_v1_parameters(kFrozen0152Payload);
    ASSERT_TRUE(params) << params.error().message;
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    const Recipe recipe = recipe_with_primaries(params.value());
    RenderRequest request;
    request.asset = recipe.asset;
    request.recipe = recipe;
    request.output_width = 64U;
    request.output_height = 48U;
    const auto rendered = engine.value().render_to_image(request, nullptr);
    ASSERT_TRUE(rendered) << rendered.error().message;
    EXPECT_EQ(rendered.value().width, 64U);
    EXPECT_EQ(rendered.value().height, 48U);
    const auto sums = channel_sums(rendered.value());
    // Ravo-owned macOS reference for frozen schema-v1 0152 parameters on
    // mire1.cr2 after the default RCD demosaic.
    EXPECT_NEAR(static_cast<double>(sums[0]), 325147.0, 2000.0);
    EXPECT_NEAR(static_cast<double>(sums[1]), 283604.0, 2000.0);
    EXPECT_NEAR(static_cast<double>(sums[2]), 243133.0, 2000.0);
}

} // namespace
} // namespace ravo
