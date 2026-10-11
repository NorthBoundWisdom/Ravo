#include <gtest/gtest.h>

#include <bit>
#include <cstdint>
#include <memory>
#include <utility>

#include "capability_ops.h"
#include "parallel_rows.h"
#include "profile_denoise_preview.h"
#include "ravo/recipe/develop.h"

namespace ravo
{
namespace
{
WorkingImage denoise_input(const std::uint32_t width = 33U, const std::uint32_t height = 65U)
{
    WorkingImage input;
    input.width = width;
    input.height = height;
    input.color_profile.kind = ColorProfileKind::kBuiltin;
    input.color_profile.model = ColorModel::kRgb;
    input.color_profile.identifier = "linear_rec709";
    input.exposure_analysis = std::make_shared<ExposureAnalysisContext>();
    input.canonical_roi_scale =
        CanonicalRoiScale::from_scaled_dimensions(width, height, width * 3U, height * 3U);
    input.rgb.resize(static_cast<std::size_t>(width) * height * 3U);
    for (std::size_t i = 0U; i < input.rgb.size(); ++i)
        input.rgb[i] =
            static_cast<float>((static_cast<std::uint32_t>(i) * 1664525U + 1013904223U) % 65521U) /
                16384.0F -
            0.2F;
    return input;
}

OperationInstance denoise_operation(const double radius = 1.0)
{
    return {"ravo.detail.denoiseprofile",
            1,
            "denoise",
            true,
            {{"strength", ParameterValue{0.31}},
             {"chroma", ParameterValue{0.17}},
             {"radius", ParameterValue{radius}}},
            std::nullopt};
}

void expect_float_bits(const WorkingImage &actual, const WorkingImage &expected)
{
    ASSERT_EQ(actual.width, expected.width);
    ASSERT_EQ(actual.height, expected.height);
    EXPECT_EQ(actual.color_profile, expected.color_profile);
    EXPECT_EQ(actual.exposure_analysis, expected.exposure_analysis);
    ASSERT_EQ(actual.rgb.size(), expected.rgb.size());
    for (std::size_t i = 0U; i < actual.rgb.size(); ++i)
        ASSERT_EQ(std::bit_cast<std::uint32_t>(actual.rgb[i]),
                  std::bit_cast<std::uint32_t>(expected.rgb[i]))
            << i;
}

TEST(ProfileDenoisePreviewTest, CapturedBasisMatchesOrdinaryFloatBitsAcrossMixAndTrueDependencies)
{
    auto rows = detail::ScopedParallelRowSession::create();
    ASSERT_TRUE(rows);
    const auto input = denoise_input();
    const auto original = input.rgb;
    detail::ProfileDenoisePreviewCache cache;
    for (const double radius : {0.5, 1.2, 3.0})
    {
        auto operation = denoise_operation(radius);
        operation.parameters["shadows"] = ParameterValue{1.4};
        operation.parameters["bias"] = ParameterValue{0.05};
        operation.parameters["noise_a"] = ParameterValue{0.0004};
        operation.parameters["noise_b"] = ParameterValue{0.0002};
        const auto key = std::to_string(radius);
        auto first = cache.prepare(input, operation, key, {});
        ASSERT_TRUE(first) << first.error().message;
        auto reference = input;
        ASSERT_TRUE(apply_denoise_profile(reference, operation, {}));
        expect_float_bits(first.value(), reference);
        ASSERT_TRUE(cache.matches(key));
        EXPECT_GT(cache.pixel_bytes(), 0U);
        EXPECT_LE(cache.pixel_bytes(), detail::ProfileDenoisePreviewCache::kPixelByteLimit);
        const auto generation = cache.generation();
        for (const double strength : {0.0, 0.01, 0.48, 0.99})
        {
            for (const double chroma : {0.0, 0.25, 1.0})
            {
                operation.parameters["strength"] = ParameterValue{strength};
                operation.parameters["chroma"] = ParameterValue{chroma};
                auto mixed = cache.mix(operation, {});
                ASSERT_TRUE(mixed) << mixed.error().message;
                reference = input;
                ASSERT_TRUE(apply_denoise_profile(reference, operation, {}));
                expect_float_bits(mixed.value(), reference);
                EXPECT_EQ(cache.generation(), generation);
                EXPECT_EQ(input.rgb, original);
            }
        }
    }
    auto moved = std::move(cache);
    EXPECT_EQ(cache.pixel_bytes(), 0U);
    EXPECT_GT(moved.pixel_bytes(), 0U);
    moved.clear();
    auto absent = moved.mix(denoise_operation(), {});
    ASSERT_FALSE(absent);
    EXPECT_EQ(absent.error().context.at("reason"), "denoise_preview_basis_unavailable");
}

TEST(ProfileDenoisePreviewTest, AdmissionNoOpsErrorsAndCancellationPublishNoPartialBasis)
{
    detail::ProfileDenoisePreviewCache cache;
    auto input = denoise_input();
    auto operation = denoise_operation();
    ASSERT_TRUE(cache.prepare(input, operation, "input-a", {}));
    const auto generation = cache.generation();
    CancellationSource cancellation;
    ASSERT_TRUE(cancellation.cancel("basis-test"));
    auto cancelled = cache.prepare(input, operation, "input-b", cancellation.token());
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, ErrorCode::kCancelled);
    EXPECT_TRUE(cache.matches("input-a"));
    EXPECT_EQ(cache.generation(), generation);
    cancelled = cache.mix(operation, cancellation.token());
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, ErrorCode::kCancelled);
    operation.parameters["strength"] = ParameterValue{2.0};
    auto invalid_mix = cache.mix(operation, {});
    ASSERT_FALSE(invalid_mix);
    EXPECT_EQ(invalid_mix.error().code, ErrorCode::kValidation);
    auto rejected = cache.prepare(input, operation, "invalid", {});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(cache.pixel_bytes(), 0U);
    EXPECT_EQ(cache.generation(), generation);
    operation = denoise_operation();
    operation.mask_id = "mask";
    rejected = cache.prepare(input, operation, "masked", {});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "unsupported_denoise_preview_basis_operation");
    operation.mask_id.reset();
    auto alias = apply_denoise_profile(input, operation, {}, &input.rgb);
    ASSERT_FALSE(alias);
    EXPECT_EQ(alias.error().context.at("reason"), "aliased_profile_denoise_candidate");
    for (const auto &tiny : {denoise_input(1U, 1U), denoise_input(7U, 11U)})
    {
        auto no_op = cache.prepare(tiny, operation, "tiny", {});
        ASSERT_TRUE(no_op);
        expect_float_bits(no_op.value(), tiny);
        EXPECT_EQ(cache.pixel_bytes(), 0U);
    }
    input.rgb.reserve(static_cast<std::size_t>(
        detail::ProfileDenoisePreviewCache::kPixelByteLimit / (2U * sizeof(float)) + 1U));
    EXPECT_FALSE(detail::ProfileDenoisePreviewCache::admits(input));
    auto ordinary = input;
    ASSERT_TRUE(apply_denoise_profile(ordinary, operation, {}));
    auto uncached = cache.prepare(std::move(input), operation, "oversized-capacity", {});
    ASSERT_TRUE(uncached);
    expect_float_bits(uncached.value(), ordinary);
    EXPECT_EQ(cache.pixel_bytes(), 0U);
}

TEST(ProfileDenoisePreviewTest, EngineStrengthAndChromaSweepsMatchOrdinaryRecipeDispatch)
{
    auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine);
    RasterBuffer raster;
    raster.width = 97U;
    raster.height = 65U;
    raster.source_width = raster.width;
    raster.source_height = raster.height;
    raster.color_profile.kind = ColorProfileKind::kBuiltin;
    raster.color_profile.model = ColorModel::kRgb;
    raster.color_profile.identifier = "srgb";
    raster.srgb.resize(static_cast<std::size_t>(raster.width) * raster.height * 3U);
    for (std::size_t i = 0U; i < raster.srgb.size(); ++i)
        raster.srgb[i] = static_cast<std::uint8_t>((i * 17U + i / 11U) % 256U);
    DevelopParams develop;
    develop.denoise = 0.31;
    develop.sigmoid_enabled = true;
    auto recipe = recipe_from_develop({"denoise", "memory:denoise", "source-a"}, develop);
    ASSERT_TRUE(recipe);
    auto working = engine.value().linear_working_from_raster(raster, recipe.value(), {});
    ASSERT_TRUE(working) << working.error().message;
    InteractivePreviewRenderCache cache;
    for (const double strength : {0.31, 0.48, 0.8})
    {
        develop.denoise = strength;
        develop.denoise_chroma = 1.0 - strength;
        recipe = recipe_from_develop(recipe.value().asset, develop);
        ASSERT_TRUE(recipe);
        auto actual = engine.value().render_interactive_linear_working(working.value(),
                                                                       recipe.value(), cache, {});
        auto expected = engine.value().render_linear_working(working.value(), recipe.value(), {});
        ASSERT_TRUE(actual) << actual.error().message;
        ASSERT_TRUE(expected) << expected.error().message;
        EXPECT_EQ(actual.value().rgb, expected.value().rgb);
    }
    for (const double radius : {0.5, 1.2, 2.4})
    {
        develop.denoise_radius = radius;
        recipe = recipe_from_develop(recipe.value().asset, develop);
        ASSERT_TRUE(recipe);
        auto actual = engine.value().render_interactive_linear_working(working.value(),
                                                                       recipe.value(), cache, {});
        auto expected = engine.value().render_linear_working(working.value(), recipe.value(), {});
        ASSERT_TRUE(actual) << actual.error().message;
        ASSERT_TRUE(expected) << expected.error().message;
        EXPECT_EQ(actual.value().rgb, expected.value().rgb);
    }
    for (auto &operation : recipe.value().operations)
        if (operation.id == "ravo.detail.denoiseprofile")
            operation.parameters["noise_a"] = ParameterValue{0.0004};
    auto noise_changed = engine.value().render_interactive_linear_working(
        working.value(), recipe.value(), cache, {});
    auto noise_reference =
        engine.value().render_linear_working(working.value(), recipe.value(), {});
    ASSERT_FALSE(noise_changed);
    ASSERT_FALSE(noise_reference);
    EXPECT_EQ(noise_changed.error().code, noise_reference.error().code);
    EXPECT_EQ(noise_changed.error().context, noise_reference.error().context);
    // An enabled stage after denoise must keep ordinary prefix evaluation.
    develop.lens_k1 = 0.01;
    recipe = recipe_from_develop(recipe.value().asset, develop);
    ASSERT_TRUE(recipe);
    auto nonterminal = engine.value().render_interactive_linear_working(working.value(),
                                                                        recipe.value(), cache, {});
    auto reference = engine.value().render_linear_working(working.value(), recipe.value(), {});
    ASSERT_TRUE(nonterminal) << nonterminal.error().message;
    ASSERT_TRUE(reference) << reference.error().message;
    EXPECT_EQ(nonterminal.value().rgb, reference.value().rgb);
    develop.lens_k1 = 0.0;
    recipe = recipe_from_develop(recipe.value().asset, develop);
    ASSERT_TRUE(recipe);
    Mask all{"all", kCanonicalMaskSchemaVersion, MaskKind::kAll};
    all.payload = AllMask{};
    all.common.opacity = 0.4;
    recipe.value().masks.push_back(std::move(all));
    for (auto &operation : recipe.value().operations)
        if (operation.id == "ravo.detail.denoiseprofile")
            operation.mask_id = "all";
    auto masked = engine.value().render_interactive_linear_working(working.value(), recipe.value(),
                                                                   cache, {});
    reference = engine.value().render_linear_working(working.value(), recipe.value(), {});
    ASSERT_FALSE(masked);
    ASSERT_FALSE(reference);
    EXPECT_EQ(masked.error().code, reference.error().code);
    EXPECT_EQ(masked.error().context, reference.error().context);
    EXPECT_EQ(masked.error().message, reference.error().message);
}
} // namespace
} // namespace ravo
