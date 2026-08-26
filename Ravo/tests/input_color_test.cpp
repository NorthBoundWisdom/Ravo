#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <lcms2.h>

#include "ravo/adapters/legacy_xmp.h"
#include "input_color.h"
#include "ravo/engine/engine.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/color.h"
#include "ravo/recipe/color_input.h"
#include "ravo/recipe/recipe.h"

namespace ravo
{
namespace
{

constexpr std::array<float, 9> kLinearSrgbToXyzD50{0.4360747F, 0.3850649F, 0.1430804F,
                                                   0.2225045F, 0.7168786F, 0.0606169F,
                                                   0.0139322F, 0.0971045F, 0.7141733F};

[[nodiscard]] std::vector<std::uint8_t> profile_bytes(const cmsHPROFILE profile)
{
    cmsUInt32Number size = 0;
    if (profile == nullptr || cmsSaveProfileToMem(profile, nullptr, &size) == 0 || size == 0)
    {
        return {};
    }
    std::vector<std::uint8_t> bytes(size);
    if (cmsSaveProfileToMem(profile, bytes.data(), &size) == 0)
    {
        return {};
    }
    bytes.resize(size);
    return bytes;
}

[[nodiscard]] std::vector<std::uint8_t> gamma_rgb_profile(const double gamma)
{
    cmsCIExyY white{0.3127, 0.3290, 1.0};
    cmsCIExyYTRIPLE primaries{{0.6400, 0.3300, 1.0}, {0.3000, 0.6000, 1.0}, {0.1500, 0.0600, 1.0}};
    std::array<cmsToneCurve *, 3> curves{};
    for (auto &curve : curves)
    {
        curve = cmsBuildGamma(nullptr, gamma);
    }
    cmsHPROFILE profile = cmsCreateRGBProfile(&white, &primaries, curves.data());
    for (auto *curve : curves)
    {
        cmsFreeToneCurve(curve);
    }
    const auto bytes = profile_bytes(profile);
    cmsCloseProfile(profile);
    return bytes;
}

[[nodiscard]] std::vector<std::uint8_t> lab_lut_profile()
{
    cmsHPROFILE profile = cmsCreateBCHSWabstractProfile(17, 0.0, 1.0, 0.0, 1.0, 6504, 6504);
    const auto bytes = profile_bytes(profile);
    cmsCloseProfile(profile);
    return bytes;
}

cmsInt32Number sample_rgb_lut(const cmsUInt16Number input[], cmsUInt16Number output[], void *)
{
    const double red = static_cast<double>(input[0]) / 65535.0;
    const double green = static_cast<double>(input[1]) / 65535.0;
    const double blue = static_cast<double>(input[2]) / 65535.0;
    const cmsCIELab lab{(red + green + blue) * (100.0 / 3.0), (red - green) * 80.0,
                        (green - blue) * 80.0};
    cmsFloat2LabEncoded(output, &lab);
    return 1;
}

[[nodiscard]] cmsStage *identity_curve_stage()
{
    std::array<cmsToneCurve *, 3> curves{};
    for (auto &curve : curves)
    {
        curve = cmsBuildGamma(nullptr, 1.0);
        if (curve == nullptr)
        {
            for (auto *owned : curves)
            {
                if (owned != nullptr)
                {
                    cmsFreeToneCurve(owned);
                }
            }
            return nullptr;
        }
    }
    cmsStage *stage = cmsStageAllocToneCurves(nullptr, 3, curves.data());
    for (auto *curve : curves)
    {
        cmsFreeToneCurve(curve);
    }
    return stage;
}

[[nodiscard]] std::vector<std::uint8_t> rgb_lut_profile()
{
    cmsHPROFILE profile = cmsCreate_sRGBProfile();
    if (profile == nullptr)
    {
        return {};
    }
    cmsSetProfileVersion(profile, 4.3);
    cmsSetDeviceClass(profile, cmsSigInputClass);
    cmsSetColorSpace(profile, cmsSigRgbData);
    cmsSetPCS(profile, cmsSigLabData);
    cmsSetHeaderRenderingIntent(profile, INTENT_PERCEPTUAL);
    cmsPipeline *pipeline = cmsPipelineAlloc(nullptr, 3, 3);
    cmsStage *input_curves = identity_curve_stage();
    cmsStage *clut = cmsStageAllocCLut16bit(nullptr, 17, 3, 3, nullptr);
    cmsStage *output_curves = identity_curve_stage();
    const bool sampled =
        clut != nullptr && cmsStageSampleCLut16bit(clut, sample_rgb_lut, nullptr, 0) != 0;
    const bool input_inserted = pipeline != nullptr && input_curves != nullptr &&
                                cmsPipelineInsertStage(pipeline, cmsAT_BEGIN, input_curves) != 0;
    const bool clut_inserted =
        input_inserted && sampled && cmsPipelineInsertStage(pipeline, cmsAT_END, clut) != 0;
    const bool output_inserted = clut_inserted && output_curves != nullptr &&
                                 cmsPipelineInsertStage(pipeline, cmsAT_END, output_curves) != 0;
    if (!output_inserted || cmsWriteTag(profile, cmsSigAToB0Tag, pipeline) == 0)
    {
        if (input_curves != nullptr && !input_inserted)
        {
            cmsStageFree(input_curves);
        }
        if (clut != nullptr && !clut_inserted)
        {
            cmsStageFree(clut);
        }
        if (output_curves != nullptr && !output_inserted)
        {
            cmsStageFree(output_curves);
        }
        if (pipeline != nullptr)
        {
            cmsPipelineFree(pipeline);
        }
        cmsCloseProfile(profile);
        return {};
    }
    cmsPipelineFree(pipeline);
    const std::array<cmsTagSignature, 6> matrix_tags{cmsSigRedColorantTag,  cmsSigGreenColorantTag,
                                                     cmsSigBlueColorantTag, cmsSigRedTRCTag,
                                                     cmsSigGreenTRCTag,     cmsSigBlueTRCTag};
    for (const auto tag : matrix_tags)
    {
        if (cmsWriteTag(profile, tag, nullptr) == 0)
        {
            cmsCloseProfile(profile);
            return {};
        }
    }
    const auto bytes = profile_bytes(profile);
    cmsCloseProfile(profile);
    return bytes;
}

[[nodiscard]] ColorProfileState linear_srgb_matrix_state()
{
    ColorProfileState state;
    state.kind = ColorProfileKind::kMatrix;
    state.model = ColorModel::kRgb;
    state.identifier = "enhanced_matrix";
    state.matrix_to_xyz_d50 = kLinearSrgbToXyzD50;
    state.has_matrix = true;
    state.camera_input = true;
    return state;
}

[[nodiscard]] ColorProfileState icc_state(std::vector<std::uint8_t> bytes,
                                          const ColorModel model = ColorModel::kRgb)
{
    ColorProfileState state;
    state.kind = ColorProfileKind::kIcc;
    state.model = model;
    state.identifier = "embedded_icc";
    state.icc_bytes = std::move(bytes);
    return state;
}

[[nodiscard]] ProfiledColorBuffer single_pixel(const std::array<float, 3> &pixel,
                                               ColorProfileState profile)
{
    return {1, 1, {pixel[0], pixel[1], pixel[2]}, std::move(profile)};
}

[[nodiscard]] Recipe input_recipe(const InputColorParams &params)
{
    Recipe recipe;
    recipe.asset = {"color", "memory:color", std::nullopt};
    recipe.operations.push_back({"ravo.color.input", 1, "color-input-1", true,
                                 input_color_to_parameters(params), std::nullopt});
    return recipe;
}

TEST(InputColorTest, MatrixIdentityAndFrozenBlueMappingAreAtomic)
{
    InputColorParams params;
    const auto input = single_pixel({0.25F, 0.5F, 0.75F}, linear_srgb_matrix_state());
    const auto original = input.channels;
    auto identity = apply_input_color(input, params, CancellationToken{});
    ASSERT_TRUE(identity) << identity.error().message;
    ASSERT_EQ(identity.value().rgb.size(), 3U);
    EXPECT_NEAR(identity.value().rgb[0], 0.25F, 2.0e-5F);
    EXPECT_NEAR(identity.value().rgb[1], 0.5F, 2.0e-5F);
    EXPECT_NEAR(identity.value().rgb[2], 0.75F, 2.0e-5F);
    EXPECT_EQ(identity.value().color_profile.identifier, kInputProfileLinearRec709);
    EXPECT_EQ(input.channels, original);

    params.blue_mapping = true;
    const auto blue = single_pixel({0.0F, 0.0F, 1.0F}, linear_srgb_matrix_state());
    auto mapped = apply_input_color(blue, params, CancellationToken{});
    ASSERT_TRUE(mapped) << mapped.error().message;
    EXPECT_NEAR(mapped.value().rgb[0], 0.0F, 2.0e-5F);
    EXPECT_NEAR(mapped.value().rgb[1], 0.11F, 2.0e-5F);
    EXPECT_NEAR(mapped.value().rgb[2], 0.89F, 2.0e-5F);
}

TEST(InputColorTest, StaticLegacy0107Through0109ParametersMapToCanonicalSchema)
{
    auto baseline = decode_legacy_colorin_parameters("gz48eJzjYRgFowABWAbaAaNgwAEAPRQAEQ==");
    ASSERT_TRUE(baseline) << baseline.error().message;
    EXPECT_EQ(baseline.value().input_profile, kInputProfileEnhancedMatrix);
    EXPECT_EQ(baseline.value().working_profile, kInputProfileLinearRec2020);
    EXPECT_EQ(baseline.value().gamut_normalize, kColorNormalizeOff);

    auto gamma = decode_legacy_colorin_parameters("gz48eJwTYRgFowABRAfaAaNgwAEAf/gAKg==");
    ASSERT_TRUE(gamma) << gamma.error().message;
    EXPECT_EQ(gamma.value().input_profile, kInputProfileRec709);
    EXPECT_EQ(gamma.value().working_profile, kInputProfileProPhotoRgb);

    auto clip = decode_legacy_colorin_parameters("gz42eJwTZRgFo4CBgRFKswyoK0bBYAAAY9QAGw==");
    ASSERT_TRUE(clip) << clip.error().message;
    EXPECT_EQ(clip.value().input_profile, kInputProfileProPhotoRgb);
    EXPECT_EQ(clip.value().working_profile, kInputProfileLinearRec2020);
    EXPECT_EQ(clip.value().gamut_normalize, kColorNormalizeSrgb);

    auto gamma_and_clip =
        decode_legacy_colorin_parameters("gz42eJyTZBgFo4CBgRFKswyoK0bBYAAAdCQAHw==");
    ASSERT_TRUE(gamma_and_clip) << gamma_and_clip.error().message;
    EXPECT_EQ(gamma_and_clip.value().input_profile, kInputProfileHlgP3);
    EXPECT_EQ(gamma_and_clip.value().working_profile, kInputProfileLinearRec2020);
    EXPECT_EQ(gamma_and_clip.value().gamut_normalize, kColorNormalizeSrgb);

    auto corrupt = decode_legacy_colorin_parameters("not-a-profile");
    ASSERT_FALSE(corrupt);
    EXPECT_EQ(corrupt.error().code, ErrorCode::kValidation);
}

TEST(InputColorTest, MatrixShaperUsesLutAndFrozenUnboundedExtrapolation)
{
    const auto bytes = gamma_rgb_profile(2.0);
    ASSERT_FALSE(bytes.empty());
    const auto input = single_pixel({0.5F, 1.5F, -0.25F}, icc_state(bytes));
    const auto original = input.channels;
    InputColorParams params;
    auto transformed = apply_input_color(input, params, CancellationToken{});
    ASSERT_TRUE(transformed) << transformed.error().message;
    EXPECT_NEAR(transformed.value().rgb[0], 0.25F, 2.0e-3F);
    EXPECT_NEAR(transformed.value().rgb[1], 2.25F, 3.0e-3F);
    EXPECT_NEAR(transformed.value().rgb[2], 0.0F, 2.0e-4F);
    EXPECT_EQ(input.channels, original);

    InputColorParams prophoto;
    prophoto.input_profile = std::string(kInputProfileProPhotoRgb);
    auto linear_prophoto =
        apply_input_color(single_pixel({0.5F, 0.5F, 0.5F}, {}), prophoto, CancellationToken{});
    ASSERT_TRUE(linear_prophoto) << linear_prophoto.error().message;
    EXPECT_NEAR(linear_prophoto.value().rgb[0], 0.5F, 2.0e-4F);
    EXPECT_NEAR(linear_prophoto.value().rgb[1], 0.5F, 2.0e-4F);
    EXPECT_NEAR(linear_prophoto.value().rgb[2], 0.5F, 3.0e-4F);

    InputColorParams hlg;
    hlg.input_profile = std::string(kInputProfileHlgP3);
    auto hlg_black =
        apply_input_color(single_pixel({0.0F, 0.0F, 0.0F}, {}), hlg, CancellationToken{});
    ASSERT_TRUE(hlg_black) << hlg_black.error().message;
    constexpr float hlg_beta_black = 0.04F * 0.04F / 3.0F;
    EXPECT_NEAR(hlg_black.value().rgb[0], hlg_beta_black, 2.0e-6F);
    EXPECT_NEAR(hlg_black.value().rgb[1], hlg_beta_black, 2.0e-6F);
    EXPECT_NEAR(hlg_black.value().rgb[2], hlg_beta_black, 2.0e-6F);
}

TEST(InputColorTest, EveryNormalizeTargetClipsInItsOwnRgbGamut)
{
    const std::array<std::string_view, 5> modes{
        kColorNormalizeOff, kColorNormalizeSrgb, kColorNormalizeAdobeRgb,
        kColorNormalizeLinearRec709, kColorNormalizeLinearRec2020};
    std::vector<std::array<float, 3>> results;
    for (const auto mode : modes)
    {
        InputColorParams params;
        params.input_profile = std::string(kInputProfileLinearRec2020);
        params.gamut_normalize = std::string(mode);
        const auto input = single_pixel({0.0F, 1.0F, 0.0F}, {});
        auto transformed = apply_input_color(input, params, CancellationToken{});
        ASSERT_TRUE(transformed) << transformed.error().message << " mode=" << mode;
        ASSERT_EQ(transformed.value().rgb.size(), 3U);
        std::array<float, 3> pixel{transformed.value().rgb[0], transformed.value().rgb[1],
                                   transformed.value().rgb[2]};
        EXPECT_TRUE(std::all_of(pixel.begin(), pixel.end(),
                                [](const float value) { return std::isfinite(value); }));
        results.push_back(pixel);
    }
    EXPECT_LT(results[0][0], 0.0F);
    EXPECT_GE(results[1][0], -2.0e-5F);
    EXPECT_LE(results[1][1], 1.00002F);
    EXPECT_GE(results[3][0], -2.0e-5F);
    EXPECT_LE(results[3][1], 1.00002F);
    EXPECT_NE(results[1], results[2]);
}

TEST(InputColorTest, LittleCmsLutAndLabPathsRejectCorruptionAndMissingFiles)
{
    const auto rgb_bytes = rgb_lut_profile();
    ASSERT_FALSE(rgb_bytes.empty());
    const auto rgb = single_pixel({0.2F, 0.4F, 0.6F}, icc_state(rgb_bytes));
    InputColorParams params;
    auto rgb_transformed = apply_input_color(rgb, params, CancellationToken{});
    ASSERT_TRUE(rgb_transformed) << rgb_transformed.error().message;
    EXPECT_TRUE(std::all_of(rgb_transformed.value().rgb.begin(), rgb_transformed.value().rgb.end(),
                            [](const float value) { return std::isfinite(value); }));

    const auto bytes = lab_lut_profile();
    ASSERT_FALSE(bytes.empty());
    const auto lab = single_pixel({50.0F, 0.0F, 0.0F}, icc_state(bytes, ColorModel::kLab));
    auto transformed = apply_input_color(lab, params, CancellationToken{});
    ASSERT_TRUE(transformed) << transformed.error().message;
    EXPECT_TRUE(std::all_of(transformed.value().rgb.begin(), transformed.value().rgb.end(),
                            [](const float value) { return std::isfinite(value); }));

    auto corrupt = lab;
    corrupt.color_profile.icc_bytes = {1, 2, 3, 4};
    auto corrupt_result = apply_input_color(corrupt, params, CancellationToken{});
    ASSERT_FALSE(corrupt_result);
    EXPECT_EQ(corrupt_result.error().code, ErrorCode::kValidation);

    params.input_profile = std::string(kInputProfileFileIcc);
    params.input_profile_filename =
        (std::filesystem::temp_directory_path() / "ravo-missing-input-profile.icc").string();
    auto missing =
        apply_input_color(single_pixel({0.5F, 0.5F, 0.5F}, {}), params, CancellationToken{});
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, ErrorCode::kNotFound);
}

TEST(InputColorTest, FacadeRejectsAnUntaggedRasterWithoutSrgbFallback)
{
    auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    RasterBuffer raster;
    raster.width = 1;
    raster.height = 1;
    raster.srgb = {128, 128, 128};
    Recipe recipe;
    recipe.asset = {"untagged", "memory:untagged", std::nullopt};
    RenderRequest request;
    request.asset = recipe.asset;
    request.recipe = recipe;
    auto rendered = engine.value().render_to_image(request, &raster);
    ASSERT_FALSE(rendered);
    EXPECT_EQ(rendered.error().code, ErrorCode::kValidation);
}

TEST(InputColorTest, FileIccContentParticipatesInTheCacheFingerprint)
{
    const auto path = std::filesystem::temp_directory_path() / "ravo-input-profile-fingerprint.icc";
    const auto write = [&path](const std::vector<std::uint8_t> &bytes)
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char *>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        return static_cast<bool>(stream);
    };
    ASSERT_TRUE(write(gamma_rgb_profile(2.0)));

    InputColorParams params;
    params.input_profile = std::string(kInputProfileFileIcc);
    params.input_profile_filename = path.string();
    const auto recipe = input_recipe(params);
    auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    auto first = engine.value().input_color_cache_fingerprint(recipe);
    ASSERT_TRUE(first) << first.error().message;

    ASSERT_TRUE(write(gamma_rgb_profile(2.2)));
    auto second = engine.value().input_color_cache_fingerprint(recipe);
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_NE(first.value(), second.value());

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

TEST(InputColorTest, Frozen0000EnhancedMatrixHasARealMire1WorkingReference)
{
    auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    const auto raw_path =
        std::filesystem::path(RAVO_REPOSITORY_ROOT) / "legacy" / "tests" / "images" / "mire1.cr2";
    auto decoded = engine.value().decode_raw_frame(raw_path.string(), CancellationToken{});
    ASSERT_TRUE(decoded) << decoded.error().message;
    const auto original_pixels = decoded.value().pixels;
    const auto original_profile = decoded.value().color_profile;
    auto legacy = decode_legacy_colorin_parameters("gz48eJzjYRgFowABWAbaAaNgwAEAPRQAEQ==");
    ASSERT_TRUE(legacy) << legacy.error().message;
    Recipe recipe = input_recipe(legacy.value());
    recipe.asset = {"mire1", raw_path.string(), std::nullopt};
    auto working = engine.value().linear_working_from_raw(decoded.value(), recipe, 64, 48,
                                                          CancellationToken{});
    ASSERT_TRUE(working) << working.error().message;
    EXPECT_EQ(working.value().color_profile.identifier, kInputProfileLinearRec2020);
    std::array<double, 3> sums{};
    for (std::size_t index = 0; index < working.value().rgb.size(); index += 3U)
    {
        sums[0] += working.value().rgb[index];
        sums[1] += working.value().rgb[index + 1U];
        sums[2] += working.value().rgb[index + 2U];
    }
    EXPECT_NEAR(sums[0], 475.36401362, 1.0e-3);
    EXPECT_NEAR(sums[1], 440.38690752, 1.0e-3);
    EXPECT_NEAR(sums[2], 419.06130994, 1.0e-3);
    EXPECT_EQ(decoded.value().pixels, original_pixels);
    EXPECT_EQ(decoded.value().color_profile, original_profile);
}

TEST(InputColorTest, RowCancellationAndNonFiniteInputNeverPublishPixels)
{
    ProfiledColorBuffer input;
    input.width = 1024;
    input.height = 4096;
    input.channels.assign(static_cast<std::size_t>(input.width) * input.height * 3U, 0.5F);
    input.color_profile = linear_srgb_matrix_state();
    const auto original = input.channels;
    const auto cancellation = CancellationSource::with_deadline(std::chrono::steady_clock::now() +
                                                                std::chrono::milliseconds{1});
    auto cancelled = apply_input_color(input, InputColorParams{}, cancellation.token());
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, ErrorCode::kCancelled);
    EXPECT_EQ(input.channels, original);

    auto invalid = single_pixel({0.0F, std::numeric_limits<float>::infinity(), 0.0F},
                                linear_srgb_matrix_state());
    auto rejected = apply_input_color(invalid, InputColorParams{}, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kValidation);

    auto invalid_matrix = linear_srgb_matrix_state();
    invalid_matrix.matrix_to_xyz_d50[4] = std::numeric_limits<float>::quiet_NaN();
    auto matrix_result = apply_input_color(single_pixel({0.2F, 0.3F, 0.4F}, invalid_matrix),
                                           InputColorParams{}, CancellationToken{});
    ASSERT_FALSE(matrix_result);
    EXPECT_EQ(matrix_result.error().code, ErrorCode::kValidation);

    auto singular = linear_srgb_matrix_state();
    singular.matrix_to_xyz_d50 = {};
    auto singular_result = apply_input_color(single_pixel({0.2F, 0.3F, 0.4F}, singular),
                                             InputColorParams{}, CancellationToken{});
    ASSERT_FALSE(singular_result);
    EXPECT_EQ(singular_result.error().code, ErrorCode::kValidation);

    InputColorParams vendor;
    vendor.input_profile = std::string(kInputProfileVendorMatrix);
    auto unavailable = apply_input_color(
        single_pixel({0.2F, 0.3F, 0.4F}, linear_srgb_matrix_state()), vendor, CancellationToken{});
    ASSERT_FALSE(unavailable);
    EXPECT_EQ(unavailable.error().code, ErrorCode::kUnsupported);
}

} // namespace
} // namespace ravo
