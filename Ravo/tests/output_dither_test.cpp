#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "output_dither.h"
#include "ravo/adapters/legacy_xmp.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/operation.h"
#include "ravo/recipe/output_dither.h"
#include "ravo/recipe/recipe.h"

namespace ravo
{
namespace
{

[[nodiscard]] ProfiledOutputBuffer
output_buffer(const std::uint32_t width, const std::uint32_t height, std::vector<float> channels)
{
    ProfiledOutputBuffer output;
    output.width = width;
    output.height = height;
    output.channels = std::move(channels);
    output.color_profile.kind = ColorProfileKind::kBuiltin;
    output.color_profile.model = ColorModel::kRgb;
    output.color_profile.identifier = "encoded-output-fixture";
    return output;
}

[[nodiscard]] std::vector<std::uint32_t> float_bits(const std::vector<float> &values)
{
    std::vector<std::uint32_t> result;
    result.reserve(values.size());
    for (const float value : values)
        result.push_back(std::bit_cast<std::uint32_t>(value));
    return result;
}

[[nodiscard]] std::string legacy_dither_xmp(const std::string_view parameters,
                                            const std::string_view blend_version,
                                            const std::string_view blend,
                                            const std::string_view enabled = "1",
                                            const std::string_view extra = {})
{
    return std::string(R"(<?xml version="1.0"?>
<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"
         xmlns:darktable="http://darktable.sf.net/">
  <rdf:Description darktable:xmp_version="6">
    <darktable:history><rdf:Seq><rdf:li darktable:num="9"
      darktable:operation="dither" darktable:enabled=")") +
           std::string(enabled) + R"(" darktable:modversion="1" darktable:params=")" +
           std::string(parameters) + R"(" darktable:multi_name="" darktable:multi_priority="0" )" +
           std::string(extra) + R"(darktable:blendop_version=")" + std::string(blend_version) +
           R"(" darktable:blendop_params=")" + std::string(blend) +
           R"("/></rdf:Seq></darktable:history>
  </rdf:Description>
</rdf:RDF>)";
}

TEST(OutputDitherRecipeTest, AllMethodsRoundTripAndDevelopKeepsExplicitDefaultPresence)
{
    for (std::int64_t index = 0; index < static_cast<std::int64_t>(kOutputDitherMethodCount);
         ++index)
    {
        auto method = output_dither_method_from_index(index);
        ASSERT_TRUE(method) << index;
        EXPECT_EQ(output_dither_method_index(method.value()), index);
        const auto name = output_dither_method_name(method.value());
        ASSERT_FALSE(name.empty());
        auto parsed = parse_output_dither_method(name);
        ASSERT_TRUE(parsed) << name;
        EXPECT_EQ(parsed.value(), method.value());
        auto parameters = output_dither_to_parameters({method.value(), -42.5});
        ASSERT_TRUE(parameters) << parameters.error().message;
        auto round_trip = output_dither_from_parameters(parameters.value());
        ASSERT_TRUE(round_trip) << round_trip.error().message;
        EXPECT_EQ(round_trip.value(), (OutputDitherParams{method.value(), -42.5}));
    }
    EXPECT_FALSE(parse_output_dither_method("ordered-blue-noise"));
    EXPECT_FALSE(output_dither_method_from_index(-1));
    EXPECT_FALSE(
        output_dither_method_from_index(static_cast<std::int64_t>(kOutputDitherMethodCount)));

    DevelopParams develop;
    develop.output_dither_present = true;
    develop.output_dither_enabled = true;
    develop.output_dither = {OutputDitherMethod::kFloydSteinbergAuto, -100.0};
    auto recipe = recipe_from_develop({"asset", "file:///fixture.raw", std::nullopt}, develop);
    ASSERT_TRUE(recipe) << recipe.error().message;
    ASSERT_GE(recipe.value().operations.size(), 2U);
    EXPECT_EQ(recipe.value().operations[recipe.value().operations.size() - 2U].id,
              "ravo.color.output");
    EXPECT_EQ(recipe.value().operations.back().id, kOutputDitherOperationId);
    auto restored = develop_from_recipe(recipe.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_TRUE(restored.value().output_dither_present);
    EXPECT_TRUE(restored.value().output_dither_enabled);
    EXPECT_EQ(restored.value().output_dither, develop.output_dither);
    EXPECT_TRUE(develop_section_modified(restored.value(), "effects"));
    EXPECT_TRUE(reset_develop_section(restored.value(), "effects"));
    EXPECT_FALSE(restored.value().output_dither_present);

    DevelopParams disabled = develop;
    disabled.output_dither_enabled = false;
    disabled.effects_effect_enabled = false;
    auto disabled_recipe =
        recipe_from_develop({"asset", "file:///fixture.raw", std::nullopt}, disabled);
    ASSERT_TRUE(disabled_recipe) << disabled_recipe.error().message;
    auto disabled_restored = develop_from_recipe(disabled_recipe.value());
    ASSERT_TRUE(disabled_restored) << disabled_restored.error().message;
    EXPECT_FALSE(disabled_restored.value().output_dither_enabled);
    EXPECT_FALSE(disabled_restored.value().effects_effect_enabled);
    ASSERT_TRUE(apply_develop_field_strict(disabled_restored.value(), "outputDitherEnabled", 1.0));
    EXPECT_TRUE(disabled_restored.value().output_dither_enabled);
    EXPECT_TRUE(disabled_restored.value().effects_effect_enabled);
}

TEST(OutputDitherTest, FloydSteinbergGrayAndRgbMatchFrozenRowOrderBits)
{
    std::vector<float> source;
    for (int index = 0; index < 9; ++index)
    {
        source.push_back(static_cast<float>(index + 1) / 10.0F);
        source.push_back(static_cast<float>(9 - index) / 10.0F);
        source.push_back(static_cast<float>((index * 3) % 10) / 10.0F);
    }
    const std::vector<std::uint32_t> expected_gray{
        0x3f800000U, 0x3f800000U, 0x3f800000U, 0x80000000U, 0x80000000U, 0x80000000U, 0x3f800000U,
        0x3f800000U, 0x3f800000U, 0x3f800000U, 0x3f800000U, 0x3f800000U, 0x80000000U, 0x80000000U,
        0x80000000U, 0x3f800000U, 0x3f800000U, 0x3f800000U, 0x80000000U, 0x80000000U, 0x80000000U,
        0x3f800000U, 0x3f800000U, 0x3f800000U, 0x80000000U, 0x80000000U, 0x80000000U};
    const std::vector<std::uint32_t> expected_rgb{
        0x3d888889U, 0x3f5ddddfU, 0x80000000U, 0x3e4cccceU, 0x3f4cccceU, 0x3e888889U, 0x3eaaaaabU,
        0x3f3bbbbcU, 0x3f19999aU, 0x3eccccceU, 0x3f19999aU, 0x3f6eeef0U, 0x3f088889U, 0x3f088889U,
        0x3e4cccceU, 0x3f19999aU, 0x3eccccceU, 0x3f088889U, 0x3f2aaaabU, 0x3e888889U, 0x3f4cccceU,
        0x3f4cccceU, 0x3e4cccceU, 0x3d888889U, 0x3f5ddddfU, 0x3d888889U, 0x3eccccceU};

    auto gray = apply_output_dither(output_buffer(3, 3, source),
                                    {OutputDitherMethod::kFloydSteinberg1BitGray, -100.0},
                                    OutputDitherTarget::kExportRgb8, {});
    ASSERT_TRUE(gray) << gray.error().message;
    EXPECT_EQ(float_bits(gray.value().channels), expected_gray);
    auto rgb = apply_output_dither(output_buffer(3, 3, source),
                                   {OutputDitherMethod::kFloydSteinberg4BitRgb, -100.0},
                                   OutputDitherTarget::kExportRgb8, {});
    ASSERT_TRUE(rgb) << rgb.error().message;
    EXPECT_EQ(float_bits(rgb.value().channels), expected_rgb);
}

TEST(OutputDitherTest, RandomTeaStreamPosterizeTinyAndAutoTargetsAreDeterministic)
{
    auto random = apply_output_dither(output_buffer(2, 2, std::vector<float>(12U, 0.5F)),
                                      {OutputDitherMethod::kRandom, -6.91400146484375},
                                      OutputDitherTarget::kExportRgb8, {});
    ASSERT_TRUE(random) << random.error().message;
    const std::array<std::uint32_t, 4> expected_random{0x3f800000U, 0x3ee4a00bU, 0x3e4ba69cU,
                                                       0x3f2d7e6cU};
    for (std::size_t pixel = 0U; pixel < expected_random.size(); ++pixel)
    {
        for (std::size_t channel = 0U; channel < 3U; ++channel)
            EXPECT_EQ(std::bit_cast<std::uint32_t>(random.value().channels[pixel * 3U + channel]),
                      expected_random[pixel]);
    }

    auto poster = apply_output_dither(
        output_buffer(3, 1, {0.16F, 0.5F, 0.84F, 0.17F, 0.49F, 0.83F, 0.0F, 0.51F, 1.0F}),
        {OutputDitherMethod::kPosterize4, -100.0}, OutputDitherTarget::kExportRgbFloat, {});
    ASSERT_TRUE(poster) << poster.error().message;
    EXPECT_EQ(
        float_bits(poster.value().channels),
        (std::vector<std::uint32_t>{0x80000000U, 0x3eaaaaabU, 0x3f800000U, 0x3eaaaaabU, 0x3eaaaaabU,
                                    0x3f2aaaabU, 0x80000000U, 0x3f2aaaabU, 0x3f800000U}));

    auto tiny = apply_output_dither(output_buffer(2, 1, {0.4F, 0.4F, 0.4F, 0.6F, 0.6F, 0.6F}),
                                    {OutputDitherMethod::kFloydSteinberg1BitRgb, -100.0},
                                    OutputDitherTarget::kExportRgb8, {});
    ASSERT_TRUE(tiny) << tiny.error().message;
    EXPECT_EQ(tiny.value().channels, (std::vector<float>{0.0F, 0.0F, 0.0F, 1.0F, 1.0F, 1.0F}));

    const auto auto_input = output_buffer(1, 1, {-0.2F, 0.5F, 1.2F});
    auto preview =
        apply_output_dither(auto_input, OutputDitherParams{}, OutputDitherTarget::kPreviewRgb8, {});
    auto floating = apply_output_dither(auto_input, OutputDitherParams{},
                                        OutputDitherTarget::kExportRgbFloat, {});
    auto integer =
        apply_output_dither(auto_input, OutputDitherParams{}, OutputDitherTarget::kExportRgb8, {});
    ASSERT_TRUE(preview);
    ASSERT_TRUE(floating);
    ASSERT_TRUE(integer);
    EXPECT_EQ(preview.value().channels, (std::vector<float>{0.0F, 0.5F, 1.0F}));
    EXPECT_EQ(floating.value().channels, preview.value().channels);
    EXPECT_FLOAT_EQ(integer.value().channels[1], 127.0F / 255.0F);
}

struct CancelAtPublication
{
    CancellationSource *source = nullptr;
};

void cancel_output_dither(void *context, const detail::OutputDitherCheckpoint checkpoint,
                          const std::uint32_t) noexcept
{
    auto &state = *static_cast<CancelAtPublication *>(context);
    if (checkpoint == detail::OutputDitherCheckpoint::kBeforePublication)
        static_cast<void>(state.source->cancel("output-dither-publication"));
}

TEST(OutputDitherTest, ValidationCancellationAndOperationOwnershipAreAtomic)
{
    auto input = output_buffer(3, 3, std::vector<float>(27U, 0.45F));
    const auto original = input;
    CancellationSource cancellation;
    CancelAtPublication state{&cancellation};
    auto cancelled = detail::apply_output_dither_controlled(
        input, {OutputDitherMethod::kFloydSteinberg8BitRgb, -100.0},
        OutputDitherTarget::kExportRgb8, cancellation.token(), {&state, cancel_output_dither});
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, ErrorCode::kCancelled);
    EXPECT_EQ(cancelled.error().context.at("reason"), "output-dither-publication");
    EXPECT_EQ(input.channels, original.channels);

    input.channels[3] = std::numeric_limits<float>::quiet_NaN();
    auto nonfinite =
        apply_output_dither(input, OutputDitherParams{}, OutputDitherTarget::kExportRgb8, {});
    ASSERT_FALSE(nonfinite);
    EXPECT_EQ(nonfinite.error().context.at("reason"), "nonfinite_output_dither_input");

    auto parameters = output_dither_to_parameters({});
    ASSERT_TRUE(parameters);
    OperationInstance masked{std::string(kOutputDitherOperationId),
                             kOutputDitherOperationSchemaVersion,
                             "dither-1",
                             true,
                             parameters.value(),
                             "mask-1"};
    auto rejected = apply_output_dither(output_buffer(1, 1, {0.5F, 0.5F, 0.5F}), masked,
                                        OutputDitherTarget::kExportRgb8, {});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "unsupported_output_dither_mask");
}

TEST(OutputDitherTest, EngineSchedulesPosterizeAfterOutputColorBeforePacking)
{
    auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    DevelopParams develop;
    develop.output_dither_present = true;
    develop.output_dither_enabled = true;
    develop.output_dither = {OutputDitherMethod::kPosterize2, -100.0};
    auto recipe = recipe_from_develop({"asset", "file:///fixture.raw", std::nullopt}, develop);
    ASSERT_TRUE(recipe) << recipe.error().message;

    LinearWorkingBuffer working;
    working.width = 1U;
    working.height = 1U;
    working.rgb = {0.25F, 0.25F, 0.25F};
    working.color_profile.kind = ColorProfileKind::kBuiltin;
    working.color_profile.model = ColorModel::kRgb;
    working.color_profile.identifier = "linear_rec709";
    working.color_profile.has_matrix = true;
    working.color_profile.matrix_to_xyz_d50 = {0.4360747F, 0.3850649F, 0.1430804F,
                                               0.2225045F, 0.7168786F, 0.0606169F,
                                               0.0139322F, 0.0971045F, 0.7141733F};
    working.canonical_roi_scale = CanonicalRoiScale::from_scaled_dimensions(1U, 1U, 1U, 1U);
    auto baseline_recipe = recipe.value();
    baseline_recipe.operations.pop_back();
    auto baseline = engine.value().render_linear_working_export(working, baseline_recipe,
                                                                RenderSampleKind::kRgbFloat, {});
    auto posterized = engine.value().render_linear_working_export(working, recipe.value(),
                                                                  RenderSampleKind::kRgbFloat, {});
    ASSERT_TRUE(baseline) << baseline.error().message;
    ASSERT_TRUE(posterized) << posterized.error().message;
    const auto &baseline_samples = std::get<std::vector<float>>(baseline.value().samples);
    const auto &poster_samples = std::get<std::vector<float>>(posterized.value().samples);
    ASSERT_EQ(baseline_samples.size(), 3U);
    ASSERT_EQ(poster_samples.size(), 3U);
    EXPECT_GT(baseline_samples[0], 0.5F);
    EXPECT_LT(baseline_samples[0], 1.0F);
    EXPECT_EQ(poster_samples, (std::vector<float>{1.0F, 1.0F, 1.0F}));
}

TEST(OutputDitherLegacyXmpTest, ThreeFrozenEnabledRecordsMapAndModifiedStateRejects)
{
    constexpr std::string_view fs =
        "01000000000000000000000000000000000000000000000000000000000048c3";
    constexpr std::string_view random =
        "00000000000000000000000000000000000000000000000000000000803fddc0";
    constexpr std::string_view poster =
        "030100000000000000000000000000000000000000000000000000000000c8c2";
    constexpr std::string_view blend10 = "gz14eJxjYIAACQYYOOHEgAYY0QVwggZ7CB6pfNoAAEkgGQQ=";
    constexpr std::string_view blend12 =
        "gz10eJxjYGBgYAFiCQYYOOHEgAZY0QVwggZ7CB6pfOygYtaVAyCMi08IAAB/xiOk";
    const std::array cases{
        std::tuple{fs, std::string_view("10"), blend10, OutputDitherMethod::kFloydSteinberg1BitGray,
                   -200.0},
        std::tuple{random, std::string_view("10"), blend10, OutputDitherMethod::kRandom,
                   -6.91400146484375},
        std::tuple{poster, std::string_view("12"), blend12, OutputDitherMethod::kPosterize4,
                   -100.0},
    };
    for (const auto &[parameters, blend_version, blend, method, damping] : cases)
    {
        auto imported = import_legacy_xmp({legacy_dither_xmp(parameters, blend_version, blend),
                                           {"asset", "file:///fixture.raw", std::nullopt}});
        ASSERT_TRUE(imported) << imported.error().message;
        ASSERT_EQ(imported.value().operations.back().id, kOutputDitherOperationId);
        auto parsed = output_dither_from_parameters(imported.value().operations.back().parameters);
        ASSERT_TRUE(parsed) << parsed.error().message;
        EXPECT_EQ(parsed.value().method, method);
        EXPECT_DOUBLE_EQ(parsed.value().random_damping_db, damping);
    }

    std::string modified(fs);
    modified.replace(0, 2, "02");
    auto unsupported = import_legacy_xmp({legacy_dither_xmp(modified, "10", blend10),
                                          {"asset", "file:///fixture.raw", std::nullopt}});
    ASSERT_FALSE(unsupported);
    EXPECT_EQ(unsupported.error().context.at("reason"), "unsupported_legacy_dither_parameters");
    auto disabled = import_legacy_xmp({legacy_dither_xmp(fs, "10", blend10, "0"),
                                       {"asset", "file:///fixture.raw", std::nullopt}});
    ASSERT_FALSE(disabled);
    EXPECT_EQ(disabled.error().context.at("reason"), "unsupported_legacy_dither_enabled_state");
    auto wrong_blend = import_legacy_xmp(
        {legacy_dither_xmp(poster, "10", blend10), {"asset", "file:///fixture.raw", std::nullopt}});
    ASSERT_FALSE(wrong_blend);
    EXPECT_EQ(wrong_blend.error().context.at("reason"), "unsupported_legacy_dither_blend");
}

} // namespace
} // namespace ravo
