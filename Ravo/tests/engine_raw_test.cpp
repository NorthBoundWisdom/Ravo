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
        std::filesystem::path(RAVO_REPOSITORY_ROOT) / "legacy" / "tests" / "images" / "mire1.cr2";
    const auto utf8 = path.generic_u8string();
    return {utf8.begin(), utf8.end()};
}

[[nodiscard]] std::string mire1_xtrans_path()
{
    const auto path = std::filesystem::path(RAVO_REPOSITORY_ROOT) / "legacy" / "tests" / "images" /
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

[[nodiscard]] OperationInstance temperature_operation(const TemperatureParams &params,
                                                      std::string instance_id = "temperature-1")
{
    return {"ravo.color.temperature",          1,           std::move(instance_id), true,
            temperature_to_parameters(params), std::nullopt};
}

[[nodiscard]] OperationInstance hot_pixels_operation(const bool permissive = false)
{
    return {"ravo.raw.hotpixels",
            1,
            "hotpixels-1",
            true,
            {{"strength", ParameterValue{0.25}},
             {"threshold", ParameterValue{0.05}},
             {"permissive", ParameterValue{permissive}}},
            std::nullopt};
}

[[nodiscard]] OperationInstance raw_ca_operation(const std::int64_t iterations = 2,
                                                 const bool avoid_color_shift = false)
{
    return {"ravo.raw.cacorrect",
            1,
            "cacorrect-1",
            true,
            {{"iterations", ParameterValue{iterations}},
             {"avoid_color_shift", ParameterValue{avoid_color_shift}}},
            std::nullopt};
}

[[nodiscard]] DecodedRaw synthetic_bayer_raw()
{
    DecodedRaw raw;
    raw.width = 9;
    raw.height = 9;
    raw.cfa_width = 2;
    raw.cfa_height = 2;
    raw.black_level = 0;
    raw.white_level = 1000;
    raw.has_as_shot_white_balance = true;
    raw.has_camera_reference_white_balance = true;
    declare_linear_srgb_matrix(raw);
    raw.cfa_channels = {0, 1, 1, 2};
    raw.pixels.assign(static_cast<std::size_t>(raw.width) * raw.height, 100);
    return raw;
}

TEST(EngineFacadeTest, ClassifiesMissingDirectoryTruncatedAndUnrecognizedRaw)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    const auto root =
        std::filesystem::temp_directory_path() / ("ravo-engine-raw-" + generate_catalog_id());
    std::filesystem::create_directories(root);

    auto empty = engine.value().inspect("", CancellationToken{});
    ASSERT_FALSE(empty);
    EXPECT_EQ(empty.error().code, ErrorCode::kInvalidArgument);
    EXPECT_EQ(empty.error().context.at("reason"), "empty_raw_path");

    auto missing = engine.value().inspect((root / "missing.cr2").string(), CancellationToken{});
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, ErrorCode::kNotFound);
    EXPECT_EQ(missing.error().context.at("reason"), "raw_not_found");

    const auto directory = root / "folder.cr2";
    std::filesystem::create_directories(directory);
    auto not_file = engine.value().inspect(directory.string(), CancellationToken{});
    ASSERT_FALSE(not_file);
    EXPECT_EQ(not_file.error().code, ErrorCode::kInvalidArgument);
    EXPECT_EQ(not_file.error().context.at("reason"), "raw_not_regular_file");

    const auto garbage_path = root / "notes.cr2";
    {
        std::ofstream output(garbage_path, std::ios::binary);
        output << "not a raw camera file";
    }
    auto garbage = engine.value().inspect(garbage_path.string(), CancellationToken{});
    ASSERT_FALSE(garbage);
    EXPECT_TRUE(garbage.error().code == ErrorCode::kUnsupported ||
                garbage.error().code == ErrorCode::kValidation);
    const auto &garbage_reason = garbage.error().context.at("reason");
    EXPECT_TRUE(garbage_reason == "libraw_unsupported_file" ||
                garbage_reason == "libraw_open_failed");

    const auto truncated_path = root / "truncated.cr2";
    {
        std::ifstream input(std::filesystem::path(mire1_path()), std::ios::binary);
        std::ofstream output(truncated_path, std::ios::binary);
        std::vector<char> prefix(512);
        input.read(prefix.data(), static_cast<std::streamsize>(prefix.size()));
        output.write(prefix.data(), input.gcount());
    }
    auto truncated = engine.value().decode_raw_frame(truncated_path.string(), CancellationToken{});
    ASSERT_FALSE(truncated);
    EXPECT_TRUE(truncated.error().code == ErrorCode::kValidation ||
                truncated.error().code == ErrorCode::kUnsupported);
    EXPECT_FALSE(truncated.error().context.at("reason").empty());

    const auto dng_path = root / "mire1.dng";
    std::filesystem::copy_file(mire1_path(), dng_path);
    auto dng = engine.value().decode_raw_frame(dng_path.string(), CancellationToken{});
    ASSERT_TRUE(dng) << dng.error().message;
    EXPECT_GT(dng.value().width, 0U);
    EXPECT_GT(dng.value().height, 0U);
    EXPECT_EQ(dng.value().cfa_width, 2U);
    EXPECT_EQ(dng.value().cfa_height, 2U);

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST(EngineFacadeTest, FirstFrameDecodePreservesXTransCfaPhase)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    auto decoded = engine.value().decode_raw_frame(mire1_xtrans_path(), CancellationToken{});
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded.value().cfa_width, 6U);
    EXPECT_EQ(decoded.value().cfa_height, 6U);
    EXPECT_EQ(decoded.value().cfa_channels.size(), 36U);
    EXPECT_EQ(
        std::count(decoded.value().cfa_channels.begin(), decoded.value().cfa_channels.end(), 0U),
        8);
    EXPECT_EQ(
        std::count(decoded.value().cfa_channels.begin(), decoded.value().cfa_channels.end(), 1U),
        20);
    EXPECT_EQ(
        std::count(decoded.value().cfa_channels.begin(), decoded.value().cfa_channels.end(), 2U),
        8);
    EXPECT_EQ(decoded.value().pixels.size(),
              static_cast<std::size_t>(decoded.value().width) * decoded.value().height);
}

TEST(EngineFacadeTest, DecodeExposesAsShotAndCameraReferenceWhiteBalance)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    auto decoded = engine.value().decode_raw_frame(mire1_path(), CancellationToken{});
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_TRUE(decoded.value().has_as_shot_white_balance);
    EXPECT_TRUE(decoded.value().has_camera_reference_white_balance);
    EXPECT_NEAR(decoded.value().as_shot_white_balance[0], 2.115234375F, 1.0e-6F);
    EXPECT_NEAR(decoded.value().as_shot_white_balance[1], 1.0F, 1.0e-6F);
    EXPECT_NEAR(decoded.value().as_shot_white_balance[2], 1.3984375F, 1.0e-6F);
    EXPECT_FLOAT_EQ(decoded.value().as_shot_white_balance[3], 1.0F);
    for (const float coefficient : decoded.value().camera_reference_white_balance)
    {
        EXPECT_TRUE(std::isfinite(coefficient));
        EXPECT_GT(coefficient, 0.0F);
        EXPECT_LE(coefficient, 8.0F);
    }
    EXPECT_NE(decoded.value().camera_reference_white_balance,
              decoded.value().as_shot_white_balance);
    EXPECT_NEAR(decoded.value().camera_reference_white_balance[0], 2.62727761F, 1.0e-6F);
    EXPECT_NEAR(decoded.value().camera_reference_white_balance[1], 1.0F, 1.0e-6F);
    EXPECT_NEAR(decoded.value().camera_reference_white_balance[2], 1.25087583F, 1.0e-6F);
    EXPECT_NEAR(decoded.value().camera_reference_white_balance[3], 1.0F, 1.0e-6F);
    EXPECT_EQ(decoded.value().color_profile.kind, ColorProfileKind::kMatrix);
    EXPECT_EQ(decoded.value().color_profile.identifier, kInputProfileEnhancedMatrix);
    EXPECT_TRUE(decoded.value().color_profile.has_matrix);
    EXPECT_TRUE(std::all_of(decoded.value().color_profile.matrix_to_xyz_d50.begin(),
                            decoded.value().color_profile.matrix_to_xyz_d50.end(),
                            [](const float value) { return std::isfinite(value); }));
}

TEST(EngineFacadeTest, ExtractsBoundedEmbeddedJpegPreview)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;

    const auto preview =
        engine.value().extract_embedded_preview(mire1_path(), 320, CancellationToken{});
    ASSERT_TRUE(preview) << preview.error().message;
    EXPECT_EQ(preview.value().mime_type, "image/jpeg");
    EXPECT_GT(preview.value().width, 0U);
    EXPECT_GT(preview.value().height, 0U);
    ASSERT_GE(preview.value().bytes.size(), 4U);
    EXPECT_EQ(preview.value().bytes[0], 0xff);
    EXPECT_EQ(preview.value().bytes[1], 0xd8);
}

TEST(EngineFacadeTest, InspectWithEmbeddedPreviewReturnsMetadataAndJpeg)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;

    const auto probed =
        engine.value().inspect_with_embedded_preview(mire1_path(), 320, CancellationToken{});
    ASSERT_TRUE(probed) << probed.error().message;
    EXPECT_TRUE(probed.value().inspection.is_raw);
    EXPECT_GT(probed.value().inspection.width, 0U);
    EXPECT_GT(probed.value().inspection.height, 0U);
    ASSERT_TRUE(probed.value().embedded_preview.has_value());
    EXPECT_EQ(probed.value().embedded_preview->mime_type, "image/jpeg");
    ASSERT_GE(probed.value().embedded_preview->bytes.size(), 4U);
    EXPECT_EQ(probed.value().embedded_preview->bytes[0], 0xff);
    EXPECT_EQ(probed.value().embedded_preview->bytes[1], 0xd8);
}

TEST(EngineFacadeTest, RenderWritesBoundedPngAndRejectsOutputConflict)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;

    Recipe recipe;
    recipe.asset = {"mire1", mire1_path(), std::nullopt};
    declare_input(recipe);
    RenderRequest request;
    request.asset = recipe.asset;
    request.recipe = recipe;
    request.output_uri = (std::filesystem::temp_directory_path() / "ravo-mire1-test.png").string();
    request.output_width = 64;
    request.output_height = 48;
    request.correlation_id = "fixture-render";
    std::error_code ignored;
    std::filesystem::remove(request.output_uri, ignored);

    const auto rendered = engine.value().render(request);
    ASSERT_TRUE(rendered) << rendered.error().message;
    EXPECT_EQ(rendered.value().width, 64U);
    EXPECT_EQ(rendered.value().height, 48U);

    std::ifstream output(request.output_uri, std::ios::binary);
    ASSERT_TRUE(output);
    std::array<char, 8> signature{};
    output.read(signature.data(), static_cast<std::streamsize>(signature.size()));
    EXPECT_EQ(std::string(signature.data(), signature.size()), std::string("\x89PNG\r\n\x1a\n", 8));
    output.seekg(0, std::ios::end);
    const auto file_size = output.tellg();
    ASSERT_GT(file_size, 0);
    output.seekg(0, std::ios::beg);
    std::string png_bytes(static_cast<std::size_t>(file_size), '\0');
    output.read(png_bytes.data(), static_cast<std::streamsize>(png_bytes.size()));
    EXPECT_EQ(png_chunk_count(png_bytes, "sRGB"), 1U);
    output.close();
    const auto decoded = read_rgb_png(request.output_uri);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->width, 64U);
    EXPECT_EQ(decoded->height, 48U);
    EXPECT_TRUE(std::any_of(decoded->pixels.begin(), decoded->pixels.end(),
                            [](const png_byte value) { return value != 0; }));

    const auto conflict = engine.value().render(request);
    ASSERT_FALSE(conflict);
    EXPECT_EQ(conflict.error().code, ErrorCode::kConflict);
    std::filesystem::remove(request.output_uri, ignored);
}

TEST(EngineFacadeTest, FastPreviewPngPreservesPixelsAndSrgbMetadata)
{
    auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    RasterBuffer source;
    source.width = 257U;
    source.height = 129U;
    declare_srgb(source);
    source.srgb.resize(static_cast<std::size_t>(source.width) * source.height * 3U);
    for (std::uint32_t y = 0; y < source.height; ++y)
    {
        for (std::uint32_t x = 0; x < source.width; ++x)
        {
            const std::size_t index = (static_cast<std::size_t>(y) * source.width + x) * 3U;
            source.srgb[index] = static_cast<std::uint8_t>((x * 17U + y * 3U) & 0xffU);
            source.srgb[index + 1U] = static_cast<std::uint8_t>((x * 5U + y * 19U) & 0xffU);
            source.srgb[index + 2U] = static_cast<std::uint8_t>((x * 11U + y * 7U) & 0xffU);
        }
    }
    Recipe recipe;
    recipe.asset = {"png-preview", "memory:png-preview", std::nullopt};
    declare_input(recipe);
    RenderRequest request;
    request.asset = recipe.asset;
    request.recipe = recipe;
    auto rendered = engine.value().render_to_image(request, &source);
    ASSERT_TRUE(rendered) << rendered.error().message;
    const auto &image = rendered.value();

    auto normal = engine.value().encode_png(image);
    ASSERT_TRUE(normal) << normal.error().message;
    auto fast = engine.value().encode_preview_png(image);
    ASSERT_TRUE(fast) << fast.error().message;
    auto normal_decoded = read_rgb_png(normal.value());
    auto fast_decoded = read_rgb_png(fast.value());
    ASSERT_TRUE(normal_decoded.has_value());
    ASSERT_TRUE(fast_decoded.has_value());
    EXPECT_EQ(normal_decoded->width, image.width);
    EXPECT_EQ(normal_decoded->height, image.height);
    EXPECT_EQ(normal_decoded->pixels, image.rgb);
    EXPECT_EQ(fast_decoded->pixels, image.rgb);
    const std::string normal_bytes(normal.value().begin(), normal.value().end());
    const std::string fast_bytes(fast.value().begin(), fast.value().end());
    EXPECT_EQ(png_chunk_count(normal_bytes, "sRGB"), 1U);
    EXPECT_EQ(png_chunk_count(fast_bytes, "sRGB"), 1U);
}

TEST(EngineFacadeTest, ExposureOperationRaisesRenderedFixtureBrightness)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;

    Recipe base_recipe;
    base_recipe.asset = {"mire1", mire1_path(), std::nullopt};
    declare_input(base_recipe);
    Recipe exposed_recipe = base_recipe;
    exposed_recipe.operations.push_back({"ravo.core.exposure",
                                         1,
                                         "exposure-1",
                                         true,
                                         {{"exposure_ev", ParameterValue{1.0}}},
                                         std::nullopt});
    const auto directory = std::filesystem::temp_directory_path();
    const auto base_path = directory / "ravo-mire1-base.png";
    const auto exposed_path = directory / "ravo-mire1-exposed.png";
    std::error_code ignored;
    std::filesystem::remove(base_path, ignored);
    std::filesystem::remove(exposed_path, ignored);

    RenderRequest base_request;
    base_request.asset = base_recipe.asset;
    base_request.recipe = base_recipe;
    base_request.output_uri = base_path.string();
    base_request.output_width = 32;
    base_request.output_height = 24;
    RenderRequest exposed_request = base_request;
    exposed_request.recipe = exposed_recipe;
    exposed_request.output_uri = exposed_path.string();

    const auto base = engine.value().render(base_request);
    const auto exposed = engine.value().render(exposed_request);
    ASSERT_TRUE(base) << base.error().message;
    ASSERT_TRUE(exposed) << exposed.error().message;
    const auto base_png = read_rgb_png(base_path);
    const auto exposed_png = read_rgb_png(exposed_path);
    ASSERT_TRUE(base_png.has_value());
    ASSERT_TRUE(exposed_png.has_value());
    const auto base_sum =
        std::accumulate(base_png->pixels.begin(), base_png->pixels.end(), std::uint64_t{0});
    const auto exposed_sum =
        std::accumulate(exposed_png->pixels.begin(), exposed_png->pixels.end(), std::uint64_t{0});
    EXPECT_GT(exposed_sum, base_sum);

    std::filesystem::remove(base_path, ignored);
    std::filesystem::remove(exposed_path, ignored);
}

TEST(EngineFacadeTest, ExposureDeflickerHasAFrozenRawReferenceAndPreservesTheSource)
{
    const auto source_before = source_file_snapshot(mire1_path());
    ASSERT_TRUE(source_before.has_value());
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    auto decoded = engine.value().decode_raw_frame(mire1_path(), CancellationToken{});
    ASSERT_TRUE(decoded) << decoded.error().message;
    const auto original_pixels = decoded.value().pixels;
    auto analysis = build_exposure_analysis_context(decoded.value(), CancellationToken{});
    ASSERT_TRUE(analysis) << analysis.error().message;
    ASSERT_EQ(analysis.value()->raw_histogram.size(), kExposureRawHistogramBins);
    EXPECT_EQ(analysis.value()->raw_pixel_count, decoded.value().pixels.size());
    EXPECT_EQ(analysis.value()->raw_black_level, 1015U);
    EXPECT_EQ(analysis.value()->raw_white_level, 16224U);
    EXPECT_EQ(analysis.value()->metadata.status, RawExposureMetadataStatus::kReady);
    EXPECT_DOUBLE_EQ(analysis.value()->metadata.exposure_bias_ev, 0.0);
    EXPECT_DOUBLE_EQ(analysis.value()->metadata.highlight_preservation_ev, 0.0);

    const double threshold = static_cast<double>(analysis.value()->raw_pixel_count) * 50.0 / 100.0;
    std::uint64_t cumulative = 0U;
    std::uint32_t median_bin = 0U;
    for (std::size_t bin = 0U; bin < analysis.value()->raw_histogram.size(); ++bin)
    {
        cumulative += analysis.value()->raw_histogram[bin];
        if (static_cast<double>(cumulative) >= threshold)
        {
            median_bin = static_cast<std::uint32_t>(bin);
            break;
        }
    }
    EXPECT_EQ(median_bin, 2535U);

    ExposureParams params;
    params.mode = std::string(kExposureModeDeflicker);
    Recipe recipe;
    recipe.asset = {"mire1", mire1_path(), std::nullopt};
    declare_input(recipe);
    recipe.operations.push_back({std::string(kExposureOperationId), kExposureOperationSchemaVersion,
                                 "exposure-deflicker-reference", true,
                                 exposure_to_parameters(params), std::nullopt});
    RenderRequest request;
    request.asset = recipe.asset;
    request.recipe = recipe;
    request.output_width = 64U;
    request.output_height = 48U;
    const std::uint64_t required_bytes = estimate_raw_render_memory(
        decoded.value(), recipe, *request.output_width, *request.output_height);
    ASSERT_GT(required_bytes, 0U);
    RenderRequest constrained = request;
    constrained.memory_budget_bytes = required_bytes - 1U;
    auto budget_rejected = engine.value().render_to_image(constrained);
    ASSERT_FALSE(budget_rejected);
    EXPECT_EQ(budget_rejected.error().code, ErrorCode::kValidation);
    EXPECT_EQ(budget_rejected.error().context.at("required_bytes"), std::to_string(required_bytes));

    auto rendered = engine.value().render_to_image(request);
    ASSERT_TRUE(rendered) << rendered.error().message;
    ASSERT_EQ(rendered.value().width, 64U);
    ASSERT_EQ(rendered.value().height, 48U);
    std::array<std::uint64_t, 3> sums{};
    for (std::size_t index = 0U; index + 2U < rendered.value().rgb.size(); index += 3U)
    {
        for (std::size_t channel = 0U; channel < sums.size(); ++channel)
        {
            sums[channel] += rendered.value().rgb[index + channel];
        }
    }
    // Ravo-owned reference statistics for the original pre-repair RAW histogram,
    // the default RCD demosaic, and the frozen default deflicker target. The
    // tolerance permits platform libm/SIMD rounding without accepting a changed
    // histogram source or exposure formula.
    EXPECT_NEAR(static_cast<double>(sums[0]), 256096.0, 1500.0);
    EXPECT_NEAR(static_cast<double>(sums[1]), 238181.0, 1500.0);
    EXPECT_NEAR(static_cast<double>(sums[2]), 227249.0, 1500.0);
    EXPECT_EQ(decoded.value().pixels, original_pixels);
    const auto source_after = source_file_snapshot(mire1_path());
    ASSERT_TRUE(source_after.has_value());
    EXPECT_EQ(*source_after, *source_before);
}

TEST(EngineFacadeTest, ValidatedRenderReportsProgressAndMissingInput)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    RecordingProgressSink progress;

    RenderRequest request;
    request.asset = test::valid_recipe().asset;
    request.recipe = test::valid_recipe();
    request.output_uri =
        (std::filesystem::temp_directory_path() / "ravo-missing-test.png").string();
    request.correlation_id = "request-1";
    const auto rendered = engine.value().render(request, &progress);

    ASSERT_FALSE(rendered);
    EXPECT_EQ(rendered.error().code, ErrorCode::kNotFound);
    ASSERT_EQ(progress.events.size(), 1U);
    EXPECT_EQ(progress.events.front().stage, "validation_complete");
}

TEST(EngineFacadeTest, RasterDevelopOpsRotateAndDesaturate)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;

    RasterBuffer raster;
    raster.width = 8;
    raster.height = 4;
    declare_srgb(raster);
    raster.srgb.resize(8U * 4U * 3U, 0);
    for (std::uint32_t y = 0; y < raster.height; ++y)
    {
        for (std::uint32_t x = 0; x < raster.width; ++x)
        {
            const std::size_t index = (static_cast<std::size_t>(y) * raster.width + x) * 3U;
            raster.srgb[index] = 220;
            raster.srgb[index + 1U] = 20;
            raster.srgb[index + 2U] = 20;
        }
    }

    Recipe recipe;
    recipe.asset = {"raster", "memory:raster", std::nullopt};
    declare_input(recipe);
    recipe.operations.push_back({"ravo.color.saturation",
                                 1,
                                 "saturation-1",
                                 true,
                                 {{"amount", ParameterValue{-1.0}}},
                                 std::nullopt});
    recipe.operations.push_back({"ravo.geometry.rotate",
                                 1,
                                 "rotate-1",
                                 true,
                                 {{"quarters", ParameterValue{std::int64_t{1}}}},
                                 std::nullopt});
    RenderRequest request;
    request.asset = recipe.asset;
    request.recipe = recipe;
    const auto rendered = engine.value().render_to_image(request, &raster);
    ASSERT_TRUE(rendered) << rendered.error().message;
    EXPECT_EQ(rendered.value().width, 4U);
    EXPECT_EQ(rendered.value().height, 8U);
    ASSERT_GE(rendered.value().rgb.size(), 3U);
    EXPECT_NEAR(rendered.value().rgb[0], rendered.value().rgb[1], 8);
    EXPECT_NEAR(rendered.value().rgb[1], rendered.value().rgb[2], 8);
}

TEST(EngineFacadeTest, LeftoverCropBoxMatchesCanonicalCropPixels)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    auto box = leftover_crop_box_to_geometry(0.25F, 0.25F, 0.75F, 0.75F);
    ASSERT_TRUE(box) << box.error().message;

    RasterBuffer source;
    source.width = 16;
    source.height = 16;
    declare_srgb(source);
    source.srgb.resize(16U * 16U * 3U);
    for (std::uint32_t y = 0; y < 16U; ++y)
    {
        for (std::uint32_t x = 0; x < 16U; ++x)
        {
            const std::size_t index = (static_cast<std::size_t>(y) * 16U + x) * 3U;
            source.srgb[index] = static_cast<std::uint8_t>(x);
            source.srgb[index + 1U] = static_cast<std::uint8_t>(y);
            source.srgb[index + 2U] = 80;
        }
    }

    Recipe recipe;
    recipe.asset = {"raster", "memory:raster", std::nullopt};
    declare_input(recipe);
    recipe.operations.push_back({"ravo.geometry.crop",
                                 1,
                                 "crop-1",
                                 true,
                                 {{"x", ParameterValue{box.value().x}},
                                  {"y", ParameterValue{box.value().y}},
                                  {"width", ParameterValue{box.value().width}},
                                  {"height", ParameterValue{box.value().height}}},
                                 std::nullopt});
    RenderRequest request;
    request.asset = recipe.asset;
    request.recipe = recipe;
    auto rendered = engine.value().render_to_image(request, &source);
    ASSERT_TRUE(rendered) << rendered.error().message;
    EXPECT_EQ(rendered.value().width, 8U);
    EXPECT_EQ(rendered.value().height, 8U);
    EXPECT_EQ(rendered.value().rgb[0], 4);
    EXPECT_EQ(rendered.value().rgb[1], 4);
    EXPECT_EQ(rendered.value().rgb[2], 80);
}

TEST(ExposureAnalysisTest, RawInputColorPrimariesAndProfileConversionPreserveOneSnapshot)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    DecodedRaw raw = synthetic_bayer_raw();
    raw.exposure_deflicker_black_level = 0U;
    raw.exposure_deflicker_white_level = 1000U;
    raw.exposure_metadata.status = RawExposureMetadataStatus::kReady;
    const auto original_pixels = raw.pixels;
    Recipe recipe;
    recipe.asset = {"synthetic-bayer", "memory:raw", std::nullopt};
    declare_input(recipe);

    auto working = engine.value().linear_working_from_raw(raw, recipe, 9U, 9U, CancellationToken{});
    ASSERT_TRUE(working) << working.error().message;
    ASSERT_TRUE(working.value().exposure_analysis);
    const auto snapshot = working.value().exposure_analysis;
    EXPECT_EQ(snapshot->raw_histogram[100U], 81U);
    EXPECT_EQ(snapshot->raw_pixel_count, 81U);

    auto converted =
        convert_working_profile(working.value(), kInputProfileLinearRec2020, CancellationToken{});
    ASSERT_TRUE(converted) << converted.error().message;
    EXPECT_EQ(converted.value().exposure_analysis, snapshot);

    auto primaries = apply_primaries(converted.value(), PrimariesParams{}, CancellationToken{});
    ASSERT_TRUE(primaries) << primaries.error().message;
    EXPECT_EQ(primaries.value().exposure_analysis, snapshot);
    EXPECT_EQ(raw.pixels, original_pixels);
}

TEST(EngineFacadeTest, RawDenoiseSmoothsBayerAndXTransSpikes)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    DecodedRaw raw;
    raw.width = 64;
    raw.height = 64;
    raw.cfa_width = 2;
    raw.cfa_height = 2;
    raw.black_level = 0;
    raw.white_level = 1000;
    raw.has_as_shot_white_balance = true;
    declare_linear_srgb_matrix(raw);
    raw.cfa_channels = {0, 1, 1, 2};
    raw.pixels.assign(64U * 64U, 200);
    raw.pixels[32U * 64U + 32U] = 900;
    Recipe recipe;
    recipe.asset = {"synthetic-bayer", "memory:raw", std::nullopt};
    declare_input(recipe);
    recipe.operations.insert(recipe.operations.begin() + 1, {"ravo.raw.denoise",
                                                             1,
                                                             "rawdenoise-1",
                                                             true,
                                                             {{"threshold", ParameterValue{0.05}}},
                                                             std::nullopt});
    auto working =
        engine.value().linear_working_from_raw(raw, recipe, 64U, 64U, CancellationToken{});
    ASSERT_TRUE(working) << working.error().message;
    Recipe identity;
    identity.asset = recipe.asset;
    declare_input(identity);
    auto clean =
        engine.value().linear_working_from_raw(raw, identity, 64U, 64U, CancellationToken{});
    ASSERT_TRUE(clean) << clean.error().message;
    EXPECT_NE(working.value().rgb, clean.value().rgb);

    DecodedRaw xtrans = raw;
    xtrans.cfa_width = 6;
    xtrans.cfa_height = 6;
    xtrans.cfa_channels = {1, 2, 1, 1, 0, 1, 0, 1, 0, 2, 1, 2, 1, 2, 1, 1, 0, 1,
                           1, 0, 1, 1, 2, 1, 2, 1, 2, 0, 1, 0, 1, 0, 1, 1, 2, 1};
    const auto xtrans_source = xtrans.pixels;
    auto denoised_xtrans =
        engine.value().linear_working_from_raw(xtrans, recipe, 64U, 64U, CancellationToken{});
    auto original_xtrans =
        engine.value().linear_working_from_raw(xtrans, identity, 64U, 64U, CancellationToken{});
    ASSERT_TRUE(denoised_xtrans) << denoised_xtrans.error().message;
    ASSERT_TRUE(original_xtrans) << original_xtrans.error().message;
    EXPECT_NE(denoised_xtrans.value().rgb, original_xtrans.value().rgb);
    EXPECT_EQ(xtrans.pixels, xtrans_source);
}

TEST(EngineFacadeTest, HotPixelsMatchesFrozenBayerNeighbourContract)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    Recipe identity;
    identity.asset = {"synthetic-bayer", "memory:raw", std::nullopt};
    declare_input(identity);
    Recipe strict = identity;
    strict.operations.push_back(hot_pixels_operation(false));
    Recipe permissive = identity;
    permissive.operations.push_back(hot_pixels_operation(true));

    const auto clean = synthetic_bayer_raw();
    auto clean_working =
        engine.value().linear_working_from_raw(clean, identity, 9, 9, CancellationToken{});
    ASSERT_TRUE(clean_working) << clean_working.error().message;

    auto single = synthetic_bayer_raw();
    const std::size_t center = 4U * single.width + 4U;
    single.pixels[center] = 1000;
    const auto original_single = single.pixels;
    auto uncorrected =
        engine.value().linear_working_from_raw(single, identity, 9, 9, CancellationToken{});
    auto corrected =
        engine.value().linear_working_from_raw(single, strict, 9, 9, CancellationToken{});
    ASSERT_TRUE(uncorrected) << uncorrected.error().message;
    ASSERT_TRUE(corrected) << corrected.error().message;
    EXPECT_NE(uncorrected.value().rgb, corrected.value().rgb);
    EXPECT_EQ(corrected.value().rgb, clean_working.value().rgb);
    EXPECT_EQ(single.pixels, original_single);

    auto pair = synthetic_bayer_raw();
    pair.pixels[center] = 1000;
    pair.pixels[4U * pair.width + 2U] = 1000;
    auto pair_uncorrected =
        engine.value().linear_working_from_raw(pair, identity, 9, 9, CancellationToken{});
    auto pair_strict =
        engine.value().linear_working_from_raw(pair, strict, 9, 9, CancellationToken{});
    auto pair_permissive =
        engine.value().linear_working_from_raw(pair, permissive, 9, 9, CancellationToken{});
    ASSERT_TRUE(pair_uncorrected) << pair_uncorrected.error().message;
    ASSERT_TRUE(pair_strict) << pair_strict.error().message;
    ASSERT_TRUE(pair_permissive) << pair_permissive.error().message;
    EXPECT_EQ(pair_strict.value().rgb, pair_uncorrected.value().rgb);
    EXPECT_EQ(pair_permissive.value().rgb, clean_working.value().rgb);

    auto edge = synthetic_bayer_raw();
    edge.pixels[1U * edge.width + 1U] = 1000;
    auto edge_uncorrected =
        engine.value().linear_working_from_raw(edge, identity, 9, 9, CancellationToken{});
    auto edge_corrected =
        engine.value().linear_working_from_raw(edge, strict, 9, 9, CancellationToken{});
    ASSERT_TRUE(edge_uncorrected) << edge_uncorrected.error().message;
    ASSERT_TRUE(edge_corrected) << edge_corrected.error().message;
    EXPECT_EQ(edge_corrected.value().rgb, edge_uncorrected.value().rgb);

    auto xtrans = synthetic_bayer_raw();
    xtrans.cfa_width = 6;
    xtrans.cfa_height = 6;
    xtrans.cfa_channels.assign(36, 1);
    auto unsupported =
        engine.value().linear_working_from_raw(xtrans, strict, 9, 9, CancellationToken{});
    ASSERT_FALSE(unsupported);
    EXPECT_EQ(unsupported.error().code, ErrorCode::kUnsupported);

    CancellationSource cancellation;
    ASSERT_TRUE(cancellation.cancel("hotpixels"));
    auto cancelled =
        engine.value().linear_working_from_raw(single, strict, 9, 9, cancellation.token());
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, ErrorCode::kCancelled);

    auto raster_rejected =
        render_op(engine.value(), solid_raster(8, 8, 120, 120, 120), hot_pixels_operation());
    ASSERT_FALSE(raster_rejected);
    EXPECT_EQ(raster_rejected.error().code, ErrorCode::kUnsupported);
}

TEST(EngineFacadeTest, RawCaCorrectRejectsUnsupportedBuffersAndHonorsCancellation)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    Recipe recipe;
    recipe.asset = {"synthetic-bayer", "memory:raw", std::nullopt};
    declare_input(recipe);
    recipe.operations.push_back(raw_ca_operation());

    auto small = synthetic_bayer_raw();
    auto too_small =
        engine.value().linear_working_from_raw(small, recipe, 9, 9, CancellationToken{});
    ASSERT_FALSE(too_small);
    EXPECT_EQ(too_small.error().code, ErrorCode::kUnsupported);

    auto xtrans = synthetic_bayer_raw();
    xtrans.width = 32;
    xtrans.height = 32;
    xtrans.pixels.assign(32U * 32U, 100);
    xtrans.cfa_width = 6;
    xtrans.cfa_height = 6;
    xtrans.cfa_channels.assign(36, 1);
    auto unsupported =
        engine.value().linear_working_from_raw(xtrans, recipe, 32, 32, CancellationToken{});
    ASSERT_FALSE(unsupported);
    EXPECT_EQ(unsupported.error().code, ErrorCode::kUnsupported);

    CancellationSource cancellation;
    ASSERT_TRUE(cancellation.cancel("cacorrect"));
    auto cancelled =
        engine.value().linear_working_from_raw(xtrans, recipe, 32, 32, cancellation.token());
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, ErrorCode::kCancelled);

    auto raster_rejected =
        render_op(engine.value(), solid_raster(8, 8, 120, 120, 120), raw_ca_operation());
    ASSERT_FALSE(raster_rejected);
    EXPECT_EQ(raster_rejected.error().code, ErrorCode::kUnsupported);

    auto invalid_iterations =
        render_op(engine.value(), solid_raster(8, 8, 120, 120, 120), raw_ca_operation(6));
    ASSERT_FALSE(invalid_iterations);
    EXPECT_EQ(invalid_iterations.error().code, ErrorCode::kValidation);
}

TEST(TemperatureTest, ScalesBayerXtransAndFourthChannelWithoutMutatingInput)
{
    const std::array<float, kTemperatureChannelCount> coefficients{2.0F, 3.0F, 4.0F, 5.0F};
    const std::vector<float> bayer_input(8U, 1.0F);
    const std::vector<std::uint8_t> four_channel_bayer{0, 1, 3, 2};
    auto bayer = scale_temperature_cfa(bayer_input, 4, 2, 2, 2, four_channel_bayer, coefficients,
                                       CancellationToken{});
    ASSERT_TRUE(bayer) << bayer.error().message;
    EXPECT_EQ(bayer.value(), (std::vector<float>{2.0F, 3.0F, 2.0F, 3.0F, 5.0F, 4.0F, 5.0F, 4.0F}));
    EXPECT_EQ(bayer_input, std::vector<float>(8U, 1.0F));

    std::vector<std::uint8_t> xtrans_pattern(36U);
    for (std::size_t index = 0; index < xtrans_pattern.size(); ++index)
    {
        xtrans_pattern[index] = static_cast<std::uint8_t>(index % 3U);
    }
    const std::vector<float> xtrans_input(72U, 0.25F);
    auto xtrans = scale_temperature_cfa(xtrans_input, 12, 6, 6, 6, xtrans_pattern, coefficients,
                                        CancellationToken{});
    ASSERT_TRUE(xtrans) << xtrans.error().message;
    for (std::uint32_t row = 0; row < 6; ++row)
    {
        for (std::uint32_t column = 0; column < 12; ++column)
        {
            const auto channel = xtrans_pattern[(row % 6U) * 6U + (column % 6U)];
            EXPECT_FLOAT_EQ(xtrans.value()[static_cast<std::size_t>(row) * 12U + column],
                            0.25F * coefficients[channel]);
        }
    }

    CancellationSource cancellation;
    ASSERT_TRUE(cancellation.cancel("temperature_test"));
    auto cancelled = scale_temperature_cfa(xtrans_input, 12, 6, 6, 6, xtrans_pattern, coefficients,
                                           cancellation.token());
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, ErrorCode::kCancelled);

    auto invalid_coefficients = coefficients;
    invalid_coefficients[2] = 0.0F;
    auto invalid = scale_temperature_cfa(xtrans_input, 12, 6, 6, 6, xtrans_pattern,
                                         invalid_coefficients, CancellationToken{});
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code, ErrorCode::kValidation);
}

TEST(TemperatureTest, SampleWhiteBalanceNeutralizesAWarmBayerPatch)
{
    DecodedRaw raw;
    raw.width = 16;
    raw.height = 16;
    raw.cfa_width = 2;
    raw.cfa_height = 2;
    raw.black_level = 0;
    raw.white_level = 1000;
    raw.cfa_channels = {0, 1, 1, 2};
    raw.pixels.resize(static_cast<std::size_t>(raw.width) * raw.height);
    for (std::uint32_t y = 0; y < raw.height; ++y)
    {
        for (std::uint32_t x = 0; x < raw.width; ++x)
        {
            const auto channel = raw.cfa_channels[(y % 2U) * 2U + (x % 2U)];
            const std::uint16_t value = channel == 0 ? 400 : channel == 2 ? 100 : 200;
            raw.pixels[static_cast<std::size_t>(y) * raw.width + x] = value;
        }
    }
    WhiteBalancePickRequest request;
    request.preview_x = 0.5;
    request.preview_y = 0.5;
    auto sampled = sample_white_balance_coefficients(raw, request);
    ASSERT_TRUE(sampled) << sampled.error().message;
    EXPECT_NEAR(sampled.value()[0], 0.5, 1.0e-3);
    EXPECT_NEAR(sampled.value()[1], 1.0, 1.0e-6);
    EXPECT_NEAR(sampled.value()[2], 2.0, 1.0e-3);
    EXPECT_NEAR(sampled.value()[3], 1.0, 1.0e-3);
    request.preview_x = -0.1;
    auto invalid = sample_white_balance_coefficients(raw, request);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code, ErrorCode::kInvalidArgument);
}

TEST(TemperatureTest, ResolvesMetadataModesAndManualRgbFailsFast)
{
    DecodedRaw raw = synthetic_bayer_raw();
    raw.as_shot_white_balance = {2.0F, 1.0F, 1.5F, 1.0F};
    raw.camera_reference_white_balance = {1.2F, 1.0F, 1.1F, 1.0F};
    Recipe recipe;
    recipe.asset = {"raw", "memory:raw", std::nullopt};
    declare_input(recipe);

    auto as_shot = resolve_raw_temperature(raw, recipe);
    ASSERT_TRUE(as_shot) << as_shot.error().message;
    EXPECT_EQ(as_shot.value().coefficients, raw.as_shot_white_balance);

    TemperatureParams reference;
    reference.mode = std::string(kTemperatureModeCameraReference);
    recipe.operations = {temperature_operation(reference)};
    auto camera = resolve_raw_temperature(raw, recipe);
    ASSERT_TRUE(camera) << camera.error().message;
    EXPECT_EQ(camera.value().coefficients, raw.camera_reference_white_balance);

    const auto manual = test::temperature_0000_params();
    recipe.operations = {temperature_operation(manual)};
    auto explicit_coefficients = resolve_raw_temperature(raw, recipe);
    ASSERT_TRUE(explicit_coefficients) << explicit_coefficients.error().message;
    ASSERT_TRUE(manual.coefficients);
    for (std::size_t index = 0; index < kTemperatureChannelCount; ++index)
    {
        EXPECT_FLOAT_EQ(explicit_coefficients.value().coefficients[index],
                        static_cast<float>((*manual.coefficients)[index]));
    }

    raw.has_camera_reference_white_balance = false;
    recipe.operations = {temperature_operation(reference)};
    auto missing_reference = resolve_raw_temperature(raw, recipe);
    ASSERT_FALSE(missing_reference);
    EXPECT_EQ(missing_reference.error().code, ErrorCode::kValidation);
    raw.has_as_shot_white_balance = false;
    recipe.operations.clear();
    auto missing_as_shot = resolve_raw_temperature(raw, recipe);
    ASSERT_FALSE(missing_as_shot);
    EXPECT_EQ(missing_as_shot.error().code, ErrorCode::kValidation);

    WorkingImage rgb{1, 1, {0.25F, 0.5F, 0.75F}, {}, {}, {}, {}};
    const auto original = rgb.rgb;
    auto automatic_on_rgb =
        apply_temperature_rgb(rgb, temperature_operation(reference), CancellationToken{});
    ASSERT_FALSE(automatic_on_rgb);
    EXPECT_EQ(automatic_on_rgb.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(rgb.rgb, original);

    CancellationSource cancelled_source;
    ASSERT_TRUE(cancelled_source.cancel("temperature_rgb"));
    auto cancelled_rgb =
        apply_temperature_rgb(rgb, temperature_operation(manual), cancelled_source.token());
    ASSERT_FALSE(cancelled_rgb);
    EXPECT_EQ(cancelled_rgb.error().code, ErrorCode::kCancelled);
    EXPECT_EQ(rgb.rgb, original);

    auto manual_on_rgb =
        apply_temperature_rgb(rgb, temperature_operation(manual), CancellationToken{});
    ASSERT_TRUE(manual_on_rgb) << manual_on_rgb.error().message;
    EXPECT_FLOAT_EQ(rgb.rgb[0], 0.25F * 2.115234375F);
    EXPECT_FLOAT_EQ(rgb.rgb[1], 0.5F);
    EXPECT_FLOAT_EQ(rgb.rgb[2], 0.75F * 1.3984375F);
}

TEST(EngineFacadeTest, ChannelMixerMatchesFrozenRgbMatrixAndV3AdjustmentPaths)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    const auto source = solid_raster(4, 4, 180, 80, 30);

    auto identity =
        render_op(engine.value(), source, channel_mixer_operation(ChannelMixerParams{}));
    ASSERT_TRUE(identity) << identity.error().message;
    EXPECT_EQ(identity.value().rgb, source.srgb);

    ChannelMixerParams red_only;
    red_only.green = {1.0, 0.0, 0.0};
    red_only.blue = {1.0, 0.0, 0.0};
    auto singular = render_op(engine.value(), source, channel_mixer_operation(red_only));
    ASSERT_TRUE(singular) << singular.error().message;
    EXPECT_NEAR(singular.value().rgb[0], 180, 1);
    EXPECT_NEAR(singular.value().rgb[1], 180, 1);
    EXPECT_NEAR(singular.value().rgb[2], 180, 1);

    ChannelMixerParams swap;
    swap.red = {0.0, 0.0, 1.0};
    swap.blue = {1.0, 0.0, 0.0};
    auto crossed = render_op(engine.value(), source, channel_mixer_operation(swap));
    ASSERT_TRUE(crossed) << crossed.error().message;
    EXPECT_NEAR(crossed.value().rgb[0], 30, 1);
    EXPECT_NEAR(crossed.value().rgb[1], 80, 1);
    EXPECT_NEAR(crossed.value().rgb[2], 180, 1);

    ChannelMixerParams normalized;
    normalized.red = {2.0, 0.0, 0.0};
    normalized.normalize_red = true;
    auto normalized_result = render_op(engine.value(), source, channel_mixer_operation(normalized));
    ASSERT_TRUE(normalized_result) << normalized_result.error().message;
    EXPECT_NEAR(normalized_result.value().rgb[0], 180, 1);
    EXPECT_NEAR(normalized_result.value().rgb[1], 80, 1);
    EXPECT_NEAR(normalized_result.value().rgb[2], 30, 1);

    // Static decode of the two schema-v3 channelmixerrgb instances in fixture 0085.
    ChannelMixerParams fixture_default;
    fixture_default.adaptation = std::string(kChannelMixerAdaptationCat16);
    fixture_default.illuminant_x = 0.3819674253463745;
    fixture_default.illuminant_y = 0.36998802423477173;
    fixture_default.gamut = 1.0;
    fixture_default.clip = true;
    auto adapted = render_op(engine.value(), source, channel_mixer_operation(fixture_default));
    ASSERT_TRUE(adapted) << adapted.error().message;
    EXPECT_NE(adapted.value().rgb, identity.value().rgb);
    EXPECT_NEAR(adapted.value().rgb[0], 171, 1);
    EXPECT_NEAR(adapted.value().rgb[1], 86, 1);
    EXPECT_NEAR(adapted.value().rgb[2], 40, 1);

    ChannelMixerParams fixture_adjusted;
    fixture_adjusted.red = {-0.968999981880188, 0.4760000705718994, 0.0};
    fixture_adjusted.green = {0.0, -0.4789999723434448, 0.0};
    fixture_adjusted.blue = {0.0, 0.0, 1.0};
    fixture_adjusted.saturation = {0.21500003337860107, -0.953000009059906, -0.5440000295639038};
    fixture_adjusted.lightness = {0.18400001525878906, -0.3050000071525574, 0.1380000114440918};
    fixture_adjusted.normalize_red = true;
    fixture_adjusted.normalize_green = true;
    fixture_adjusted.normalize_blue = true;
    fixture_adjusted.adaptation = std::string(kChannelMixerAdaptationCat16);
    fixture_adjusted.illuminant_x = 0.3098124563694;
    fixture_adjusted.illuminant_y = 0.3276206851005554;
    fixture_adjusted.gamut = 1.0;
    fixture_adjusted.clip = true;
    auto adjusted = render_op(engine.value(), source, channel_mixer_operation(fixture_adjusted));
    ASSERT_TRUE(adjusted) << adjusted.error().message;
    EXPECT_NE(adjusted.value().rgb, adapted.value().rgb);
    EXPECT_NEAR(adjusted.value().rgb[0], 231, 1);
    EXPECT_NEAR(adjusted.value().rgb[1], 52, 1);
    EXPECT_NEAR(adjusted.value().rgb[2], 58, 1);
}


} // namespace
} // namespace ravo
