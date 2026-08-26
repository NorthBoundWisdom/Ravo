#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <QFile>
#include <QXmlStreamReader>
#include <gtest/gtest.h>
#include <lcms2.h>

#include "ravo/adapters/legacy_xmp.h"
#include "ravo/engine/engine.h"
#include "output_color.h"
#include "ravo/foundation/color.h"
#include "ravo/recipe/color_output.h"
#include "ravo/recipe/develop.h"

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
    if (profile == nullptr || cmsSaveProfileToMem(profile, nullptr, &size) == 0 || size == 0U)
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

struct OutputLutVariant
{
    double bias = 0.0;
};

std::string output_lut_profile_error;

cmsInt32Number sample_output_lut(const cmsUInt16Number input[], cmsUInt16Number output[],
                                 void *const cargo)
{
    cmsCIELab lab{};
    cmsLabEncoded2Float(&lab, input);
    const auto *variant = static_cast<const OutputLutVariant *>(cargo);
    const double lightness = lab.L / 100.0;
    const double red = std::clamp(lightness + lab.a / 640.0 + variant->bias, 0.0, 1.0);
    const double green = std::clamp(lightness - lab.a / 1280.0 + lab.b / 1280.0, 0.0, 1.0);
    const double blue = std::clamp(lightness - lab.b / 640.0 - variant->bias, 0.0, 1.0);
    output[0] = static_cast<cmsUInt16Number>(std::lround(red * 65535.0));
    output[1] = static_cast<cmsUInt16Number>(std::lround(green * 65535.0));
    output[2] = static_cast<cmsUInt16Number>(std::lround(blue * 65535.0));
    return 1;
}

cmsInt32Number sample_input_lut(const cmsUInt16Number input[], cmsUInt16Number output[], void *)
{
    const double red = static_cast<double>(input[0]) / 65535.0;
    const double green = static_cast<double>(input[1]) / 65535.0;
    const double blue = static_cast<double>(input[2]) / 65535.0;
    const cmsCIELab lab{20.0 + (red + green + blue) * (80.0 / 3.0), (red - green) * 40.0,
                        (green - blue) * 40.0};
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

[[nodiscard]] cmsPipeline *output_lut_pipeline(OutputLutVariant *const variant)
{
    cmsPipeline *pipeline = cmsPipelineAlloc(nullptr, 3, 3);
    cmsStage *input_curves = identity_curve_stage();
    cmsStage *clut = cmsStageAllocCLut16bit(nullptr, 17, 3, 3, nullptr);
    cmsStage *output_curves = identity_curve_stage();
    const bool input_inserted = pipeline != nullptr && input_curves != nullptr &&
                                cmsPipelineInsertStage(pipeline, cmsAT_BEGIN, input_curves) != 0;
    const bool sampled =
        clut != nullptr && cmsStageSampleCLut16bit(clut, sample_output_lut, variant, 0) != 0;
    const bool clut_inserted =
        input_inserted && sampled && cmsPipelineInsertStage(pipeline, cmsAT_END, clut) != 0;
    const bool output_inserted = clut_inserted && output_curves != nullptr &&
                                 cmsPipelineInsertStage(pipeline, cmsAT_END, output_curves) != 0;
    if (!output_inserted)
    {
        output_lut_profile_error = "pipeline allocation, sampling, or insertion failed";
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
        return nullptr;
    }
    return pipeline;
}

[[nodiscard]] cmsPipeline *input_lut_pipeline()
{
    cmsPipeline *pipeline = cmsPipelineAlloc(nullptr, 3, 3);
    cmsStage *input_curves = identity_curve_stage();
    cmsStage *clut = cmsStageAllocCLut16bit(nullptr, 17, 3, 3, nullptr);
    cmsStage *output_curves = identity_curve_stage();
    const bool input_inserted = pipeline != nullptr && input_curves != nullptr &&
                                cmsPipelineInsertStage(pipeline, cmsAT_BEGIN, input_curves) != 0;
    const bool sampled =
        clut != nullptr && cmsStageSampleCLut16bit(clut, sample_input_lut, nullptr, 0) != 0;
    const bool clut_inserted =
        input_inserted && sampled && cmsPipelineInsertStage(pipeline, cmsAT_END, clut) != 0;
    const bool output_inserted = clut_inserted && output_curves != nullptr &&
                                 cmsPipelineInsertStage(pipeline, cmsAT_END, output_curves) != 0;
    if (!output_inserted)
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
        return nullptr;
    }
    return pipeline;
}

[[nodiscard]] std::vector<std::uint8_t> rgb_output_lut_profile()
{
    output_lut_profile_error.clear();
    cmsHPROFILE profile = cmsCreateProfilePlaceholder(nullptr);
    if (profile == nullptr)
    {
        output_lut_profile_error = "profile placeholder allocation failed";
        return {};
    }
    cmsSetProfileVersion(profile, 4.3);
    cmsSetDeviceClass(profile, cmsSigOutputClass);
    cmsSetColorSpace(profile, cmsSigRgbData);
    cmsSetPCS(profile, cmsSigLabData);
    cmsSetHeaderRenderingIntent(profile, INTENT_PERCEPTUAL);
    if (cmsWriteTag(profile, cmsSigMediaWhitePointTag, cmsD50_XYZ()) == 0)
    {
        output_lut_profile_error = "media white point tag failed";
        cmsCloseProfile(profile);
        return {};
    }
    const cmsCIEXYZ black_point{0.02, 0.02, 0.02};
    if (cmsWriteTag(profile, cmsSigMediaBlackPointTag, &black_point) == 0)
    {
        output_lut_profile_error = "media black point tag failed";
        cmsCloseProfile(profile);
        return {};
    }
    const std::array<cmsTagSignature, 3> tags{cmsSigBToA0Tag, cmsSigBToA1Tag, cmsSigBToA2Tag};
    std::array<OutputLutVariant, 3> variants{{{0.0}, {0.04}, {-0.04}}};
    for (std::size_t index = 0; index < tags.size(); ++index)
    {
        cmsPipeline *pipeline = output_lut_pipeline(&variants[index]);
        if (pipeline == nullptr || cmsWriteTag(profile, tags[index], pipeline) == 0)
        {
            output_lut_profile_error = "BToA tag failed at index " + std::to_string(index);
            if (pipeline != nullptr)
            {
                cmsPipelineFree(pipeline);
            }
            cmsCloseProfile(profile);
            return {};
        }
        cmsPipelineFree(pipeline);
    }
    cmsPipeline *input_pipeline = input_lut_pipeline();
    if (input_pipeline == nullptr || cmsWriteTag(profile, cmsSigAToB0Tag, input_pipeline) == 0)
    {
        if (input_pipeline != nullptr)
        {
            cmsPipelineFree(input_pipeline);
        }
        output_lut_profile_error = "AToB tag failed";
        cmsCloseProfile(profile);
        return {};
    }
    cmsPipelineFree(input_pipeline);
    if (cmsLinkTag(profile, cmsSigAToB1Tag, cmsSigAToB0Tag) == 0 ||
        cmsLinkTag(profile, cmsSigAToB2Tag, cmsSigAToB0Tag) == 0)
    {
        output_lut_profile_error = "AToB intent links failed";
        cmsCloseProfile(profile);
        return {};
    }
    const auto bytes = profile_bytes(profile);
    if (bytes.empty())
    {
        if (output_lut_profile_error.empty())
        {
            output_lut_profile_error = "profile serialization failed";
        }
    }
    cmsCloseProfile(profile);
    return bytes;
}

[[nodiscard]] LinearWorkingBuffer working_pixel(const std::array<float, 3> &pixel)
{
    LinearWorkingBuffer working;
    working.width = 1;
    working.height = 1;
    working.rgb = {pixel[0], pixel[1], pixel[2]};
    working.color_profile.kind = ColorProfileKind::kBuiltin;
    working.color_profile.model = ColorModel::kRgb;
    working.color_profile.identifier = "linear_rec709";
    working.color_profile.matrix_to_xyz_d50 = kLinearSrgbToXyzD50;
    working.color_profile.has_matrix = true;
    return working;
}

[[nodiscard]] bool write_bytes(const std::filesystem::path &path,
                               const std::vector<std::uint8_t> &bytes)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(stream);
}

[[nodiscard]] ProfiledOutputBuffer
profiled_output(const std::uint32_t width, const std::uint32_t height, std::vector<float> channels)
{
    ProfiledOutputBuffer output;
    output.width = width;
    output.height = height;
    output.channels = std::move(channels);
    output.color_profile.kind = ColorProfileKind::kIcc;
    output.color_profile.model = ColorModel::kRgb;
    output.color_profile.identifier = "fixture-output.icc";
    static const auto fixture_icc = gamma_rgb_profile(2.2);
    output.color_profile.icc_bytes = fixture_icc;
    output.color_profile.matrix_to_xyz_d50 = kLinearSrgbToXyzD50;
    output.color_profile.has_matrix = true;
    return output;
}

TEST(OutputColorTest, FinalRgb8PackerMatchesFrozenCopyOutputAndKeepsRgbOrder)
{
    auto input = profiled_output(2, 1, {-0.25F, 0.0F, 0.5F, 1.0F, 1.5F, 0.25F});
    const auto original_channels = input.channels;
    const auto original_profile = input.color_profile;
    ASSERT_FALSE(original_profile.icc_bytes.empty());

    auto packed = encode_profiled_output_rgb8(input, CancellationToken{});
    ASSERT_TRUE(packed) << packed.error().message;
    EXPECT_EQ(packed.value().width, 2U);
    EXPECT_EQ(packed.value().height, 1U);
    EXPECT_EQ(packed.value().rgb, (std::vector<std::uint8_t>{0U, 0U, 128U, 255U, 255U, 64U}));
    EXPECT_EQ(packed.value().color_profile, original_profile);
    EXPECT_EQ(input.channels, original_channels);
    EXPECT_EQ(input.color_profile, original_profile);

    const auto owned_rgb = packed.value().rgb;
    const auto owned_profile = packed.value().color_profile;
    input.channels.assign(input.channels.size(), 0.0F);
    input.color_profile.identifier = "mutated";
    input.color_profile.icc_bytes.clear();
    EXPECT_EQ(packed.value().rgb, owned_rgb);
    EXPECT_EQ(packed.value().color_profile, owned_profile);
}

TEST(OutputColorTest, FinalRgb8PackerRejectsInvalidDimensionsSizeAndModel)
{
    for (auto input : {profiled_output(0, 1, {}), profiled_output(1, 0, {})})
    {
        auto rejected = encode_profiled_output_rgb8(input, CancellationToken{});
        ASSERT_FALSE(rejected);
        EXPECT_EQ(rejected.error().code, ErrorCode::kValidation);
        EXPECT_EQ(rejected.error().context.at("reason"), "invalid_dimensions");
    }

    auto overflow = profiled_output(std::numeric_limits<std::uint32_t>::max(),
                                    std::numeric_limits<std::uint32_t>::max(), {});
    auto overflow_rejected = encode_profiled_output_rgb8(overflow, CancellationToken{});
    ASSERT_FALSE(overflow_rejected);
    EXPECT_EQ(overflow_rejected.error().code, ErrorCode::kValidation);
    EXPECT_EQ(overflow_rejected.error().context.at("reason"), "dimensions_overflow");

    auto wrong_size = profiled_output(1, 1, {0.0F, 0.0F});
    const auto original_channels = wrong_size.channels;
    auto size_rejected = encode_profiled_output_rgb8(wrong_size, CancellationToken{});
    ASSERT_FALSE(size_rejected);
    EXPECT_EQ(size_rejected.error().code, ErrorCode::kValidation);
    EXPECT_EQ(size_rejected.error().context.at("reason"), "channel_count_mismatch");
    EXPECT_EQ(wrong_size.channels, original_channels);

    auto lab = profiled_output(1, 1, {0.0F, 0.0F, 0.0F});
    lab.color_profile.model = ColorModel::kLab;
    const auto original_profile = lab.color_profile;
    auto model_rejected = encode_profiled_output_rgb8(lab, CancellationToken{});
    ASSERT_FALSE(model_rejected);
    EXPECT_EQ(model_rejected.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(model_rejected.error().context.at("reason"), "unsupported_color_model");
    EXPECT_EQ(lab.color_profile, original_profile);
}

TEST(OutputColorTest, FinalRgb8PackerRejectsEveryNonFiniteSampleWithoutMutatingSource)
{
    const std::array<float, 3> nonfinite{std::numeric_limits<float>::quiet_NaN(),
                                         std::numeric_limits<float>::infinity(),
                                         -std::numeric_limits<float>::infinity()};
    for (const float sample : nonfinite)
    {
        auto input = profiled_output(1, 1, {0.25F, sample, 0.75F});
        const auto original_profile = input.color_profile;
        auto rejected = encode_profiled_output_rgb8(input, CancellationToken{});
        ASSERT_FALSE(rejected);
        EXPECT_EQ(rejected.error().code, ErrorCode::kValidation);
        EXPECT_EQ(rejected.error().context.at("reason"), "non_finite_sample");
        EXPECT_EQ(rejected.error().context.at("sample_index"), "1");
        EXPECT_EQ(input.channels[0], 0.25F);
        if (std::isnan(sample))
        {
            EXPECT_TRUE(std::isnan(input.channels[1]));
        }
        else
        {
            EXPECT_EQ(input.channels[1], sample);
        }
        EXPECT_EQ(input.channels[2], 0.75F);
        EXPECT_EQ(input.color_profile, original_profile);
    }
}

TEST(OutputColorTest, FinalRgb8PackerChecksCancellationBeforeAllocationAndBetweenRows)
{
    auto input = profiled_output(1, 1, {0.25F, 0.5F, 0.75F});
    const auto original_channels = input.channels;
    CancellationSource cancelled;
    ASSERT_TRUE(cancelled.cancel("before_output_pack"));
    auto pre_cancelled = encode_profiled_output_rgb8(input, cancelled.token());
    ASSERT_FALSE(pre_cancelled);
    EXPECT_EQ(pre_cancelled.error().code, ErrorCode::kCancelled);
    EXPECT_EQ(input.channels, original_channels);

    auto large = profiled_output(1024, 4096, {});
    large.channels.assign(static_cast<std::size_t>(large.width) * large.height * 3U, 0.5F);
    const auto deadline = CancellationSource::with_deadline(std::chrono::steady_clock::now() +
                                                            std::chrono::milliseconds{1});
    auto row_cancelled = encode_profiled_output_rgb8(large, deadline.token());
    ASSERT_FALSE(row_cancelled);
    EXPECT_EQ(row_cancelled.error().code, ErrorCode::kCancelled);
    EXPECT_EQ(large.channels.front(), 0.5F);
    EXPECT_EQ(large.channels.back(), 0.5F);
}

TEST(OutputColorTest, EveryFrozenSchemaFivePayloadMapsToSrgbPerceptual)
{
    const auto tests_root = std::filesystem::path(RAVO_REPOSITORY_ROOT) / "legacy" / "tests";
    std::set<std::string> distinct_payloads;
    std::size_t entries = 0;
    for (const auto &directory : std::filesystem::directory_iterator(tests_root))
    {
        if (!directory.is_directory() || directory.path().filename().string().size() < 5U)
        {
            continue;
        }
        for (const auto &path : std::filesystem::directory_iterator(directory.path()))
        {
            if (!path.is_regular_file() || path.path().extension() != ".xmp")
            {
                continue;
            }
            QFile file(QString::fromStdString(path.path().string()));
            ASSERT_TRUE(file.open(QIODevice::ReadOnly));
            QXmlStreamReader reader(file.readAll());
            while (!reader.atEnd())
            {
                reader.readNext();
                if (!reader.isStartElement() || reader.name() != u"li")
                {
                    continue;
                }
                const auto attributes = reader.attributes();
                const auto attribute = [&attributes](const QStringView name)
                {
                    for (const auto &item : attributes)
                    {
                        if (item.name() == name)
                        {
                            return item.value().toString();
                        }
                    }
                    return QString{};
                };
                if (attribute(u"operation") != u"colorout")
                {
                    continue;
                }
                EXPECT_EQ(attribute(u"modversion"), u"5");
                EXPECT_EQ(attribute(u"enabled"), u"1");
                const auto payload = attribute(u"params").toUtf8().toStdString();
                ASSERT_FALSE(payload.empty());
                distinct_payloads.insert(payload);
                auto decoded = decode_legacy_colorout_parameters(payload);
                ASSERT_TRUE(decoded) << decoded.error().message
                                     << " fixture=" << directory.path().filename().string();
                EXPECT_EQ(decoded.value().output_profile, kInputProfileSrgb);
                EXPECT_TRUE(decoded.value().output_profile_filename.empty());
                EXPECT_EQ(decoded.value().rendering_intent, kColorIntentPerceptual);
                EXPECT_TRUE(decoded.value().is_identity());
                ++entries;
            }
            ASSERT_FALSE(reader.hasError()) << reader.errorString().toStdString();
        }
    }
    EXPECT_EQ(entries, 158U);
    EXPECT_EQ(distinct_payloads.size(), 13U);

    auto corrupt = decode_legacy_colorout_parameters("not-a-profile");
    ASSERT_FALSE(corrupt);
    EXPECT_EQ(corrupt.error().code, ErrorCode::kValidation);
}

TEST(OutputColorTest, BuiltInRgbOutputUsesExplicitMatrixAndShaper)
{
    auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    RasterBuffer raster;
    raster.width = 1;
    raster.height = 1;
    raster.srgb = {128, 96, 64};
    raster.color_profile.kind = ColorProfileKind::kBuiltin;
    raster.color_profile.model = ColorModel::kRgb;
    raster.color_profile.identifier = "srgb";

    DevelopParams develop;
    auto default_recipe = recipe_from_develop({"raster", "memory:raster", std::nullopt}, develop);
    ASSERT_TRUE(default_recipe) << default_recipe.error().message;
    RenderRequest request;
    request.asset = default_recipe.value().asset;
    request.recipe = default_recipe.value();
    auto rendered = engine.value().render_to_image(request, &raster);
    ASSERT_TRUE(rendered) << rendered.error().message;
    EXPECT_EQ(rendered.value().color_profile.identifier, kInputProfileSrgb);
    ASSERT_FALSE(rendered.value().color_profile.icc_bytes.empty());
    cmsHPROFILE embedded = cmsOpenProfileFromMem(
        rendered.value().color_profile.icc_bytes.data(),
        static_cast<cmsUInt32Number>(rendered.value().color_profile.icc_bytes.size()));
    ASSERT_NE(embedded, nullptr);
    EXPECT_EQ(cmsGetColorSpace(embedded), cmsSigRgbData);
    cmsCloseProfile(embedded);

    develop.output_color.output_profile = std::string(kInputProfileDisplayP3);
    auto wide_recipe = recipe_from_develop({"raster", "memory:raster", std::nullopt}, develop);
    ASSERT_TRUE(wide_recipe) << wide_recipe.error().message;
    request.recipe = wide_recipe.value();
    auto wide = engine.value().render_to_image(request, &raster);
    ASSERT_TRUE(wide) << wide.error().message;
    EXPECT_EQ(wide.value().color_profile.identifier, kInputProfileDisplayP3);
    EXPECT_NE(wide.value().rgb, rendered.value().rgb);

    develop.output_color.proof_mode = std::string(kProofModeSoftproof);
    auto proof_recipe = recipe_from_develop({"raster", "memory:raster", std::nullopt}, develop);
    ASSERT_TRUE(proof_recipe) << proof_recipe.error().message;
    request.recipe = proof_recipe.value();
    auto proofed = engine.value().render_to_image(request, &raster);
    ASSERT_TRUE(proofed) << proofed.error().message;
    EXPECT_EQ(proofed.value().color_profile.identifier, kInputProfileDisplayP3);
    EXPECT_FALSE(proofed.value().color_profile.icc_bytes.empty());
}

TEST(OutputColorTest, FileIccGeneralLutIntentsAndContentFingerprintAreExplicit)
{
    const auto profile_path =
        std::filesystem::temp_directory_path() / "ravo-output-profile-fingerprint.icc";
    const auto gamma_two = gamma_rgb_profile(2.0);
    ASSERT_FALSE(gamma_two.empty());
    ASSERT_TRUE(write_bytes(profile_path, gamma_two));

    OutputColorParams params;
    params.output_profile = std::string(kInputProfileFileIcc);
    params.output_profile_filename = profile_path.string();
    auto matrix_output =
        apply_output_color(working_pixel({0.2F, 0.4F, 0.6F}), params, CancellationToken{});
    ASSERT_TRUE(matrix_output) << matrix_output.error().message;
    EXPECT_EQ(matrix_output.value().color_profile.kind, ColorProfileKind::kIcc);
    EXPECT_EQ(matrix_output.value().color_profile.model, ColorModel::kRgb);
    EXPECT_EQ(matrix_output.value().color_profile.icc_bytes, gamma_two);

    DevelopParams develop;
    develop.output_color = params;
    auto recipe = recipe_from_develop({"output", "memory:output", std::nullopt}, develop);
    ASSERT_TRUE(recipe) << recipe.error().message;
    auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    auto first = engine.value().output_color_cache_fingerprint(recipe.value());
    ASSERT_TRUE(first) << first.error().message;
    ASSERT_TRUE(write_bytes(profile_path, gamma_rgb_profile(2.2)));
    auto second = engine.value().output_color_cache_fingerprint(recipe.value());
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_NE(first.value(), second.value());

    params.output_profile = std::string(kInputProfileSrgb);
    params.output_profile_filename.clear();
    params.proof_mode = std::string(kProofModeSoftproof);
    params.proof_profile = std::string(kInputProfileFileIcc);
    params.proof_profile_filename = profile_path.string();
    develop.output_color = params;
    auto proof_recipe = recipe_from_develop({"proof", "memory:proof", std::nullopt}, develop);
    ASSERT_TRUE(proof_recipe) << proof_recipe.error().message;
    auto proof_first = engine.value().output_color_cache_fingerprint(proof_recipe.value());
    ASSERT_TRUE(proof_first) << proof_first.error().message;
    auto file_proof =
        apply_output_color(working_pixel({0.2F, 0.4F, 0.6F}), params, CancellationToken{});
    ASSERT_TRUE(file_proof) << file_proof.error().message;
    ASSERT_TRUE(write_bytes(profile_path, gamma_two));
    auto proof_second = engine.value().output_color_cache_fingerprint(proof_recipe.value());
    ASSERT_TRUE(proof_second) << proof_second.error().message;
    EXPECT_NE(proof_first.value(), proof_second.value());

    const auto lut_bytes = rgb_output_lut_profile();
    ASSERT_FALSE(lut_bytes.empty()) << output_lut_profile_error;
    ASSERT_TRUE(write_bytes(profile_path, lut_bytes));
    params.output_profile = std::string(kInputProfileFileIcc);
    params.output_profile_filename = profile_path.string();
    params.proof_mode = std::string(kProofModeOff);
    params.proof_profile = std::string(kInputProfileSrgb);
    params.proof_profile_filename.clear();
    std::vector<std::array<float, 3>> intent_pixels;
    for (const auto intent : kSelectableColorIntents)
    {
        params.rendering_intent = std::string(intent);
        auto transformed =
            apply_output_color(working_pixel({0.2F, 0.4F, 0.6F}), params, CancellationToken{});
        ASSERT_TRUE(transformed) << transformed.error().message << " intent=" << intent;
        EXPECT_EQ(transformed.value().color_profile.icc_bytes, lut_bytes);
        intent_pixels.push_back({transformed.value().channels[0], transformed.value().channels[1],
                                 transformed.value().channels[2]});
    }
    EXPECT_NE(intent_pixels[0], intent_pixels[2]);

    params.rendering_intent = std::string(kColorIntentRelative);
    params.black_point_compensation = false;
    auto without_bpc =
        apply_output_color(working_pixel({0.02F, 0.03F, 0.04F}), params, CancellationToken{});
    ASSERT_TRUE(without_bpc) << without_bpc.error().message;
    params.black_point_compensation = true;
    auto with_bpc =
        apply_output_color(working_pixel({0.02F, 0.03F, 0.04F}), params, CancellationToken{});
    ASSERT_TRUE(with_bpc) << with_bpc.error().message;
    EXPECT_NE(with_bpc.value().channels, without_bpc.value().channels);

    ASSERT_TRUE(write_bytes(profile_path, {1U, 2U, 3U, 4U}));
    auto corrupt =
        apply_output_color(working_pixel({0.2F, 0.4F, 0.6F}), params, CancellationToken{});
    ASSERT_FALSE(corrupt);
    EXPECT_EQ(corrupt.error().code, ErrorCode::kValidation);
    std::error_code ignored;
    std::filesystem::remove(profile_path, ignored);
    auto missing =
        apply_output_color(working_pixel({0.2F, 0.4F, 0.6F}), params, CancellationToken{});
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, ErrorCode::kNotFound);
}

TEST(OutputColorTest, XyzLabSoftproofAndGamutWarningUseOwnedResultState)
{
    const auto working = working_pixel({0.2F, 0.4F, 0.6F});
    OutputColorParams params;
    params.output_profile = std::string(kInputProfileXyz);
    auto xyz = apply_output_color(working, params, CancellationToken{});
    ASSERT_TRUE(xyz) << xyz.error().message;
    EXPECT_EQ(xyz.value().color_profile.model, ColorModel::kXyz);
    EXPECT_EQ(xyz.value().color_profile.identifier, kInputProfileXyz);
    EXPECT_FALSE(xyz.value().color_profile.icc_bytes.empty());
    EXPECT_NEAR(xyz.value().channels[0],
                0.2F * kLinearSrgbToXyzD50[0] + 0.4F * kLinearSrgbToXyzD50[1] +
                    0.6F * kLinearSrgbToXyzD50[2],
                2.0e-4F);

    params.output_profile = std::string(kInputProfileLab);
    auto lab = apply_output_color(working, params, CancellationToken{});
    ASSERT_TRUE(lab) << lab.error().message;
    EXPECT_EQ(lab.value().color_profile.model, ColorModel::kLab);
    EXPECT_GT(lab.value().channels[0], 0.0F);
    EXPECT_TRUE(std::all_of(lab.value().channels.begin(), lab.value().channels.end(),
                            [](const float value) { return std::isfinite(value); }));

    params.output_profile = std::string(kInputProfileDisplayP3);
    params.proof_profile = std::string(kInputProfileSrgb);
    const auto proof_source = working_pixel({1.2F, 0.0F, 0.0F});
    auto normal = apply_output_color(proof_source, params, CancellationToken{});
    ASSERT_TRUE(normal) << normal.error().message;
    params.proof_mode = std::string(kProofModeSoftproof);
    auto proofed = apply_output_color(proof_source, params, CancellationToken{});
    ASSERT_TRUE(proofed) << proofed.error().message;
    EXPECT_EQ(proofed.value().color_profile.identifier, kInputProfileDisplayP3);
    EXPECT_NE(proofed.value().channels, normal.value().channels);

    params.proof_mode = std::string(kProofModeGamutCheck);
    auto gamut =
        apply_output_color(working_pixel({2.0F, -0.5F, 1.5F}), params, CancellationToken{});
    ASSERT_TRUE(gamut) << gamut.error().message;
    EXPECT_NEAR(gamut.value().channels[0], 0.0F, 1.0e-6F);
    EXPECT_NEAR(gamut.value().channels[1], 1.0F, 1.0e-6F);
    EXPECT_NEAR(gamut.value().channels[2], 1.0F, 1.0e-6F);
}

TEST(OutputColorTest, FrozenNopAndMire1HaveSrgbWideAndFileIccReferences)
{
    const auto repository = std::filesystem::path(RAVO_REPOSITORY_ROOT);
    const auto xmp_path = repository / "legacy" / "tests" / "0000-nop" / "nop.xmp";
    const auto raw_path = repository / "legacy" / "tests" / "images" / "mire1.cr2";
    QFile xmp(QString::fromStdString(xmp_path.string()));
    ASSERT_TRUE(xmp.open(QIODevice::ReadOnly));
    const auto xmp_bytes = xmp.readAll();
    auto recipe = import_legacy_xmp(
        {std::string_view(xmp_bytes.constData(), static_cast<std::size_t>(xmp_bytes.size())),
         {"mire1", raw_path.string(), std::nullopt}});
    ASSERT_TRUE(recipe) << recipe.error().message;
    auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    RenderRequest request;
    request.asset = recipe.value().asset;
    request.recipe = recipe.value();
    request.output_width = 64;
    request.output_height = 48;
    const auto sums = [](const RenderedImage &image)
    {
        std::array<std::uint64_t, 3> result{};
        for (std::size_t offset = 0; offset < image.rgb.size(); offset += 3U)
        {
            for (std::size_t channel = 0; channel < result.size(); ++channel)
            {
                result[channel] += image.rgb[offset + channel];
            }
        }
        return result;
    };

    auto srgb = engine.value().render_to_image(request);
    ASSERT_TRUE(srgb) << srgb.error().message;
    EXPECT_EQ(srgb.value().color_profile.identifier, kInputProfileSrgb);
    const auto srgb_sums = sums(srgb.value());

    auto set_output = [&request](const OutputColorParams &params)
    {
        for (auto &operation : request.recipe.operations)
        {
            if (operation.id == "ravo.color.output")
            {
                operation.parameters = output_color_to_parameters(params);
                return true;
            }
        }
        return false;
    };
    OutputColorParams wide_params;
    wide_params.output_profile = std::string(kInputProfileDisplayP3);
    ASSERT_TRUE(set_output(wide_params));
    auto wide = engine.value().render_to_image(request);
    ASSERT_TRUE(wide) << wide.error().message;
    const auto wide_sums = sums(wide.value());

    const auto profile_path =
        std::filesystem::temp_directory_path() / "ravo-mire1-output-reference.icc";
    const auto file_profile = gamma_rgb_profile(2.2);
    ASSERT_TRUE(write_bytes(profile_path, file_profile));
    OutputColorParams file_params;
    file_params.output_profile = std::string(kInputProfileFileIcc);
    file_params.output_profile_filename = profile_path.string();
    ASSERT_TRUE(set_output(file_params));
    auto file = engine.value().render_to_image(request);
    ASSERT_TRUE(file) << file.error().message;
    const auto file_sums = sums(file.value());
    EXPECT_EQ(file.value().color_profile.icc_bytes, file_profile);

    const std::array<std::uint64_t, 3> srgb_reference{315223U, 294062U, 276997U};
    const std::array<std::uint64_t, 3> wide_reference{313102U, 295339U, 281568U};
    const std::array<std::uint64_t, 3> file_reference{316541U, 296731U, 280878U};
    for (std::size_t channel = 0; channel < 3U; ++channel)
    {
        EXPECT_NEAR(static_cast<double>(srgb_sums[channel]),
                    static_cast<double>(srgb_reference[channel]), 2000.0);
        EXPECT_NEAR(static_cast<double>(wide_sums[channel]),
                    static_cast<double>(wide_reference[channel]), 2000.0);
        EXPECT_NEAR(static_cast<double>(file_sums[channel]),
                    static_cast<double>(file_reference[channel]), 2000.0);
    }
    EXPECT_NE(wide_sums, srgb_sums);
    EXPECT_NE(file_sums, srgb_sums);
    std::error_code ignored;
    std::filesystem::remove(profile_path, ignored);
}

TEST(OutputColorTest, UnboundedCancellationAndNonFiniteInputNeverPublishPixels)
{
    LinearWorkingBuffer working;
    working.width = 1;
    working.height = 1;
    working.rgb = {0.25F, 1.5F, -0.25F};
    working.color_profile.kind = ColorProfileKind::kBuiltin;
    working.color_profile.model = ColorModel::kRgb;
    working.color_profile.identifier = "linear_rec709";
    working.color_profile.matrix_to_xyz_d50 = {0.4360747F, 0.3850649F, 0.1430804F,
                                               0.2225045F, 0.7168786F, 0.0606169F,
                                               0.0139322F, 0.0971045F, 0.7141733F};
    working.color_profile.has_matrix = true;
    const auto original = working;

    auto output = apply_output_color(working, OutputColorParams{}, CancellationToken{});
    ASSERT_TRUE(output) << output.error().message;
    EXPECT_NEAR(output.value().channels[0], 0.5370987F, 2.0e-5F);
    EXPECT_GT(output.value().channels[1], 1.0F);
    EXPECT_NEAR(output.value().channels[2], 0.0F, 1.0e-6F);
    EXPECT_EQ(working.width, original.width);
    EXPECT_EQ(working.height, original.height);
    EXPECT_EQ(working.rgb, original.rgb);
    EXPECT_EQ(working.color_profile, original.color_profile);

    CancellationSource cancellation;
    ASSERT_TRUE(cancellation.cancel("output_color"));
    auto cancelled = apply_output_color(working, OutputColorParams{}, cancellation.token());
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, ErrorCode::kCancelled);

    auto large = working_pixel({0.5F, 0.5F, 0.5F});
    large.width = 1024;
    large.height = 4096;
    large.rgb.assign(static_cast<std::size_t>(large.width) * large.height * 3U, 0.5F);
    const auto large_original = large.rgb;
    const auto deadline = CancellationSource::with_deadline(std::chrono::steady_clock::now() +
                                                            std::chrono::milliseconds{1});
    auto row_cancelled = apply_output_color(large, OutputColorParams{}, deadline.token());
    ASSERT_FALSE(row_cancelled);
    EXPECT_EQ(row_cancelled.error().code, ErrorCode::kCancelled);
    EXPECT_EQ(large.rgb, large_original);

    working.rgb[1] = std::numeric_limits<float>::infinity();
    auto invalid = apply_output_color(working, OutputColorParams{}, CancellationToken{});
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code, ErrorCode::kValidation);

    working = working_pixel({0.2F, 0.3F, 0.4F});
    working.color_profile.matrix_to_xyz_d50 = {};
    OutputColorParams display_p3;
    display_p3.output_profile = std::string(kInputProfileDisplayP3);
    auto singular = apply_output_color(working, display_p3, CancellationToken{});
    ASSERT_FALSE(singular);
    EXPECT_EQ(singular.error().code, ErrorCode::kValidation);

    auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    RenderedImage corrupt_image;
    corrupt_image.width = 1;
    corrupt_image.height = 1;
    corrupt_image.rgb = {0U, 0U, 0U};
    corrupt_image.color_profile.kind = ColorProfileKind::kIcc;
    corrupt_image.color_profile.model = ColorModel::kRgb;
    corrupt_image.color_profile.identifier = "corrupt.icc";
    corrupt_image.color_profile.icc_bytes = {1U, 2U, 3U, 4U};
    auto corrupt_png = engine.value().encode_png(corrupt_image);
    ASSERT_FALSE(corrupt_png);
    EXPECT_EQ(corrupt_png.error().code, ErrorCode::kValidation);
}

} // namespace
} // namespace ravo
