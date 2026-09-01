#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cfenv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <memory>
#include <numeric>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <png.h>

#include <QBuffer>
#include <QColor>
#include <QFile>
#include <QImage>
#include <zlib.h>

#include "ravo/domain/types.h"
#include "ravo/engine/engine.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/operation.h"

#include "color_balance_fixture.h"
#include "capability_ops.h"
#include "capture_metadata_test_support.h"
#include "color_balance_rgb.h"
#include "color_checker.h"
#include "color_harmonizer.h"
#include "color_contrast.h"
#include "d50_lab.h"
#include "dt_ucs.h"
#include "harmony_geometry.h"
#include "image_ops.h"
#include "input_color.h"
#include "primaries.h"
#include "raw_pipeline.h"
#include "raw_temperature.h"
#include "recursive_gaussian.h"
#include "temperature_fixture.h"
#include "test_support.h"

namespace ravo
{
namespace
{

class RecordingProgressSink final : public ProgressSink
{
public:
    void on_progress(const ProgressEvent &event) override
    {
        events.push_back(event);
    }

    std::vector<ProgressEvent> events;
};

[[nodiscard]] std::string mire1_path()
{
    const auto path =
        std::filesystem::path(RAVO_REPOSITORY_ROOT) / "Ravo" / "tests" / "fixtures" / "frozen" / "images" / "mire1.cr2";
    const auto utf8 = path.generic_u8string();
    return {utf8.begin(), utf8.end()};
}

[[nodiscard]] std::string mire1_xtrans_path()
{
    const auto path = std::filesystem::path(RAVO_REPOSITORY_ROOT) / "Ravo" / "tests" / "fixtures" / "frozen" / "images" /
                      "mire1-xtrans.raf";
    const auto utf8 = path.generic_u8string();
    return {utf8.begin(), utf8.end()};
}

struct SourceFileSnapshot
{
    std::uintmax_t size = 0;
    std::filesystem::file_time_type modified;
    std::uint64_t content_hash = 1469598103934665603ULL;

    bool operator==(const SourceFileSnapshot &) const = default;
};

[[nodiscard]] std::optional<SourceFileSnapshot> source_file_snapshot(const std::string &path)
{
    std::error_code error;
    SourceFileSnapshot result;
    result.size = std::filesystem::file_size(path, error);
    if (error)
    {
        return std::nullopt;
    }
    result.modified = std::filesystem::last_write_time(path, error);
    if (error)
    {
        return std::nullopt;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        return std::nullopt;
    }
    std::array<char, 64U * 1024U> block{};
    while (input)
    {
        input.read(block.data(), static_cast<std::streamsize>(block.size()));
        const auto read = input.gcount();
        for (std::streamsize index = 0; index < read; ++index)
        {
            result.content_hash ^=
                static_cast<std::uint8_t>(block[static_cast<std::size_t>(index)]);
            result.content_hash *= 1099511628211ULL;
        }
    }
    if (!input.eof())
    {
        return std::nullopt;
    }
    return result;
}

struct DecodedPng
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<png_byte> pixels;
};

void declare_srgb(RasterBuffer &raster)
{
    raster.color_profile.kind = ColorProfileKind::kBuiltin;
    raster.color_profile.model = ColorModel::kRgb;
    raster.color_profile.identifier = "srgb";
}

void declare_linear_srgb_matrix(DecodedRaw &raw)
{
    raw.color_profile.kind = ColorProfileKind::kMatrix;
    raw.color_profile.model = ColorModel::kRgb;
    raw.color_profile.identifier = "enhanced_matrix";
    raw.color_profile.matrix_to_xyz_d50 = {0.4360747F, 0.3850649F, 0.1430804F,
                                           0.2225045F, 0.7168786F, 0.0606169F,
                                           0.0139322F, 0.0971045F, 0.7141733F};
    raw.color_profile.has_matrix = true;
    raw.color_profile.camera_input = true;
}

void declare_input(Recipe &recipe)
{
    recipe.operations.push_back({"ravo.color.input", 1, "color-input-1", true,
                                 input_color_to_parameters(InputColorParams{}), std::nullopt});
    recipe.operations.push_back({"ravo.color.output", 1, "color-output-1", true,
                                 output_color_to_parameters(OutputColorParams{}), std::nullopt});
}

[[nodiscard]] std::shared_ptr<const ExposureAnalysisContext>
exposure_analysis(const std::initializer_list<std::pair<std::uint16_t, std::uint32_t>> bins,
                  const std::uint32_t black_level, const std::uint32_t white_level,
                  RawExposureMetadata metadata = {})
{
    auto context = std::make_shared<ExposureAnalysisContext>();
    context->raw_histogram.assign(kExposureRawHistogramBins, 0U);
    for (const auto &[bin, count] : bins)
    {
        context->raw_histogram[bin] += count;
        context->raw_pixel_count += count;
    }
    context->raw_black_level = black_level;
    context->raw_white_level = white_level;
    context->metadata = std::move(metadata);
    return context;
}

[[nodiscard]] std::optional<DecodedPng> read_rgb_png(const std::filesystem::path &path)
{
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    if (png_image_begin_read_from_file(&image, path.string().c_str()) == 0)
    {
        return std::nullopt;
    }
    image.format = PNG_FORMAT_RGB;
    DecodedPng result{image.width, image.height, std::vector<png_byte>(PNG_IMAGE_SIZE(image))};
    if (png_image_finish_read(&image, nullptr, result.pixels.data(), 0, nullptr) == 0)
    {
        png_image_free(&image);
        return std::nullopt;
    }
    png_image_free(&image);
    return result;
}

[[nodiscard]] std::optional<DecodedPng> read_rgb_png(const std::vector<std::uint8_t> &encoded)
{
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    if (encoded.empty() ||
        png_image_begin_read_from_memory(&image, encoded.data(), encoded.size()) == 0)
    {
        return std::nullopt;
    }
    image.format = PNG_FORMAT_RGB;
    DecodedPng result{image.width, image.height, std::vector<png_byte>(PNG_IMAGE_SIZE(image))};
    if (png_image_finish_read(&image, nullptr, result.pixels.data(), 0, nullptr) == 0)
    {
        png_image_free(&image);
        return std::nullopt;
    }
    png_image_free(&image);
    return result;
}

[[nodiscard]] std::size_t png_chunk_count(const std::string &png_bytes,
                                          const std::string_view chunk_type)
{
    if (png_bytes.size() < 8U || chunk_type.size() != 4U)
    {
        return 0;
    }
    std::size_t offset = 8U;
    std::size_t count = 0;
    while (png_bytes.size() - offset >= 12U)
    {
        const auto byte = [&png_bytes, offset](const std::size_t index)
        {
            return static_cast<std::uint32_t>(
                static_cast<unsigned char>(png_bytes[offset + index]));
        };
        const auto length = (byte(0U) << 24U) | (byte(1U) << 16U) | (byte(2U) << 8U) | byte(3U);
        const auto remaining = png_bytes.size() - offset;
        if (static_cast<std::size_t>(length) > remaining - 12U)
        {
            return 0;
        }
        if (std::equal(chunk_type.begin(), chunk_type.end(), png_bytes.data() + offset + 4U))
        {
            ++count;
        }
        offset += 12U + static_cast<std::size_t>(length);
    }
    return offset == png_bytes.size() ? count : 0;
}

[[nodiscard]] RasterBuffer solid_raster(const std::uint32_t width, const std::uint32_t height,
                                        const std::uint8_t r, const std::uint8_t g,
                                        const std::uint8_t b)
{
    RasterBuffer raster;
    raster.width = width;
    raster.height = height;
    raster.source_width = width;
    raster.source_height = height;
    declare_srgb(raster);
    raster.srgb.resize(static_cast<std::size_t>(width) * height * 3U);
    for (std::size_t index = 0; index < raster.srgb.size(); index += 3)
    {
        raster.srgb[index] = r;
        raster.srgb[index + 1U] = g;
        raster.srgb[index + 2U] = b;
    }
    return raster;
}

[[nodiscard]] RasterBuffer gradient_raster()
{
    RasterBuffer raster;
    raster.width = 16;
    raster.height = 16;
    raster.source_width = 16;
    raster.source_height = 16;
    declare_srgb(raster);
    raster.srgb.resize(16U * 16U * 3U);
    for (std::uint32_t y = 0; y < 16; ++y)
    {
        for (std::uint32_t x = 0; x < 16; ++x)
        {
            const std::size_t index = (static_cast<std::size_t>(y) * 16U + x) * 3U;
            raster.srgb[index] = static_cast<std::uint8_t>(x * 16U);
            raster.srgb[index + 1U] = static_cast<std::uint8_t>(y * 16U);
            raster.srgb[index + 2U] = 80;
        }
    }
    return raster;
}

[[nodiscard]] std::uint64_t mean_luma(const RenderedImage &image)
{
    std::uint64_t sum = 0;
    for (std::size_t index = 0; index + 2 < image.rgb.size(); index += 3)
    {
        sum += static_cast<std::uint64_t>(image.rgb[index]) * 21U +
               static_cast<std::uint64_t>(image.rgb[index + 1U]) * 72U +
               static_cast<std::uint64_t>(image.rgb[index + 2U]) * 7U;
    }
    return sum / std::max<std::size_t>(1, image.rgb.size() / 3U);
}

[[nodiscard]] Result<RenderedImage> render_op(const EngineFacade &engine, RasterBuffer raster,
                                              OperationInstance operation)
{
    if (raster.color_profile.kind == ColorProfileKind::kMissing)
    {
        declare_srgb(raster);
    }
    Recipe recipe;
    recipe.asset = {"raster", "memory:raster", std::nullopt};
    declare_input(recipe);
    recipe.operations.push_back(std::move(operation));
    RenderRequest request;
    request.asset = recipe.asset;
    request.recipe = recipe;
    return engine.render_to_image(request, &raster);
}

[[nodiscard]] OperationInstance channel_mixer_operation(const ChannelMixerParams &params,
                                                        std::string instance_id = "calibration-1")
{
    return {"ravo.color.channelmixerrgb",        1,           std::move(instance_id), true,
            channel_mixer_to_parameters(params), std::nullopt};
}

[[nodiscard]] OperationInstance
color_balance_rgb_operation(const ColorBalanceRgbParams &params,
                            std::string instance_id = "colorbalancergb-1")
{
    return {"ravo.color.colorbalancergb",
            1,
            std::move(instance_id),
            true,
            color_balance_rgb_to_parameters(params),
            std::nullopt};
}

[[nodiscard]] OperationInstance
legacy_color_balance_operation(const ColorBalanceParams &params,
                               std::string instance_id = "colorbalance-1")
{
    return {std::string(kColorBalanceOperationId),
            kColorBalanceOperationSchemaVersion,
            std::move(instance_id),
            true,
            color_balance_to_parameters(params),
            std::nullopt};
}

[[nodiscard]] WorkingImage legacy_color_balance_working_fixture()
{
    ColorProfileState profile;
    profile.kind = ColorProfileKind::kIcc;
    profile.model = ColorModel::kRgb;
    profile.identifier = std::string(kInputProfileLinearRec709);
    profile.icc_bytes = {1U, 2U, 3U, 4U};
    auto analysis = std::make_shared<ExposureAnalysisContext>();
    analysis->raw_pixel_count = 6U;
    return {
        2U, 1U, {0.03F, 0.18F, 0.72F, 0.91F, 0.42F, 0.07F}, std::move(profile), std::move(analysis),
        {}, {}};
}

TEST(ProfileDenoiseTest, AdaptiveWaveletsReduceFlatNoiseAndPreserveStepEdges)
{
    WorkingImage source;
    source.width = 256U;
    source.height = 128U;
    source.canonical_roi_scale = CanonicalRoiScale::from_scaled_dimensions(256U, 128U, 256U, 128U);
    std::uint32_t state = 0x4d595df4U;
    for (std::uint32_t y = 0U; y < source.height; ++y)
    {
        for (std::uint32_t x = 0U; x < source.width; ++x)
        {
            float noise = 0.0F;
            for (int sample = 0; sample < 6; ++sample)
            {
                state = state * 1664525U + 1013904223U;
                noise += static_cast<float>((state >> 16U) & 0xffffU) / 65535.0F - 0.5F;
            }
            noise *= 0.012F;
            const float base = x < source.width / 2U ? 0.06F : 0.32F;
            const float value = base + noise;
            source.rgb.insert(source.rgb.end(), {value, value, value});
        }
    }
    const auto original = source;
    WorkingImage denoised = source;
    const OperationInstance operation{"ravo.detail.denoiseprofile",
                                      1,
                                      "adaptive-denoise",
                                      true,
                                      {{"strength", ParameterValue{1.0}},
                                       {"chroma", ParameterValue{1.0}},
                                       {"radius", ParameterValue{1.0}}},
                                      std::nullopt};
    const auto applied = apply_denoise_profile(denoised, operation, CancellationToken{});
    ASSERT_TRUE(applied) << applied.error().message;

    const auto region_stats =
        [](const WorkingImage &image, const std::uint32_t first_x, const std::uint32_t last_x)
    {
        double sum = 0.0;
        double squared = 0.0;
        std::size_t count = 0U;
        for (std::uint32_t y = 8U; y + 8U < image.height; ++y)
        {
            for (std::uint32_t x = first_x; x < last_x; ++x)
            {
                const float value = image.rgb[(static_cast<std::size_t>(y) * image.width + x) * 3U];
                sum += value;
                squared += static_cast<double>(value) * value;
                ++count;
            }
        }
        const double mean = sum / static_cast<double>(count);
        return std::array<double, 2>{mean, squared / static_cast<double>(count) - mean * mean};
    };
    const auto source_dark = region_stats(source, 16U, 96U);
    const auto source_bright = region_stats(source, 160U, 240U);
    const auto output_dark = region_stats(denoised, 16U, 96U);
    const auto output_bright = region_stats(denoised, 160U, 240U);
    EXPECT_LT(output_dark[1], source_dark[1] * 0.35);
    EXPECT_LT(output_bright[1], source_bright[1] * 0.35);
    EXPECT_GT(output_bright[0] - output_dark[0], 0.95 * (source_bright[0] - source_dark[0]));
    EXPECT_EQ(source.rgb, original.rgb);
    EXPECT_EQ(denoised.canonical_roi_scale.value(), source.canonical_roi_scale.value());
}

TEST(ProfileDenoiseTest, ChromaMixAndRadiusHaveIndependentObservableResponses)
{
    WorkingImage source;
    source.width = 256U;
    source.height = 128U;
    source.canonical_roi_scale = CanonicalRoiScale::from_scaled_dimensions(256U, 128U, 256U, 128U);
    for (std::uint32_t y = 0U; y < source.height; ++y)
    {
        for (std::uint32_t x = 0U; x < source.width; ++x)
        {
            const float fine = ((x * 37U + y * 19U) % 101U) / 2500.0F - 0.02F;
            const float broad = 0.012F * std::sin(static_cast<float>(x) * 0.12F);
            const float red = 0.16F + fine + broad;
            const float green = 0.16F - fine * (0.2126F / 0.7152F) + broad;
            const float blue = 0.16F + broad;
            source.rgb.insert(source.rgb.end(), {red, green, blue});
        }
    }
    const auto apply = [&](const double chroma, const double radius)
    {
        WorkingImage output = source;
        const OperationInstance operation{"ravo.detail.denoiseprofile",
                                          1,
                                          "adaptive-denoise",
                                          true,
                                          {{"strength", ParameterValue{1.0}},
                                           {"chroma", ParameterValue{chroma}},
                                           {"radius", ParameterValue{radius}}},
                                          std::nullopt};
        const auto result = apply_denoise_profile(output, operation, CancellationToken{});
        EXPECT_TRUE(result) << (result ? "" : result.error().message);
        return output;
    };
    const auto luma_only = apply(0.0, 1.0);
    const auto full_chroma = apply(1.0, 1.0);
    const auto wide = apply(1.0, 8.0);
    const auto chroma_variance = [](const WorkingImage &image)
    {
        double sum = 0.0;
        double squared = 0.0;
        const std::size_t count = static_cast<std::size_t>(image.width) * image.height;
        for (std::size_t pixel = 0U; pixel < count; ++pixel)
        {
            const double chroma = image.rgb[pixel * 3U] - image.rgb[pixel * 3U + 1U];
            sum += chroma;
            squared += chroma * chroma;
        }
        const double mean = sum / static_cast<double>(count);
        return squared / static_cast<double>(count) - mean * mean;
    };
    EXPECT_GT(chroma_variance(luma_only), chroma_variance(source) * 0.95);
    EXPECT_LT(chroma_variance(full_chroma), chroma_variance(source) * 0.45);
    double radius_difference = 0.0;
    for (std::size_t index = 0U; index < wide.rgb.size(); ++index)
    {
        radius_difference += std::abs(wide.rgb[index] - full_chroma.rgb[index]);
    }
    EXPECT_GT(radius_difference / static_cast<double>(wide.rgb.size()), 1.0e-4);
}

TEST(ProfileDenoiseTest, CanonicalScaleTracksARepresentedTwoByTwoReduction)
{
    WorkingImage full;
    full.width = 256U;
    full.height = 128U;
    full.canonical_roi_scale = CanonicalRoiScale::from_scaled_dimensions(256U, 128U, 256U, 128U);
    for (std::uint32_t y = 0U; y < full.height; ++y)
    {
        for (std::uint32_t x = 0U; x < full.width; ++x)
        {
            const std::uint32_t bx = x / 2U;
            const std::uint32_t by = y / 2U;
            const float noise = ((bx * 29U + by * 43U) % 97U) / 5000.0F - 0.0096F;
            const float base = 0.08F + 0.22F * static_cast<float>(bx) / 127.0F;
            const float value = base + noise;
            full.rgb.insert(full.rgb.end(), {value, value, value});
        }
    }
    WorkingImage half;
    half.width = 128U;
    half.height = 64U;
    half.canonical_roi_scale = CanonicalRoiScale::from_scaled_dimensions(128U, 64U, 256U, 128U);
    for (std::uint32_t y = 0U; y < half.height; ++y)
    {
        for (std::uint32_t x = 0U; x < half.width; ++x)
        {
            const std::size_t source_pixel =
                (static_cast<std::size_t>(y * 2U) * full.width + x * 2U) * 3U;
            half.rgb.insert(half.rgb.end(), {full.rgb[source_pixel], full.rgb[source_pixel + 1U],
                                             full.rgb[source_pixel + 2U]});
        }
    }
    const OperationInstance operation{"ravo.detail.denoiseprofile",
                                      1,
                                      "adaptive-denoise",
                                      true,
                                      {{"strength", ParameterValue{0.7}},
                                       {"chroma", ParameterValue{0.7}},
                                       {"radius", ParameterValue{2.0}}},
                                      std::nullopt};
    ASSERT_TRUE(apply_denoise_profile(full, operation, CancellationToken{}));
    ASSERT_TRUE(apply_denoise_profile(half, operation, CancellationToken{}));
    double error = 0.0;
    std::size_t samples = 0U;
    for (std::uint32_t y = 8U; y + 8U < half.height; ++y)
    {
        for (std::uint32_t x = 8U; x + 8U < half.width; ++x)
        {
            const std::size_t half_offset = (static_cast<std::size_t>(y) * half.width + x) * 3U;
            const std::size_t full_offset =
                (static_cast<std::size_t>(y * 2U) * full.width + x * 2U) * 3U;
            error += std::abs(half.rgb[half_offset] - full.rgb[full_offset]);
            ++samples;
        }
    }
    EXPECT_LT(error / static_cast<double>(samples), 0.012);
}

TEST(ProfileDenoiseTest, InvalidInputCancellationAndMemoryBudgetFailAtomically)
{
    WorkingImage source;
    source.width = 64U;
    source.height = 32U;
    source.rgb.assign(64U * 32U * 3U, 0.1F);
    const auto original = source;
    OperationInstance operation{"ravo.detail.denoiseprofile",
                                1,
                                "adaptive-denoise",
                                true,
                                {{"strength", ParameterValue{0.8}},
                                 {"chroma", ParameterValue{0.6}},
                                 {"radius", ParameterValue{2.0}}},
                                std::nullopt};
    operation.parameters["strength"] = ParameterValue{0.0};
    auto identity = apply_denoise_profile(source, operation, CancellationToken{});
    ASSERT_TRUE(identity) << identity.error().message;
    EXPECT_EQ(source.rgb, original.rgb);
    operation.parameters["strength"] = ParameterValue{0.8};
    auto rejected = apply_denoise_profile(source, operation, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "invalid_profile_denoise_roi_scale");
    EXPECT_EQ(source.rgb, original.rgb);

    source.canonical_roi_scale = CanonicalRoiScale::from_scaled_dimensions(64U, 32U, 64U, 32U);
    source.rgb[17U] = std::numeric_limits<float>::quiet_NaN();
    const auto non_finite = source.rgb;
    rejected = apply_denoise_profile(source, operation, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "non_finite_profile_denoise_input");
    for (std::size_t index = 0U; index < source.rgb.size(); ++index)
    {
        EXPECT_EQ(std::bit_cast<std::uint32_t>(source.rgb[index]),
                  std::bit_cast<std::uint32_t>(non_finite[index]));
    }

    source.rgb.assign(source.rgb.size(), 0.1F);
    operation.parameters["strength"] = ParameterValue{2.0};
    rejected = apply_denoise_profile(source, operation, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "invalid_profile_denoise_parameters");
    operation.parameters["strength"] = ParameterValue{0.8};
    CancellationSource cancellation;
    ASSERT_TRUE(cancellation.cancel("profile-denoise-pre"));
    rejected = apply_denoise_profile(source, operation, cancellation.token());
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kCancelled);
    EXPECT_EQ(source.rgb, std::vector<float>(source.rgb.size(), 0.1F));

    WorkingImage row_cancelled = source;
    row_cancelled.width = 512U;
    row_cancelled.height = 1024U;
    row_cancelled.rgb.assign(
        static_cast<std::size_t>(row_cancelled.width) * row_cancelled.height * 3U, 0.1F);
    row_cancelled.canonical_roi_scale = CanonicalRoiScale::from_scaled_dimensions(
        row_cancelled.width, row_cancelled.height, row_cancelled.width, row_cancelled.height);
    const auto deadline = CancellationSource::with_deadline(std::chrono::steady_clock::now() +
                                                            std::chrono::milliseconds{1});
    rejected = apply_denoise_profile(row_cancelled, operation, deadline.token());
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kCancelled);
    EXPECT_EQ(row_cancelled.rgb.front(), 0.1F);
    EXPECT_EQ(row_cancelled.rgb.back(), 0.1F);

    DecodedRaw raw;
    raw.width = 64U;
    raw.height = 32U;
    raw.pixels.assign(64U * 32U, 0U);
    Recipe baseline_recipe;
    Recipe denoise_recipe;
    denoise_recipe.operations.push_back(operation);
    const std::uint64_t baseline = estimate_raw_render_memory(raw, baseline_recipe, 64U, 32U);
    const std::uint64_t estimated = estimate_raw_render_memory(raw, denoise_recipe, 64U, 32U);
    const std::uint64_t rgb_plane = 64U * 32U * 3U * sizeof(float);
    const std::uint64_t sample_bytes = 64U * 32U * sizeof(float);
    const std::uint64_t coordinate_bytes = (64U + 32U) * 5U * sizeof(int);
    EXPECT_EQ(estimated - baseline, 4U * rgb_plane + std::max(sample_bytes, coordinate_bytes));
}

TEST(ToneEqualizerTest, FiveControlsDriveNormalizedNineBandEvAnchors)
{
    constexpr std::array<float, 9> anchors{-8.0F, -7.0F, -6.0F, -5.0F, -4.0F,
                                           -3.0F, -2.0F, -1.0F, 0.0F};
    constexpr std::array<float, 5> centers{-8.0F, -6.0F, -4.0F, -2.0F, 0.0F};
    constexpr std::array<std::string_view, 5> names{"blacks", "shadows", "midtones", "highlights",
                                                    "whites"};
    constexpr std::array<float, 3> colour{0.78F, 0.43F, 0.19F};
    const float colour_norm = std::hypot(colour[0], colour[1], colour[2]);

    for (std::size_t selected = 0U; selected < names.size(); ++selected)
    {
        WorkingImage source;
        source.width = 32U;
        source.height = 8U;
        source.canonical_roi_scale = CanonicalRoiScale::from_scaled_dimensions(32U, 8U, 32U, 8U);
        const float scale = std::exp2(centers[selected]) / colour_norm;
        for (std::size_t pixel = 0U; pixel < source.width * source.height; ++pixel)
        {
            source.rgb.insert(source.rgb.end(),
                              {colour[0] * scale, colour[1] * scale, colour[2] * scale});
        }
        const auto original = source;
        Recipe recipe;
        recipe.operations.push_back({"ravo.core.toneequal",
                                     1,
                                     "toneequal-normalized",
                                     true,
                                     {{std::string(names[selected]), ParameterValue{1.0}}},
                                     std::nullopt});

        const auto adjusted = apply_recipe_ops(source, recipe, CancellationToken{});
        ASSERT_TRUE(adjusted) << adjusted.error().message;
        std::array<double, 5> band_ev{};
        band_ev[selected] = 1.0;
        const std::array<double, 9> anchor_ev{
            band_ev[0], 0.5 * (band_ev[0] + band_ev[1]),
            band_ev[1], 0.5 * (band_ev[1] + band_ev[2]),
            band_ev[2], 0.5 * (band_ev[2] + band_ev[3]),
            band_ev[3], 0.5 * (band_ev[3] + band_ev[4]),
            band_ev[4],
        };
        double weighted = 0.0;
        double weight_sum = 0.0;
        for (std::size_t anchor = 0U; anchor < anchors.size(); ++anchor)
        {
            const double distance = centers[selected] - anchors[anchor];
            const double weight = std::exp(-(distance * distance) / 4.0);
            weighted += weight * std::exp2(anchor_ev[anchor]);
            weight_sum += weight;
        }
        const float expected_correction = static_cast<float>(weighted / weight_sum);
        for (std::size_t channel = 0U; channel < colour.size(); ++channel)
        {
            EXPECT_NEAR(adjusted.value().rgb[channel] / source.rgb[channel], expected_correction,
                        2.0e-4F);
        }
        EXPECT_EQ(source.rgb, original.rgb);
    }
}

TEST(ToneEqualizerTest, LogGuidedMaskPreservesDarkTextureWithoutEdgeHaloAcrossScales)
{
    const auto operation = []
    {
        Recipe recipe;
        recipe.operations.push_back({"ravo.core.toneequal",
                                     1,
                                     "toneequal-detail",
                                     true,
                                     {{"shadows", ParameterValue{1.0}}},
                                     std::nullopt});
        return recipe;
    }();
    const auto fixture = [](const std::uint32_t width, const std::uint32_t height,
                            const std::uint32_t original_width, const std::uint32_t original_height,
                            const bool texture)
    {
        WorkingImage source;
        source.width = width;
        source.height = height;
        source.canonical_roi_scale = CanonicalRoiScale::from_scaled_dimensions(
            width, height, original_width, original_height);
        for (std::uint32_t y = 0U; y < height; ++y)
        {
            for (std::uint32_t x = 0U; x < width; ++x)
            {
                const bool dark = x >= width / 2U;
                const float detail = dark && texture ? (x % 2U == 0U ? -0.08F : 0.08F) : 0.0F;
                const float energy = std::exp2((dark ? -6.0F : -2.0F) + detail);
                constexpr std::array<float, 3> flower{0.74F, 0.12F, 0.66F};
                constexpr std::array<float, 3> stem{0.18F, 0.72F, 0.24F};
                const auto &colour = dark ? stem : flower;
                const float norm = std::hypot(colour[0], colour[1], colour[2]);
                source.rgb.insert(source.rgb.end(),
                                  {energy * colour[0] / norm, energy * colour[1] / norm,
                                   energy * colour[2] / norm});
            }
        }
        return source;
    };
    const auto correction_at =
        [](const WorkingImage &before, const WorkingImage &after, const std::uint32_t x)
    {
        return after.rgb[static_cast<std::size_t>(x) * 3U] /
               before.rgb[static_cast<std::size_t>(x) * 3U];
    };

    const WorkingImage textured = fixture(1024U, 8U, 1024U, 8U, true);
    const auto adjusted = apply_recipe_ops(textured, operation, CancellationToken{});
    ASSERT_TRUE(adjusted) << adjusted.error().message;
    const float bright_far = correction_at(textured, adjusted.value(), 200U);
    const float bright_edge = correction_at(textured, adjusted.value(), 508U);
    const float dark_edge = correction_at(textured, adjusted.value(), 516U);
    const float dark_far = correction_at(textured, adjusted.value(), 800U);
    EXPECT_NEAR(bright_edge, bright_far, bright_far * 0.03F);
    EXPECT_NEAR(dark_edge, dark_far, dark_far * 0.03F);
    const float input_detail_ev = std::log2(textured.rgb[800U * 3U] / textured.rgb[801U * 3U]);
    const float output_detail_ev =
        std::log2(adjusted.value().rgb[800U * 3U] / adjusted.value().rgb[801U * 3U]);
    EXPECT_NEAR(output_detail_ev, input_detail_ev, 0.03F);

    const WorkingImage full = fixture(1024U, 8U, 1024U, 8U, false);
    const WorkingImage half = fixture(512U, 4U, 1024U, 8U, false);
    const auto full_adjusted = apply_recipe_ops(full, operation, CancellationToken{});
    const auto half_adjusted = apply_recipe_ops(half, operation, CancellationToken{});
    ASSERT_TRUE(full_adjusted) << full_adjusted.error().message;
    ASSERT_TRUE(half_adjusted) << half_adjusted.error().message;
    for (const std::uint32_t full_x : {200U, 508U, 516U, 800U})
    {
        EXPECT_NEAR(correction_at(full, full_adjusted.value(), full_x),
                    correction_at(half, half_adjusted.value(), full_x / 2U), 0.01F);
    }
}

TEST(ToneEqualizerTest, InvalidDataCancellationAndMemoryBudgetFailExplicitly)
{
    Recipe recipe;
    recipe.operations.push_back({"ravo.core.toneequal",
                                 1,
                                 "toneequal-errors",
                                 true,
                                 {{"shadows", ParameterValue{1.0}}},
                                 std::nullopt});
    WorkingImage source;
    source.width = 4U;
    source.height = 1U;
    source.rgb.assign(12U, 0.01F);
    const auto original = source;

    auto rejected = apply_recipe_ops(source, recipe, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "invalid_tone_equalizer_roi_scale");
    source.canonical_roi_scale = CanonicalRoiScale::from_scaled_dimensions(4U, 1U, 4U, 1U);
    source.rgb[6U] = std::numeric_limits<float>::quiet_NaN();
    rejected = apply_recipe_ops(source, recipe, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "non_finite_tone_equalizer_input");
    EXPECT_EQ(rejected.error().context.at("pixel_index"), "2");

    source.rgb.assign(12U, std::numeric_limits<float>::max());
    recipe.operations.front().parameters = {{"whites", ParameterValue{2.0}}};
    rejected = apply_recipe_ops(source, recipe, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "non_finite_tone_equalizer_output");

    CancellationSource cancellation;
    ASSERT_TRUE(cancellation.cancel("toneequal-pre"));
    rejected = apply_recipe_ops(original, recipe, cancellation.token());
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kCancelled);
    EXPECT_EQ(original.rgb, std::vector<float>(12U, 0.01F));

    DecodedRaw raw;
    raw.width = 8U;
    raw.height = 4U;
    raw.pixels.assign(32U, 0U);
    Recipe baseline_recipe;
    const std::uint64_t baseline = estimate_raw_render_memory(raw, baseline_recipe, 8U, 4U);
    const std::uint64_t estimated = estimate_raw_render_memory(raw, recipe, 8U, 4U);
    constexpr std::uint64_t lut_bytes = (8U * 10000U + 1U) * sizeof(float);
    EXPECT_EQ(estimated - baseline, 8U * 4U * 5U * sizeof(float) + lut_bytes);
}

TEST(EngineFacadeTest, VignetteHonorsSignedAmountShapeAndCenter)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    const auto source = solid_raster(32, 32, 200, 200, 200);
    const auto luma_at =
        [](const RenderedImage &image, const std::uint32_t x, const std::uint32_t y)
    {
        const std::size_t index = (static_cast<std::size_t>(y) * image.width + x) * 3U;
        return static_cast<int>(image.rgb[index]) + static_cast<int>(image.rgb[index + 1U]) +
               static_cast<int>(image.rgb[index + 2U]);
    };

    auto darken = render_op(engine.value(), source,
                            {"ravo.effect.vignette",
                             1,
                             "vig-dark",
                             true,
                             {{"amount", ParameterValue{1.0}},
                              {"midpoint", ParameterValue{0.2}},
                              {"falloff", ParameterValue{0.8}},
                              {"shape", ParameterValue{1.0}},
                              {"center_x", ParameterValue{0.0}},
                              {"center_y", ParameterValue{0.0}}},
                             std::nullopt});
    ASSERT_TRUE(darken) << darken.error().message;
    EXPECT_GT(luma_at(darken.value(), 16, 16), luma_at(darken.value(), 0, 0));

    auto brighten = render_op(engine.value(), source,
                              {"ravo.effect.vignette",
                               1,
                               "vig-bright",
                               true,
                               {{"amount", ParameterValue{-1.0}},
                                {"midpoint", ParameterValue{0.2}},
                                {"falloff", ParameterValue{0.8}}},
                               std::nullopt});
    ASSERT_TRUE(brighten) << brighten.error().message;
    EXPECT_LT(luma_at(brighten.value(), 16, 16), luma_at(brighten.value(), 0, 0));

    auto shifted = render_op(engine.value(), source,
                             {"ravo.effect.vignette",
                              1,
                              "vig-shift",
                              true,
                              {{"amount", ParameterValue{1.0}},
                               {"midpoint", ParameterValue{0.1}},
                               {"falloff", ParameterValue{0.9}},
                               {"center_x", ParameterValue{0.8}},
                               {"center_y", ParameterValue{0.0}}},
                              std::nullopt});
    ASSERT_TRUE(shifted) << shifted.error().message;
    EXPECT_LT(luma_at(shifted.value(), 0, 16), luma_at(shifted.value(), 31, 16));

    auto round = render_op(engine.value(), source,
                           {"ravo.effect.vignette",
                            1,
                            "vig-round",
                            true,
                            {{"amount", ParameterValue{1.0}},
                             {"midpoint", ParameterValue{0.2}},
                             {"falloff", ParameterValue{0.8}},
                             {"shape", ParameterValue{1.0}}},
                            std::nullopt});
    auto diamond = render_op(engine.value(), source,
                             {"ravo.effect.vignette",
                              1,
                              "vig-diamond",
                              true,
                              {{"amount", ParameterValue{1.0}},
                               {"midpoint", ParameterValue{0.2}},
                               {"falloff", ParameterValue{0.8}},
                               {"shape", ParameterValue{5.0}}},
                              std::nullopt});
    ASSERT_TRUE(round) << round.error().message;
    ASSERT_TRUE(diamond) << diamond.error().message;
    EXPECT_NE(round.value().rgb, diamond.value().rgb);
}

TEST(EngineFacadeTest, BasicAdjustmentParametersFollowDarktableCpuResponse)
{
    const WorkingImage source{1, 1, {0.08F, 0.18F, 0.40F}, {}, {}, {}, {}};
    const auto apply = [&](OperationInstance operation)
    {
        Recipe recipe;
        recipe.operations.push_back(std::move(operation));
        return apply_recipe_ops(source, recipe, CancellationToken{});
    };

    auto exposure = apply({"ravo.core.exposure",
                           1,
                           "exposure-1",
                           true,
                           {{"exposure_ev", ParameterValue{-1.0}}},
                           std::nullopt});
    ASSERT_TRUE(exposure) << exposure.error().message;
    EXPECT_NEAR(exposure.value().rgb[0], source.rgb[0] * 0.5F, 1.0e-7F);
    EXPECT_NEAR(exposure.value().rgb[1], source.rgb[1] * 0.5F, 1.0e-7F);
    EXPECT_NEAR(exposure.value().rgb[2], source.rgb[2] * 0.5F, 1.0e-7F);

    auto contrast = apply({"ravo.core.contrast",
                           1,
                           "contrast-1",
                           true,
                           {{"amount", ParameterValue{0.25}}},
                           std::nullopt});
    ASSERT_TRUE(contrast) << contrast.error().message;
    constexpr float middle_grey = 0.1842F;
    const float luminance =
        0.2225045F * source.rgb[0] + 0.7168786F * source.rgb[1] + 0.0606169F * source.rgb[2];
    const float contrast_luminance = std::pow(luminance / middle_grey, 1.25F) * middle_grey;
    const float contrast_scale = contrast_luminance / luminance;
    EXPECT_NEAR(contrast.value().rgb[0], source.rgb[0] * contrast_scale, 1.0e-6F);
    EXPECT_NEAR(contrast.value().rgb[1], source.rgb[1] * contrast_scale, 1.0e-6F);
    EXPECT_NEAR(contrast.value().rgb[2], source.rgb[2] * contrast_scale, 1.0e-6F);

    auto saturation = apply({"ravo.color.saturation",
                             1,
                             "saturation-1",
                             true,
                             {{"amount", ParameterValue{0.25}}},
                             std::nullopt});
    ASSERT_TRUE(saturation) << saturation.error().message;
    const float average = (source.rgb[0] + source.rgb[1] + source.rgb[2]) / 3.0F;
    for (std::size_t channel = 0; channel < 3U; ++channel)
    {
        const float expected = average + 1.25F * (source.rgb[channel] - average);
        EXPECT_NEAR(saturation.value().rgb[channel], expected, 1.0e-6F);
    }

    auto vibrance = apply({"ravo.color.vibrance",
                           1,
                           "vibrance-1",
                           true,
                           {{"amount", ParameterValue{0.7}}},
                           std::nullopt});
    ASSERT_TRUE(vibrance) << vibrance.error().message;
    const float dr = average - source.rgb[0];
    const float dg = average - source.rgb[1];
    const float db = average - source.rgb[2];
    const float delta = std::sqrt(dr * dr + dg * dg + db * db);
    const float vibrance_gain = 0.5F * (1.0F - std::sqrt(delta));
    for (std::size_t channel = 0; channel < 3U; ++channel)
    {
        const float expected = average + (1.0F + vibrance_gain) * (source.rgb[channel] - average);
        EXPECT_NEAR(vibrance.value().rgb[channel], expected, 1.0e-6F);
    }

    Recipe combined_recipe;
    combined_recipe.operations.push_back({"ravo.color.vibrance",
                                          1,
                                          "vibrance-1",
                                          true,
                                          {{"amount", ParameterValue{0.7}}},
                                          std::nullopt});
    combined_recipe.operations.push_back({"ravo.color.saturation",
                                          1,
                                          "saturation-1",
                                          true,
                                          {{"amount", ParameterValue{0.25}}},
                                          std::nullopt});
    auto combined = apply_recipe_ops(source, combined_recipe, CancellationToken{});
    ASSERT_TRUE(combined) << combined.error().message;
    for (std::size_t channel = 0; channel < 3U; ++channel)
    {
        const float expected = average + (1.25F + vibrance_gain) * (source.rgb[channel] - average);
        EXPECT_NEAR(combined.value().rgb[channel], expected, 1.0e-6F);
    }
}

TEST(EngineFacadeTest, HighlightsAndShadowsUseCalibratedSceneEvEnvelopes)
{
    constexpr float middle_grey = 0.1842F;
    constexpr std::array<float, 5> stops{-6.0F, -3.0F, 0.0F, 1.0F, 3.0F};
    WorkingImage source;
    source.width = static_cast<std::uint32_t>(stops.size());
    source.height = 1U;
    for (const float stop : stops)
    {
        const float value = middle_grey * std::exp2(stop);
        source.rgb.insert(source.rgb.end(), {value, value, value});
    }
    const auto apply = [&](const std::string &id, const double amount)
    {
        Recipe recipe;
        recipe.operations.push_back(
            {id, 1, "calibrated-1", true, {{"amount", ParameterValue{amount}}}, std::nullopt});
        return apply_recipe_ops(source, recipe, CancellationToken{});
    };
    const auto smoothstep = [](const float start, const float end, const float value)
    {
        const float t = std::clamp((value - start) / (end - start), 0.0F, 1.0F);
        return t * t * (3.0F - 2.0F * t);
    };

    auto highlights = apply("ravo.core.highlights", 1.0);
    ASSERT_TRUE(highlights) << highlights.error().message;
    auto shadows = apply("ravo.core.shadows", -1.0);
    ASSERT_TRUE(shadows) << shadows.error().message;
    for (std::size_t pixel = 0; pixel < stops.size(); ++pixel)
    {
        const float input = source.rgb[pixel * 3U];
        const float highlight_mask = smoothstep(-4.5F, 2.75F, stops[pixel]);
        const float shadow_mask = 1.0F - smoothstep(-6.0F, 0.75F, stops[pixel]);
        const float expected_highlight = input * std::exp2(0.9F * highlight_mask);
        const float expected_shadow = input * std::exp2(-2.9F * shadow_mask);
        for (std::size_t channel = 0; channel < 3U; ++channel)
        {
            EXPECT_NEAR(highlights.value().rgb[pixel * 3U + channel], expected_highlight, 2e-6F);
            EXPECT_NEAR(shadows.value().rgb[pixel * 3U + channel], expected_shadow, 2e-6F);
        }
    }
    EXPECT_NEAR(highlights.value().rgb[0], source.rgb[0], 1e-8F);
    EXPECT_NEAR(shadows.value().rgb.back(), source.rgb.back(), 1e-5F);

    CancellationSource cancelled;
    ASSERT_TRUE(cancelled.cancel("calibrated light response"));
    Recipe cancelled_recipe;
    cancelled_recipe.operations.push_back({"ravo.core.highlights",
                                           1,
                                           "calibrated-cancelled-1",
                                           true,
                                           {{"amount", ParameterValue{1.0}}},
                                           std::nullopt});
    auto stopped = apply_recipe_ops(source, cancelled_recipe, cancelled.token());
    ASSERT_FALSE(stopped);
    EXPECT_EQ(stopped.error().code, ErrorCode::kCancelled);
}

TEST(EngineFacadeTest, WhitesAndBlacksPreservePositiveOrderedColour)
{
    constexpr float middle_grey = 0.1842F;
    constexpr std::array<float, 3> colour{0.72F, 0.36F, 0.12F};
    constexpr float colour_luminance =
        0.2225045F * colour[0] + 0.7168786F * colour[1] + 0.0606169F * colour[2];
    std::vector<float> stops;
    WorkingImage source;
    source.height = 1U;
    source.color_profile.model = ColorModel::kRgb;
    for (int quarter_stop = -48; quarter_stop <= 24; ++quarter_stop)
    {
        const float stop = static_cast<float>(quarter_stop) / 4.0F;
        stops.push_back(stop);
        const float scale = middle_grey * std::exp2(stop) / colour_luminance;
        source.rgb.insert(source.rgb.end(),
                          {colour[0] * scale, colour[1] * scale, colour[2] * scale});
    }
    source.width = static_cast<std::uint32_t>(stops.size());

    const auto apply = [&](const double whites, const double blacks)
    {
        Recipe recipe;
        recipe.operations.push_back({"ravo.core.whites",
                                     1,
                                     "whites-1",
                                     true,
                                     {{"amount", ParameterValue{whites}}},
                                     std::nullopt});
        recipe.operations.push_back({"ravo.core.blacks",
                                     1,
                                     "blacks-1",
                                     true,
                                     {{"amount", ParameterValue{blacks}}},
                                     std::nullopt});
        return apply_recipe_ops(source, recipe, CancellationToken{});
    };

    constexpr std::array<std::array<double, 2>, 6> controls{{
        {{-1.0, 0.0}},
        {{1.0, 0.0}},
        {{0.0, -1.0}},
        {{0.0, 1.0}},
        {{-0.77, 0.2}},
        {{1.0, 1.0}},
    }};
    for (const auto &control : controls)
    {
        auto adjusted = apply(control[0], control[1]);
        ASSERT_TRUE(adjusted) << adjusted.error().message;
        float prior_luminance = -1.0F;
        for (std::size_t pixel = 0; pixel < stops.size(); ++pixel)
        {
            const std::size_t index = pixel * 3U;
            const float red_scale = adjusted.value().rgb[index] / source.rgb[index];
            for (std::size_t channel = 0; channel < 3U; ++channel)
            {
                EXPECT_GT(adjusted.value().rgb[index + channel], 0.0F);
                EXPECT_NEAR(adjusted.value().rgb[index + channel] / source.rgb[index + channel],
                            red_scale, std::max(1.0e-6F, std::abs(red_scale) * 2.0e-6F));
            }
            const float luminance = 0.2225045F * adjusted.value().rgb[index] +
                                    0.7168786F * adjusted.value().rgb[index + 1U] +
                                    0.0606169F * adjusted.value().rgb[index + 2U];
            EXPECT_GT(luminance, prior_luminance);
            prior_luminance = luminance;
        }
    }

    auto preset_response = apply(-0.77, 0.2);
    ASSERT_TRUE(preset_response) << preset_response.error().message;
    const auto smoothstep = [](const float start, const float end, const float value)
    {
        const float t = std::clamp((value - start) / (end - start), 0.0F, 1.0F);
        return t * t * (3.0F - 2.0F * t);
    };
    for (std::size_t pixel = 0; pixel < stops.size(); ++pixel)
    {
        const float white_weight = smoothstep(0.0F, 4.0F, stops[pixel]);
        const float black_weight = 1.0F - smoothstep(-8.0F, 0.0F, stops[pixel]);
        const float expected_scale =
            std::exp2(-0.77F * 1.8F * white_weight + 0.2F * 2.0F * black_weight);
        for (std::size_t channel = 0; channel < 3U; ++channel)
        {
            const float expected = source.rgb[pixel * 3U + channel] * expected_scale;
            EXPECT_NEAR(preset_response.value().rgb[pixel * 3U + channel], expected,
                        std::max(1.0e-7F, std::abs(expected) * 2.0e-6F));
        }
    }

    CancellationSource cancelled;
    ASSERT_TRUE(cancelled.cancel("whites and blacks response"));
    Recipe cancelled_recipe;
    cancelled_recipe.operations.push_back({"ravo.core.whites",
                                           1,
                                           "whites-cancelled-1",
                                           true,
                                           {{"amount", ParameterValue{-0.77}}},
                                           std::nullopt});
    cancelled_recipe.operations.push_back({"ravo.core.blacks",
                                           1,
                                           "blacks-cancelled-1",
                                           true,
                                           {{"amount", ParameterValue{0.2}}},
                                           std::nullopt});
    auto stopped = apply_recipe_ops(source, cancelled_recipe, cancelled.token());
    ASSERT_FALSE(stopped);
    EXPECT_EQ(stopped.error().code, ErrorCode::kCancelled);
}

TEST(EngineFacadeTest, CanonicalLightPassMatchesMonotonicOrderedComposition)
{
    WorkingImage source;
    source.height = 1U;
    source.color_profile.model = ColorModel::kRgb;
    for (int quarter_stop = -48; quarter_stop <= 24; ++quarter_stop)
    {
        const float sample = 0.1842F * std::exp2(static_cast<float>(quarter_stop) / 4.0F);
        source.rgb.insert(source.rgb.end(), {sample * 1.2F, sample * 0.8F, sample * 0.4F});
    }
    source.width = static_cast<std::uint32_t>(source.rgb.size() / 3U);

    constexpr std::array<std::array<double, 4>, 4> endpoints{{
        {{-1.0, 1.0, -1.0, 1.0}},
        {{1.0, -1.0, 1.0, -1.0}},
        {{1.0, 1.0, 1.0, 1.0}},
        {{-1.0, -1.0, -1.0, -1.0}},
    }};
    constexpr std::array<std::string_view, 4> ids{"ravo.core.highlights", "ravo.core.shadows",
                                                  "ravo.core.whites", "ravo.core.blacks"};
    for (const auto &amounts : endpoints)
    {
        Recipe combined;
        Recipe separated;
        for (std::size_t control = 0; control < ids.size(); ++control)
        {
            OperationInstance light{std::string(ids[control]),
                                    1,
                                    "light-" + std::to_string(control),
                                    true,
                                    {{"amount", ParameterValue{amounts[control]}}},
                                    std::nullopt};
            combined.operations.push_back(light);
            separated.operations.push_back(std::move(light));
            if (control + 1U < ids.size())
            {
                separated.operations.push_back({"ravo.core.gamma",
                                                1,
                                                "separator-" + std::to_string(control),
                                                true,
                                                {{"gamma", ParameterValue{1.0}}},
                                                std::nullopt});
            }
        }

        auto one_pass = apply_recipe_ops(source, combined, CancellationToken{});
        ASSERT_TRUE(one_pass) << one_pass.error().message;
        auto ordered = apply_recipe_ops(source, separated, CancellationToken{});
        ASSERT_TRUE(ordered) << ordered.error().message;
        ASSERT_EQ(one_pass.value().rgb.size(), ordered.value().rgb.size());
        float previous_luminance = -1.0F;
        for (std::size_t index = 0; index < one_pass.value().rgb.size(); index += 3U)
        {
            for (std::size_t channel = 0; channel < 3U; ++channel)
            {
                const float actual = one_pass.value().rgb[index + channel];
                EXPECT_GT(actual, 0.0F);
                EXPECT_NEAR(actual, ordered.value().rgb[index + channel],
                            std::max(1.0e-7F, std::abs(actual) * 3.0e-6F));
            }
            const float luminance = 0.2225045F * one_pass.value().rgb[index] +
                                    0.7168786F * one_pass.value().rgb[index + 1U] +
                                    0.0606169F * one_pass.value().rgb[index + 2U];
            EXPECT_GT(luminance, previous_luminance);
            previous_luminance = luminance;
        }
    }
}

TEST(EngineFacadeTest, DarkExposureAndRaisedBlacksDoNotCreateAZeroPlateau)
{
    WorkingImage source;
    source.height = 1U;
    source.color_profile.model = ColorModel::kRgb;
    for (int quarter_stop = -40; quarter_stop <= 16; ++quarter_stop)
    {
        const float sample = 0.1842F * std::exp2(static_cast<float>(quarter_stop) / 4.0F);
        source.rgb.insert(source.rgb.end(), {sample, sample, sample});
    }
    source.width = static_cast<std::uint32_t>(source.rgb.size() / 3U);

    ExposureParams exposure;
    exposure.exposure_ev = -1.398;
    exposure.black = -0.0059;
    Recipe recipe;
    recipe.operations.push_back({std::string(kExposureOperationId), kExposureOperationSchemaVersion,
                                 "exposure-1", true, exposure_to_parameters(exposure),
                                 std::nullopt});
    recipe.operations.push_back({"ravo.core.whites",
                                 1,
                                 "whites-1",
                                 true,
                                 {{"amount", ParameterValue{-0.77}}},
                                 std::nullopt});
    recipe.operations.push_back(
        {"ravo.core.blacks", 1, "blacks-1", true, {{"amount", ParameterValue{0.2}}}, std::nullopt});

    auto adjusted = apply_recipe_ops(source, recipe, CancellationToken{});
    ASSERT_TRUE(adjusted) << adjusted.error().message;
    float previous = -1.0F;
    for (std::size_t index = 0; index < adjusted.value().rgb.size(); index += 3U)
    {
        EXPECT_GT(adjusted.value().rgb[index], 0.0F);
        EXPECT_GT(adjusted.value().rgb[index], previous);
        EXPECT_FLOAT_EQ(adjusted.value().rgb[index], adjusted.value().rgb[index + 1U]);
        EXPECT_FLOAT_EQ(adjusted.value().rgb[index], adjusted.value().rgb[index + 2U]);
        previous = adjusted.value().rgb[index];
    }
}

TEST(EngineFacadeTest, DisplaySrgbCurveEvaluatesEncodedIndependentChannels)
{
    constexpr float encoded_half_linear = 0.21404114F;
    const WorkingImage source{
        1, 1, {encoded_half_linear, encoded_half_linear, encoded_half_linear}, {}, {}, {}, {}};
    const auto points = tone_curve_points_to_parameter({{0.0, 0.0}, {0.5, 0.25}, {1.0, 1.0}});
    Recipe recipe;
    recipe.operations.push_back(
        {"ravo.color.rgbcurve",
         1,
         "display-curve-1",
         true,
         {{"mode", ParameterValue{std::string(kRgbLevelsModeIndependent)}},
          {"preserve_colors", ParameterValue{std::string(kToneCurvePreserveColorsNone)}},
          {"application_space", ParameterValue{std::string(kRgbCurveApplicationSpaceDisplaySrgb)}},
          {"points", points},
          {"points_g", points},
          {"points_b", points}},
         std::nullopt});
    auto curved = apply_recipe_ops(source, recipe, CancellationToken{});
    ASSERT_TRUE(curved) << curved.error().message;
    constexpr float encoded_quarter_linear = 0.05087609F;
    for (const float channel : curved.value().rgb)
        EXPECT_NEAR(channel, encoded_quarter_linear, 2e-6F);

    recipe.operations[0].parameters["mode"] = ParameterValue{std::string(kRgbLevelsModeLinked)};
    auto unsupported = apply_recipe_ops(source, recipe, CancellationToken{});
    ASSERT_FALSE(unsupported);
    EXPECT_EQ(unsupported.error().code, ErrorCode::kValidation);
    EXPECT_EQ(unsupported.error().context.at("reason"), "unsupported_display_srgb_curve_policy");
}

TEST(EngineFacadeTest, EffectDefaultsAvoidJumpsAndDehazeRequiresSourceStage)
{
    WorkingImage midtone;
    midtone.width = 8;
    midtone.height = 8;
    midtone.rgb.resize(8U * 8U * 3U);
    for (std::size_t index = 0; index < midtone.rgb.size(); index += 3U)
    {
        midtone.rgb[index] = 0.30F;
        midtone.rgb[index + 1U] = 0.20F;
        midtone.rgb[index + 2U] = 0.10F;
    }
    Recipe bloom_recipe;
    bloom_recipe.operations.push_back(
        {"ravo.effect.bloom", 1, "bloom-1", true, {{"amount", ParameterValue{0.1}}}, std::nullopt});
    auto bloom = apply_recipe_ops(midtone, bloom_recipe, CancellationToken{});
    ASSERT_TRUE(bloom) << bloom.error().message;
    EXPECT_EQ(bloom.value().rgb, midtone.rgb);

    const WorkingImage haze_source{1, 1, {0.20F, 0.30F, 0.40F}, {}, {}, {}, {}};
    Recipe haze_recipe;
    haze_recipe.operations.push_back({"ravo.effect.dehaze",
                                      1,
                                      "dehaze-1",
                                      true,
                                      {{"amount", ParameterValue{-0.1}}},
                                      std::nullopt});
    auto haze = apply_recipe_ops(haze_source, haze_recipe, CancellationToken{});
    ASSERT_FALSE(haze);
    EXPECT_EQ(haze.error().context.at("reason"), "dehaze_source_stage_required");
}

TEST(EngineFacadeTest, LabDevelopOperationsUseTheD50WorkingConversion)
{
    WorkingImage source;
    source.width = 1U;
    source.height = 1U;
    source.rgb = {0.08F, 0.18F, 0.40F};
    source.color_profile.model = ColorModel::kRgb;
    source.color_profile.identifier = std::string(kInputProfileLinearRec709);
    Recipe recipe;
    recipe.operations.push_back({"ravo.color.colorcontrast",
                                 1,
                                 "colorcontrast-1",
                                 true,
                                 {{"amount", ParameterValue{0.1}}},
                                 std::nullopt});
    auto adjusted = apply_recipe_ops(source, recipe, CancellationToken{});
    ASSERT_TRUE(adjusted) << adjusted.error().message;
    ASSERT_EQ(adjusted.value().rgb.size(), 3U);
    EXPECT_NEAR(adjusted.value().rgb[0], 0.06855496F, 2.0e-6F);
    EXPECT_NEAR(adjusted.value().rgb[1], 0.18104838F, 2.0e-6F);
    EXPECT_NEAR(adjusted.value().rgb[2], 0.42961232F, 2.0e-6F);
}


} // namespace
} // namespace ravo
