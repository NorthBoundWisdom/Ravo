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

TEST(EngineFacadeTest, ExposesExactlyTheReservedPhaseOneDescriptors)
{
    const auto engine = EngineFacade::create_phase1();

    ASSERT_TRUE(engine) << engine.error().message;
    ASSERT_EQ(engine.value().operations().size(), kPhase1OperationCount);
    EXPECT_EQ(engine.value().operations().front().id, "ravo.core.identity");
    EXPECT_EQ(engine.value().operations().back().id, "ravo.output.scale");
    EXPECT_NE(engine.value().operations().end(),
              std::find_if(engine.value().operations().begin(), engine.value().operations().end(),
                           [](const OperationDescriptor &item)
                           { return item.id == "ravo.detail.sharpen"; }));
    EXPECT_NE(engine.value().operations().end(),
              std::find_if(engine.value().operations().begin(), engine.value().operations().end(),
                           [](const OperationDescriptor &item)
                           { return item.id == "ravo.core.tonecurve"; }));
    EXPECT_NE(engine.value().operations().end(),
              std::find_if(engine.value().operations().begin(), engine.value().operations().end(),
                           [](const OperationDescriptor &item)
                           { return item.id == "ravo.display.sigmoid"; }));
}

TEST(EngineFacadeTest, CancelledRequestsNeverReachRendering)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    CancellationSource cancellation;
    ASSERT_TRUE(cancellation.cancel("test_cancel"));

    RenderRequest request;
    request.asset = test::valid_recipe().asset;
    request.recipe = test::valid_recipe();
    request.cancellation = cancellation.token();
    const auto rendered = engine.value().render(request);

    ASSERT_FALSE(rendered);
    EXPECT_EQ(rendered.error().code, ErrorCode::kCancelled);
}

TEST(ExposureTest, ManualEvAndBlackUseTheFrozenLegacyFormulaWithoutMutatingSource)
{
    ColorProfileState profile;
    profile.kind = ColorProfileKind::kMatrix;
    profile.model = ColorModel::kRgb;
    profile.identifier = "working-fixture";
    profile.has_matrix = true;
    profile.camera_input = true;
    profile.icc_bytes = {1U, 2U, 3U};
    const WorkingImage input{2, 1, {-0.5F, 0.0F, 0.25F, 0.5F, 1.0F, 2.0F}, profile, {}};
    const auto original = input;
    ExposureParams params;
    params.black = -0.25;
    params.exposure_ev = 1.0;

    auto output = apply_exposure(input, params, CancellationToken{});

    ASSERT_TRUE(output) << output.error().message;
    const double scale = 1.0 / (std::exp2(-params.exposure_ev) - params.black);
    ASSERT_EQ(output.value().rgb.size(), input.rgb.size());
    for (std::size_t index = 0; index < input.rgb.size(); ++index)
    {
        EXPECT_NEAR(output.value().rgb[index],
                    (static_cast<double>(input.rgb[index]) - params.black) * scale, 1.0e-6)
            << index;
    }
    EXPECT_EQ(output.value().width, input.width);
    EXPECT_EQ(output.value().height, input.height);
    EXPECT_EQ(output.value().color_profile, profile);
    EXPECT_NE(output.value().rgb.data(), input.rgb.data());
    ASSERT_FALSE(output.value().color_profile.icc_bytes.empty());
    EXPECT_NE(output.value().color_profile.icc_bytes.data(), input.color_profile.icc_bytes.data());
    output.value().rgb[0] = 42.0F;
    output.value().color_profile.icc_bytes[0] = 99U;
    EXPECT_EQ(input.width, original.width);
    EXPECT_EQ(input.height, original.height);
    EXPECT_EQ(input.rgb, original.rgb);
    EXPECT_EQ(input.color_profile, original.color_profile);
}

TEST(ExposureTest, BoundaryFailuresNeverPublishOrMutatePixels)
{
    WorkingImage input{1, 1, {0.25F, 0.5F, 0.75F}, {}, {}};
    input.color_profile.kind = ColorProfileKind::kBuiltin;
    input.color_profile.model = ColorModel::kRgb;
    input.color_profile.identifier = "linear-rec709";
    const auto original = input;

    WorkingImage zero = input;
    zero.width = 0;
    auto zero_result = apply_exposure(zero, ExposureParams{}, CancellationToken{});
    ASSERT_FALSE(zero_result);
    EXPECT_EQ(zero_result.error().code, ErrorCode::kValidation);

    WorkingImage wrong_size = input;
    wrong_size.width = 2;
    auto size_result = apply_exposure(wrong_size, ExposureParams{}, CancellationToken{});
    ASSERT_FALSE(size_result);
    EXPECT_EQ(size_result.error().code, ErrorCode::kValidation);

    WorkingImage overflow_dimensions;
    overflow_dimensions.width = std::numeric_limits<std::uint32_t>::max();
    overflow_dimensions.height = std::numeric_limits<std::uint32_t>::max();
    auto dimensions_result =
        apply_exposure(overflow_dimensions, ExposureParams{}, CancellationToken{});
    ASSERT_FALSE(dimensions_result);
    EXPECT_EQ(dimensions_result.error().code, ErrorCode::kValidation);

    WorkingImage lab = input;
    lab.color_profile.model = ColorModel::kLab;
    auto model_result = apply_exposure(lab, ExposureParams{}, CancellationToken{});
    ASSERT_FALSE(model_result);
    EXPECT_EQ(model_result.error().code, ErrorCode::kUnsupported);

    for (const float invalid_sample :
         {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
          -std::numeric_limits<float>::infinity()})
    {
        WorkingImage invalid = input;
        invalid.rgb[1] = invalid_sample;
        auto finite_result = apply_exposure(invalid, ExposureParams{}, CancellationToken{});
        ASSERT_FALSE(finite_result);
        EXPECT_EQ(finite_result.error().code, ErrorCode::kValidation);
        EXPECT_EQ(invalid.rgb[0], input.rgb[0]);
        EXPECT_EQ(invalid.rgb[2], input.rgb[2]);
    }

    ExposureParams invalid_black;
    invalid_black.black = 1.0;
    auto denominator_result = apply_exposure(input, invalid_black, CancellationToken{});
    ASSERT_FALSE(denominator_result);
    EXPECT_EQ(denominator_result.error().code, ErrorCode::kValidation);
    EXPECT_EQ(denominator_result.error().context.at("reason"), "invalid_exposure_denominator");

    ExposureParams overflow;
    overflow.exposure_ev = kExposureEvMax;
    WorkingImage maximum = input;
    maximum.rgb[0] = std::numeric_limits<float>::max();
    auto overflow_result = apply_exposure(maximum, overflow, CancellationToken{});
    ASSERT_FALSE(overflow_result);
    EXPECT_EQ(overflow_result.error().code, ErrorCode::kValidation);
    EXPECT_EQ(overflow_result.error().context.at("reason"), "unrepresentable_exposure_sample");

    CancellationSource cancelled;
    ASSERT_TRUE(cancelled.cancel("exposure-pre-cancel"));
    auto pre_cancelled = apply_exposure(input, ExposureParams{}, cancelled.token());
    ASSERT_FALSE(pre_cancelled);
    EXPECT_EQ(pre_cancelled.error().code, ErrorCode::kCancelled);
    EXPECT_EQ(input.rgb, original.rgb);

    WorkingImage large;
    large.width = 1024;
    large.height = 4096;
    large.color_profile = input.color_profile;
    large.rgb.assign(static_cast<std::size_t>(large.width) * large.height * 3U, 0.5F);
    const auto deadline = CancellationSource::with_deadline(std::chrono::steady_clock::now() +
                                                            std::chrono::milliseconds{1});
    auto row_cancelled = apply_exposure(large, ExposureParams{}, deadline.token());
    ASSERT_FALSE(row_cancelled);
    EXPECT_EQ(row_cancelled.error().code, ErrorCode::kCancelled);
    EXPECT_FLOAT_EQ(large.rgb.front(), 0.5F);
    EXPECT_FLOAT_EQ(large.rgb.back(), 0.5F);
}

TEST(ExposureTest, RasterDeflickerAndMetadataCompensationFailWithFrozenReasons)
{
    WorkingImage input{1, 1, {0.25F, 0.5F, 0.75F}, {}, {}};
    input.color_profile.model = ColorModel::kRgb;

    ExposureParams deflicker;
    deflicker.mode = std::string(kExposureModeDeflicker);
    auto automatic = apply_exposure(input, deflicker, CancellationToken{});
    ASSERT_FALSE(automatic);
    EXPECT_EQ(automatic.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(automatic.error().context.at("reason"), "exposure_deflicker_requires_raw_histogram");

    ExposureParams bias;
    bias.compensate_exposure_bias = true;
    auto bias_result = apply_exposure(input, bias, CancellationToken{});
    ASSERT_FALSE(bias_result);
    EXPECT_EQ(bias_result.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(bias_result.error().context.at("reason"),
              "exposure_bias_compensation_requires_metadata");

    ExposureParams highlight;
    highlight.compensate_highlight_preservation = true;
    auto highlight_result = apply_exposure(input, highlight, CancellationToken{});
    ASSERT_FALSE(highlight_result);
    EXPECT_EQ(highlight_result.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(highlight_result.error().context.at("reason"),
              "exposure_highlight_compensation_requires_metadata");

    OperationInstance masked{std::string(kExposureOperationId),
                             kExposureOperationSchemaVersion,
                             "exposure-1",
                             true,
                             exposure_to_parameters(ExposureParams{}),
                             "mask-1"};
    auto masked_result = apply_exposure(input, masked, CancellationToken{});
    ASSERT_FALSE(masked_result);
    EXPECT_EQ(masked_result.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(masked_result.error().context.at("reason"), "exposure_mask_graph_unavailable");
}

TEST(ExposureTest, ManualMetadataCompensationUsesLegacyOrderBoundsAndOwnedOutput)
{
    WorkingImage input{1, 1, {0.25F, 0.5F, 0.75F}, {}, {}};
    input.color_profile.kind = ColorProfileKind::kMatrix;
    input.color_profile.model = ColorModel::kRgb;
    input.color_profile.identifier = "metadata-fixture";
    input.color_profile.icc_bytes = {4U, 5U, 6U};
    RawExposureMetadata metadata;
    metadata.status = RawExposureMetadataStatus::kReady;
    metadata.exposure_bias_ev = 2.25;
    metadata.highlight_preservation_ev = 0.66;
    input.exposure_analysis = exposure_analysis({}, 0U, 1024U, metadata);
    const auto original = input;

    ExposureParams params;
    params.black = -0.125;
    params.exposure_ev = 1.0;
    params.compensate_exposure_bias = true;
    params.compensate_highlight_preservation = true;
    auto result = apply_exposure(input, params, CancellationToken{});

    ASSERT_TRUE(result) << result.error().message;
    const double effective_ev = 1.0 - 2.25 + 0.66;
    const double scale = 1.0 / (std::exp2(-effective_ev) - params.black);
    for (std::size_t index = 0; index < input.rgb.size(); ++index)
    {
        EXPECT_NEAR(result.value().rgb[index],
                    (static_cast<double>(input.rgb[index]) - params.black) * scale, 1.0e-6);
    }
    EXPECT_EQ(result.value().exposure_analysis, input.exposure_analysis);
    EXPECT_NE(result.value().rgb.data(), input.rgb.data());
    EXPECT_NE(result.value().color_profile.icc_bytes.data(), input.color_profile.icc_bytes.data());
    EXPECT_EQ(input.rgb, original.rgb);
    EXPECT_EQ(input.color_profile, original.color_profile);
    EXPECT_EQ(input.exposure_analysis, original.exposure_analysis);

    auto clamped_input = input;
    auto clamped_context = std::make_shared<ExposureAnalysisContext>(*input.exposure_analysis);
    clamped_context->metadata.exposure_bias_ev = 50.0;
    clamped_context->metadata.highlight_preservation_ev = 50.0;
    clamped_input.exposure_analysis = clamped_context;
    params.black = 0.0;
    auto clamped = apply_exposure(clamped_input, params, CancellationToken{});
    ASSERT_TRUE(clamped) << clamped.error().message;
    for (std::size_t index = 0; index < input.rgb.size(); ++index)
    {
        EXPECT_NEAR(clamped.value().rgb[index], input.rgb[index], 1.0e-6) << index;
    }
}

TEST(ExposureTest, MetadataReadStateDistinguishesAbsentTagsFailuresAndCorruption)
{
    WorkingImage input{1, 1, {0.25F, 0.5F, 0.75F}, {}, {}};
    input.color_profile.model = ColorModel::kRgb;
    RawExposureMetadata ready;
    ready.status = RawExposureMetadataStatus::kReady;
    input.exposure_analysis = exposure_analysis({}, 0U, 1024U, ready);
    ExposureParams requested;
    requested.exposure_ev = 1.0;
    requested.compensate_exposure_bias = true;
    requested.compensate_highlight_preservation = true;

    auto missing_tags_are_zero = apply_exposure(input, requested, CancellationToken{});
    ASSERT_TRUE(missing_tags_are_zero) << missing_tags_are_zero.error().message;
    EXPECT_NEAR(missing_tags_are_zero.value().rgb[0], input.rgb[0] * 2.0, 1.0e-6);

    auto failed_context = std::make_shared<ExposureAnalysisContext>(*input.exposure_analysis);
    failed_context->metadata.status = RawExposureMetadataStatus::kReadFailed;
    failed_context->metadata.failure_detail = "fixture metadata failure";
    input.exposure_analysis = failed_context;
    auto ordinary_manual = apply_exposure(input, ExposureParams{}, CancellationToken{});
    ASSERT_TRUE(ordinary_manual) << ordinary_manual.error().message;
    auto failed = apply_exposure(input, requested, CancellationToken{});
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().code, ErrorCode::kIo);
    EXPECT_EQ(failed.error().context.at("reason"), "exposure_metadata_read_failed");
    EXPECT_EQ(failed.error().context.at("detail"), "fixture metadata failure");

    auto unavailable_context = std::make_shared<ExposureAnalysisContext>(*failed_context);
    unavailable_context->metadata = {};
    input.exposure_analysis = unavailable_context;
    auto unavailable = apply_exposure(input, requested, CancellationToken{});
    ASSERT_FALSE(unavailable);
    EXPECT_EQ(unavailable.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(unavailable.error().context.at("reason"), "exposure_metadata_unavailable");

    auto corrupt_context = std::make_shared<ExposureAnalysisContext>(*unavailable_context);
    corrupt_context->metadata.status = RawExposureMetadataStatus::kReady;
    corrupt_context->metadata.exposure_bias_ev = std::numeric_limits<double>::infinity();
    input.exposure_analysis = corrupt_context;
    auto corrupt = apply_exposure(input, requested, CancellationToken{});
    ASSERT_FALSE(corrupt);
    EXPECT_EQ(corrupt.error().code, ErrorCode::kValidation);
    EXPECT_EQ(corrupt.error().context.at("reason"), "non_finite_exposure_metadata");
}

TEST(ExposureMetadataTest, BiasUsesPhotoThenImagePriorityAndLegacyClamp)
{
    LegacyExposureMetadataTags tags;
    auto absent = resolve_legacy_exposure_metadata(tags);
    ASSERT_TRUE(absent) << absent.error().message;
    EXPECT_EQ(absent.value().status, RawExposureMetadataStatus::kReady);
    EXPECT_DOUBLE_EQ(absent.value().exposure_bias_ev, 0.0);
    EXPECT_DOUBLE_EQ(absent.value().highlight_preservation_ev, 0.0);

    tags.photo_exposure_bias_ev = 9.0;
    tags.image_exposure_bias_ev = -9.0;
    auto photo = resolve_legacy_exposure_metadata(tags);
    ASSERT_TRUE(photo) << photo.error().message;
    EXPECT_DOUBLE_EQ(photo.value().exposure_bias_ev, 5.0);

    tags.photo_exposure_bias_ev.reset();
    auto image = resolve_legacy_exposure_metadata(tags);
    ASSERT_TRUE(image) << image.error().message;
    EXPECT_DOUBLE_EQ(image.value().exposure_bias_ev, -5.0);

    tags.image_exposure_bias_ev = std::numeric_limits<double>::quiet_NaN();
    auto invalid = resolve_legacy_exposure_metadata(tags);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().context.at("reason"), "non_finite_exposure_bias_metadata");
}

TEST(ExposureMetadataTest, HighlightMappingFreezesMakerPriorityAndValues)
{
    for (const auto &[state, expected] : std::array<std::pair<std::int64_t, double>, 4>{
             std::pair{0, 0.50}, std::pair{1, 0.33}, std::pair{2, 0.66}, std::pair{3, 0.0}})
    {
        LegacyExposureMetadataTags tags;
        tags.canon_lighting_opt = std::vector<std::int64_t>{12, 1, state};
        tags.fuji_development_dynamic_range = 400;
        auto mapped = resolve_legacy_exposure_metadata(tags);
        ASSERT_TRUE(mapped) << mapped.error().message;
        EXPECT_DOUBLE_EQ(mapped.value().highlight_preservation_ev, expected) << state;
    }

    LegacyExposureMetadataTags canon_cr3;
    canon_cr3.canon_auto_lighting_optimizer = std::vector<std::int64_t>{2};
    auto canon = resolve_legacy_exposure_metadata(canon_cr3);
    ASSERT_TRUE(canon) << canon.error().message;
    EXPECT_DOUBLE_EQ(canon.value().highlight_preservation_ev, 0.66);

    LegacyExposureMetadataTags fuji;
    fuji.fuji_development_dynamic_range = 200;
    fuji.fuji_auto_dynamic_range = 400;
    auto development = resolve_legacy_exposure_metadata(fuji);
    ASSERT_TRUE(development) << development.error().message;
    EXPECT_DOUBLE_EQ(development.value().highlight_preservation_ev, 1.0);
    fuji.fuji_development_dynamic_range.reset();
    auto automatic = resolve_legacy_exposure_metadata(fuji);
    ASSERT_TRUE(automatic) << automatic.error().message;
    EXPECT_DOUBLE_EQ(automatic.value().highlight_preservation_ev, 2.0);

    LegacyExposureMetadataTags nikon;
    nikon.nikon_color_space = 4;
    nikon.nikon_active_d_lighting = 11;
    auto hlg = resolve_legacy_exposure_metadata(nikon);
    ASSERT_TRUE(hlg) << hlg.error().message;
    EXPECT_DOUBLE_EQ(hlg.value().highlight_preservation_ev, 2.0);
    nikon.nikon_color_space.reset();
    const std::array<std::pair<std::int64_t, double>, 8> nikon_cases{
        std::pair{3, 0.33}, std::pair{5, 0.66}, std::pair{7, 1.0},   std::pair{8, 1.1},
        std::pair{9, 1.2},  std::pair{10, 1.3}, std::pair{11, 1.33}, std::pair{65535, 0.0}};
    for (const auto &[state, expected] : nikon_cases)
    {
        nikon.nikon_active_d_lighting = state;
        auto mapped = resolve_legacy_exposure_metadata(nikon);
        ASSERT_TRUE(mapped) << mapped.error().message;
        EXPECT_DOUBLE_EQ(mapped.value().highlight_preservation_ev, expected) << state;
    }

    LegacyExposureMetadataTags olympus;
    olympus.olympus_camera_settings_gradation = std::vector<std::int64_t>{0, 0, 0, 1};
    auto overridden = resolve_legacy_exposure_metadata(olympus);
    ASSERT_TRUE(overridden) << overridden.error().message;
    EXPECT_DOUBLE_EQ(overridden.value().highlight_preservation_ev, 0.33);
    olympus.olympus_camera_settings_gradation = std::vector<std::int64_t>{-1, -1, 1};
    auto low_key = resolve_legacy_exposure_metadata(olympus);
    ASSERT_TRUE(low_key) << low_key.error().message;
    EXPECT_DOUBLE_EQ(low_key.value().highlight_preservation_ev, 0.66);

    LegacyExposureMetadataTags pentax;
    pentax.pentax_dynamic_range_expansion = std::vector<std::uint8_t>{1U, 2U};
    auto expanded = resolve_legacy_exposure_metadata(pentax);
    ASSERT_TRUE(expanded) << expanded.error().message;
    EXPECT_DOUBLE_EQ(expanded.value().highlight_preservation_ev, 1.0);

    LegacyExposureMetadataTags malformed;
    malformed.canon_lighting_opt = std::vector<std::int64_t>{1, 2};
    auto rejected = resolve_legacy_exposure_metadata(malformed);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "malformed_highlight_preservation_metadata");
}

TEST(ExposureTest, DeflickerPercentilesUseOriginalUint16HistogramAndOverrideCompensations)
{
    WorkingImage input{1, 1, {0.25F, 0.5F, 0.75F}, {}, {}};
    input.color_profile.model = ColorModel::kRgb;
    RawExposureMetadata failed_metadata;
    failed_metadata.status = RawExposureMetadataStatus::kReadFailed;
    failed_metadata.failure_detail = "must be ignored by deflicker";
    input.exposure_analysis =
        exposure_analysis({{100U, 1U}, {200U, 1U}, {400U, 2U}}, 64U, 1024U, failed_metadata);
    const auto original = input;

    struct Case
    {
        double percentile;
        std::uint32_t selected_raw;
    };
    for (const Case test_case : {Case{0.0, 0U}, Case{50.0, 200U}, Case{100.0, 400U}})
    {
        ExposureParams params;
        params.mode = std::string(kExposureModeDeflicker);
        params.black = -0.125;
        params.exposure_ev = kExposureEvMax;
        params.deflicker_percentile = test_case.percentile;
        params.deflicker_target_ev = -4.0;
        params.compensate_exposure_bias = true;
        params.compensate_highlight_preservation = true;
        auto result = apply_exposure(input, params, CancellationToken{});

        ASSERT_TRUE(result) << result.error().message;
        const double raw_range = 1024.0 - 64.0;
        const double black_relative = static_cast<double>(
            std::max<std::int64_t>(static_cast<std::int64_t>(test_case.selected_raw) - 64, 1));
        const double raw_ev = -std::log2(raw_range) + std::log2(black_relative);
        const double effective_ev = params.deflicker_target_ev - raw_ev;
        const double scale = 1.0 / (std::exp2(-effective_ev) - params.black);
        for (std::size_t index = 0; index < input.rgb.size(); ++index)
        {
            EXPECT_NEAR(result.value().rgb[index],
                        (static_cast<double>(input.rgb[index]) - params.black) * scale, 1.0e-6)
                << test_case.percentile << ":" << index;
        }
        EXPECT_EQ(result.value().exposure_analysis, input.exposure_analysis);
    }
    EXPECT_EQ(input.rgb, original.rgb);
    EXPECT_EQ(input.exposure_analysis, original.exposure_analysis);
}

TEST(ExposureTest, DeflickerRejectsMalformedHistogramCountsAndRawLevels)
{
    WorkingImage input{1, 1, {0.25F, 0.5F, 0.75F}, {}, {}};
    input.color_profile.model = ColorModel::kRgb;
    ExposureParams params;
    params.mode = std::string(kExposureModeDeflicker);

    auto malformed = std::make_shared<ExposureAnalysisContext>();
    malformed->raw_pixel_count = 1U;
    malformed->raw_black_level = 0U;
    malformed->raw_white_level = 1024U;
    input.exposure_analysis = malformed;
    auto wrong_bins = apply_exposure(input, params, CancellationToken{});
    ASSERT_FALSE(wrong_bins);
    EXPECT_EQ(wrong_bins.error().context.at("reason"), "invalid_exposure_raw_histogram");

    malformed->raw_histogram.assign(kExposureRawHistogramBins, 0U);
    malformed->raw_histogram[100U] = 2U;
    auto wrong_count = apply_exposure(input, params, CancellationToken{});
    ASSERT_FALSE(wrong_count);
    EXPECT_EQ(wrong_count.error().context.at("reason"), "invalid_exposure_raw_histogram_count");

    malformed->raw_pixel_count = 2U;
    malformed->raw_black_level = 1024U;
    malformed->raw_white_level = 1024U;
    auto wrong_levels = apply_exposure(input, params, CancellationToken{});
    ASSERT_FALSE(wrong_levels);
    EXPECT_EQ(wrong_levels.error().context.at("reason"), "invalid_exposure_raw_levels");
}

TEST(ExposureAnalysisTest, SnapshotOwnsOriginalRawHistogramAndHonorsCancellation)
{
    DecodedRaw raw;
    raw.width = 3U;
    raw.height = 2U;
    raw.pixels = {0U, 100U, 100U, 400U, 65535U, 400U};
    raw.exposure_deflicker_black_level = 64U;
    raw.exposure_deflicker_white_level = 1024U;
    raw.exposure_metadata.status = RawExposureMetadataStatus::kReady;
    raw.exposure_metadata.exposure_bias_ev = 0.5;
    const auto source = raw;

    auto snapshot = build_exposure_analysis_context(raw, CancellationToken{});
    ASSERT_TRUE(snapshot) << snapshot.error().message;
    ASSERT_EQ(snapshot.value()->raw_histogram.size(), kExposureRawHistogramBins);
    EXPECT_EQ(snapshot.value()->raw_pixel_count, 6U);
    EXPECT_EQ(snapshot.value()->raw_histogram[0U], 1U);
    EXPECT_EQ(snapshot.value()->raw_histogram[100U], 2U);
    EXPECT_EQ(snapshot.value()->raw_histogram[400U], 2U);
    EXPECT_EQ(snapshot.value()->raw_histogram[65535U], 1U);
    EXPECT_EQ(snapshot.value()->raw_black_level, 64U);
    EXPECT_EQ(snapshot.value()->raw_white_level, 1024U);
    EXPECT_DOUBLE_EQ(snapshot.value()->metadata.exposure_bias_ev, 0.5);
    EXPECT_EQ(raw.pixels, source.pixels);
    EXPECT_EQ(raw.exposure_metadata.exposure_bias_ev, source.exposure_metadata.exposure_bias_ev);

    CancellationSource cancelled;
    ASSERT_TRUE(cancelled.cancel("analysis-pre-cancel"));
    auto pre_cancelled = build_exposure_analysis_context(raw, cancelled.token());
    ASSERT_FALSE(pre_cancelled);
    EXPECT_EQ(pre_cancelled.error().code, ErrorCode::kCancelled);

    DecodedRaw large = raw;
    large.width = 1024U;
    large.height = 8192U;
    large.pixels.assign(static_cast<std::size_t>(large.width) * large.height, 100U);
    const auto deadline = CancellationSource::with_deadline(std::chrono::steady_clock::now() +
                                                            std::chrono::milliseconds{1});
    auto row_cancelled = build_exposure_analysis_context(large, deadline.token());
    ASSERT_FALSE(row_cancelled);
    EXPECT_EQ(row_cancelled.error().code, ErrorCode::kCancelled);
    EXPECT_EQ(large.pixels.front(), 100U);
    EXPECT_EQ(large.pixels.back(), 100U);
}

TEST(ExposureAnalysisTest, MemoryEstimateIncludesHistogramContextAndOwnedFailureDetail)
{
    DecodedRaw raw;
    raw.width = 2U;
    raw.height = 2U;
    raw.pixels.assign(4U, 0U);
    Recipe recipe;
    const std::uint64_t empty_capacity = raw.exposure_metadata.failure_detail.capacity();
    const std::uint64_t without_detail = estimate_raw_render_memory(raw, recipe, 2U, 2U);
    raw.exposure_metadata.failure_detail.reserve(4096U);
    raw.exposure_metadata.failure_detail = "seventeen-characters";
    const std::uint64_t failure_capacity = raw.exposure_metadata.failure_detail.capacity();
    const std::uint64_t with_detail = estimate_raw_render_memory(raw, recipe, 2U, 2U);

    // DecodedRaw and the immutable analysis snapshot coexist during render, so
    // both owned string allocations (including their terminators) are budgeted.
    EXPECT_EQ(with_detail - without_detail, 2U * (failure_capacity - empty_capacity));
    EXPECT_GE(without_detail,
              kExposureRawHistogramBins * sizeof(std::uint32_t) + sizeof(ExposureAnalysisContext));

    recipe.operations.push_back({"ravo.raw.highlights", 1, "highlights-1", true, {}, std::nullopt});
    const std::uint64_t with_raw_copy = estimate_raw_render_memory(raw, recipe, 2U, 2U);
    EXPECT_EQ(with_raw_copy - with_detail,
              raw.pixels.size() * sizeof(std::uint16_t) + failure_capacity + 1U);
}

TEST(ColorCheckerTest, MemoryEstimateIncludesTypedParamsFitAndLinearSolveScratch)
{
    DecodedRaw raw;
    raw.width = 2U;
    raw.height = 2U;
    raw.pixels.assign(4U, 0U);
    Recipe recipe;
    const std::uint64_t baseline = estimate_raw_render_memory(raw, recipe, 2U, 2U);
    ColorCheckerParams params;
    auto parameters = color_checker_to_parameters(params);
    ASSERT_TRUE(parameters) << parameters.error().message;
    recipe.operations.push_back({std::string(kColorCheckerOperationId),
                                 kColorCheckerOperationSchemaVersion, "colorchecker-1", true,
                                 std::move(parameters).value(), std::nullopt});

    const std::uint64_t estimated = estimate_raw_render_memory(raw, recipe, 2U, 2U);
    const std::uint64_t count = kColorCheckerDefaultPatchCount;
    const std::uint64_t fit_size = count + 4U;
    const std::uint64_t expected_scratch =
        count * sizeof(ColorCheckerPatch) + count * sizeof(std::array<float, 3>) +
        3U * fit_size * sizeof(float) + fit_size * fit_size * sizeof(double) +
        fit_size * sizeof(int) + fit_size * sizeof(double);
    EXPECT_EQ(estimated - baseline, expected_scratch);
}

TEST(EngineMemoryEstimateTest, OutputSampleWidthAddsThreeSixOrTwelveBytesPerPixel)
{
    DecodedRaw raw;
    raw.width = 2U;
    raw.height = 2U;
    raw.pixels.assign(4U, 0U);
    Recipe recipe;
    const std::uint64_t rgb8 = estimate_raw_render_memory(raw, recipe, 2U, 2U, 3U);
    const std::uint64_t rgb16 = estimate_raw_render_memory(raw, recipe, 2U, 2U, 6U);
    const std::uint64_t rgb_float = estimate_raw_render_memory(raw, recipe, 2U, 2U, 12U);
    EXPECT_EQ(rgb16 - rgb8, 2U * 2U * 3U);
    EXPECT_EQ(rgb_float - rgb8, 2U * 2U * 9U);
    EXPECT_EQ(estimate_raw_render_memory(raw, recipe, 2U, 2U), rgb8);
}

TEST(EngineMemoryEstimateTest, ExtremeOutputDimensionsSaturateInsteadOfWrapping)
{
    DecodedRaw raw;
    Recipe recipe;
    EXPECT_EQ(estimate_raw_render_memory(raw, recipe, std::numeric_limits<std::uint32_t>::max(),
                                         std::numeric_limits<std::uint32_t>::max(), 12U),
              std::numeric_limits<std::uint64_t>::max());
}

TEST(EngineOrientationTest, OddQuarterTurnsSwapDisplaySize)
{
    std::uint32_t width = 9504;
    std::uint32_t height = 6336;
    apply_display_rotation_to_size(width, height, 3);
    EXPECT_EQ(width, 6336U);
    EXPECT_EQ(height, 9504U);
    apply_display_rotation_to_size(width, height, 0);
    EXPECT_EQ(width, 6336U);
    EXPECT_EQ(height, 9504U);
    apply_display_rotation_to_size(width, height, 2);
    EXPECT_EQ(width, 6336U);
    EXPECT_EQ(height, 9504U);
}

TEST(CanonicalRoiScaleTest, RasterAndOrientedRawCreationCarryOnlyProvenProportionalScale)
{
    const auto full = CanonicalRoiScale::from_scaled_dimensions(8U, 6U, 8U, 6U);
    const auto downscaled = CanonicalRoiScale::from_scaled_dimensions(4U, 3U, 8U, 6U);
    EXPECT_TRUE(full.valid());
    EXPECT_TRUE(downscaled.valid());
    EXPECT_FLOAT_EQ(full.value(), 1.0F);
    EXPECT_FLOAT_EQ(downscaled.value(), 0.5F);
    EXPECT_FALSE(CanonicalRoiScale::from_scaled_dimensions(4U, 4U, 8U, 6U).valid());
    EXPECT_FALSE(CanonicalRoiScale::from_scaled_dimensions(4U, 3U, 0U, 6U).valid());

    RasterBuffer raster;
    raster.width = 4U;
    raster.height = 3U;
    raster.source_width = 8U;
    raster.source_height = 6U;
    raster.srgb.assign(4U * 3U * 3U, 127U);
    const auto raster_working = working_from_encoded_rgb8(raster);
    ASSERT_TRUE(raster_working) << raster_working.error().message;
    EXPECT_TRUE(raster_working.value().canonical_roi_scale.valid());
    EXPECT_FLOAT_EQ(raster_working.value().canonical_roi_scale.value(), 0.5F);
    raster.source_width = 0U;
    const auto unknown_raster_working = working_from_encoded_rgb8(raster);
    ASSERT_TRUE(unknown_raster_working) << unknown_raster_working.error().message;
    EXPECT_FALSE(unknown_raster_working.value().canonical_roi_scale.valid());

    DecodedRaw raw;
    raw.width = 4U;
    raw.height = 2U;
    raw.rotate_quarters = 1;
    raw.cfa_width = 2U;
    raw.cfa_height = 2U;
    raw.cfa_channels = {0U, 1U, 1U, 2U};
    raw.pixels.assign(8U, 1024U);
    raw.white_level = 4095U;
    // Existing callers pass display-oriented dimensions. A 4x2 RAW rotated
    // 90 degrees therefore has a 2x4 display source and a 1x2 half-scale
    // target; assert the actual final dimensions, not the pre-rotate buffer.
    const auto raw_working =
        working_from_raw(raw, 1U, 2U, {1.0F, 1.0F, 1.0F, 1.0F}, CancellationToken{});
    ASSERT_TRUE(raw_working) << raw_working.error().message;
    EXPECT_EQ(raw_working.value().width, 1U);
    EXPECT_EQ(raw_working.value().height, 2U);
    EXPECT_TRUE(raw_working.value().canonical_roi_scale.valid());
    EXPECT_FLOAT_EQ(raw_working.value().canonical_roi_scale.value(), 0.5F);
}

TEST(EngineFacadeTest, ReadsMire1EmbeddedCaptureAsLocalTimeWithoutOffset)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    const auto before = source_file_snapshot(mire1_path());
    ASSERT_TRUE(before);
    auto extracted =
        engine.value().read_embedded_capture_metadata(mire1_path(), CancellationToken{});
    ASSERT_TRUE(extracted) << extracted.error().message;
    ASSERT_TRUE(extracted.value().captured_datetime);
    EXPECT_EQ(extracted.value().captured_datetime->local_exif, "2007:09:11 13:53:33");
    ASSERT_TRUE(extracted.value().captured_datetime->subsecond_digits);
    EXPECT_EQ(*extracted.value().captured_datetime->subsecond_digits, "18");
    EXPECT_FALSE(extracted.value().captured_datetime->utc_offset_minutes);
    EXPECT_FALSE(extracted.value().location);
    EXPECT_EQ(source_file_snapshot(mire1_path()), before);

    CancellationSource cancelled;
    ASSERT_TRUE(cancelled.cancel("capture-pre-cancel"));
    auto stopped = engine.value().read_embedded_capture_metadata(mire1_path(), cancelled.token());
    ASSERT_FALSE(stopped);
    EXPECT_EQ(stopped.error().code, ErrorCode::kCancelled);
}

TEST(EngineFacadeTest, ConvertsUnsignedExifRationalsExactlyAtTheReaderBoundary)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    const auto root =
        std::filesystem::temp_directory_path() / ("ravo-engine-exif-" + generate_catalog_id());
    std::filesystem::create_directories(root);
    const auto read = [&](const std::string &name, const test_support::CaptureExifProfile &profile)
    {
        const auto path = root / name;
        const auto bytes = test_support::make_capture_exif_tiff(profile);
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char *>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        output.close();
        return engine.value().read_embedded_capture_metadata(path.string(), CancellationToken{});
    };

    test_support::CaptureExifProfile large;
    large.latitude = {{{4294967291U, 4294967291U}, {0U, 4294967279U}, {1U, 4294967231U}}};
    large.longitude = large.latitude;
    large.altitude = {4294967295U, 4294967295U};
    auto converted = read("large-coprime.tif", large);
    ASSERT_TRUE(converted) << converted.error().message;
    ASSERT_TRUE(converted.value().location);
    EXPECT_EQ(converted.value().location->latitude_e6, 1000000);
    EXPECT_EQ(converted.value().location->longitude_e6, 1000000);
    ASSERT_TRUE(converted.value().location->altitude);
    EXPECT_EQ(converted.value().location->altitude->magnitude_mm, 1000U);

    test_support::CaptureExifProfile north_tie;
    north_tie.latitude = {{{1U, 2000000U}, {0U, 1U}, {0U, 1U}}};
    converted = read("north-tie.tif", north_tie);
    ASSERT_TRUE(converted) << converted.error().message;
    EXPECT_EQ(converted.value().location->latitude_e6, 1);
    north_tie.latitude_ref = 'S';
    converted = read("south-tie.tif", north_tie);
    ASSERT_TRUE(converted) << converted.error().message;
    EXPECT_EQ(converted.value().location->latitude_e6, -1);

    test_support::CaptureExifProfile boundaries;
    boundaries.latitude = {{{90U, 1U}, {0U, 1U}, {0U, 1U}}};
    boundaries.longitude = {{{180U, 1U}, {0U, 1U}, {0U, 1U}}};
    boundaries.altitude_ref = 1U;
    boundaries.altitude = {12000U, 1U};
    converted = read("boundaries.tif", boundaries);
    ASSERT_TRUE(converted) << converted.error().message;
    EXPECT_EQ(converted.value().location->latitude_e6, 90000000);
    EXPECT_EQ(converted.value().location->longitude_e6, 180000000);
    EXPECT_EQ(converted.value().location->altitude->magnitude_mm, 12000000U);
    EXPECT_EQ(converted.value().location->altitude->reference,
              EngineCaptureAltitudeReference::kBelowSeaLevel);

    test_support::CaptureExifProfile over = boundaries;
    over.latitude = {{{90U, 1U}, {0U, 1U}, {36U, 25000U}}};
    auto rejected = read("latitude-over.tif", over);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "invalid_capture_gps_bounds");
    over = boundaries;
    over.longitude = {{{180U, 1U}, {0U, 1U}, {36U, 25000U}}};
    rejected = read("longitude-over.tif", over);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "invalid_capture_gps_bounds");

    test_support::CaptureExifProfile zero_denominator;
    zero_denominator.latitude[0].denominator = 0U;
    rejected = read("zero-denominator.tif", zero_denominator);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "invalid_capture_gps_rational");

    test_support::CaptureExifProfile altitude_over;
    altitude_over.altitude = {500000002U, 5000U};
    rejected = read("altitude-over.tif", altitude_over);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "invalid_capture_altitude");
    altitude_over.altitude = {1U, 0U};
    rejected = read("altitude-zero-denominator.tif", altitude_over);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "invalid_capture_gps_rational");

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST(EngineFacadeTest, RejectsMalformedPresentExifFieldsWithoutSilentOmission)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    const auto root =
        std::filesystem::temp_directory_path() / ("ravo-engine-tags-" + generate_catalog_id());
    std::filesystem::create_directories(root);
    const auto read_bytes = [&](const std::string &name, const std::vector<std::uint8_t> &bytes)
    {
        const auto path = root / name;
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char *>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        output.close();
        return engine.value().read_embedded_capture_metadata(path.string(), CancellationToken{});
    };
    const auto expect_reason = [&](const std::string &name,
                                   const test_support::CaptureExifProfile &profile,
                                   const std::string_view reason)
    {
        const auto result = read_bytes(name, test_support::make_capture_exif_tiff(profile));
        ASSERT_FALSE(result) << name;
        EXPECT_EQ(result.error().context.at("reason"), reason) << name;
    };

    test_support::CaptureExifProfile profile;
    profile.datetime = "2007:02:29 13:53:33";
    expect_reason("invalid-calendar.tif", profile, "invalid_capture_datetime");
    profile = {};
    profile.datetime[10] = '\0';
    expect_reason("embedded-nul.tif", profile, "contains_nul");
    profile = {};
    profile.subsecond = "1234567890";
    expect_reason("oversized-subsecond.tif", profile, "invalid_capture_tag_count");
    profile = {};
    profile.offset = "-00:00";
    expect_reason("negative-zero-offset.tif", profile, "invalid_capture_utc_offset");
    profile = {};
    profile.latitude_ref = 'Q';
    auto invalid_ref = read_bytes("invalid-ref.tif", test_support::make_capture_exif_tiff(profile));
    ASSERT_FALSE(invalid_ref);
    EXPECT_EQ(invalid_ref.error().context.at("reason"), "invalid_capture_gps_ref");
    EXPECT_EQ(invalid_ref.error().context.at("field"), "gps_latitude_ref");
    EXPECT_EQ(invalid_ref.error().context.at("path"), "Exif.GPSInfo.GPSLatitudeRef");
    profile = {};
    profile.altitude_ref = 2U;
    expect_reason("invalid-altitude-ref.tif", profile, "invalid_capture_altitude_ref");

    auto partial = test_support::make_capture_exif_tiff();
    ASSERT_TRUE(test_support::rewrite_linked_ifd_entry(partial, 34853U, 1U, 0xC001U));
    auto rejected = read_bytes("partial-location.tif", partial);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "incomplete_capture_location");

    auto orphan_altitude = test_support::make_capture_exif_tiff();
    for (std::uint16_t tag = 1U; tag <= 4U; ++tag)
    {
        ASSERT_TRUE(test_support::rewrite_linked_ifd_entry(
            orphan_altitude, 34853U, tag, static_cast<std::uint16_t>(0xC010U + tag)));
    }
    rejected = read_bytes("orphan-altitude.tif", orphan_altitude);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "orphan_capture_altitude");

    auto version_only = test_support::make_capture_exif_tiff();
    for (std::uint16_t tag = 1U; tag <= 6U; ++tag)
    {
        ASSERT_TRUE(test_support::rewrite_linked_ifd_entry(
            version_only, 34853U, tag, static_cast<std::uint16_t>(0xC020U + tag)));
    }
    auto absent = read_bytes("version-only.tif", version_only);
    ASSERT_TRUE(absent) << absent.error().message;
    EXPECT_FALSE(absent.value().location);

    auto wrong_type = test_support::make_capture_exif_tiff();
    ASSERT_TRUE(
        test_support::rewrite_linked_ifd_entry(wrong_type, 34665U, 0x9003U, std::nullopt, 3U, 1U));
    rejected = read_bytes("wrong-datetime-type.tif", wrong_type);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "wrong_type");
    EXPECT_EQ(rejected.error().context.at("field"), "captured_datetime");

    auto wrong_count = test_support::make_capture_exif_tiff();
    ASSERT_TRUE(test_support::rewrite_linked_ifd_entry(wrong_count, 34853U, 2U, std::nullopt,
                                                       std::nullopt, 2U));
    rejected = read_bytes("wrong-gps-count.tif", wrong_count);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "multi_value");
    EXPECT_EQ(rejected.error().context.at("field"), "gps_latitude_e6");

    auto orphan_time = test_support::make_capture_exif_tiff();
    ASSERT_TRUE(test_support::rewrite_linked_ifd_entry(orphan_time, 34665U, 0x9003U, 0x9004U));
    rejected = read_bytes("orphan-time.tif", orphan_time);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "orphan_capture_datetime_component");

    for (const std::string offset : {"+14:00", "-14:00"})
    {
        profile = {};
        profile.offset = offset;
        auto accepted = read_bytes("offset-" + offset.substr(1U, 2U) +
                                       (offset.front() == '+' ? "-plus.tif" : "-minus.tif"),
                                   test_support::make_capture_exif_tiff(profile));
        ASSERT_TRUE(accepted) << accepted.error().message;
        EXPECT_EQ(accepted.value().captured_datetime->utc_offset_minutes,
                  offset.front() == '+' ? 840 : -840);
    }
    profile = {};
    profile.latitude_ref = 'S';
    profile.longitude_ref = 'W';
    auto southwest = read_bytes("southwest.tif", test_support::make_capture_exif_tiff(profile));
    ASSERT_TRUE(southwest) << southwest.error().message;
    EXPECT_EQ(southwest.value().location->latitude_e6, -49253239);
    EXPECT_EQ(southwest.value().location->longitude_e6, -3050766);

    profile = {};
    profile.image_datetime = profile.datetime;
    auto matching_precedence = read_bytes("matching-datetime-precedence.tif",
                                          test_support::make_capture_exif_tiff(profile));
    ASSERT_TRUE(matching_precedence) << matching_precedence.error().message;
    EXPECT_EQ(matching_precedence.value().captured_datetime->local_exif, profile.datetime);

    profile.image_datetime = "2007:09:11 13:53:34";
    rejected =
        read_bytes("conflicting-datetime.tif", test_support::make_capture_exif_tiff(profile));
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "conflicting_capture_datetime");

    profile = {};
    profile.image_datetime = "2007:09:11 13:53:34";
    auto image_fallback = test_support::make_capture_exif_tiff(profile);
    ASSERT_TRUE(test_support::rewrite_linked_ifd_entry(image_fallback, 34665U, 0x9003U, 0x9004U));
    auto fallback = read_bytes("image-datetime-fallback.tif", image_fallback);
    ASSERT_TRUE(fallback) << fallback.error().message;
    EXPECT_EQ(fallback.value().captured_datetime->local_exif, *profile.image_datetime);

    profile = {};
    profile.duplicate_photo_datetime = profile.datetime;
    rejected =
        read_bytes("duplicate-photo-datetime.tif", test_support::make_capture_exif_tiff(profile));
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "duplicate_capture_tag");

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST(EngineFacadeTest, ReportsMissingNonRegularAndMalformedCaptureSourcesWithBoundedContext)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    const auto root =
        std::filesystem::temp_directory_path() / ("ravo-engine-source-" + generate_catalog_id());
    std::filesystem::create_directories(root);
    auto missing = engine.value().read_embedded_capture_metadata((root / "missing.tif").string(),
                                                                 CancellationToken{});
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, ErrorCode::kNotFound);
    auto directory =
        engine.value().read_embedded_capture_metadata(root.string(), CancellationToken{});
    ASSERT_FALSE(directory);
    EXPECT_EQ(directory.error().context.at("reason"), "non_regular_capture_source");
    const auto malformed_path = root / "malformed.tif";
    {
        std::ofstream malformed(malformed_path, std::ios::binary);
        malformed << "not a tiff";
    }
    auto malformed =
        engine.value().read_embedded_capture_metadata(malformed_path.string(), CancellationToken{});
    ASSERT_FALSE(malformed);
    for (const auto &[key, value] : malformed.error().context)
    {
        EXPECT_LE(value.size(), key == "input_uri" ? 512U : 256U);
        EXPECT_TRUE(std::all_of(value.begin(), value.end(),
                                [](const unsigned char ch) { return ch >= 0x20U && ch <= 0x7EU; }));
    }
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST(EngineFacadeTest, RejectsMalformedPngExifChunks)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    const auto root =
        std::filesystem::temp_directory_path() / ("ravo-engine-png-" + generate_catalog_id());
    std::filesystem::create_directories(root);
    const auto write_png =
        [&](const std::string &name, const std::function<void(QByteArray &)> &mutate)
    {
        QImage image(8, 8, QImage::Format_RGB888);
        image.fill(QColor(1, 2, 3));
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        EXPECT_TRUE(image.save(&buffer, "PNG"));
        mutate(png);
        const auto path = (root / name).string();
        QFile file(QString::fromStdString(path));
        EXPECT_TRUE(file.open(QIODevice::WriteOnly));
        EXPECT_EQ(file.write(png), png.size());
        file.close();
        return path;
    };
    const auto insert_exif = [](QByteArray &png, const QByteArray &payload, const bool bad_crc)
    {
        QByteArray rebuilt = png.left(8);
        qsizetype offset = 8;
        bool inserted = false;
        while (offset + 12 <= png.size())
        {
            const auto length =
                (static_cast<std::uint32_t>(static_cast<unsigned char>(png[offset])) << 24U) |
                (static_cast<std::uint32_t>(static_cast<unsigned char>(png[offset + 1])) << 16U) |
                (static_cast<std::uint32_t>(static_cast<unsigned char>(png[offset + 2])) << 8U) |
                static_cast<std::uint32_t>(static_cast<unsigned char>(png[offset + 3]));
            const QByteArray type = png.mid(offset + 4, 4);
            if (!inserted && type == "IDAT")
            {
                const auto len = static_cast<std::uint32_t>(payload.size());
                unsigned char header[8] = {static_cast<unsigned char>(len >> 24U),
                                           static_cast<unsigned char>(len >> 16U),
                                           static_cast<unsigned char>(len >> 8U),
                                           static_cast<unsigned char>(len),
                                           'e',
                                           'X',
                                           'I',
                                           'f'};
                rebuilt.append(reinterpret_cast<const char *>(header), 8);
                rebuilt.append(payload);
                uLong crc = crc32(0L, Z_NULL, 0);
                crc = crc32(crc, reinterpret_cast<const Bytef *>("eXIf"), 4);
                if (!payload.isEmpty())
                {
                    crc = crc32(crc, reinterpret_cast<const Bytef *>(payload.constData()),
                                static_cast<uInt>(payload.size()));
                }
                auto stored = static_cast<std::uint32_t>(crc);
                if (bad_crc)
                {
                    stored ^= 1U;
                }
                const unsigned char crc_bytes[4] = {static_cast<unsigned char>(stored >> 24U),
                                                    static_cast<unsigned char>(stored >> 16U),
                                                    static_cast<unsigned char>(stored >> 8U),
                                                    static_cast<unsigned char>(stored)};
                rebuilt.append(reinterpret_cast<const char *>(crc_bytes), 4);
                inserted = true;
            }
            rebuilt.append(png.mid(offset, 12 + static_cast<qsizetype>(length)));
            offset += 12 + static_cast<qsizetype>(length);
        }
        png = rebuilt;
    };
    auto bad = write_png("bad-crc.png",
                         [&](QByteArray &png) { insert_exif(png, QByteArray("II"), true); });
    auto extracted = engine.value().read_embedded_capture_metadata(bad, CancellationToken{});
    ASSERT_FALSE(extracted);
    EXPECT_EQ(extracted.error().context.at("reason"), "png_chunk_crc_mismatch");

    auto empty =
        write_png("empty.png", [&](QByteArray &png) { insert_exif(png, QByteArray(), false); });
    extracted = engine.value().read_embedded_capture_metadata(empty, CancellationToken{});
    ASSERT_FALSE(extracted);
    EXPECT_EQ(extracted.error().context.at("reason"), "empty_png_exif_chunk");

    auto prefixed = write_png("prefix.png", [&](QByteArray &png)
                              { insert_exif(png, QByteArray("Exif\0\0II", 8), false); });
    extracted = engine.value().read_embedded_capture_metadata(prefixed, CancellationToken{});
    ASSERT_FALSE(extracted);
    EXPECT_EQ(extracted.error().context.at("reason"), "jpeg_exif_prefix_in_png");

    const auto append_chunk = [](QByteArray &png, const char type[5], const QByteArray &data,
                                 const std::optional<std::uint32_t> declared_length = {})
    {
        const std::uint32_t length =
            declared_length.value_or(static_cast<std::uint32_t>(data.size()));
        const unsigned char header[8] = {
            static_cast<unsigned char>(length >> 24U), static_cast<unsigned char>(length >> 16U),
            static_cast<unsigned char>(length >> 8U),  static_cast<unsigned char>(length),
            static_cast<unsigned char>(type[0]),       static_cast<unsigned char>(type[1]),
            static_cast<unsigned char>(type[2]),       static_cast<unsigned char>(type[3]),
        };
        png.append(reinterpret_cast<const char *>(header), 8);
        png.append(data);
        if (declared_length && *declared_length != static_cast<std::uint32_t>(data.size()))
        {
            return;
        }
        uLong crc = crc32(0L, Z_NULL, 0);
        crc = crc32(crc, reinterpret_cast<const Bytef *>(type), 4U);
        if (!data.isEmpty())
        {
            crc = crc32(crc, reinterpret_cast<const Bytef *>(data.constData()),
                        static_cast<uInt>(data.size()));
        }
        const auto stored = static_cast<std::uint32_t>(crc);
        const unsigned char crc_bytes[4] = {
            static_cast<unsigned char>(stored >> 24U),
            static_cast<unsigned char>(stored >> 16U),
            static_cast<unsigned char>(stored >> 8U),
            static_cast<unsigned char>(stored),
        };
        png.append(reinterpret_cast<const char *>(crc_bytes), 4);
    };
    const auto structural_png =
        [&](const std::string &name, const std::function<void(QByteArray &)> &chunks)
    {
        QByteArray bytes("\x89PNG\r\n\x1a\n", 8);
        chunks(bytes);
        const auto path = (root / name).string();
        QFile file(QString::fromStdString(path));
        EXPECT_TRUE(file.open(QIODevice::WriteOnly));
        EXPECT_EQ(file.write(bytes), bytes.size());
        file.close();
        return path;
    };
    const QByteArray ihdr("\0\0\0\1\0\0\0\1\x08\x02\0\0\0", 13);
    const auto expect_reason = [&](const std::string &path, const std::string_view reason)
    {
        auto result = engine.value().read_embedded_capture_metadata(path, CancellationToken{});
        ASSERT_FALSE(result) << path;
        ASSERT_TRUE(result.error().context.contains("reason"));
        EXPECT_EQ(result.error().context.at("reason"), reason);
    };

    auto bounded_truncated =
        structural_png("bounded-truncated.png",
                       [&](QByteArray &png)
                       {
                           append_chunk(png, "IHDR", ihdr);
                           append_chunk(png, "ruSt", QByteArray(), 0xffffffffU);
                       });
    expect_reason(bounded_truncated, "truncated_png_chunk_payload");

    auto oversized_exif =
        structural_png("oversized-exif.png",
                       [&](QByteArray &png)
                       {
                           append_chunk(png, "IHDR", ihdr);
                           append_chunk(png, "eXIf", QByteArray(), 16U * 1024U * 1024U + 1U);
                       });
    expect_reason(oversized_exif, "png_exif_payload_too_large");

    auto duplicate_exif = structural_png("duplicate-exif.png",
                                         [&](QByteArray &png)
                                         {
                                             append_chunk(png, "IHDR", ihdr);
                                             append_chunk(png, "eXIf", QByteArray("II", 2));
                                             append_chunk(png, "eXIf", QByteArray("II", 2));
                                         });
    expect_reason(duplicate_exif, "duplicate_png_exif_chunk");

    auto exif_after_idat = structural_png("exif-after-idat.png",
                                          [&](QByteArray &png)
                                          {
                                              append_chunk(png, "IHDR", ihdr);
                                              append_chunk(png, "IDAT", QByteArray());
                                              append_chunk(png, "eXIf", QByteArray("II", 2));
                                          });
    expect_reason(exif_after_idat, "png_exif_after_idat");

    auto bad_ihdr = structural_png("bad-ihdr.png", [&](QByteArray &png)
                                   { append_chunk(png, "IHDR", QByteArray(12, '\0')); });
    expect_reason(bad_ihdr, "invalid_png_ihdr_length");

    auto bad_type = structural_png("bad-type.png",
                                   [&](QByteArray &png)
                                   {
                                       append_chunk(png, "IHDR", ihdr);
                                       append_chunk(png, "abct", QByteArray());
                                   });
    expect_reason(bad_type, "invalid_png_chunk_type");

    auto no_idat = structural_png("no-idat.png",
                                  [&](QByteArray &png)
                                  {
                                      append_chunk(png, "IHDR", ihdr);
                                      append_chunk(png, "IEND", QByteArray());
                                  });
    expect_reason(no_idat, "png_iend_before_idat");

    auto split_idat = structural_png("split-idat.png",
                                     [&](QByteArray &png)
                                     {
                                         append_chunk(png, "IHDR", ihdr);
                                         append_chunk(png, "IDAT", QByteArray());
                                         append_chunk(png, "tEXt", QByteArray());
                                         append_chunk(png, "IDAT", QByteArray());
                                     });
    expect_reason(split_idat, "nonconsecutive_png_idat");

    auto bad_iend = structural_png("bad-iend.png",
                                   [&](QByteArray &png)
                                   {
                                       append_chunk(png, "IHDR", ihdr);
                                       append_chunk(png, "IDAT", QByteArray());
                                       append_chunk(png, "IEND", QByteArray(1, '\0'));
                                   });
    expect_reason(bad_iend, "invalid_png_iend_length");

    auto trailing = write_png("trailing.png", [](QByteArray &png) { png.append('x'); });
    expect_reason(trailing, "png_trailing_data");
    auto metadata_free = write_png("metadata-free.png", [](QByteArray &) {});
    extracted = engine.value().read_embedded_capture_metadata(metadata_free, CancellationToken{});
    ASSERT_TRUE(extracted) << extracted.error().message;
    EXPECT_FALSE(extracted.value().captured_datetime);
    EXPECT_FALSE(extracted.value().location);

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST(EngineFacadeTest, InspectReadsTheFrozenRawFixture)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;

    const auto inspected = engine.value().inspect(mire1_path(), CancellationToken{});
    ASSERT_TRUE(inspected) << inspected.error().message;
    EXPECT_TRUE(inspected.value().is_raw);
    EXPECT_EQ(inspected.value().format, "raw");
    EXPECT_FALSE(inspected.value().make.empty());
    EXPECT_FALSE(inspected.value().model.empty());
    EXPECT_GT(inspected.value().width, 0U);
    EXPECT_GT(inspected.value().height, 0U);
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

TEST(EngineFacadeTest, FirstFrameDecodeRejectsXTransSensor)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    auto decoded = engine.value().decode_raw_frame(mire1_xtrans_path(), CancellationToken{});
    ASSERT_FALSE(decoded);
    EXPECT_EQ(decoded.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(decoded.error().context.at("reason"), "unsupported_raw_sensor");
    EXPECT_EQ(decoded.error().context.at("sensor"), "xtrans");
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
    // Ravo-owned reference statistics for the original pre-repair RAW histogram and
    // the frozen default deflicker target. The tolerance permits platform libm/SIMD
    // rounding without accepting a changed histogram source or exposure formula.
    EXPECT_NEAR(static_cast<double>(sums[0]), 251749.0, 1500.0);
    EXPECT_NEAR(static_cast<double>(sums[1]), 234182.0, 1500.0);
    EXPECT_NEAR(static_cast<double>(sums[2]), 220350.0, 1500.0);
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

[[nodiscard]] RasterBuffer solid_raster(const std::uint32_t width, const std::uint32_t height,
                                        const std::uint8_t r, const std::uint8_t g,
                                        const std::uint8_t b)
{
    RasterBuffer raster;
    raster.width = width;
    raster.height = height;
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
    return {2U,
            1U,
            {0.03F, 0.18F, 0.72F, 0.91F, 0.42F, 0.07F},
            std::move(profile),
            std::move(analysis)};
}

using FrozenD50Triplet = std::array<float, 3>;
inline constexpr float kPlatformLibmReferenceTolerance = 1.0e-5F;

// Independent scalar oracle transcribed from the frozen
// common/colorspaces_inline_conversions.h owner. In particular, it preserves
// the transposed-matrix addition order, the pre-rounded D50 reciprocals, and
// the Lab inverse scale/add order instead of calling the production seam.
[[nodiscard]] FrozenD50Triplet frozen_linear_rec709_to_xyz_d50(const FrozenD50Triplet &rgb) noexcept
{
    return {0.4360747F * rgb[0] + 0.3850649F * rgb[1] + 0.1430804F * rgb[2],
            0.2225045F * rgb[0] + 0.7168786F * rgb[1] + 0.0606169F * rgb[2],
            0.0139322F * rgb[0] + 0.0971045F * rgb[1] + 0.7141733F * rgb[2]};
}

[[nodiscard]] FrozenD50Triplet frozen_xyz_d50_to_linear_rec709(const FrozenD50Triplet &xyz) noexcept
{
    // Keep every negative coefficient as an added product, matching the frozen
    // dt_apply_transposed_color_matrix expression order rather than subtraction.
    return {3.1338561F * xyz[0] + (-1.6168667F) * xyz[1] + (-0.4906146F) * xyz[2],
            (-0.9787684F) * xyz[0] + 1.9161415F * xyz[1] + 0.0334540F * xyz[2],
            0.0719453F * xyz[0] + (-0.2289914F) * xyz[1] + 1.4052427F * xyz[2]};
}

[[nodiscard]] FrozenD50Triplet frozen_xyz_d50_to_lab(const FrozenD50Triplet &xyz) noexcept
{
    constexpr FrozenD50Triplet d50_inverse{1.0F / 0.9642F, 1.0F, 1.0F / 0.8249F};
    constexpr float epsilon = 216.0F / 24389.0F;
    constexpr float kappa = 24389.0F / 27.0F;
    FrozenD50Triplet transformed{};
    for (std::size_t channel = 0U; channel < transformed.size(); ++channel)
    {
        const float normalized = xyz[channel] * d50_inverse[channel];
        transformed[channel] =
            normalized > epsilon ? std::cbrt(normalized) : (kappa * normalized + 16.0F) / 116.0F;
    }
    return {116.0F * transformed[1] - 16.0F, 500.0F * (transformed[0] - transformed[1]),
            -200.0F * (transformed[2] - transformed[1])};
}

[[nodiscard]] FrozenD50Triplet frozen_lab_to_xyz_d50(const FrozenD50Triplet &lab) noexcept
{
    constexpr FrozenD50Triplet d50{0.9642F, 1.0F, 0.8249F};
    constexpr FrozenD50Triplet offset{0.0F, 16.0F, 0.0F};
    constexpr FrozenD50Triplet coefficient{1.0F / 500.0F, 1.0F / 116.0F, -1.0F / 200.0F};
    constexpr FrozenD50Triplet add_coefficient{1.0F, 0.0F, 1.0F};
    constexpr float epsilon = 0.20689655172413796F;
    constexpr float kappa = 24389.0F / 27.0F;
    const FrozenD50Triplet reordered{lab[1], lab[0], lab[2]};
    FrozenD50Triplet scaled{};
    for (std::size_t channel = 0U; channel < scaled.size(); ++channel)
    {
        scaled[channel] = (reordered[channel] + offset[channel]) * coefficient[channel];
    }
    FrozenD50Triplet xyz{};
    for (std::size_t channel = 0U; channel < xyz.size(); ++channel)
    {
        const float value = scaled[channel] + scaled[1] * add_coefficient[channel];
        const float inverse =
            value > epsilon ? value * value * value : (116.0F * value - 16.0F) / kappa;
        xyz[channel] = d50[channel] * inverse;
    }
    return xyz;
}

// Independent scalar oracle transcribed from frozen colorcontrast.c v2. It
// intentionally calls neither the production Color Contrast helper nor the
// production D50 bridge, preserving the source multiply/add and CLAMPS order.
[[nodiscard]] FrozenD50Triplet frozen_color_contrast_lab(const ColorContrastParams &params,
                                                         const FrozenD50Triplet &lab) noexcept
{
    const float a_steepness = static_cast<float>(params.a_steepness);
    const float a_offset = static_cast<float>(params.a_offset);
    const float b_steepness = static_cast<float>(params.b_steepness);
    const float b_offset = static_cast<float>(params.b_offset);
    float a = lab[1] * a_steepness + a_offset;
    float b = lab[2] * b_steepness + b_offset;
    if (!params.unbound)
    {
        a = a > -128.0F ? (a < 128.0F ? a : 128.0F) : -128.0F;
        b = b > -128.0F ? (b < 128.0F ? b : 128.0F) : -128.0F;
    }
    return {lab[0], a, b};
}

[[nodiscard]] FrozenD50Triplet frozen_color_contrast_rgb(const ColorContrastParams &params,
                                                         const FrozenD50Triplet &rgb) noexcept
{
    const auto lab = frozen_xyz_d50_to_lab(frozen_linear_rec709_to_xyz_d50(rgb));
    return frozen_xyz_d50_to_linear_rec709(
        frozen_lab_to_xyz_d50(frozen_color_contrast_lab(params, lab)));
}

[[nodiscard]] std::array<std::uint32_t, 3>
d50_triplet_bits(const FrozenD50Triplet &triplet) noexcept
{
    return {std::bit_cast<std::uint32_t>(triplet[0]), std::bit_cast<std::uint32_t>(triplet[1]),
            std::bit_cast<std::uint32_t>(triplet[2])};
}

void expect_frozen_d50_bits(const FrozenD50Triplet &actual, const FrozenD50Triplet &oracle,
                            const std::array<std::uint32_t, 3> &golden)
{
    EXPECT_EQ(d50_triplet_bits(oracle), golden);
    EXPECT_EQ(d50_triplet_bits(actual), golden);
}

void expect_frozen_d50_cbrt_reference(const FrozenD50Triplet &actual,
                                      const FrozenD50Triplet &oracle,
                                      const std::array<std::uint32_t, 3> &reference)
{
    EXPECT_EQ(d50_triplet_bits(actual), d50_triplet_bits(oracle));
    for (std::size_t channel = 0U; channel < reference.size(); ++channel)
    {
        // cbrtf is platform libm code. Preserve exact host-local source-order
        // agreement while retaining a tight, recorded cross-platform envelope.
        EXPECT_NEAR(actual[channel], std::bit_cast<float>(reference[channel]),
                    kPlatformLibmReferenceTolerance);
    }
}

[[nodiscard]] float frozen_dt_ucs_matrix_row(const float coefficient0, const float value0,
                                             const float coefficient1, const float value1,
                                             const float coefficient2, const float value2) noexcept
{
    // Keep every source multiplication and addition at its declared float
    // stage. In particular, this is neither a dot-product abstraction nor FMA.
    const float product0 = coefficient0 * value0;
    const float product1 = coefficient1 * value1;
    const float first_sum = product0 + product1;
    const float product2 = coefficient2 * value2;
    return first_sum + product2;
}

[[nodiscard]] FrozenD50Triplet frozen_dt_ucs_xyz_d50_to_d65(const FrozenD50Triplet xyz) noexcept
{
    return {frozen_dt_ucs_matrix_row(0.989466254F, xyz[0], -0.0400304626F, xyz[1], 0.0440530317F,
                                     xyz[2]),
            frozen_dt_ucs_matrix_row(-0.00540518733F, xyz[0], 1.00666069F, xyz[1], -0.00175551955F,
                                     xyz[2]),
            frozen_dt_ucs_matrix_row(-0.000403920992F, xyz[0], 0.0150768030F, xyz[1], 1.30210211F,
                                     xyz[2])};
}

[[nodiscard]] FrozenD50Triplet frozen_dt_ucs_xyz_d65_to_d50(const FrozenD50Triplet xyz) noexcept
{
    return {frozen_dt_ucs_matrix_row(1.01085433F, xyz[0], 0.0407086103F, xyz[1], -0.0341445825F,
                                     xyz[2]),
            frozen_dt_ucs_matrix_row(0.00542814201F, xyz[0], 0.993581926F, xyz[1], 0.00115592039F,
                                     xyz[2]),
            frozen_dt_ucs_matrix_row(0.000250722468F, xyz[0], -0.0114918759F, xyz[1], 0.767964947F,
                                     xyz[2])};
}

[[nodiscard]] float frozen_dt_ucs_nonnegative(const float value) noexcept
{
    // Frozen dt_vector_max(value, 0) selects its second operand for NaN and
    // signed zero as well as for negative values.
    return value > 0.0F ? value : 0.0F;
}

[[nodiscard]] FrozenD50Triplet frozen_dt_ucs_xyz_d65_to_xyy(const FrozenD50Triplet xyz) noexcept
{
    const FrozenD50Triplet nonnegative{frozen_dt_ucs_nonnegative(xyz[0]),
                                       frozen_dt_ucs_nonnegative(xyz[1]),
                                       frozen_dt_ucs_nonnegative(xyz[2])};
    const float sum = nonnegative[0] + nonnegative[1] + nonnegative[2];
    return {sum > 0.0F ? nonnegative[0] / sum : static_cast<float>(0.31271),
            sum > 0.0F ? nonnegative[1] / sum : static_cast<float>(0.32902), nonnegative[1]};
}

[[nodiscard]] FrozenD50Triplet frozen_dt_ucs_xyy_to_xyz_d65(const FrozenD50Triplet xyy) noexcept
{
    const bool zero_denominator = xyy[1] == 0.0F;
    return {zero_denominator ? 0.0F : xyy[2] * xyy[0] / xyy[1], zero_denominator ? 0.0F : xyy[2],
            zero_denominator ? 0.0F : xyy[2] * (1.0F - xyy[0] - xyy[1]) / xyy[1]};
}

[[nodiscard]] float frozen_dt_ucs_signed_denominator(const float value) noexcept
{
    constexpr float minimum = std::numeric_limits<float>::min();
    return value >= 0.0F ? (minimum > value ? minimum : value) :
                           (-minimum < value ? -minimum : value);
}

[[nodiscard]] float frozen_dt_ucs_y_to_lightness(const float luminance) noexcept
{
    const float powered = std::pow(luminance, 0.631651345306265F);
    const float numerator = 2.098883786377F * powered;
    const float denominator = powered + 1.12426773749357F;
    return numerator / denominator;
}

[[nodiscard]] float frozen_dt_ucs_lightness_to_y(const float lightness) noexcept
{
    const float numerator = 1.12426773749357F * lightness;
    const float denominator = 2.098883786377F - lightness;
    const float ratio = numerator / denominator;
    return std::pow(ratio, 1.5831518565279648F);
}

struct FrozenDtUcsJchOracle
{
    FrozenD50Triplet jch{};
    float lightness = 0.0F;
    float squared_colorfulness = 0.0F;
    float source_order_chroma = 0.0F;
    float reassociated_chroma = 0.0F;
};

[[nodiscard]] FrozenDtUcsJchOracle
frozen_dt_ucs_xyy_to_jch_oracle(const FrozenD50Triplet xyy, const float white_lightness) noexcept
{
    constexpr FrozenD50Triplet x_factors{-0.783941002840055F, 0.745273540913283F,
                                         0.318707282433486F};
    constexpr FrozenD50Triplet y_factors{0.277512987809202F, -0.205375866083878F,
                                         2.16743692732158F};
    constexpr FrozenD50Triplet offsets{0.153836578598858F, -0.165478376301988F, 0.291320554395942F};
    FrozenD50Triplet uvd{};
    for (std::size_t channel = 0U; channel < uvd.size(); ++channel)
    {
        const float x_product = x_factors[channel] * xyy[0];
        const float y_product = y_factors[channel] * xyy[1];
        uvd[channel] = (x_product + y_product) + offsets[channel];
    }
    const float divisor = frozen_dt_ucs_signed_denominator(uvd[2]);
    uvd[0] /= divisor;
    uvd[1] /= divisor;

    constexpr std::array<float, 2> factors{1.39656225667F, 1.4513954287F};
    constexpr std::array<float, 2> half_values{1.49217352929F, 1.52488637914F};
    std::array<float, 2> uv_star{};
    for (std::size_t channel = 0U; channel < uv_star.size(); ++channel)
    {
        const float numerator = factors[channel] * uvd[channel];
        const float denominator = std::fabs(uvd[channel]) + half_values[channel];
        uv_star[channel] = numerator / denominator;
    }
    const float u_prime_first = -1.124983854323892F * uv_star[0];
    const float u_prime_second = 0.980483721769325F * uv_star[1];
    const float u_prime = u_prime_first - u_prime_second;
    const float v_prime_first = 1.86323315098672F * uv_star[0];
    const float v_prime_second = 1.971853092390862F * uv_star[1];
    const float v_prime = v_prime_first + v_prime_second;
    const float squared_u_prime = u_prime * u_prime;
    const float squared_v_prime = v_prime * v_prime;
    const float squared_colorfulness = squared_u_prime + squared_v_prime;
    const float lightness = frozen_dt_ucs_y_to_lightness(xyy[2]);
    const float lightness_power = std::pow(lightness, 0.6523997524738018F);
    const float colorfulness_power = std::pow(squared_colorfulness, 0.6007557017508491F);
    const float coefficient_times_lightness = 15.932993652962535F * lightness_power;
    const float product = coefficient_times_lightness * colorfulness_power;
    const float chroma = product / white_lightness;
    const float reassociated_chroma =
        coefficient_times_lightness * (colorfulness_power / white_lightness);
    return {{lightness / white_lightness, chroma, std::atan2(v_prime, u_prime)},
            lightness,
            squared_colorfulness,
            chroma,
            reassociated_chroma};
}

[[nodiscard]] FrozenD50Triplet frozen_dt_ucs_jch_to_xyy(const FrozenD50Triplet jch,
                                                        const float white_lightness) noexcept
{
    constexpr float lightness_upper_limit = 2.09885F;
    const float raw_lightness = jch[0] * white_lightness;
    const float lightness =
        raw_lightness >= 0.0F ?
            (raw_lightness <= lightness_upper_limit ? raw_lightness : lightness_upper_limit) :
            0.0F;
    float colorfulness = 0.0F;
    if (lightness != 0.0F)
    {
        const float powered_lightness = std::pow(lightness, 0.6523997524738018F);
        const float denominator = 15.932993652962535F * powered_lightness;
        const float ratio = jch[1] * white_lightness / denominator;
        colorfulness = std::pow(ratio, 0.8322850678616855F);
    }
    const float u_prime = colorfulness * std::cos(jch[2]);
    const float v_prime = colorfulness * std::sin(jch[2]);
    const float u_star_first = -5.037522385190711F * u_prime;
    const float u_star_second = 2.504856328185843F * v_prime;
    const float v_star_first = 4.760029407436461F * u_prime;
    const float v_star_second = 2.874012963239247F * v_prime;
    const std::array<float, 2> uv_star{u_star_first - u_star_second, v_star_first + v_star_second};
    constexpr std::array<float, 2> factors{1.39656225667F, 1.4513954287F};
    constexpr std::array<float, 2> half_values{1.49217352929F, 1.52488637914F};
    std::array<float, 2> uv{};
    for (std::size_t channel = 0U; channel < uv.size(); ++channel)
    {
        const float numerator = -half_values[channel] * uv_star[channel];
        const float denominator = std::fabs(uv_star[channel]) - factors[channel];
        uv[channel] = numerator / denominator;
    }

    constexpr FrozenD50Triplet u_factors{0.167171472114775F, -0.150959086409163F,
                                         0.940254742367256F};
    constexpr FrozenD50Triplet v_factors{0.141299802443708F, -0.155185060382272F,
                                         1.000000000000000F};
    constexpr FrozenD50Triplet offsets{-0.00801531300850582F, -0.00843312433578007F,
                                       -0.0256325967652889F};
    FrozenD50Triplet xyd{};
    for (std::size_t channel = 0U; channel < xyd.size(); ++channel)
    {
        const float u_product = u_factors[channel] * uv[0];
        const float v_product = v_factors[channel] * uv[1];
        xyd[channel] = (u_product + v_product) + offsets[channel];
    }
    const float divisor = frozen_dt_ucs_signed_denominator(xyd[2]);
    return {xyd[0] / divisor, xyd[1] / divisor, frozen_dt_ucs_lightness_to_y(lightness)};
}

[[nodiscard]] FrozenD50Triplet frozen_dt_ucs_xyz_d50_to_jch(const FrozenD50Triplet xyz,
                                                            const float white_lightness) noexcept
{
    return frozen_dt_ucs_xyy_to_jch_oracle(
               frozen_dt_ucs_xyz_d65_to_xyy(frozen_dt_ucs_xyz_d50_to_d65(xyz)), white_lightness)
        .jch;
}

[[nodiscard]] FrozenD50Triplet frozen_dt_ucs_jch_to_xyz_d50(const FrozenD50Triplet jch,
                                                            const float white_lightness) noexcept
{
    return frozen_dt_ucs_xyz_d65_to_d50(
        frozen_dt_ucs_xyy_to_xyz_d65(frozen_dt_ucs_jch_to_xyy(jch, white_lightness)));
}

void expect_dt_ucs_local_oracle(const FrozenD50Triplet actual, const FrozenD50Triplet oracle)
{
    // powf/atan2f/cosf/sinf are platform libm boundaries, so this is a
    // same-platform bit comparison to an independent source-order oracle, not
    // a claim that their decimal results are cross-platform bit-stable.
    EXPECT_EQ(d50_triplet_bits(actual), d50_triplet_bits(oracle));
}

using FrozenHarmonyHueTable = std::array<float, harmony_geometry::kHueTableSteps>;

struct FrozenHarmonyHueTables
{
    FrozenHarmonyHueTable ucs_to_ryb{};
    FrozenHarmonyHueTable ryb_to_ucs{};
};

struct FrozenHarmonyNodes
{
    std::array<float, harmony_geometry::kMaxHarmonyNodes> hues{};
    std::size_t count = 0U;
};

struct FrozenHarmonyAttraction
{
    float shift = 0.0F;
    std::size_t winning_index = 0U;
    float weight = 0.0F;
};

[[nodiscard]] float frozen_harmony_clamp01(const float value) noexcept
{
    return value >= 0.0F ? (value <= 1.0F ? value : 1.0F) : 0.0F;
}

[[nodiscard]] FrozenD50Triplet
frozen_harmony_xyz_d65_to_linear_rec709(const FrozenD50Triplet xyz,
                                        const bool transpose_discriminator = false) noexcept
{
    if (transpose_discriminator)
    {
        return {
            frozen_dt_ucs_matrix_row(3.2404542F, xyz[0], -0.9692660F, xyz[1], 0.0556434F, xyz[2]),
            frozen_dt_ucs_matrix_row(-1.5371385F, xyz[0], 1.8760108F, xyz[1], -0.2040259F, xyz[2]),
            frozen_dt_ucs_matrix_row(-0.4985314F, xyz[0], 0.0415560F, xyz[1], 1.0572252F, xyz[2])};
    }
    return {frozen_dt_ucs_matrix_row(3.2404542F, xyz[0], -1.5371385F, xyz[1], -0.4985314F, xyz[2]),
            frozen_dt_ucs_matrix_row(-0.9692660F, xyz[0], 1.8760108F, xyz[1], 0.0415560F, xyz[2]),
            frozen_dt_ucs_matrix_row(0.0556434F, xyz[0], -0.2040259F, xyz[1], 1.0572252F, xyz[2])};
}

[[nodiscard]] float frozen_harmony_srgb_to_linear(const float srgb,
                                                  const float threshold = 0.04045F) noexcept
{
    const float toe = srgb / 12.92F;
    const float offset_srgb = srgb + 0.055F;
    const float scaled_srgb = offset_srgb / 1.055F;
    const float linearized = std::pow(scaled_srgb, 2.4F);
    return srgb <= threshold ? toe : linearized;
}

[[nodiscard]] FrozenD50Triplet
frozen_harmony_jch_to_srgb(const FrozenD50Triplet jch, const float white_lightness,
                           const bool transpose_discriminator = false) noexcept
{
    const auto xyy = frozen_dt_ucs_jch_to_xyy(jch, white_lightness);
    const auto xyz_d65 = frozen_dt_ucs_xyy_to_xyz_d65(xyy);
    const auto linear = frozen_harmony_xyz_d65_to_linear_rec709(xyz_d65, transpose_discriminator);
    FrozenD50Triplet srgb{};
    for (std::size_t channel = 0U; channel < srgb.size(); ++channel)
    {
        if (linear[channel] <= 0.0031308F)
        {
            srgb[channel] = 12.92F * linear[channel];
        }
        else
        {
            const float curved = std::pow(linear[channel], 1.0F / 2.4F);
            const float scaled = 1.055F * curved;
            srgb[channel] = scaled - 0.055F;
        }
    }
    return srgb;
}

[[nodiscard]] float frozen_harmony_max_chroma(const float hue, const int iterations = 16,
                                              const bool transpose_discriminator = false) noexcept
{
    const float white_lightness = frozen_dt_ucs_y_to_lightness(1.0F);
    const float scaled_hue = hue * 6.28318530717958647693F;
    const float angle = scaled_hue - 3.14159265358979323846F;
    constexpr float lightness = 0.65F;
    float lower = 0.0F;
    float upper = 2.0F;
    for (int iteration = 0; iteration < iterations; ++iteration)
    {
        const float middle = (lower + upper) * 0.5F;
        const auto srgb = frozen_harmony_jch_to_srgb({lightness, middle, angle}, white_lightness,
                                                     transpose_discriminator);
        const bool inside = srgb[0] >= 0.0F && srgb[1] >= 0.0F && srgb[2] >= 0.0F &&
                            srgb[0] <= 1.0F && srgb[1] <= 1.0F && srgb[2] <= 1.0F;
        if (inside)
        {
            lower = middle;
        }
        else
        {
            upper = middle;
        }
    }
    return lower;
}

[[nodiscard]] float frozen_harmony_rgb_hue_to_ryb(const float hue,
                                                  const float middle_knot = 0.472217F) noexcept
{
    constexpr std::array<float, 7> input_knots{0.0F,        1.0F / 6.0F, 2.0F / 6.0F, 3.0F / 6.0F,
                                               4.0F / 6.0F, 5.0F / 6.0F, 1.0F};
    const std::array<float, 7> output_knots{0.0F,      1.0F / 3.0F, middle_knot, 0.611105F,
                                            0.715271F, 5.0F / 6.0F, 1.0F};
    const float wrapped = hue - std::floor(hue);
    std::size_t index = 0U;
    while (index < 5U && wrapped >= input_knots[index + 1U])
    {
        ++index;
    }
    const float numerator = wrapped - input_knots[index];
    const float denominator = input_knots[index + 1U] - input_knots[index];
    const float fraction = numerator / denominator;
    const float output_delta = output_knots[index + 1U] - output_knots[index];
    const float scaled_delta = fraction * output_delta;
    return output_knots[index] + scaled_delta;
}

[[nodiscard]] float frozen_harmony_ucs_to_ryb(const float hue, const int iterations = 16,
                                              const bool transpose_discriminator = false,
                                              const float middle_knot = 0.472217F) noexcept
{
    const float white_lightness = frozen_dt_ucs_y_to_lightness(1.0F);
    const float scaled_hue = hue * 6.28318530717958647693F;
    const float angle = scaled_hue - 3.14159265358979323846F;
    const float chroma =
        frozen_harmony_max_chroma(hue, iterations, transpose_discriminator) * 0.85F;
    auto srgb = frozen_harmony_jch_to_srgb({0.65F, chroma, angle}, white_lightness,
                                           transpose_discriminator);
    for (float &channel : srgb)
    {
        channel = frozen_harmony_clamp01(channel);
        channel = frozen_harmony_srgb_to_linear(channel);
    }
    const float minimum = std::fmin(std::fmin(srgb[0], srgb[1]), srgb[2]);
    const float maximum = std::fmax(std::fmax(srgb[0], srgb[1]), srgb[2]);
    const float delta = maximum - minimum;
    float rgb_hue = 0.0F;
    if (std::fabs(maximum) > 1.0e-6F && std::fabs(delta) > 1.0e-6F)
    {
        if (srgb[0] == maximum)
        {
            rgb_hue = (srgb[1] - srgb[2]) / delta;
        }
        else if (srgb[1] == maximum)
        {
            rgb_hue = 2.0F + (srgb[2] - srgb[0]) / delta;
        }
        else
        {
            rgb_hue = 4.0F + (srgb[0] - srgb[1]) / delta;
        }
        rgb_hue /= 6.0F;
        rgb_hue -= std::floor(rgb_hue);
    }
    return frozen_harmony_rgb_hue_to_ryb(rgb_hue, middle_knot);
}

[[nodiscard]] FrozenHarmonyHueTable
frozen_harmony_forward_table(const int iterations = 16, const bool transpose_discriminator = false,
                             const float middle_knot = 0.472217F) noexcept
{
    FrozenHarmonyHueTable table{};
    for (std::size_t index = 0U; index < table.size(); ++index)
    {
        table[index] =
            frozen_harmony_ucs_to_ryb(static_cast<float>(index) / static_cast<float>(table.size()),
                                      iterations, transpose_discriminator, middle_knot);
    }
    return table;
}

[[nodiscard]] FrozenHarmonyHueTable
frozen_harmony_inverse_table(const FrozenHarmonyHueTable &forward) noexcept
{
    FrozenHarmonyHueTable inverse{};
    for (std::size_t target_index = 0U; target_index < inverse.size(); ++target_index)
    {
        const float target = static_cast<float>(target_index) / static_cast<float>(inverse.size());
        float best_distance = 1.0F;
        float best_ucs = 0.0F;
        for (std::size_t index = 0U; index < forward.size(); ++index)
        {
            float distance = std::fabs(forward[index] - target);
            if (distance > 0.5F)
            {
                distance = 1.0F - distance;
            }
            if (distance < best_distance)
            {
                best_distance = distance;
                best_ucs = static_cast<float>(index) / static_cast<float>(forward.size());
            }
        }
        inverse[target_index] = best_ucs;
    }
    return inverse;
}

[[nodiscard]] FrozenHarmonyHueTables frozen_harmony_tables() noexcept
{
    FrozenHarmonyHueTables tables;
    tables.ucs_to_ryb = frozen_harmony_forward_table();
    tables.ryb_to_ucs = frozen_harmony_inverse_table(tables.ucs_to_ryb);
    return tables;
}

[[nodiscard]] float frozen_harmony_lerp(float first, float second, const float fraction) noexcept
{
    if (second - first > 0.5F)
    {
        second -= 1.0F;
    }
    else if (first - second > 0.5F)
    {
        first -= 1.0F;
    }
    const float difference = second - first;
    const float scaled_difference = fraction * difference;
    float result = first + scaled_difference;
    if (result < 0.0F)
    {
        result += 1.0F;
    }
    return result;
}

[[nodiscard]] float frozen_harmony_lookup(const FrozenHarmonyHueTable &table,
                                          const float hue) noexcept
{
    const float position = hue * static_cast<float>(table.size());
    const int integral_position = static_cast<int>(position);
    const std::size_t first = static_cast<std::size_t>(integral_position) % table.size();
    const std::size_t second = (first + 1U) % table.size();
    return frozen_harmony_lerp(table[first], table[second],
                               position - static_cast<float>(integral_position));
}

[[nodiscard]] FrozenHarmonyNodes
frozen_predefined_harmony_nodes(const harmony_geometry::StandardRule rule, const float anchor_hue,
                                const FrozenHarmonyHueTables &tables) noexcept
{
    constexpr std::array<std::size_t, 9> counts{1U, 3U, 4U, 2U, 3U, 2U, 3U, 4U, 4U};
    constexpr std::array<std::array<float, 4>, 9> offsets{
        std::array<float, 4>{0.0F / 12.0F, 0.0F, 0.0F, 0.0F},
        std::array<float, 4>{-1.0F / 12.0F, 0.0F / 12.0F, 1.0F / 12.0F, 0.0F},
        std::array<float, 4>{-1.0F / 12.0F, 0.0F / 12.0F, 1.0F / 12.0F, 6.0F / 12.0F},
        std::array<float, 4>{0.0F / 12.0F, 6.0F / 12.0F, 0.0F, 0.0F},
        std::array<float, 4>{0.0F / 12.0F, 5.0F / 12.0F, 7.0F / 12.0F, 0.0F},
        std::array<float, 4>{-1.0F / 12.0F, 1.0F / 12.0F, 0.0F, 0.0F},
        std::array<float, 4>{0.0F / 12.0F, 4.0F / 12.0F, 8.0F / 12.0F, 0.0F},
        std::array<float, 4>{-1.0F / 12.0F, 1.0F / 12.0F, 5.0F / 12.0F, 7.0F / 12.0F},
        std::array<float, 4>{0.0F / 12.0F, 3.0F / 12.0F, 6.0F / 12.0F, 9.0F / 12.0F},
    };
    const std::size_t rule_index = static_cast<std::size_t>(rule);
    FrozenHarmonyNodes nodes;
    nodes.count = counts[rule_index];
    const float mapped_anchor = frozen_harmony_lookup(tables.ucs_to_ryb, anchor_hue);
    const int rotation = static_cast<int>(std::round(mapped_anchor * 360.0F)) % 360;
    const float sector_anchor = static_cast<float>(rotation) / 360.0F;
    for (std::size_t index = 0U; index < nodes.count; ++index)
    {
        float angle = offsets[rule_index][index] + sector_anchor;
        angle -= std::floor(angle);
        nodes.hues[index] = frozen_harmony_lookup(tables.ryb_to_ucs, angle);
    }
    return nodes;
}

[[nodiscard]] FrozenHarmonyAttraction frozen_harmony_attraction(const float pixel_hue,
                                                                const std::span<const float> nodes,
                                                                const float pull_width) noexcept
{
    const float sigma = pull_width * 0.5F / static_cast<float>(nodes.size());
    const float inverse_two_sigma_squared = 1.0F / (2.0F * sigma * sigma);
    FrozenHarmonyAttraction result;
    for (std::size_t index = 0U; index < nodes.size(); ++index)
    {
        float distance = std::fabs(pixel_hue - nodes[index]);
        if (distance > 0.5F)
        {
            distance = 1.0F - distance;
        }
        const float weight = std::exp(-distance * distance * inverse_two_sigma_squared);
        float difference = nodes[index] - pixel_hue;
        if (difference > 0.5F)
        {
            difference -= 1.0F;
        }
        else if (difference < -0.5F)
        {
            difference += 1.0F;
        }
        if (weight > result.weight)
        {
            result.weight = weight;
            result.winning_index = index;
            result.shift = difference;
        }
    }
    result.shift *= result.weight;
    return result;
}

[[nodiscard]] std::uint64_t frozen_harmony_table_hash(const FrozenHarmonyHueTable &table) noexcept
{
    std::uint64_t hash = 14695981039346656037ULL;
    for (const float value : table)
    {
        const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
        for (const unsigned int shift : {0U, 8U, 16U, 24U})
        {
            hash ^= static_cast<std::uint8_t>(bits >> shift);
            hash *= 1099511628211ULL;
        }
    }
    return hash;
}

[[nodiscard]] std::array<float, 9>
frozen_color_harmonizer_inverse(const std::array<float, 9> &matrix) noexcept
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
    const volatile double ei = e * i;
    const volatile double fh = f * h;
    const volatile double di = d * i;
    const volatile double fg = f * g;
    const volatile double dh = d * h;
    const volatile double eg = e * g;
    const double minor0 = ei - fh;
    const double minor1 = di - fg;
    const double minor2 = dh - eg;
    const volatile double first = a * minor0;
    const volatile double second = b * minor1;
    const volatile double third = c * minor2;
    const double determinant = (first - second) + third;
    const double inverse = 1.0 / determinant;
    const auto difference =
        [](const double lhs0, const double lhs1, const double rhs0, const double rhs1)
    {
        const volatile double lhs = lhs0 * lhs1;
        const volatile double rhs = rhs0 * rhs1;
        return lhs - rhs;
    };
    return {static_cast<float>(minor0 * inverse),
            static_cast<float>(difference(c, h, b, i) * inverse),
            static_cast<float>(difference(b, f, c, e) * inverse),
            static_cast<float>(difference(f, g, d, i) * inverse),
            static_cast<float>(difference(a, i, c, g) * inverse),
            static_cast<float>(difference(c, d, a, f) * inverse),
            static_cast<float>(difference(d, h, e, g) * inverse),
            static_cast<float>(difference(b, g, a, h) * inverse),
            static_cast<float>(difference(a, e, b, d) * inverse)};
}

[[nodiscard]] FrozenD50Triplet frozen_color_harmonizer_matrix(const std::array<float, 9> &matrix,
                                                              const FrozenD50Triplet value) noexcept
{
    const auto row = [](const float coefficient0, const float value0, const float coefficient1,
                        const float value1, const float coefficient2, const float value2)
    {
        const volatile float product0 = coefficient0 * value0;
        const volatile float product1 = coefficient1 * value1;
        const volatile float first_sum = product0 + product1;
        const volatile float product2 = coefficient2 * value2;
        return first_sum + product2;
    };
    return {row(matrix[0], value[0], matrix[1], value[1], matrix[2], value[2]),
            row(matrix[3], value[0], matrix[4], value[1], matrix[5], value[2]),
            row(matrix[6], value[0], matrix[7], value[1], matrix[8], value[2])};
}

[[nodiscard]] FrozenHarmonyNodes
frozen_color_harmonizer_nodes(const ColorHarmonizerParams &params,
                              const FrozenHarmonyHueTables &tables) noexcept
{
    FrozenHarmonyNodes nodes;
    if (params.rule == ColorHarmonizerRule::kCustom)
    {
        nodes.count = static_cast<std::size_t>(params.num_custom_nodes);
        for (std::size_t index = 0U; index < nodes.count; ++index)
        {
            nodes.hues[index] = static_cast<float>(params.custom_hue[index]);
        }
        return nodes;
    }
    return frozen_predefined_harmony_nodes(static_cast<harmony_geometry::StandardRule>(params.rule),
                                           static_cast<float>(params.anchor_hue), tables);
}

// Independent scalar composition of the frozen smoothing-zero process. The
// conversion, geometry, matrix, and correction stages call only the source
// transcriptions above, never apply_color_harmonizer or a production bridge.
[[nodiscard]] FrozenD50Triplet frozen_color_harmonizer_rgb(
    const ColorHarmonizerParams &params, const FrozenD50Triplet input,
    const std::array<float, 9> &working_to_xyz_d50, const FrozenHarmonyHueTables &tables,
    const bool skip_negative_clip = false, const bool linear_neutral_protection = false) noexcept
{
    const auto inverse = frozen_color_harmonizer_inverse(working_to_xyz_d50);
    const FrozenD50Triplet nonnegative{skip_negative_clip ? input[0] : std::fmax(input[0], 0.0F),
                                       skip_negative_clip ? input[1] : std::fmax(input[1], 0.0F),
                                       skip_negative_clip ? input[2] : std::fmax(input[2], 0.0F)};
    const float white_lightness = frozen_dt_ucs_y_to_lightness(1.0F);
    auto jch = frozen_dt_ucs_xyz_d50_to_jch(
        frozen_color_harmonizer_matrix(working_to_xyz_d50, nonnegative), white_lightness);
    constexpr float pi = 3.14159265358979323846F;
    constexpr float two_pi = 6.28318530717958647693F;
    const float hue = (jch[2] + pi) / two_pi;
    const float chroma = jch[1];
    const auto nodes = frozen_color_harmonizer_nodes(params, tables);
    const auto attraction =
        frozen_harmony_attraction(hue, std::span<const float>(nodes.hues.data(), nodes.count),
                                  static_cast<float>(params.pull_width));
    const volatile float saturation_delta =
        (static_cast<float>(params.node_saturation[attraction.winning_index]) - 1.0F) *
        attraction.weight;
    const float neutral = static_cast<float>(params.neutral_protection);
    const volatile float neutral_squared = neutral * neutral;
    const volatile float neutral_cubed = neutral_squared * neutral;
    const volatile float cutoff = (linear_neutral_protection ? neutral : neutral_cubed) * 0.03F;
    const volatile float chroma_plus_cutoff = chroma + cutoff;
    const volatile float denominator = chroma_plus_cutoff + 1.0e-5F;
    const volatile float chroma_weight = chroma / denominator;
    const volatile float strength_shift =
        attraction.shift * static_cast<float>(params.pull_strength);
    const volatile float weighted_shift = strength_shift * chroma_weight;
    const volatile float shifted_hue = hue + weighted_shift;
    float corrected_hue = std::fmod(shifted_hue, 1.0F);
    if (corrected_hue < 0.0F)
    {
        corrected_hue += 1.0F;
    }
    const volatile float scaled_hue = corrected_hue * two_pi;
    jch[2] = scaled_hue - pi;
    const volatile float weighted_saturation = saturation_delta * chroma_weight;
    const volatile float saturation_scale = 1.0F + weighted_saturation;
    const volatile float corrected_chroma = chroma * saturation_scale;
    jch[1] = std::fmax(corrected_chroma, 0.0F);
    return frozen_color_harmonizer_matrix(inverse,
                                          frozen_dt_ucs_jch_to_xyz_d50(jch, white_lightness));
}

[[nodiscard]] ColorHarmonizerParams frozen_color_harmonizer_0176_record13() noexcept
{
    ColorHarmonizerParams params;
    params.rule = ColorHarmonizerRule::kSplitComplementary;
    params.anchor_hue = 0.55000001192092896;
    params.pull_strength = 0.81999999284744263;
    params.pull_width = 1.8400000333786011;
    params.node_saturation = {1.2599999904632568, 0.18000000715255737, 1.5199999809265137, 1.0};
    return params;
}

[[nodiscard]] WorkingImage color_harmonizer_working_fixture()
{
    WorkingImage input;
    input.width = 4U;
    input.height = 1U;
    input.rgb = {0.03F, 0.18F, 0.72F, 0.91F, 0.42F, 0.07F, -0.25F, 0.5F, 1.7F, 0.0F, 0.0F, 0.0F};
    input.color_profile.kind = ColorProfileKind::kIcc;
    input.color_profile.model = ColorModel::kRgb;
    input.color_profile.identifier = std::string(kInputProfileLinearRec709);
    input.color_profile.icc_bytes = {1U, 2U, 3U, 4U};
    input.color_profile.matrix_to_xyz_d50 = {0.4360747F, 0.3850649F, 0.1430804F,
                                             0.2225045F, 0.7168786F, 0.0606169F,
                                             0.0139322F, 0.0971045F, 0.7141733F};
    input.color_profile.has_matrix = true;
    auto analysis = std::make_shared<ExposureAnalysisContext>();
    analysis->raw_pixel_count = 4U;
    input.exposure_analysis = std::move(analysis);
    return input;
}

struct FrozenRecursiveGaussianCoefficients
{
    float a0 = 0.0F;
    float a1 = 0.0F;
    float a2 = 0.0F;
    float a3 = 0.0F;
    float b1 = 0.0F;
    float b2 = 0.0F;
    float coefp = 0.0F;
    float coefn = 0.0F;
};

[[nodiscard]] float frozen_gaussian_clamp(const float value) noexcept
{
    return value >= -1.0e9F ? (value <= 1.0e9F ? value : 1.0e9F) : -1.0e9F;
}

[[nodiscard]] std::vector<float> frozen_gaussian_zero_2c(std::vector<float> signal,
                                                         const std::uint32_t width,
                                                         const std::uint32_t height,
                                                         const float sigma)
{
    const float alpha = 1.695F / sigma;
    const float ema = std::exp(-alpha);
    const float ema2 = std::exp(-2.0F * alpha);
    FrozenRecursiveGaussianCoefficients coefficients;
    coefficients.b1 = -2.0F * ema;
    coefficients.b2 = ema2;
    const float k = (1.0F - ema) * (1.0F - ema) / (1.0F + (2.0F * alpha * ema) - ema2);
    coefficients.a0 = k;
    coefficients.a1 = k * (alpha - 1.0F) * ema;
    coefficients.a2 = k * (alpha + 1.0F) * ema;
    coefficients.a3 = -k * ema2;
    coefficients.coefp =
        (coefficients.a0 + coefficients.a1) / (1.0F + coefficients.b1 + coefficients.b2);
    coefficients.coefn =
        (coefficients.a2 + coefficients.a3) / (1.0F + coefficients.b1 + coefficients.b2);
    std::vector<float> scratch(signal.size());
    for (std::uint32_t column = 0U; column < width; ++column)
    {
        float xp[2]{};
        float yb[2]{};
        float yp[2]{};
        for (std::size_t channel = 0U; channel < 2U; ++channel)
        {
            xp[channel] =
                frozen_gaussian_clamp(signal[static_cast<std::size_t>(column) * 2U + channel]);
            yb[channel] = xp[channel] * coefficients.coefp;
            yp[channel] = yb[channel];
        }
        float xc[2]{};
        float yc[2]{};
        float xn[2]{};
        float xa[2]{};
        float yn[2]{};
        float ya[2]{};
        for (std::uint32_t row = 0U; row < height; ++row)
        {
            const std::size_t offset = (static_cast<std::size_t>(row) * width + column) * 2U;
            for (std::size_t channel = 0U; channel < 2U; ++channel)
            {
                xc[channel] = frozen_gaussian_clamp(signal[offset + channel]);
                yc[channel] = (coefficients.a0 * xc[channel]) + (coefficients.a1 * xp[channel]) -
                              (coefficients.b1 * yp[channel]) - (coefficients.b2 * yb[channel]);
                scratch[offset + channel] = yc[channel];
                xp[channel] = xc[channel];
                yb[channel] = yp[channel];
                yp[channel] = yc[channel];
            }
        }
        const std::size_t last = (static_cast<std::size_t>(height - 1U) * width + column) * 2U;
        for (std::size_t channel = 0U; channel < 2U; ++channel)
        {
            xn[channel] = frozen_gaussian_clamp(signal[last + channel]);
            xa[channel] = xn[channel];
            yn[channel] = xn[channel] * coefficients.coefn;
            ya[channel] = yn[channel];
        }
        for (std::uint32_t row = height; row > 0U; --row)
        {
            const std::size_t offset = (static_cast<std::size_t>(row - 1U) * width + column) * 2U;
            for (std::size_t channel = 0U; channel < 2U; ++channel)
            {
                xc[channel] = frozen_gaussian_clamp(signal[offset + channel]);
                yc[channel] = (coefficients.a2 * xn[channel]) + (coefficients.a3 * xa[channel]) -
                              (coefficients.b1 * yn[channel]) - (coefficients.b2 * ya[channel]);
                xa[channel] = xn[channel];
                xn[channel] = xc[channel];
                ya[channel] = yn[channel];
                yn[channel] = yc[channel];
                scratch[offset + channel] += yc[channel];
            }
        }
    }
    for (std::uint32_t row = 0U; row < height; ++row)
    {
        float xp[2]{};
        float yb[2]{};
        float yp[2]{};
        const std::size_t first = static_cast<std::size_t>(row) * width * 2U;
        for (std::size_t channel = 0U; channel < 2U; ++channel)
        {
            xp[channel] = frozen_gaussian_clamp(scratch[first + channel]);
            yb[channel] = xp[channel] * coefficients.coefp;
            yp[channel] = yb[channel];
        }
        float xc[2]{};
        float yc[2]{};
        float xn[2]{};
        float xa[2]{};
        float yn[2]{};
        float ya[2]{};
        for (std::uint32_t column = 0U; column < width; ++column)
        {
            const std::size_t offset = (static_cast<std::size_t>(row) * width + column) * 2U;
            for (std::size_t channel = 0U; channel < 2U; ++channel)
            {
                xc[channel] = frozen_gaussian_clamp(scratch[offset + channel]);
                yc[channel] = (coefficients.a0 * xc[channel]) + (coefficients.a1 * xp[channel]) -
                              (coefficients.b1 * yp[channel]) - (coefficients.b2 * yb[channel]);
                signal[offset + channel] = yc[channel];
                xp[channel] = xc[channel];
                yb[channel] = yp[channel];
                yp[channel] = yc[channel];
            }
        }
        const std::size_t last = (static_cast<std::size_t>(row + 1U) * width - 1U) * 2U;
        for (std::size_t channel = 0U; channel < 2U; ++channel)
        {
            xn[channel] = frozen_gaussian_clamp(scratch[last + channel]);
            xa[channel] = xn[channel];
            yn[channel] = xn[channel] * coefficients.coefn;
            ya[channel] = yn[channel];
        }
        for (std::uint32_t column = width; column > 0U; --column)
        {
            const std::size_t offset = (static_cast<std::size_t>(row) * width + (column - 1U)) * 2U;
            for (std::size_t channel = 0U; channel < 2U; ++channel)
            {
                xc[channel] = frozen_gaussian_clamp(scratch[offset + channel]);
                yc[channel] = (coefficients.a2 * xn[channel]) + (coefficients.a3 * xa[channel]) -
                              (coefficients.b1 * yn[channel]) - (coefficients.b2 * ya[channel]);
                xa[channel] = xn[channel];
                xn[channel] = xc[channel];
                ya[channel] = yn[channel];
                yn[channel] = yc[channel];
                signal[offset + channel] += yc[channel];
            }
        }
    }
    return signal;
}

[[nodiscard]] std::vector<float>
frozen_color_harmonizer_two_pass(const WorkingImage &input, const ColorHarmonizerParams &params,
                                 const float roi_scale, const FrozenHarmonyHueTables &tables)
{
    const auto nodes = frozen_color_harmonizer_nodes(params, tables);
    const auto inverse = frozen_color_harmonizer_inverse(input.color_profile.matrix_to_xyz_d50);
    const float white_lightness = frozen_dt_ucs_y_to_lightness(1.0F);
    constexpr float pi = 3.14159265358979323846F;
    constexpr float two_pi = 6.28318530717958647693F;
    const std::size_t pixels = static_cast<std::size_t>(input.width) * input.height;
    std::vector<float> jch(pixels * 3U);
    std::vector<float> corrections(pixels * 2U);
    for (std::size_t pixel = 0U; pixel < pixels; ++pixel)
    {
        const std::size_t rgb = pixel * 3U;
        const FrozenD50Triplet nonnegative{std::fmax(input.rgb[rgb], 0.0F),
                                           std::fmax(input.rgb[rgb + 1U], 0.0F),
                                           std::fmax(input.rgb[rgb + 2U], 0.0F)};
        const auto converted = frozen_dt_ucs_xyz_d50_to_jch(
            frozen_color_harmonizer_matrix(input.color_profile.matrix_to_xyz_d50, nonnegative),
            white_lightness);
        const float hue = (converted[2] + pi) / two_pi;
        const auto attraction =
            frozen_harmony_attraction(hue, std::span<const float>(nodes.hues.data(), nodes.count),
                                      static_cast<float>(params.pull_width));
        jch[rgb] = converted[0];
        jch[rgb + 1U] = converted[1];
        jch[rgb + 2U] = hue;
        corrections[pixel * 2U] = attraction.shift;
        corrections[pixel * 2U + 1U] =
            (static_cast<float>(params.node_saturation[attraction.winning_index]) - 1.0F) *
            attraction.weight;
    }
    const float sigma = static_cast<float>(params.smoothing) * std::fmax(1.5F, 8.0F * roi_scale) *
                        std::fmax(1.0F, static_cast<float>(params.pull_width));
    corrections = frozen_gaussian_zero_2c(std::move(corrections), input.width, input.height, sigma);
    const float neutral = static_cast<float>(params.neutral_protection);
    const float cutoff = neutral * neutral * neutral * 0.03F;
    std::vector<float> output(input.rgb.size());
    for (std::size_t pixel = 0U; pixel < pixels; ++pixel)
    {
        const std::size_t rgb = pixel * 3U;
        const float chroma = jch[rgb + 1U];
        const float weight = chroma / (chroma + cutoff + 1.0e-5F);
        float hue = std::fmod(jch[rgb + 2U] + corrections[pixel * 2U] *
                                                  static_cast<float>(params.pull_strength) * weight,
                              1.0F);
        if (hue < 0.0F)
        {
            hue += 1.0F;
        }
        const FrozenD50Triplet converted{
            jch[rgb], std::fmax(chroma * (1.0F + corrections[pixel * 2U + 1U] * weight), 0.0F),
            hue * two_pi - pi};
        const auto rgb_result = frozen_color_harmonizer_matrix(
            inverse, frozen_dt_ucs_jch_to_xyz_d50(converted, white_lightness));
        output[rgb] = rgb_result[0];
        output[rgb + 1U] = rgb_result[1];
        output[rgb + 2U] = rgb_result[2];
    }
    return output;
}

struct ColorHarmonizerCancellationFixture
{
    CancellationSource *source = nullptr;
    detail::ColorHarmonizerCheckpoint target = detail::ColorHarmonizerCheckpoint::kBeforeValidation;
    bool fired = false;
};

void cancel_color_harmonizer(void *const context,
                             const detail::ColorHarmonizerCheckpoint checkpoint,
                             std::uint32_t) noexcept
{
    auto &fixture = *static_cast<ColorHarmonizerCancellationFixture *>(context);
    if (!fixture.fired && checkpoint == fixture.target)
    {
        fixture.fired = fixture.source->cancel("colorharmonizer-checkpoint");
    }
}

// Independent scalar oracle transcribed from the frozen colorbalance.c
// commit_params(), _process_sop(), and _process_lgg() paths. It deliberately
// calls no production Color Balance helper, so the fixed goldens below do not
// merely restate apply_color_balance().
[[nodiscard]] std::vector<float>
frozen_legacy_color_balance_reference(const WorkingImage &input, const ColorBalanceParams &params)
{
    const auto linear_to_xyz = [](const std::array<float, 3> &rgb)
    {
        return std::array<float, 3>{0.4360747F * rgb[0] + 0.3850649F * rgb[1] + 0.1430804F * rgb[2],
                                    0.2225045F * rgb[0] + 0.7168786F * rgb[1] + 0.0606169F * rgb[2],
                                    0.0139322F * rgb[0] + 0.0971045F * rgb[1] +
                                        0.7141733F * rgb[2]};
    };
    const auto xyz_to_linear = [](const std::array<float, 3> &xyz)
    {
        return std::array<float, 3>{
            3.1338561F * xyz[0] - 1.6168667F * xyz[1] - 0.4906146F * xyz[2],
            -0.9787684F * xyz[0] + 1.9161415F * xyz[1] + 0.0334540F * xyz[2],
            0.0719453F * xyz[0] - 0.2289914F * xyz[1] + 1.4052427F * xyz[2]};
    };
    const auto xyz_to_lab = [](const std::array<float, 3> &xyz)
    {
        constexpr std::array<float, 3> d50{0.9642F, 1.0F, 0.8249F};
        constexpr float epsilon = 216.0F / 24389.0F;
        constexpr float kappa = 24389.0F / 27.0F;
        std::array<float, 3> f{};
        for (std::size_t channel = 0U; channel < f.size(); ++channel)
        {
            const float value = xyz[channel] / d50[channel];
            f[channel] = value > epsilon ? std::cbrt(value) : (kappa * value + 16.0F) / 116.0F;
        }
        return std::array<float, 3>{116.0F * f[1] - 16.0F, 500.0F * (f[0] - f[1]),
                                    200.0F * (f[1] - f[2])};
    };
    const auto lab_to_xyz = [](const std::array<float, 3> &lab)
    {
        constexpr std::array<float, 3> d50{0.9642F, 1.0F, 0.8249F};
        constexpr float epsilon = 0.20689655172413796F;
        constexpr float kappa = 24389.0F / 27.0F;
        const float fy = (lab[0] + 16.0F) / 116.0F;
        const std::array<float, 3> f{fy + lab[1] / 500.0F, fy, fy - lab[2] / 200.0F};
        std::array<float, 3> xyz{};
        for (std::size_t channel = 0U; channel < xyz.size(); ++channel)
        {
            const float value = f[channel] > epsilon ? f[channel] * f[channel] * f[channel] :
                                                       (116.0F * f[channel] - 16.0F) / kappa;
            xyz[channel] = d50[channel] * value;
        }
        return xyz;
    };
    const auto xyz_to_prophoto = [](const std::array<float, 3> &xyz)
    {
        return std::array<float, 3>{
            1.3459433F * xyz[0] - 0.2556075F * xyz[1] - 0.0511118F * xyz[2],
            -0.5445989F * xyz[0] + 1.5081673F * xyz[1] + 0.0205351F * xyz[2], 1.2118128F * xyz[2]};
    };
    const auto prophoto_to_xyz = [](const std::array<float, 3> &rgb)
    {
        return std::array<float, 3>{0.7976749F * rgb[0] + 0.1351917F * rgb[1] + 0.0313534F * rgb[2],
                                    0.2880402F * rgb[0] + 0.7118741F * rgb[1] + 0.0000857F * rgb[2],
                                    0.8252100F * rgb[2]};
    };
    const auto corrected = [&](const std::array<double, 4> &values)
    {
        const float red = static_cast<float>(values[1]);
        const float green = static_cast<float>(values[2]);
        const float blue = static_cast<float>(values[3]);
        const float luma = 0.2880402F * red + 0.7118741F * green + 0.0000857F * blue;
        return std::array<float, 4>{static_cast<float>(values[0]), red - luma + 1.0F,
                                    green - luma + 1.0F, blue - luma + 1.0F};
    };

    const auto lift = corrected(params.lift);
    const auto gamma = corrected(params.gamma);
    const auto gain = corrected(params.gain);
    const bool lgg = params.mode == kColorBalanceModeLiftGammaGain;
    std::array<float, 3> effective_lift{};
    std::array<float, 3> effective_gain{};
    std::array<float, 3> effective_power{};
    for (std::size_t channel = 0U; channel < 3U; ++channel)
    {
        effective_gain[channel] = gain[channel + 1U] * gain[0];
        if (lgg)
        {
            effective_lift[channel] = 2.0F - lift[channel + 1U] * lift[0];
            const float denominator = gamma[channel + 1U] * gamma[0];
            effective_power[channel] =
                2.2F * (denominator != 0.0F ? 1.0F / denominator : 1000000.0F);
        }
        else
        {
            effective_lift[channel] = lift[channel + 1U] + lift[0] - 2.0F;
            effective_power[channel] = (2.0F - gamma[channel + 1U]) * (2.0F - gamma[0]);
        }
    }
    const float input_saturation = static_cast<float>(params.input_saturation);
    const float output_saturation = static_cast<float>(params.output_saturation);
    const float contrast = static_cast<float>(params.contrast);
    const float contrast_power = 1.0F / contrast;
    const float grey = static_cast<float>(params.grey_fulcrum_percent / 100.0);
    const bool run_input_saturation = std::abs(input_saturation - 1.0F) > 1.0e-6F;
    const bool run_output_saturation = std::abs(output_saturation - 1.0F) > 1.0e-6F;
    const bool run_contrast = std::abs((lgg ? contrast_power : contrast) - 1.0F) > 1.0e-6F;

    std::vector<float> result(input.rgb.size());
    for (std::size_t index = 0U; index < input.rgb.size(); index += 3U)
    {
        const std::array<float, 3> source{input.rgb[index], input.rgb[index + 1U],
                                          input.rgb[index + 2U]};
        auto xyz = lab_to_xyz(xyz_to_lab(linear_to_xyz(source)));
        auto rgb = xyz_to_prophoto(xyz);
        if (run_input_saturation)
        {
            for (float &sample : rgb)
            {
                sample = xyz[1] + input_saturation * (sample - xyz[1]);
            }
        }
        for (std::size_t channel = 0U; channel < rgb.size(); ++channel)
        {
            if (lgg)
            {
                float value = std::pow(std::max(rgb[channel], 0.0F), 1.0F / 2.2F);
                value = ((value - 1.0F) * effective_lift[channel] + 1.0F) * effective_gain[channel];
                rgb[channel] = std::pow(std::max(value, 0.0F), effective_power[channel]);
            }
            else
            {
                rgb[channel] = std::pow(
                    std::max(effective_gain[channel] * rgb[channel] + effective_lift[channel],
                             0.0F),
                    effective_power[channel]);
            }
        }
        if (run_output_saturation)
        {
            const float luma = prophoto_to_xyz(rgb)[1];
            for (float &sample : rgb)
            {
                sample = luma + output_saturation * (sample - luma);
            }
        }
        if (run_contrast)
        {
            for (float &sample : rgb)
            {
                sample = std::pow(std::max(sample, 0.0F) / grey, contrast_power) * grey;
            }
        }
        const auto output = xyz_to_linear(lab_to_xyz(xyz_to_lab(prophoto_to_xyz(rgb))));
        std::copy(output.begin(), output.end(),
                  result.begin() + static_cast<std::ptrdiff_t>(index));
    }
    return result;
}

struct FrozenColorCheckerFit
{
    std::vector<std::array<float, 3>> sources;
    std::array<std::vector<float>, 3> coefficients;
};

[[nodiscard]] float frozen_color_checker_kernel_oracle(const std::array<float, 3> &left,
                                                       const std::array<float, 3> &right,
                                                       const bool use_libm = false)
{
    std::array<float, 3> squared{};
    for (std::size_t channel = 0U; channel < squared.size(); ++channel)
    {
        squared[channel] = left[channel] - right[channel];
        squared[channel] *= squared[channel];
    }
    const float radius_squared = squared[0] + squared[1] + squared[2];
    if (use_libm)
    {
        return radius_squared * std::log(std::max(1.0e-8F, radius_squared));
    }
    const float argument = std::max(1.0e-8F, radius_squared);
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(argument);
    const float mantissa = std::bit_cast<float>((bits & 0x007fffffU) | 0x3f000000U);
    float exponent = static_cast<float>(bits);
    exponent *= 1.1920928955078125e-7F;
    const float log2 = exponent - 124.22551499F - 1.498030302F * mantissa -
                       1.72587999F / (0.3520887068F + mantissa);
    return radius_squared * (0.69314718055994530942F * log2);
}

[[nodiscard]] bool frozen_color_checker_triangular(std::vector<double> &matrix,
                                                   std::vector<int> &pivots, const std::size_t size)
{
    pivots[size - 1U] = static_cast<int>(size - 1U);
    for (std::size_t column = 0U; column < size; ++column)
    {
        std::size_t best_row = column;
        for (std::size_t row = column + 1U; row < size; ++row)
        {
            if (std::fabs(matrix[row * size + column]) >
                std::fabs(matrix[best_row * size + column]))
            {
                best_row = row;
            }
        }
        pivots[column] = static_cast<int>(best_row);
        const double pivot = matrix[best_row * size + column];
        std::swap(matrix[best_row * size + column], matrix[column * size + column]);
        if (pivot == 0.0)
        {
            return false;
        }
        for (std::size_t row = column + 1U; row < size; ++row)
        {
            matrix[row * size + column] /= -pivot;
        }
        if (best_row != column)
        {
            for (std::size_t remaining = column + 1U; remaining < size; ++remaining)
            {
                std::swap(matrix[best_row * size + remaining], matrix[column * size + remaining]);
            }
        }
        for (std::size_t row = column + 1U; row < size; ++row)
        {
            for (std::size_t remaining = column + 1U; remaining < size; ++remaining)
            {
                matrix[row * size + remaining] +=
                    matrix[row * size + column] * matrix[column * size + remaining];
            }
        }
    }
    return true;
}

void frozen_color_checker_back_substitute(const std::vector<double> &matrix,
                                          const std::vector<int> &pivots,
                                          std::vector<double> &right, const std::size_t size)
{
    for (std::size_t column = 0U; column + 1U < size; ++column)
    {
        const std::size_t pivot = static_cast<std::size_t>(pivots[column]);
        const double value = right[pivot];
        std::swap(right[pivot], right[column]);
        for (std::size_t row = column + 1U; row < size; ++row)
        {
            right[row] += matrix[row * size + column] * value;
        }
    }
    for (std::size_t column = size - 1U; column > 0U; --column)
    {
        right[column] /= matrix[column * size + column];
        for (std::size_t row = 0U; row < column; ++row)
        {
            right[row] -= matrix[row * size + column] * right[column];
        }
    }
    right[0] /= matrix[0];
}

[[nodiscard]] bool frozen_color_checker_solve(std::vector<double> matrix,
                                              std::vector<double> &right)
{
    std::vector<int> pivots(right.size());
    if (!frozen_color_checker_triangular(matrix, pivots, right.size()))
    {
        return false;
    }
    frozen_color_checker_back_substitute(matrix, pivots, right, right.size());
    return true;
}

[[nodiscard]] FrozenColorCheckerFit
frozen_color_checker_fit_oracle(const ColorCheckerParams &params, const bool use_libm = false,
                                const bool promote_n3_sum = false)
{
    FrozenColorCheckerFit fit;
    fit.sources.reserve(params.patches.size());
    for (const auto &patch : params.patches)
    {
        fit.sources.push_back({static_cast<float>(patch.source_lab[0]),
                               static_cast<float>(patch.source_lab[1]),
                               static_cast<float>(patch.source_lab[2])});
    }
    const std::size_t count = fit.sources.size();
    for (auto &coefficients : fit.coefficients)
    {
        coefficients.assign(count + 4U, 0.0F);
    }
    fit.coefficients[0][count + 1U] = 1.0F;
    fit.coefficients[1][count + 2U] = 1.0F;
    fit.coefficients[2][count + 3U] = 1.0F;
    const auto target = [&](const std::size_t patch, const std::size_t channel)
    { return static_cast<float>(params.patches[patch].target_lab[channel]); };

    if (count == 0U)
    {
        return fit;
    }
    if (count == 1U)
    {
        for (std::size_t channel = 0U; channel < 3U; ++channel)
        {
            fit.coefficients[channel][count + channel + 1U] =
                target(0U, channel) / fit.sources[0][channel];
        }
        return fit;
    }
    if (count == 2U)
    {
        for (std::size_t channel = 0U; channel < 3U; ++channel)
        {
            std::vector<double> right{target(0U, channel), target(1U, channel)};
            if (!frozen_color_checker_solve(
                    {1.0, fit.sources[0][channel], 1.0, fit.sources[1][channel]}, right))
            {
                return fit;
            }
            fit.coefficients[channel][count] = static_cast<float>(right[0]);
            fit.coefficients[channel][count + channel + 1U] = static_cast<float>(right[1]);
        }
        return fit;
    }
    if (count == 3U)
    {
        for (std::size_t channel = 0U; channel < 3U; ++channel)
        {
            std::vector<double> matrix;
            for (std::size_t patch = 0U; patch < count; ++patch)
            {
                const std::size_t other0 = (channel + 1U) % 3U;
                const std::size_t other1 = (channel + 2U) % 3U;
                const double other_sum = promote_n3_sum ?
                                             static_cast<double>(fit.sources[patch][other0]) +
                                                 fit.sources[patch][other1] :
                                             static_cast<double>(fit.sources[patch][other0] +
                                                                 fit.sources[patch][other1]);
                matrix.insert(matrix.end(), {1.0, fit.sources[patch][channel], other_sum});
            }
            std::vector<double> right{target(0U, channel), target(1U, channel),
                                      target(2U, channel)};
            if (!frozen_color_checker_solve(std::move(matrix), right))
            {
                return fit;
            }
            fit.coefficients[channel][count] = static_cast<float>(right[0]);
            fit.coefficients[channel][count + channel + 1U] = static_cast<float>(right[1]);
            for (std::size_t input = 0U; input < 3U; ++input)
            {
                if (input != channel)
                {
                    fit.coefficients[channel][count + input + 1U] = static_cast<float>(right[2]);
                }
            }
        }
        return fit;
    }

    const std::size_t fit_size = count == 4U ? 4U : count + 4U;
    std::vector<double> matrix(fit_size * fit_size, 0.0);
    if (count == 4U)
    {
        for (std::size_t patch = 0U; patch < count; ++patch)
        {
            matrix[patch * fit_size] = 1.0;
            for (std::size_t channel = 0U; channel < 3U; ++channel)
            {
                matrix[patch * fit_size + channel + 1U] = fit.sources[patch][channel];
            }
        }
    }
    else
    {
        for (std::size_t row = 0U; row < count; ++row)
        {
            for (std::size_t column = 0U; column < count; ++column)
            {
                matrix[row * fit_size + column] = frozen_color_checker_kernel_oracle(
                    fit.sources[row], fit.sources[column], use_libm);
            }
            matrix[row * fit_size + count] = matrix[count * fit_size + row] = 1.0;
            for (std::size_t channel = 0U; channel < 3U; ++channel)
            {
                matrix[row * fit_size + count + channel + 1U] =
                    matrix[(count + channel + 1U) * fit_size + row] = fit.sources[row][channel];
            }
        }
    }
    std::vector<int> pivots(fit_size);
    if (!frozen_color_checker_triangular(matrix, pivots, fit_size))
    {
        return fit;
    }
    for (std::size_t channel = 0U; channel < 3U; ++channel)
    {
        std::vector<double> right(fit_size, 0.0);
        for (std::size_t patch = 0U; patch < count; ++patch)
        {
            right[patch] = target(patch, channel);
        }
        frozen_color_checker_back_substitute(matrix, pivots, right, fit_size);
        const std::size_t offset = count == 4U ? count : 0U;
        for (std::size_t index = 0U; index < fit_size; ++index)
        {
            fit.coefficients[channel][offset + index] = static_cast<float>(right[index]);
        }
    }
    return fit;
}

[[nodiscard]] std::array<float, 3>
frozen_color_checker_lab_reference(const ColorCheckerParams &params,
                                   const std::array<float, 3> &lab, const bool use_libm = false,
                                   const bool promote_n3_sum = false)
{
    const auto fit = frozen_color_checker_fit_oracle(params, use_libm, promote_n3_sum);
    const std::size_t count = fit.sources.size();
    std::array<float, 3> result{};
    for (std::size_t channel = 0U; channel < result.size(); ++channel)
    {
        const float term_l = fit.coefficients[channel][count + 1U] * lab[0];
        const float term_a = fit.coefficients[channel][count + 2U] * lab[1];
        const float term_b = fit.coefficients[channel][count + 3U] * lab[2];
        result[channel] = fit.coefficients[channel][count] + (term_l + term_a + term_b);
    }
    for (std::size_t patch = 0U; patch < count; ++patch)
    {
        const float phi = frozen_color_checker_kernel_oracle(lab, fit.sources[patch], use_libm);
        for (std::size_t channel = 0U; channel < result.size(); ++channel)
        {
            result[channel] += fit.coefficients[channel][patch] * phi;
        }
    }
    return result;
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

    WorkingImage rgb{1, 1, {0.25F, 0.5F, 0.75F}, {}, {}};
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

TEST(LegacyColorBalanceTest, SopAndLggModesPreserveFrozenMathAndOwnedPublication)
{
    const auto input = legacy_color_balance_working_fixture();
    const auto original = input;

    ColorBalanceParams sop;
    sop.lift = {0.96, 1.03, 0.98, 1.06};
    sop.gamma = {1.08, 0.91, 1.05, 0.97};
    sop.gain = {1.04, 1.12, 0.95, 1.08};
    sop.input_saturation = 0.84;
    sop.contrast = 1.16;
    sop.grey_fulcrum_percent = 18.0;
    sop.output_saturation = 1.09;
    auto sop_result = apply_color_balance(input, sop, CancellationToken{});
    ASSERT_TRUE(sop_result) << sop_result.error().message;
    ASSERT_EQ(sop_result.value().rgb.size(), input.rgb.size());
    const std::array<float, 6> expected_sop{0.10232526F, 0.15027370F, 0.66838688F,
                                            0.85773712F, 0.34731370F, 0.18906617F};
    const auto reference_sop = frozen_legacy_color_balance_reference(input, sop);
    for (std::size_t index = 0U; index < expected_sop.size(); ++index)
    {
        EXPECT_NEAR(reference_sop[index], expected_sop[index], 2.0e-5F) << index;
        EXPECT_NEAR(sop_result.value().rgb[index], expected_sop[index], 2.0e-5F) << index;
        EXPECT_NEAR(sop_result.value().rgb[index], reference_sop[index], 2.0e-5F) << index;
    }
    auto channel_order_perturbation = sop;
    std::swap(channel_order_perturbation.lift[1], channel_order_perturbation.lift[3]);
    const auto perturbed_reference =
        frozen_legacy_color_balance_reference(input, channel_order_perturbation);
    bool perturbation_detected = false;
    for (std::size_t index = 0U; index < reference_sop.size(); ++index)
    {
        perturbation_detected |=
            std::abs(reference_sop[index] - perturbed_reference[index]) > 1.0e-3F;
    }
    EXPECT_TRUE(perturbation_detected)
        << "the independent oracle must detect a frozen RGB channel-order perturbation";

    ColorBalanceParams lgg = sop;
    lgg.mode = std::string(kColorBalanceModeLiftGammaGain);
    auto lgg_result = apply_color_balance(input, lgg, CancellationToken{});
    ASSERT_TRUE(lgg_result) << lgg_result.error().message;
    const std::array<float, 6> expected_lgg{0.12932241F, 0.17394857F, 0.73170942F,
                                            1.06095791F, 0.34121433F, 0.19952966F};
    const auto reference_lgg = frozen_legacy_color_balance_reference(input, lgg);
    for (std::size_t index = 0U; index < expected_lgg.size(); ++index)
    {
        EXPECT_NEAR(reference_lgg[index], expected_lgg[index], 2.0e-5F) << index;
        EXPECT_NEAR(lgg_result.value().rgb[index], expected_lgg[index], 2.0e-5F) << index;
        EXPECT_NEAR(lgg_result.value().rgb[index], reference_lgg[index], 2.0e-5F) << index;
    }
    EXPECT_NE(lgg_result.value().rgb, sop_result.value().rgb);

    auto defaults = apply_color_balance(input, ColorBalanceParams{}, CancellationToken{});
    ASSERT_TRUE(defaults) << defaults.error().message;
    const auto reference_defaults =
        frozen_legacy_color_balance_reference(input, ColorBalanceParams{});
    // The frozen operation performs its Lab/ProPhoto conversion boundary even at defaults.
    EXPECT_NE(defaults.value().rgb, input.rgb);
    ASSERT_EQ(defaults.value().rgb.size(), reference_defaults.size());
    for (std::size_t index = 0U; index < reference_defaults.size(); ++index)
    {
        EXPECT_NEAR(defaults.value().rgb[index], reference_defaults[index], 2.0e-5F) << index;
    }
    EXPECT_EQ(sop_result.value().width, input.width);
    EXPECT_EQ(sop_result.value().height, input.height);
    EXPECT_EQ(sop_result.value().color_profile, input.color_profile);
    EXPECT_EQ(sop_result.value().exposure_analysis, input.exposure_analysis);
    EXPECT_NE(sop_result.value().rgb.data(), input.rgb.data());
    ASSERT_FALSE(sop_result.value().color_profile.icc_bytes.empty());
    EXPECT_NE(sop_result.value().color_profile.icc_bytes.data(),
              input.color_profile.icc_bytes.data());
    sop_result.value().rgb[0] = 42.0F;
    sop_result.value().color_profile.icc_bytes[0] = 99U;
    EXPECT_EQ(input.width, original.width);
    EXPECT_EQ(input.height, original.height);
    EXPECT_EQ(input.rgb, original.rgb);
    EXPECT_EQ(input.color_profile, original.color_profile);
    EXPECT_EQ(input.exposure_analysis, original.exposure_analysis);
}

TEST(ColorCheckerTest, ThinPlateKernelUsesTheFrozenFastLogApproximation)
{
    struct KernelCase
    {
        std::array<float, 3> point;
        float squared_distance;
        std::uint32_t expected_bits;
    };
    const std::array<KernelCase, 4> cases{{
        {{1.0F, 0.0F, 0.0F}, 1.0F, 0xb5ddce9eU},
        {{1.0F, 1.0F, 0.0F}, 2.0F, 0x3fb171fcU},
        {{3.0F, 1.0F, 0.0F}, 10.0F, 0x41b8340aU},
        {{100.0F, 0.0F, 0.0F}, 10000.0F, 0x47b3e369U},
    }};
    for (const auto &[point, squared_distance, expected_bits] : cases)
    {
        const float actual = color_checker_thin_plate_kernel(point, {});
        EXPECT_EQ(std::bit_cast<std::uint32_t>(actual), expected_bits) << squared_distance;
    }
}

TEST(ColorCheckerTest, TwoPatchGaussianOrientationMatchesTheFrozenScalarOracle)
{
    ColorCheckerParams params{{
        {{{1.0, 2.0, 4.0}}, {{3.0, 8.0, 20.0}}},
        {{{3.0, 5.0, 9.0}}, {{7.0, 20.0, 45.0}}},
    }};
    const std::array<float, 3> input{2.0F, 3.0F, 6.0F};
    const auto oracle = frozen_color_checker_lab_reference(params, input);
    const std::array<float, 3> golden{5.0F, 12.0F, 30.0F};
    EXPECT_EQ(oracle, golden);

    auto actual = apply_color_checker_lab(params, input, CancellationToken{});
    ASSERT_TRUE(actual) << actual.error().message;
    EXPECT_EQ(actual.value(), golden);
    EXPECT_EQ(actual.value(), oracle);
}

TEST(ColorCheckerTest, ThreePatchOtherChannelSumRoundsInFloatBeforePromotion)
{
    ColorCheckerParams params{{
        {{{-52.407073974609375, 16777224.0, -16777207.0}},
         {{-5.947298526763916, 67.29228973388672, -4.729358196258545}}},
        {{{-5.189292907714844, 16777200.0, -16777184.0}},
         {{27.813627243041992, -69.87671661376953, 26.972131729125977}}},
        {{{-6.1535325050354, 16777212.0, -16777192.0}},
         {{73.60906219482422, 4.636241912841797, 48.250370025634766}}},
    }};
    const std::array<float, 3> input{12.5F, 16777220.0F, -16777216.0F};
    const auto oracle = frozen_color_checker_lab_reference(params, input);
    const auto promoted = frozen_color_checker_lab_reference(params, input, false, true);
    EXPECT_NE(oracle, promoted)
        << "the independent oracle must detect promotion before the frozen float addition";
    EXPECT_FLOAT_EQ(oracle[1], 48.0F);
    EXPECT_FLOAT_EQ(promoted[1], 40.0F);

    auto actual = apply_color_checker_lab(params, input, CancellationToken{});
    ASSERT_TRUE(actual) << actual.error().message;
    EXPECT_EQ(actual.value(), oracle);
}

TEST(ColorCheckerTest, Real0098PayloadMatchesIndependentRbfOracleAndFixedLabGolden)
{
    ColorCheckerParams params;
    params.patches[7].target_lab = {92.74998474121094, 97.59593200683594, 82.81928253173828};
    params.patches[19].target_lab = {72.97999572753906, 43.90998840332031, 35.799983978271484};
    params.patches[22].target_lab = {45.439998626708984, -0.41999998688697815, 59.32999801635742};
    const std::array<float, 3> input{50.0F, 0.0F, 0.0F};
    // Fixed source-order results for the verbatim 0098 v2 payload. The
    // independent scalar oracle below protects the frozen kernel and solver
    // order without calling a production Ravo helper.
    const std::array<float, 3> golden{std::bit_cast<float>(0x4249cbc8U),
                                      std::bit_cast<float>(0x3eb8d900U),
                                      std::bit_cast<float>(0x404095c0U)};

    const auto oracle = frozen_color_checker_lab_reference(params, input);
    const auto libm_perturbation = frozen_color_checker_lab_reference(params, input, true);
    bool oracle_detects_libm = false;
    for (std::size_t channel = 0U; channel < golden.size(); ++channel)
    {
        EXPECT_EQ(std::bit_cast<std::uint32_t>(oracle[channel]),
                  std::bit_cast<std::uint32_t>(golden[channel]))
            << channel;
        oracle_detects_libm |= std::abs(oracle[channel] - libm_perturbation[channel]) > 1.0e-5F;
    }
    EXPECT_TRUE(oracle_detects_libm)
        << "the independent oracle must distinguish the frozen fastlog from libm";

    auto actual = apply_color_checker_lab(params, input, CancellationToken{});
    ASSERT_TRUE(actual) << actual.error().message;
    for (std::size_t channel = 0U; channel < golden.size(); ++channel)
    {
        EXPECT_EQ(std::bit_cast<std::uint32_t>(actual.value()[channel]),
                  std::bit_cast<std::uint32_t>(golden[channel]))
            << channel;
        EXPECT_EQ(std::bit_cast<std::uint32_t>(actual.value()[channel]),
                  std::bit_cast<std::uint32_t>(oracle[channel]))
            << channel;
    }
}

TEST(ColorCheckerTest, ZeroOneFourAndRbfPatchModesMatchTheIndependentOracle)
{
    const std::array<float, 3> input{0.25F, 0.5F, 0.75F};

    ColorCheckerParams zero{{}};
    auto actual = apply_color_checker_lab(zero, input, CancellationToken{});
    ASSERT_TRUE(actual) << actual.error().message;
    EXPECT_EQ(actual.value(), input);

    ColorCheckerParams one{{{{{2.0, 4.0, 5.0}}, {{6.0, 2.0, 10.0}}}}};
    actual = apply_color_checker_lab(one, {1.0F, 8.0F, 2.5F}, CancellationToken{});
    ASSERT_TRUE(actual) << actual.error().message;
    EXPECT_EQ(actual.value(), (std::array<float, 3>{3.0F, 4.0F, 5.0F}));

    ColorCheckerParams four{{
        {{{0.0, 0.0, 0.0}}, {{1.0, -2.0, 0.5}}},
        {{{1.0, 0.0, 0.0}}, {{3.0, -1.5, -0.5}}},
        {{{0.0, 1.0, 0.0}}, {{4.0, -1.0, 2.5}}},
        {{{0.0, 0.0, 1.0}}, {{5.0, -0.5, 4.5}}},
    }};
    const auto four_oracle = frozen_color_checker_lab_reference(four, input);
    EXPECT_EQ(four_oracle, (std::array<float, 3>{6.0F, -0.25F, 4.25F}));
    actual = apply_color_checker_lab(four, input, CancellationToken{});
    ASSERT_TRUE(actual) << actual.error().message;
    EXPECT_EQ(actual.value(), four_oracle);

    ColorCheckerParams five = four;
    five.patches.push_back({{{1.0, 1.0, 1.0}}, {{12.0, 3.0, -7.0}}});
    const auto five_oracle = frozen_color_checker_lab_reference(five, input);
    actual = apply_color_checker_lab(five, input, CancellationToken{});
    ASSERT_TRUE(actual) << actual.error().message;
    for (std::size_t channel = 0U; channel < 3U; ++channel)
    {
        EXPECT_NEAR(actual.value()[channel], five_oracle[channel], 2.0e-5F) << channel;
    }
    EXPECT_NE(actual.value(), input);

    auto expanded = color_checker_params_for_preset("expanded_color_checker");
    ASSERT_TRUE(expanded) << expanded.error().message;
    ASSERT_EQ(expanded.value().patches.size(), kColorCheckerMaxPatchCount);
    const std::array<float, 3> expanded_input{52.0F, 18.0F, -21.0F};
    const auto expanded_oracle =
        frozen_color_checker_lab_reference(expanded.value(), expanded_input);
    actual = apply_color_checker_lab(expanded.value(), expanded_input, CancellationToken{});
    ASSERT_TRUE(actual) << actual.error().message;
    for (std::size_t channel = 0U; channel < 3U; ++channel)
    {
        EXPECT_NEAR(actual.value()[channel], expanded_oracle[channel], 2.0e-5F) << channel;
    }
}

TEST(ColorCheckerTest, SingularFallbackPreservesFrozenSequentialAndSharedMatrixSemantics)
{
    ColorCheckerParams two{{
        {{{1.0, 2.0, 3.0}}, {{2.0, 7.0, 11.0}}},
        {{{3.0, 2.0, 6.0}}, {{10.0, 9.0, 17.0}}},
    }};
    const std::array<float, 3> input{2.0F, 5.0F, 7.0F};
    const auto two_oracle = frozen_color_checker_lab_reference(two, input);
    EXPECT_FLOAT_EQ(two_oracle[0], 6.0F);
    EXPECT_FLOAT_EQ(two_oracle[1], input[1]);
    EXPECT_FLOAT_EQ(two_oracle[2], input[2]);
    auto actual = apply_color_checker_lab(two, input, CancellationToken{});
    ASSERT_TRUE(actual) << actual.error().message;
    EXPECT_EQ(actual.value(), two_oracle);

    ColorCheckerParams three{{
        {{{1.0, 1.0, 5.0}}, {{2.0, -1.0, 12.0}}},
        {{{2.0, 3.0, 5.0}}, {{7.0, 4.0, 18.0}}},
        {{{4.0, 2.0, 5.0}}, {{11.0, 9.0, 24.0}}},
    }};
    const auto three_oracle = frozen_color_checker_lab_reference(three, input);
    EXPECT_NE(three_oracle[0], input[0]);
    EXPECT_NE(three_oracle[1], input[1]);
    EXPECT_FLOAT_EQ(three_oracle[2], input[2]);
    actual = apply_color_checker_lab(three, input, CancellationToken{});
    ASSERT_TRUE(actual) << actual.error().message;
    EXPECT_EQ(actual.value(), three_oracle);

    ColorCheckerParams four_singular{{
        {{{1.0, 2.0, 3.0}}, {{8.0, 9.0, 10.0}}},
        {{{1.0, 2.0, 3.0}}, {{11.0, 12.0, 13.0}}},
        {{{2.0, 3.0, 4.0}}, {{14.0, 15.0, 16.0}}},
        {{{3.0, 4.0, 5.0}}, {{17.0, 18.0, 19.0}}},
    }};
    actual = apply_color_checker_lab(four_singular, input, CancellationToken{});
    ASSERT_TRUE(actual) << actual.error().message;
    EXPECT_EQ(actual.value(), input);

    ColorCheckerParams rbf_singular = four_singular;
    rbf_singular.patches.push_back({{{4.0, 5.0, 6.0}}, {{20.0, 21.0, 22.0}}});
    actual = apply_color_checker_lab(rbf_singular, input, CancellationToken{});
    ASSERT_TRUE(actual) << actual.error().message;
    EXPECT_EQ(actual.value(), input);
}

TEST(ColorCheckerTest, WorkingPublicationIsOwnedImmutableAndRejectsEveryInvalidBoundary)
{
    WorkingImage input = legacy_color_balance_working_fixture();
    input.rgb.front() = -0.1F;
    input.rgb.back() = 1.2F;
    const WorkingImage original = input;

    auto output = apply_color_checker(input, ColorCheckerParams{}, CancellationToken{});
    ASSERT_TRUE(output) << output.error().message;
    EXPECT_EQ(output.value().width, input.width);
    EXPECT_EQ(output.value().height, input.height);
    EXPECT_EQ(output.value().color_profile, input.color_profile);
    EXPECT_EQ(output.value().exposure_analysis, input.exposure_analysis);
    EXPECT_NE(output.value().rgb.data(), input.rgb.data());
    ASSERT_FALSE(input.color_profile.icc_bytes.empty());
    EXPECT_NE(output.value().color_profile.icc_bytes.data(), input.color_profile.icc_bytes.data());
    output.value().rgb[0] = 42.0F;
    output.value().color_profile.icc_bytes[0] = 99U;
    EXPECT_EQ(input.width, original.width);
    EXPECT_EQ(input.height, original.height);
    EXPECT_EQ(input.rgb, original.rgb);
    EXPECT_EQ(input.color_profile, original.color_profile);
    EXPECT_EQ(input.exposure_analysis, original.exposure_analysis);

    auto operation_parameters = color_checker_to_parameters(ColorCheckerParams{});
    ASSERT_TRUE(operation_parameters) << operation_parameters.error().message;
    OperationInstance operation{std::string(kColorCheckerOperationId),
                                kColorCheckerOperationSchemaVersion,
                                "colorchecker-dispatch",
                                true,
                                operation_parameters.value(),
                                std::nullopt};
    auto dispatched = apply_color_checker(input, operation, CancellationToken{});
    ASSERT_TRUE(dispatched) << dispatched.error().message;
    auto direct = apply_color_checker(input, ColorCheckerParams{}, CancellationToken{});
    ASSERT_TRUE(direct) << direct.error().message;
    EXPECT_EQ(dispatched.value().rgb, direct.value().rgb);
    operation.enabled = false;
    auto disabled = apply_color_checker(input, operation, CancellationToken{});
    ASSERT_TRUE(disabled) << disabled.error().message;
    EXPECT_EQ(disabled.value().rgb, input.rgb);
    EXPECT_NE(disabled.value().rgb.data(), input.rgb.data());

    WorkingImage zero = input;
    zero.width = 0U;
    auto rejected = apply_color_checker(zero, ColorCheckerParams{}, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "invalid_colorchecker_dimensions");
    WorkingImage wrong_size = input;
    wrong_size.rgb.pop_back();
    rejected = apply_color_checker(wrong_size, ColorCheckerParams{}, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "invalid_colorchecker_buffer");
    WorkingImage wrong_model = input;
    wrong_model.color_profile.model = ColorModel::kLab;
    rejected = apply_color_checker(wrong_model, ColorCheckerParams{}, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "unsupported_colorchecker_working_space");
    WorkingImage wrong_profile = input;
    wrong_profile.color_profile.identifier = "srgb";
    rejected = apply_color_checker(wrong_profile, ColorCheckerParams{}, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "unsupported_colorchecker_working_space");
    for (const float invalid :
         {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
          -std::numeric_limits<float>::infinity()})
    {
        WorkingImage nonfinite = input;
        nonfinite.rgb[1] = invalid;
        const auto source = nonfinite.rgb;
        rejected = apply_color_checker(nonfinite, ColorCheckerParams{}, CancellationToken{});
        ASSERT_FALSE(rejected);
        EXPECT_EQ(rejected.error().context.at("reason"), "nonfinite_colorchecker_input");
        ASSERT_EQ(nonfinite.rgb.size(), source.size());
        for (std::size_t index = 0U; index < source.size(); ++index)
        {
            EXPECT_EQ(std::bit_cast<std::uint32_t>(nonfinite.rgb[index]),
                      std::bit_cast<std::uint32_t>(source[index]));
        }
    }

    ColorCheckerParams invalid_params;
    invalid_params.patches[0].target_lab[1] = std::numeric_limits<double>::infinity();
    auto invalid_fit =
        apply_color_checker_lab(invalid_params, {50.0F, 0.0F, 0.0F}, CancellationToken{});
    ASSERT_FALSE(invalid_fit);
    ColorCheckerParams zero_denominator{{{{{1.0, 0.0, 2.0}}, {{2.0, 1.0, 4.0}}}}};
    invalid_fit =
        apply_color_checker_lab(zero_denominator, {1.0F, 2.0F, 3.0F}, CancellationToken{});
    ASSERT_FALSE(invalid_fit);
    EXPECT_EQ(invalid_fit.error().context.at("reason"), "invalid_colorchecker_denominator");

    CancellationSource pre_cancelled;
    ASSERT_TRUE(pre_cancelled.cancel("colorchecker-pre"));
    rejected = apply_color_checker(input, ColorCheckerParams{}, pre_cancelled.token());
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kCancelled);
    EXPECT_EQ(input.width, original.width);
    EXPECT_EQ(input.height, original.height);
    EXPECT_EQ(input.rgb, original.rgb);
    EXPECT_EQ(input.color_profile, original.color_profile);
    EXPECT_EQ(input.exposure_analysis, original.exposure_analysis);

    auto canonical = color_checker_to_parameters(ColorCheckerParams{});
    ASSERT_TRUE(canonical) << canonical.error().message;
    OperationInstance masked{std::string(kColorCheckerOperationId),
                             kColorCheckerOperationSchemaVersion,
                             "colorchecker-mask",
                             true,
                             std::move(canonical).value(),
                             "mask-1"};
    rejected = apply_color_checker(input, masked, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "colorchecker_mask_graph_unavailable");
    EXPECT_EQ(input.width, original.width);
    EXPECT_EQ(input.height, original.height);
    EXPECT_EQ(input.rgb, original.rgb);
    EXPECT_EQ(input.color_profile, original.color_profile);
    EXPECT_EQ(input.exposure_analysis, original.exposure_analysis);
}

TEST(D50LabBridgeTest, MatricesAndD50WhiteBlackMatchFrozenBitGoldens)
{
    struct MatrixCase
    {
        FrozenD50Triplet input;
        std::array<std::uint32_t, 3> forward;
        std::array<std::uint32_t, 3> inverse;
    };
    const std::array cases{
        MatrixCase{{1.0F, 0.0F, 0.0F},
                   {0x3edf452fU, 0x3e63d838U, 0x3c6443e2U},
                   {0x40489119U, 0xbf7a9091U, 0x3d93580fU}},
        MatrixCase{{0.0F, 1.0F, 0.0F},
                   {0x3ec5273aU, 0x3f37855bU, 0x3dc6deb9U},
                   {0xbfcef57dU, 0x3ff54420U, 0xbe6a7cb9U}},
        MatrixCase{{0.0F, 0.0F, 1.0F},
                   {0x3e1283abU, 0x3d78496dU, 0x3f36d410U},
                   {0xbefb31d6U, 0x3d090710U, 0x3fb3defeU}},
    };
    for (const auto &[input, forward, inverse] : cases)
    {
        expect_frozen_d50_bits(d50_lab::linear_rec709_to_xyz(input),
                               frozen_linear_rec709_to_xyz_d50(input), forward);
        expect_frozen_d50_bits(d50_lab::xyz_to_linear_rec709(input),
                               frozen_xyz_d50_to_linear_rec709(input), inverse);
    }

    constexpr FrozenD50Triplet black_xyz{0.0F, 0.0F, 0.0F};
    constexpr FrozenD50Triplet d50_white{0.9642F, 1.0F, 0.8249F};
    constexpr FrozenD50Triplet black_lab{0.0F, 0.0F, 0.0F};
    constexpr FrozenD50Triplet white_lab{100.0F, 0.0F, 0.0F};
    expect_frozen_d50_bits(d50_lab::xyz_to_lab(black_xyz), frozen_xyz_d50_to_lab(black_xyz),
                           {0x00000000U, 0x00000000U, 0x80000000U});
    expect_frozen_d50_bits(d50_lab::xyz_to_lab(d50_white), frozen_xyz_d50_to_lab(d50_white),
                           {0x42c80000U, 0x00000000U, 0x80000000U});
    expect_frozen_d50_bits(d50_lab::lab_to_xyz(black_lab), frozen_lab_to_xyz_d50(black_lab),
                           {0x00000000U, 0x00000000U, 0x00000000U});
    expect_frozen_d50_bits(d50_lab::lab_to_xyz(white_lab), frozen_lab_to_xyz_d50(white_lab),
                           {0x3f76d5d0U, 0x3f800000U, 0x3f532ca5U});
}

TEST(D50LabBridgeTest, XyzToLabFreezesEpsilonAndReciprocalMultiplyOrder)
{
    constexpr float epsilon = 216.0F / 24389.0F;
    struct BranchCase
    {
        float y;
        std::array<std::uint32_t, 3> expected;
    };
    const std::array cases{
        BranchCase{epsilon * 0.99F, {0x40fd70a8U, 0xc2088d41U, 0x415a7b9bU}},
        BranchCase{epsilon, {0x41000000U, 0xc209ee59U, 0x415cb08fU}},
        BranchCase{epsilon * 1.01F, {0x41014698U, 0xc20b4e47U, 0x415ee3a5U}},
    };
    for (const auto &[y, expected] : cases)
    {
        const FrozenD50Triplet xyz{0.0F, y, 0.0F};
        expect_frozen_d50_cbrt_reference(d50_lab::xyz_to_lab(xyz), frozen_xyz_d50_to_lab(xyz),
                                         expected);
    }

    constexpr FrozenD50Triplet rgb{0.1938238604679151F, 0.36766030739017674F, 0.38827863670090734F};
    const auto xyz = frozen_linear_rec709_to_xyz_d50(rgb);
    const auto expected = frozen_xyz_d50_to_lab(xyz);
    expect_frozen_d50_cbrt_reference(d50_lab::xyz_to_lab(d50_lab::linear_rec709_to_xyz(rgb)),
                                     expected, {0x42805bf3U, 0xc15d8c10U, 0xc0deecf2U});
}

TEST(D50LabBridgeTest, LabToXyzFreezesInverseThresholdAndScaleMultiplyOrder)
{
    struct BranchCase
    {
        float lightness;
        std::array<std::uint32_t, 3> expected;
    };
    const std::array cases{
        BranchCase{7.99F, {0x3c0bbc07U, 0x3c10ec37U, 0x3bef17efU}},
        BranchCase{8.0F, {0x3c0be8cdU, 0x3c111aa6U, 0x3bef648aU}},
        BranchCase{8.01F, {0x3c0c1598U, 0x3c11491bU, 0x3befb12fU}},
    };
    for (const auto &[lightness, expected] : cases)
    {
        const FrozenD50Triplet lab{lightness, 0.0F, 0.0F};
        expect_frozen_d50_bits(d50_lab::lab_to_xyz(lab), frozen_lab_to_xyz_d50(lab), expected);
    }

    constexpr FrozenD50Triplet lab{50.0F, 20.0F, -30.0F};
    const auto expected = frozen_lab_to_xyz_d50(lab);
    expect_frozen_d50_bits(d50_lab::lab_to_xyz(lab), expected,
                           {0x3e5ef828U, 0x3e3c9b63U, 0x3e9cf659U});

    constexpr FrozenD50Triplet d50{0.9642F, 1.0F, 0.8249F};
    constexpr float threshold = 0.20689655172413796F;
    constexpr float kappa = 24389.0F / 27.0F;
    const float fy = (lab[0] + 16.0F) / 116.0F;
    const FrozenD50Triplet divided{fy + lab[1] / 500.0F, fy, fy - lab[2] / 200.0F};
    FrozenD50Triplet divide_perturbation{};
    for (std::size_t channel = 0U; channel < divide_perturbation.size(); ++channel)
    {
        const float value = divided[channel] > threshold ?
                                divided[channel] * divided[channel] * divided[channel] :
                                (116.0F * divided[channel] - 16.0F) / kappa;
        divide_perturbation[channel] = d50[channel] * value;
    }
    EXPECT_EQ(d50_triplet_bits(divide_perturbation),
              (std::array<std::uint32_t, 3>{0x3e5ef828U, 0x3e3c9b63U, 0x3e9cf65cU}));
    EXPECT_NE(d50_triplet_bits(divide_perturbation), d50_triplet_bits(expected));
}

TEST(D50LabBridgeTest, ExtendedRoundTripsAndNonFiniteValuesPreserveFrozenClassification)
{
    constexpr FrozenD50Triplet extended_rgb{-0.25F, 0.5F, 1.75F};
    const auto expected_lab = frozen_xyz_d50_to_lab(frozen_linear_rec709_to_xyz_d50(extended_rgb));
    expect_frozen_d50_bits(d50_lab::xyz_to_lab(d50_lab::linear_rec709_to_xyz(extended_rgb)),
                           expected_lab, {0x428c3252U, 0xc19ff315U, 0xc2a7fbbeU});

    constexpr FrozenD50Triplet extended_lab{-25.0F, 120.0F, -90.0F};
    expect_frozen_d50_bits(d50_lab::lab_to_xyz(extended_lab), frozen_lab_to_xyz_d50(extended_lab),
                           {0x3b46abe5U, 0xbce2b9a4U, 0x3d2e846eU});

    struct RoundTripCase
    {
        FrozenD50Triplet input;
        std::array<std::uint32_t, 3> expected;
    };
    const std::array cases{
        RoundTripCase{{0.25F, 0.5F, 0.75F}, {0x3e800006U, 0x3efffffcU, 0x3f400000U}},
        RoundTripCase{extended_rgb, {0xbe7ffffaU, 0x3efffffcU, 0x3fe00002U}},
    };
    for (const auto &[input, expected] : cases)
    {
        const auto oracle = frozen_xyz_d50_to_linear_rec709(
            frozen_lab_to_xyz_d50(frozen_xyz_d50_to_lab(frozen_linear_rec709_to_xyz_d50(input))));
        const auto actual = d50_lab::xyz_to_linear_rec709(
            d50_lab::lab_to_xyz(d50_lab::xyz_to_lab(d50_lab::linear_rec709_to_xyz(input))));
        expect_frozen_d50_cbrt_reference(actual, oracle, expected);
    }

    const std::array nonfinite{std::numeric_limits<float>::quiet_NaN(),
                               std::numeric_limits<float>::infinity(),
                               -std::numeric_limits<float>::infinity()};
    const auto all_nonfinite = [](const FrozenD50Triplet &value)
    {
        return std::ranges::all_of(value,
                                   [](const float sample) { return !std::isfinite(sample); });
    };
    for (const float sample : nonfinite)
    {
        const FrozenD50Triplet value{sample, sample, sample};
        EXPECT_TRUE(all_nonfinite(d50_lab::linear_rec709_to_xyz(value)));
        EXPECT_TRUE(all_nonfinite(d50_lab::xyz_to_linear_rec709(value)));
        EXPECT_TRUE(all_nonfinite(d50_lab::xyz_to_lab(value)));
        EXPECT_TRUE(all_nonfinite(d50_lab::lab_to_xyz(value)));
    }
}

TEST(DtUcsBridgeTest, Cat16DirectionsBasisWhiteBlackAndSourceOrderMatchFrozenBits)
{
    struct MatrixCase
    {
        FrozenD50Triplet input;
        std::array<std::uint32_t, 3> d50_to_d65;
        std::array<std::uint32_t, 3> d65_to_d50;
    };
    const std::array cases{
        MatrixCase{{1.0F, 0.0F, 0.0F},
                   {0x3f7d4da9U, 0xbbb11dffU, 0xb9d3c55cU},
                   {0x3f8163adU, 0x3bb1de8eU, 0x39837366U}},
        MatrixCase{{0.0F, 1.0F, 0.0F},
                   {0xbd23f6fbU, 0x3f80da42U, 0x3c7704b2U},
                   {0x3d26be12U, 0x3f7e5b63U, 0xbc3c486cU}},
        MatrixCase{{0.0F, 0.0F, 1.0F},
                   {0x3d3470f4U, 0xbae61976U, 0x3fa6ab48U},
                   {0xbd0bdb31U, 0x3a978241U, 0x3f44995aU}},
    };
    for (const auto &[input, forward, inverse] : cases)
    {
        expect_frozen_d50_bits(dt_ucs::xyz_d50_to_d65(input), frozen_dt_ucs_xyz_d50_to_d65(input),
                               forward);
        expect_frozen_d50_bits(dt_ucs::xyz_d65_to_d50(input), frozen_dt_ucs_xyz_d65_to_d50(input),
                               inverse);
    }

    constexpr FrozenD50Triplet black{0.0F, 0.0F, 0.0F};
    constexpr FrozenD50Triplet d50_white{0.9642119944211994F, 1.0F, 0.8251882845188288F};
    constexpr FrozenD50Triplet d65_white{0.95047F, 1.0F, 1.08883F};
    expect_frozen_d50_bits(dt_ucs::xyz_d50_to_d65(black), frozen_dt_ucs_xyz_d50_to_d65(black),
                           {0x00000000U, 0x00000000U, 0x00000000U});
    expect_frozen_d50_bits(dt_ucs::xyz_d50_to_d65(d50_white),
                           frozen_dt_ucs_xyz_d50_to_d65(d50_white),
                           {0x3f734be5U, 0x3f800003U, 0x3f8b69d0U});
    expect_frozen_d50_bits(dt_ucs::xyz_d65_to_d50(d65_white),
                           frozen_dt_ucs_xyz_d65_to_d50(d65_white),
                           {0x3f76dd87U, 0x3f7ffffdU, 0x3f532e98U});

    // This extended vector differs by one ULP when the first row is silently
    // contracted/reassociated with FMA. The fixed source-order result also
    // distinguishes the transposed CAT16 owner from the display matrix form.
    constexpr FrozenD50Triplet fma_discriminator{2.05470204F, 1.34176934F, -1.5565666F};
    const auto expected = frozen_dt_ucs_xyz_d50_to_d65(fma_discriminator);
    expect_frozen_d50_bits(dt_ucs::xyz_d50_to_d65(fma_discriminator), expected,
                           {0x3ff49449U, 0x3fabd192U, 0xc0007963U});
    const FrozenD50Triplet contracted{std::fma(0.989466254F, fma_discriminator[0],
                                               std::fma(-0.0400304626F, fma_discriminator[1],
                                                        0.0440530317F * fma_discriminator[2])),
                                      std::fma(-0.00540518733F, fma_discriminator[0],
                                               std::fma(1.00666069F, fma_discriminator[1],
                                                        -0.00175551955F * fma_discriminator[2])),
                                      std::fma(-0.000403920992F, fma_discriminator[0],
                                               std::fma(0.0150768030F, fma_discriminator[1],
                                                        1.30210211F * fma_discriminator[2]))};
    EXPECT_EQ(d50_triplet_bits(contracted),
              (std::array<std::uint32_t, 3>{0x3ff49448U, 0x3fabd192U, 0xc0007963U}));
    EXPECT_NE(d50_triplet_bits(contracted), d50_triplet_bits(expected));
}

TEST(DtUcsBridgeTest, XyYSourceClampZeroAndMinNormalDenominatorsMatchFrozenBits)
{
    constexpr FrozenD50Triplet black{0.0F, 0.0F, 0.0F};
    constexpr FrozenD50Triplet negative_extended{-0.25F, 0.5F, 1.75F};
    expect_frozen_d50_bits(dt_ucs::xyz_d65_to_xyy(black), frozen_dt_ucs_xyz_d65_to_xyy(black),
                           {0x3ea01b86U, 0x3ea8754fU, 0x00000000U});
    expect_frozen_d50_bits(dt_ucs::xyz_d65_to_xyy({1.0F, 0.0F, 0.0F}),
                           frozen_dt_ucs_xyz_d65_to_xyy({1.0F, 0.0F, 0.0F}),
                           {0x3f800000U, 0x00000000U, 0x00000000U});
    expect_frozen_d50_bits(dt_ucs::xyz_d65_to_xyy(negative_extended),
                           frozen_dt_ucs_xyz_d65_to_xyy(negative_extended),
                           {0x00000000U, 0x3e638e39U, 0x3f000000U});

    constexpr float minimum = std::numeric_limits<float>::min();
    const FrozenD50Triplet positive_minimum_y{0.25F, minimum, minimum};
    const FrozenD50Triplet negative_minimum_y{0.25F, -minimum, minimum};
    expect_frozen_d50_bits(dt_ucs::xyy_to_xyz_d65(positive_minimum_y),
                           frozen_dt_ucs_xyy_to_xyz_d65(positive_minimum_y),
                           {0x3e800000U, 0x00800000U, 0x3f400000U});
    expect_frozen_d50_bits(dt_ucs::xyy_to_xyz_d65(negative_minimum_y),
                           frozen_dt_ucs_xyy_to_xyz_d65(negative_minimum_y),
                           {0xbe800000U, 0x00800000U, 0xbf400000U});
    for (const float signed_zero : {0.0F, -0.0F})
    {
        const FrozenD50Triplet xyy{0.25F, signed_zero, 0.5F};
        expect_frozen_d50_bits(dt_ucs::xyy_to_xyz_d65(xyy), frozen_dt_ucs_xyy_to_xyz_d65(xyy),
                               {0x00000000U, 0x00000000U, 0x00000000U});
    }

    const float nan = std::numeric_limits<float>::quiet_NaN();
    expect_frozen_d50_bits(dt_ucs::xyz_d65_to_xyy({nan, 0.0F, 0.0F}),
                           frozen_dt_ucs_xyz_d65_to_xyy({nan, 0.0F, 0.0F}),
                           {0x3ea01b86U, 0x3ea8754fU, 0x00000000U});
    const auto infinite =
        dt_ucs::xyz_d65_to_xyy({std::numeric_limits<float>::infinity(), 0.0F, 0.0F});
    EXPECT_TRUE(std::isnan(infinite[0]));
    EXPECT_EQ(infinite[1], 0.0F);
    EXPECT_EQ(infinite[2], 0.0F);

    // This representable xy pair makes the frozen UVD denominator exactly
    // +0. The source's +FLT_MIN substitution keeps the forward JCH finite.
    constexpr FrozenD50Triplet singular_xyy{0.0F, -0.134407863F, 0.25F};
    const float singular_denominator =
        (0.318707282433486F * singular_xyy[0] + 2.16743692732158F * singular_xyy[1]) +
        0.291320554395942F;
    EXPECT_EQ(std::bit_cast<std::uint32_t>(singular_denominator), 0x00000000U);
    const auto singular_oracle =
        frozen_dt_ucs_xyy_to_jch_oracle(singular_xyy, frozen_dt_ucs_y_to_lightness(1.0F));
    const auto singular_actual = dt_ucs::xyy_to_jch(singular_xyy, dt_ucs::y_to_lightness(1.0F));
    expect_dt_ucs_local_oracle(singular_actual, singular_oracle.jch);
    EXPECT_TRUE(std::ranges::all_of(singular_actual,
                                    [](const float value) { return std::isfinite(value); }));
}

TEST(DtUcsBridgeTest, LightnessJchPowAndAdditionOrderMatchIndependentOracle)
{
    for (const float luminance : {0.0F, 0.18F, 1.0F, 4.0F, 1.0e8F})
    {
        EXPECT_EQ(std::bit_cast<std::uint32_t>(dt_ucs::y_to_lightness(luminance)),
                  std::bit_cast<std::uint32_t>(frozen_dt_ucs_y_to_lightness(luminance)));
    }
    for (const float lightness : {0.0F, 0.25F, 1.0F, 2.0F, 2.09885F})
    {
        EXPECT_EQ(std::bit_cast<std::uint32_t>(dt_ucs::lightness_to_y(lightness)),
                  std::bit_cast<std::uint32_t>(frozen_dt_ucs_lightness_to_y(lightness)));
    }
    const float white_lightness = dt_ucs::y_to_lightness(1.0F);
    EXPECT_NEAR(white_lightness, 0.98805058F, 1.0e-7F);

    constexpr FrozenD50Triplet black_xyy{0.31271F, 0.32902F, 0.0F};
    constexpr FrozenD50Triplet white_xyy{0.31271F, 0.32902F, 1.0F};
    expect_dt_ucs_local_oracle(
        dt_ucs::xyy_to_jch(black_xyy, white_lightness),
        frozen_dt_ucs_xyy_to_jch_oracle(black_xyy, frozen_dt_ucs_y_to_lightness(1.0F)).jch);
    const auto white = dt_ucs::xyy_to_jch(white_xyy, white_lightness);
    const auto white_oracle =
        frozen_dt_ucs_xyy_to_jch_oracle(white_xyy, frozen_dt_ucs_y_to_lightness(1.0F));
    expect_dt_ucs_local_oracle(white, white_oracle.jch);
    EXPECT_EQ(white[0], 1.0F);
    EXPECT_GT(white[1], 0.0F);

    // This source-derived extended vector changes by one ULP if the two powf
    // products and white division are reassociated. We compare production to
    // the independent local-libm oracle, not to a cross-platform decimal bit
    // promise.
    constexpr FrozenD50Triplet pow_order_discriminator{0.71667999F, -0.921235025F, 0.585777998F};
    const auto oracle = frozen_dt_ucs_xyy_to_jch_oracle(pow_order_discriminator, white_lightness);
    EXPECT_NE(std::bit_cast<std::uint32_t>(oracle.source_order_chroma),
              std::bit_cast<std::uint32_t>(oracle.reassociated_chroma));
    expect_dt_ucs_local_oracle(dt_ucs::xyy_to_jch(pow_order_discriminator, white_lightness),
                               oracle.jch);
}

TEST(DtUcsBridgeTest, InverseClampsOnlyFrozenLightnessAndPropagatesOtherInvalidMath)
{
    const float white_lightness = frozen_dt_ucs_y_to_lightness(1.0F);
    constexpr float upper_lightness = 2.09885F;
    const float upper_j = upper_lightness / white_lightness;
    const FrozenD50Triplet below{std::nextafter(upper_j, 0.0F), 0.4F, 0.7F};
    const FrozenD50Triplet exact{upper_j, 0.4F, 0.7F};
    const FrozenD50Triplet above{std::nextafter(upper_j, std::numeric_limits<float>::infinity()),
                                 0.4F, 0.7F};
    const auto below_actual = dt_ucs::jch_to_xyy(below, white_lightness);
    const auto exact_actual = dt_ucs::jch_to_xyy(exact, white_lightness);
    const auto above_actual = dt_ucs::jch_to_xyy(above, white_lightness);
    expect_dt_ucs_local_oracle(below_actual, frozen_dt_ucs_jch_to_xyy(below, white_lightness));
    expect_dt_ucs_local_oracle(exact_actual, frozen_dt_ucs_jch_to_xyy(exact, white_lightness));
    expect_dt_ucs_local_oracle(above_actual, frozen_dt_ucs_jch_to_xyy(above, white_lightness));
    EXPECT_NE(d50_triplet_bits(below_actual), d50_triplet_bits(exact_actual));
    EXPECT_EQ(d50_triplet_bits(exact_actual), d50_triplet_bits(above_actual));

    const auto negative = dt_ucs::jch_to_xyy({-1.0F, 0.4F, 0.7F}, white_lightness);
    expect_dt_ucs_local_oracle(negative,
                               frozen_dt_ucs_jch_to_xyy({-1.0F, 0.4F, 0.7F}, white_lightness));
    EXPECT_EQ(negative[2], 0.0F);

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();
    const auto nan_j = dt_ucs::jch_to_xyy({nan, 0.0F, 0.0F}, white_lightness);
    expect_dt_ucs_local_oracle(nan_j, frozen_dt_ucs_jch_to_xyy({nan, 0.0F, 0.0F}, white_lightness));
    EXPECT_TRUE(std::ranges::all_of(nan_j, [](const float value) { return std::isfinite(value); }));
    const auto nan_chroma = dt_ucs::jch_to_xyy({1.0F, nan, 0.0F}, white_lightness);
    EXPECT_TRUE(std::isnan(nan_chroma[0]));
    EXPECT_TRUE(std::isnan(nan_chroma[1]));
    EXPECT_TRUE(std::isfinite(nan_chroma[2]));
    const auto infinite_hue = dt_ucs::jch_to_xyy({1.0F, 0.4F, infinity}, white_lightness);
    EXPECT_TRUE(std::isnan(infinite_hue[0]));
    EXPECT_TRUE(std::isnan(infinite_hue[1]));
    EXPECT_TRUE(std::isfinite(infinite_hue[2]));
}

TEST(DtUcsBridgeTest, ExtendedRoundTripsAndNonFiniteClassificationFollowFrozenMath)
{
    const float white_lightness = frozen_dt_ucs_y_to_lightness(1.0F);
    const std::array xyy_cases{
        FrozenD50Triplet{0.25F, 0.4F, 0.18F},
        FrozenD50Triplet{1.25F, -0.2F, 4.0F},
        FrozenD50Triplet{-0.4F, 0.8F, 0.75F},
    };
    for (const auto &xyy : xyy_cases)
    {
        const auto jch_oracle = frozen_dt_ucs_xyy_to_jch_oracle(xyy, white_lightness).jch;
        const auto jch = dt_ucs::xyy_to_jch(xyy, white_lightness);
        expect_dt_ucs_local_oracle(jch, jch_oracle);
        const auto actual = dt_ucs::jch_to_xyy(jch, white_lightness);
        const auto oracle = frozen_dt_ucs_jch_to_xyy(jch_oracle, white_lightness);
        expect_dt_ucs_local_oracle(actual, oracle);
        // The frozen forward/inverse fit is not algebraically exact for
        // extended chromaticities; the independent oracle above remains bit
        // exact, while this bound only characterizes its round-trip residual.
        EXPECT_NEAR(actual[0], xyy[0], 1.0e-5F);
        EXPECT_NEAR(actual[1], xyy[1], 1.0e-5F);
        EXPECT_NEAR(actual[2], xyy[2], 1.0e-5F);
    }

    constexpr FrozenD50Triplet extended_xyz{0.1938238604679151F, 0.36766030739017674F,
                                            0.38827863670090734F};
    const auto jch = dt_ucs::xyz_d50_to_jch(extended_xyz, white_lightness);
    expect_dt_ucs_local_oracle(jch, frozen_dt_ucs_xyz_d50_to_jch(extended_xyz, white_lightness));
    const auto roundtrip = dt_ucs::jch_to_xyz_d50(jch, white_lightness);
    expect_dt_ucs_local_oracle(roundtrip, frozen_dt_ucs_jch_to_xyz_d50(jch, white_lightness));
    for (std::size_t channel = 0U; channel < roundtrip.size(); ++channel)
    {
        EXPECT_NEAR(roundtrip[channel], extended_xyz[channel], 3.0e-7F);
    }

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();
    for (const float sample : {nan, infinity, -infinity})
    {
        const FrozenD50Triplet value{sample, sample, sample};
        const auto cat_forward = dt_ucs::xyz_d50_to_d65(value);
        const auto cat_inverse = dt_ucs::xyz_d65_to_d50(value);
        EXPECT_TRUE(std::ranges::all_of(cat_forward,
                                        [](const float item) { return !std::isfinite(item); }));
        EXPECT_TRUE(std::ranges::all_of(cat_inverse,
                                        [](const float item) { return !std::isfinite(item); }));
    }
    EXPECT_TRUE(std::isnan(dt_ucs::y_to_lightness(-1.0F)));
    EXPECT_TRUE(std::isnan(dt_ucs::y_to_lightness(nan)));
    EXPECT_TRUE(std::isnan(dt_ucs::y_to_lightness(infinity)));
    EXPECT_TRUE(std::isnan(dt_ucs::lightness_to_y(-1.0F)));
    EXPECT_TRUE(std::isnan(dt_ucs::lightness_to_y(nan)));

    const auto negative_luminance = dt_ucs::xyy_to_jch({0.25F, 0.4F, -1.0F}, white_lightness);
    EXPECT_TRUE(std::isnan(negative_luminance[0]));
    EXPECT_TRUE(std::isnan(negative_luminance[1]));
    EXPECT_TRUE(std::isfinite(negative_luminance[2]));
}

TEST(DtUcsBridgeTest, CommonXyzBoundaryIsIndependentOfRec709OrRec2020Coordinates)
{
    const auto apply_matrix = [](const std::array<float, 9> &matrix, const FrozenD50Triplet value)
    {
        return FrozenD50Triplet{
            frozen_dt_ucs_matrix_row(matrix[0], value[0], matrix[1], value[1], matrix[2], value[2]),
            frozen_dt_ucs_matrix_row(matrix[3], value[0], matrix[4], value[1], matrix[5], value[2]),
            frozen_dt_ucs_matrix_row(matrix[6], value[0], matrix[7], value[1], matrix[8],
                                     value[2])};
    };
    constexpr std::array<float, 9> rec709_to_xyz_d65{0.4124564F, 0.3575761F, 0.1804375F,
                                                     0.2126729F, 0.7151522F, 0.0721750F,
                                                     0.0193339F, 0.1191920F, 0.9503041F};
    constexpr std::array<float, 9> xyz_d65_to_rec2020{
        1.7166511880F, -0.3556707838F, -0.2533662814F, -0.6666843518F, 1.6164812366F,
        0.0157685458F, 0.0176398574F,  -0.0427706133F, 0.9421031212F};
    constexpr std::array<float, 9> rec2020_to_xyz_d65{0.6369580483F, 0.1446169036F, 0.1688809752F,
                                                      0.2627002120F, 0.6779980715F, 0.0593017165F,
                                                      0.0000000000F, 0.0280726930F, 1.0609850577F};
    constexpr FrozenD50Triplet rec709{0.15F, 0.4F, 0.8F};
    const auto xyz_from_rec709 = apply_matrix(rec709_to_xyz_d65, rec709);
    const auto rec2020 = apply_matrix(xyz_d65_to_rec2020, xyz_from_rec709);
    const auto xyz_from_rec2020 = apply_matrix(rec2020_to_xyz_d65, rec2020);
    for (std::size_t channel = 0U; channel < xyz_from_rec709.size(); ++channel)
    {
        EXPECT_NEAR(xyz_from_rec2020[channel], xyz_from_rec709[channel], 2.0e-7F);
    }

    const float white_lightness = frozen_dt_ucs_y_to_lightness(1.0F);
    const auto d50_from_rec709 = dt_ucs::xyz_d65_to_d50(xyz_from_rec709);
    const auto d50_from_rec2020 = dt_ucs::xyz_d65_to_d50(xyz_from_rec2020);
    const auto rec709_jch = dt_ucs::xyz_d50_to_jch(d50_from_rec709, white_lightness);
    const auto rec2020_jch = dt_ucs::xyz_d50_to_jch(d50_from_rec2020, white_lightness);
    expect_dt_ucs_local_oracle(rec709_jch,
                               frozen_dt_ucs_xyz_d50_to_jch(d50_from_rec709, white_lightness));
    expect_dt_ucs_local_oracle(rec2020_jch,
                               frozen_dt_ucs_xyz_d50_to_jch(d50_from_rec2020, white_lightness));
    for (std::size_t channel = 0U; channel < rec709_jch.size(); ++channel)
    {
        EXPECT_NEAR(rec2020_jch[channel], rec709_jch[channel], 6.0e-7F);
    }
}

TEST(HarmonyGeometryTest, FullTablesMatchIndependentOracleAndReferenceInvariants)
{
    // The oracle is a scalar transcription of colorharmonizer.c's 16-step
    // gamut search, D65 sRGB bridge, HCV conversion, Gossett knots, and strict
    // nearest inverse scan. It never calls a production harmony helper.
    const auto oracle = frozen_harmony_tables();
    const std::uint64_t forward_hash = frozen_harmony_table_hash(oracle.ucs_to_ryb);
    const std::uint64_t inverse_hash = frozen_harmony_table_hash(oracle.ryb_to_ucs);

    const auto actual = harmony_geometry::build_harmony_hue_tables();
    EXPECT_EQ(frozen_harmony_table_hash(actual.ucs_to_ryb), forward_hash);
    EXPECT_EQ(frozen_harmony_table_hash(actual.ryb_to_ucs), inverse_hash);
    for (std::size_t index = 0U; index < harmony_geometry::kHueTableSteps; ++index)
    {
        EXPECT_EQ(std::bit_cast<std::uint32_t>(actual.ucs_to_ryb[index]),
                  std::bit_cast<std::uint32_t>(oracle.ucs_to_ryb[index]));
        EXPECT_EQ(std::bit_cast<std::uint32_t>(actual.ryb_to_ucs[index]),
                  std::bit_cast<std::uint32_t>(oracle.ryb_to_ucs[index]));
    }

    // These deliberate oracle perturbations prove that the table hash catches
    // the frozen search count, transposed matrix orientation, and RYB knot
    // constants rather than merely hashing an arbitrary smooth curve.
    EXPECT_NE(frozen_harmony_table_hash(frozen_harmony_forward_table(15)), forward_hash);
    EXPECT_NE(frozen_harmony_table_hash(frozen_harmony_forward_table(16, true)), forward_hash);
    EXPECT_NE(frozen_harmony_table_hash(
                  frozen_harmony_forward_table(16, false, std::nextafter(0.472217F, 1.0F))),
              forward_hash);

    // Frozen CLAMP routes NaN to zero even though valid table construction does
    // not normally feed non-finite swatch samples.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(std::bit_cast<std::uint32_t>(frozen_harmony_clamp01(nan)), 0x00000000U);

    // The legacy conditional transfer curve is lazy: negative linear sRGB
    // takes the toe and must not evaluate powf on the discarded branch.
    const float white_lightness = frozen_dt_ucs_y_to_lightness(1.0F);
    constexpr FrozenD50Triplet extended_jch{0.65F, 2.0F, 0.0F};
    const auto extended_linear = frozen_harmony_xyz_d65_to_linear_rec709(
        frozen_dt_ucs_xyy_to_xyz_d65(frozen_dt_ucs_jch_to_xyy(extended_jch, white_lightness)));
    ASSERT_TRUE(
        std::ranges::any_of(extended_linear, [](const float value) { return value < 0.0F; }));
    std::feclearexcept(FE_ALL_EXCEPT);
    static_cast<void>(frozen_harmony_jch_to_srgb(extended_jch, white_lightness));
    EXPECT_EQ(std::fetestexcept(FE_INVALID), 0);
    std::feclearexcept(FE_ALL_EXCEPT);
    static_cast<void>(harmony_geometry::build_harmony_hue_tables());
    EXPECT_EQ(std::fetestexcept(FE_INVALID), 0);

    EXPECT_EQ(std::bit_cast<std::uint32_t>(frozen_harmony_rgb_hue_to_ryb(0.0F)), 0x00000000U);
    EXPECT_EQ(std::bit_cast<std::uint32_t>(frozen_harmony_rgb_hue_to_ryb(1.0F / 6.0F)),
              std::bit_cast<std::uint32_t>(1.0F / 3.0F));
    EXPECT_EQ(std::bit_cast<std::uint32_t>(frozen_harmony_rgb_hue_to_ryb(2.0F / 6.0F)),
              std::bit_cast<std::uint32_t>(0.472217F));
    EXPECT_EQ(std::bit_cast<std::uint32_t>(frozen_harmony_rgb_hue_to_ryb(5.0F / 6.0F)),
              std::bit_cast<std::uint32_t>(5.0F / 6.0F));
    EXPECT_EQ(std::bit_cast<std::uint32_t>(frozen_harmony_rgb_hue_to_ryb(1.0F)), 0x00000000U);

    const float transfer_input = 0.045F;
    EXPECT_NE(std::bit_cast<std::uint32_t>(frozen_harmony_srgb_to_linear(transfer_input)),
              std::bit_cast<std::uint32_t>(frozen_harmony_srgb_to_linear(transfer_input, 0.05F)));
}

TEST(ColorHarmonizerTest, Real0176StatesMatchIndependentSourceOrderReferences)
{
    const auto tables = frozen_harmony_tables();
    const WorkingImage input = color_harmonizer_working_fixture();
    const WorkingImage original = input;
    const std::array cases{ColorHarmonizerParams{}, frozen_color_harmonizer_0176_record13()};
    const std::array<std::array<std::array<std::uint32_t, 3>, 4>, 2> goldens{{
        {{{1022738880U, 1043878391U, 1060655590U},
          {1063843274U, 1054280255U, 1032805390U},
          {3039821824U, 1056964611U, 1071225240U},
          {0U, 0U, 0U}}},
        {{{3202216542U, 1051481554U, 1057828450U},
          {1066933552U, 1051086807U, 1044202570U},
          {3212784549U, 1062758668U, 1067310102U},
          {0U, 0U, 0U}}},
    }};
    for (std::size_t case_index = 0U; case_index < cases.size(); ++case_index)
    {
        auto actual = apply_color_harmonizer(input, cases[case_index], CancellationToken{});
        ASSERT_TRUE(actual) << actual.error().message;
        ASSERT_EQ(actual.value().rgb.size(), input.rgb.size());
        for (std::size_t pixel = 0U; pixel < input.rgb.size() / 3U; ++pixel)
        {
            const std::size_t index = pixel * 3U;
            const FrozenD50Triplet source{input.rgb[index], input.rgb[index + 1U],
                                          input.rgb[index + 2U]};
            const auto oracle = frozen_color_harmonizer_rgb(
                cases[case_index], source, input.color_profile.matrix_to_xyz_d50, tables);
            const FrozenD50Triplet produced{actual.value().rgb[index],
                                            actual.value().rgb[index + 1U],
                                            actual.value().rgb[index + 2U]};
            EXPECT_EQ(d50_triplet_bits(produced), d50_triplet_bits(oracle));
            for (std::size_t channel = 0U; channel < produced.size(); ++channel)
            {
                if (goldens[case_index][pixel][channel] == 0U)
                {
                    EXPECT_EQ(std::bit_cast<std::uint32_t>(produced[channel]),
                              goldens[case_index][pixel][channel]);
                }
                else
                {
                    EXPECT_NEAR(produced[channel],
                                std::bit_cast<float>(goldens[case_index][pixel][channel]),
                                kPlatformLibmReferenceTolerance);
                }
            }
        }
        EXPECT_EQ(actual.value().color_profile, input.color_profile);
        EXPECT_EQ(actual.value().exposure_analysis, input.exposure_analysis);
        EXPECT_NE(actual.value().rgb.data(), input.rgb.data());
        EXPECT_NE(actual.value().color_profile.icc_bytes.data(),
                  input.color_profile.icc_bytes.data());
        actual.value().rgb.front() = 99.0F;
        actual.value().color_profile.icc_bytes.front() = 99U;
        EXPECT_EQ(input.rgb, original.rgb);
        EXPECT_EQ(input.color_profile, original.color_profile);
    }
    const FrozenD50Triplet clipped_input{-0.25F, 0.5F, 1.7F};
    const auto clipped = frozen_color_harmonizer_rgb(cases.back(), clipped_input,
                                                     input.color_profile.matrix_to_xyz_d50, tables);
    const auto no_clip = frozen_color_harmonizer_rgb(
        cases.back(), clipped_input, input.color_profile.matrix_to_xyz_d50, tables, true);
    EXPECT_NE(d50_triplet_bits(no_clip), d50_triplet_bits(clipped))
        << "the extended fixture must detect omission of frozen fmaxf clipping";
    const FrozenD50Triplet neutral_input{0.03F, 0.18F, 0.72F};
    const auto cubic_neutral = frozen_color_harmonizer_rgb(
        cases.back(), neutral_input, input.color_profile.matrix_to_xyz_d50, tables);
    const auto linear_neutral = frozen_color_harmonizer_rgb(
        cases.back(), neutral_input, input.color_profile.matrix_to_xyz_d50, tables, false, true);
    EXPECT_NE(d50_triplet_bits(linear_neutral), d50_triplet_bits(cubic_neutral))
        << "the fixture must detect changing the frozen cubic neutral protection";
    EXPECT_EQ(input.rgb, original.rgb);
    EXPECT_EQ(input.color_profile, original.color_profile);
    EXPECT_EQ(input.exposure_analysis, original.exposure_analysis);
}

TEST(ColorHarmonizerTest, EveryPredefinedRuleAndCustomNodeCountUseCanonicalDispatch)
{
    const auto tables = frozen_harmony_tables();
    const auto production_tables = harmony_geometry::build_harmony_hue_tables();
    auto input = color_harmonizer_working_fixture();
    input.width = 1U;
    input.rgb.resize(3U);
    for (std::size_t rule_index = 0U; rule_index <= 9U; ++rule_index)
    {
        ColorHarmonizerParams params = frozen_color_harmonizer_0176_record13();
        params.rule = static_cast<ColorHarmonizerRule>(rule_index);
        if (params.rule == ColorHarmonizerRule::kCustom)
        {
            params.custom_hue = {0.03, 0.29, 0.61, 0.87};
        }
        const std::array node_counts = params.rule == ColorHarmonizerRule::kCustom ?
                                           std::array<std::int64_t, 3>{2, 3, 4} :
                                           std::array<std::int64_t, 3>{4, 4, 4};
        const std::size_t count = params.rule == ColorHarmonizerRule::kCustom ? 3U : 1U;
        for (std::size_t node_case = 0U; node_case < count; ++node_case)
        {
            params.num_custom_nodes = node_counts[node_case];
            const auto oracle_nodes = frozen_color_harmonizer_nodes(params, tables);
            std::array<float, 4> production_nodes{};
            std::size_t production_count = 0U;
            if (params.rule == ColorHarmonizerRule::kCustom)
            {
                production_count = static_cast<std::size_t>(params.num_custom_nodes);
                for (std::size_t index = 0U; index < production_count; ++index)
                {
                    production_nodes[index] = static_cast<float>(params.custom_hue[index]);
                }
            }
            else
            {
                const auto nodes = harmony_geometry::predefined_harmony_nodes(
                    static_cast<harmony_geometry::StandardRule>(params.rule),
                    static_cast<float>(params.anchor_hue), production_tables);
                ASSERT_TRUE(nodes) << nodes.error().message;
                production_nodes = nodes.value().hues;
                production_count = nodes.value().count;
            }
            EXPECT_EQ(production_count, oracle_nodes.count);
            for (std::size_t index = 0U; index < production_count; ++index)
            {
                EXPECT_EQ(std::bit_cast<std::uint32_t>(production_nodes[index]),
                          std::bit_cast<std::uint32_t>(oracle_nodes.hues[index]));
            }
            const FrozenD50Triplet clipped{std::fmax(input.rgb[0], 0.0F),
                                           std::fmax(input.rgb[1], 0.0F),
                                           std::fmax(input.rgb[2], 0.0F)};
            const auto xyz =
                frozen_color_harmonizer_matrix(input.color_profile.matrix_to_xyz_d50, clipped);
            const float white_lightness = frozen_dt_ucs_y_to_lightness(1.0F);
            const auto production_jch = dt_ucs::xyz_d50_to_jch(xyz, white_lightness);
            const auto oracle_jch = frozen_dt_ucs_xyz_d50_to_jch(xyz, white_lightness);
            EXPECT_EQ(d50_triplet_bits(production_jch), d50_triplet_bits(oracle_jch));
            constexpr float pi = 3.14159265358979323846F;
            constexpr float two_pi = 6.28318530717958647693F;
            const float hue = (oracle_jch[2] + pi) / two_pi;
            const auto production_attraction = harmony_geometry::harmony_attraction(
                hue, std::span<const float>(production_nodes.data(), production_count),
                static_cast<float>(params.pull_width));
            ASSERT_TRUE(production_attraction) << production_attraction.error().message;
            const auto oracle_attraction = frozen_harmony_attraction(
                hue, std::span<const float>(oracle_nodes.hues.data(), oracle_nodes.count),
                static_cast<float>(params.pull_width));
            EXPECT_EQ(production_attraction.value().winning_index, oracle_attraction.winning_index);
            EXPECT_EQ(std::bit_cast<std::uint32_t>(production_attraction.value().weight),
                      std::bit_cast<std::uint32_t>(oracle_attraction.weight));
            EXPECT_EQ(std::bit_cast<std::uint32_t>(production_attraction.value().shift),
                      std::bit_cast<std::uint32_t>(oracle_attraction.shift));
            const float neutral = static_cast<float>(params.neutral_protection);
            const float neutral_squared = neutral * neutral;
            const float neutral_cubed = neutral_squared * neutral;
            const float cutoff = neutral_cubed * 0.03F;
            const float denominator = (production_jch[1] + cutoff) + 1.0e-5F;
            const float chroma_weight = production_jch[1] / denominator;
            const float saturation_delta =
                (static_cast<float>(
                     params.node_saturation[production_attraction.value().winning_index]) -
                 1.0F) *
                production_attraction.value().weight;
            auto corrected_jch = production_jch;
            float corrected_hue =
                std::fmod(hue + production_attraction.value().shift *
                                    static_cast<float>(params.pull_strength) * chroma_weight,
                          1.0F);
            if (corrected_hue < 0.0F)
            {
                corrected_hue += 1.0F;
            }
            corrected_jch[2] = corrected_hue * two_pi - pi;
            corrected_jch[1] =
                std::fmax(production_jch[1] * (1.0F + saturation_delta * chroma_weight), 0.0F);
            const auto production_xyz = dt_ucs::jch_to_xyz_d50(corrected_jch, white_lightness);
            const auto oracle_xyz = frozen_dt_ucs_jch_to_xyz_d50(corrected_jch, white_lightness);
            EXPECT_EQ(d50_triplet_bits(production_xyz), d50_triplet_bits(oracle_xyz));
            const auto direct = apply_color_harmonizer(input, params, CancellationToken{});
            ASSERT_TRUE(direct) << direct.error().message;
            const auto oracle =
                frozen_color_harmonizer_rgb(params, {input.rgb[0], input.rgb[1], input.rgb[2]},
                                            input.color_profile.matrix_to_xyz_d50, tables);
            EXPECT_EQ(d50_triplet_bits(
                          {direct.value().rgb[0], direct.value().rgb[1], direct.value().rgb[2]}),
                      d50_triplet_bits(oracle));
            const auto parameters = color_harmonizer_to_parameters(params);
            ASSERT_TRUE(parameters) << parameters.error().message;
            Recipe recipe;
            recipe.operations.push_back(
                {std::string(kColorHarmonizerOperationId), kColorHarmonizerOperationSchemaVersion,
                 "colorharmonizer-dispatch", true, parameters.value(), std::nullopt});
            const auto dispatched = apply_recipe_ops(input, recipe, CancellationToken{});
            ASSERT_TRUE(dispatched) << dispatched.error().message;
            EXPECT_EQ(dispatched.value().rgb, direct.value().rgb);
        }
    }
}

TEST(ColorHarmonizerTest, InvalidStatesAndUnknownScaleFailAtomicallyBeforePublication)
{
    WorkingImage input = color_harmonizer_working_fixture();
    const WorkingImage original = input;
    ColorHarmonizerParams params = frozen_color_harmonizer_0176_record13();
    params.smoothing = 0.01;
    auto rejected = apply_color_harmonizer(input, params, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kValidation);
    EXPECT_EQ(rejected.error().context.at("reason"), "invalid_colorharmonizer_roi_scale");
    EXPECT_EQ(input.rgb, original.rgb);
    input.canonical_roi_scale = CanonicalRoiScale::from_scaled_dimensions(4U, 1U, 4U, 1U);
    ASSERT_TRUE(input.canonical_roi_scale.valid());
    const auto smoothed = apply_color_harmonizer(input, params, CancellationToken{});
    ASSERT_TRUE(smoothed) << smoothed.error().message;
    EXPECT_EQ(smoothed.value().canonical_roi_scale.value(), input.canonical_roi_scale.value());

    params.smoothing = 0.0;
    auto parameters = color_harmonizer_to_parameters(params);
    ASSERT_TRUE(parameters) << parameters.error().message;
    OperationInstance operation{std::string(kColorHarmonizerOperationId),
                                kColorHarmonizerOperationSchemaVersion,
                                "colorharmonizer-mask",
                                true,
                                parameters.value(),
                                "mask-1"};
    rejected = apply_color_harmonizer(input, operation, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(rejected.error().context.at("reason"), "colorharmonizer_mask_graph_unavailable");
    operation.mask_id.reset();
    operation.schema_version += 1;
    rejected = apply_color_harmonizer(input, operation, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kUnsupported);
    operation.schema_version = kColorHarmonizerOperationSchemaVersion;
    operation.id = "ravo.color.colorcontrast";
    rejected = apply_color_harmonizer(input, operation, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kValidation);

    const auto expect_reason = [&](WorkingImage invalid, const std::string_view reason)
    {
        const auto result = apply_color_harmonizer(invalid, params, CancellationToken{});
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().context.at("reason"), reason);
    };
    auto invalid = input;
    invalid.width = 0U;
    expect_reason(invalid, "invalid_colorharmonizer_dimensions");
    invalid = input;
    invalid.rgb.pop_back();
    expect_reason(invalid, "invalid_colorharmonizer_buffer");
    invalid = input;
    invalid.width = std::numeric_limits<std::uint32_t>::max();
    invalid.height = std::numeric_limits<std::uint32_t>::max();
    invalid.rgb.clear();
    expect_reason(invalid, "invalid_colorharmonizer_buffer");
    invalid = input;
    invalid.color_profile.kind = ColorProfileKind::kMissing;
    expect_reason(invalid, "unsupported_colorharmonizer_working_space");
    invalid = input;
    invalid.color_profile.model = ColorModel::kLab;
    expect_reason(invalid, "unsupported_colorharmonizer_working_space");
    invalid = input;
    invalid.color_profile.has_matrix = false;
    expect_reason(invalid, "unsupported_colorharmonizer_working_space");
    invalid = input;
    invalid.color_profile.matrix_to_xyz_d50[0] = std::numeric_limits<float>::quiet_NaN();
    expect_reason(invalid, "invalid_colorharmonizer_profile_matrix");
    invalid = input;
    invalid.color_profile.matrix_to_xyz_d50.fill(0.0F);
    expect_reason(invalid, "invalid_colorharmonizer_profile_matrix");
    for (const float sample :
         {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
          -std::numeric_limits<float>::infinity()})
    {
        invalid = input;
        invalid.rgb[1] = sample;
        expect_reason(invalid, "nonfinite_colorharmonizer_input");
    }
    invalid = input;
    invalid.rgb = {std::numeric_limits<float>::max(),
                   std::numeric_limits<float>::max(),
                   std::numeric_limits<float>::max(),
                   0.91F,
                   0.42F,
                   0.07F,
                   -0.25F,
                   0.5F,
                   1.7F,
                   0.0F,
                   0.0F,
                   0.0F};
    rejected = apply_color_harmonizer(invalid, params, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "invalid_colorharmonizer_geometry");

    CancellationSource cancelled;
    ASSERT_TRUE(cancelled.cancel("colorharmonizer-pre"));
    rejected = apply_color_harmonizer(input, params, cancelled.token());
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kCancelled);
    EXPECT_EQ(input.rgb, original.rgb);
    EXPECT_EQ(input.color_profile, original.color_profile);
    EXPECT_EQ(input.exposure_analysis, original.exposure_analysis);
}

TEST(ColorHarmonizerSmoothingTest, MatchesIndependentTwoPassOracleAtFullAndDownscaledRoiScale)
{
    auto input = color_harmonizer_working_fixture();
    ColorHarmonizerParams params = frozen_color_harmonizer_0176_record13();
    params.smoothing = 0.5;
    params.pull_width = 0.25; // freezes the fmaxf(1, pull_width) sigma floor.
    const auto tables = frozen_harmony_tables();
    const auto run = [&](const CanonicalRoiScale scale)
    {
        input.canonical_roi_scale = scale;
        EXPECT_TRUE(input.canonical_roi_scale.valid());
        if (!input.canonical_roi_scale.valid())
        {
            return std::vector<float>{};
        }
        const float sigma = static_cast<float>(params.smoothing) *
                            std::fmax(1.5F, 8.0F * scale.value()) *
                            std::fmax(1.0F, static_cast<float>(params.pull_width));
        const auto expected =
            frozen_color_harmonizer_two_pass(input, params, scale.value(), tables);
        const auto actual = apply_color_harmonizer(input, params, CancellationToken{});
        EXPECT_TRUE(actual) << (actual ? "" : actual.error().message);
        if (!actual)
        {
            return std::vector<float>{};
        }
        EXPECT_FLOAT_EQ(sigma, scale.value() == 1.0F ? 4.0F : 2.0F);
        EXPECT_EQ(actual.value().rgb.size(), expected.size());
        for (std::size_t index = 0U; index < expected.size(); ++index)
        {
            EXPECT_NEAR(actual.value().rgb[index], expected[index], kPlatformLibmReferenceTolerance)
                << index;
        }
        EXPECT_EQ(actual.value().color_profile, input.color_profile);
        EXPECT_EQ(actual.value().exposure_analysis, input.exposure_analysis);
        EXPECT_EQ(actual.value().canonical_roi_scale.value(), scale.value());
        EXPECT_NE(actual.value().rgb.data(), input.rgb.data());
        EXPECT_NE(actual.value().color_profile.icc_bytes.data(),
                  input.color_profile.icc_bytes.data());
        return actual.value().rgb;
    };

    const auto full = run(CanonicalRoiScale::from_scaled_dimensions(4U, 1U, 4U, 1U));
    const auto downscaled = run(CanonicalRoiScale::from_scaled_dimensions(4U, 1U, 8U, 2U));
    EXPECT_NE(full, downscaled);
    EXPECT_EQ(input.rgb, color_harmonizer_working_fixture().rgb);
}

TEST(ColorHarmonizerSmoothingTest, ZeroScaleMetadataDoesNotAffectZeroButPositiveFails)
{
    auto input = color_harmonizer_working_fixture();
    const auto zero_without_scale =
        apply_color_harmonizer(input, ColorHarmonizerParams{}, CancellationToken{});
    ASSERT_TRUE(zero_without_scale) << zero_without_scale.error().message;
    input.canonical_roi_scale = CanonicalRoiScale::from_scaled_dimensions(4U, 1U, 4U, 1U);
    const auto zero_with_scale =
        apply_color_harmonizer(input, ColorHarmonizerParams{}, CancellationToken{});
    ASSERT_TRUE(zero_with_scale) << zero_with_scale.error().message;
    EXPECT_EQ(zero_without_scale.value().rgb, zero_with_scale.value().rgb);

    input.canonical_roi_scale = {};
    ColorHarmonizerParams positive = frozen_color_harmonizer_0176_record13();
    positive.smoothing = 0.25;
    const auto rejected = apply_color_harmonizer(input, positive, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "invalid_colorharmonizer_roi_scale");
}

TEST(ColorHarmonizerSmoothingTest,
     ControlledCancellationAtMapGaussianApplyAndPublicationNeverPublishes)
{
    auto input = color_harmonizer_working_fixture();
    input.width = 128U;
    input.height = 2U;
    input.rgb.assign(static_cast<std::size_t>(input.width) * input.height * 3U, 0.25F);
    input.canonical_roi_scale = CanonicalRoiScale::from_scaled_dimensions(128U, 2U, 128U, 2U);
    const auto original = input;
    ColorHarmonizerParams params = frozen_color_harmonizer_0176_record13();
    params.smoothing = 0.25;
    const std::array checkpoints{detail::ColorHarmonizerCheckpoint::kBeforeValidation,
                                 detail::ColorHarmonizerCheckpoint::kMapChunk,
                                 detail::ColorHarmonizerCheckpoint::kGaussian,
                                 detail::ColorHarmonizerCheckpoint::kApplyChunk,
                                 detail::ColorHarmonizerCheckpoint::kBeforePublication};
    for (const auto checkpoint : checkpoints)
    {
        CancellationSource cancellation;
        ColorHarmonizerCancellationFixture fixture{&cancellation, checkpoint};
        const auto rejected = detail::apply_color_harmonizer_controlled(
            input, params, cancellation.token(), {&fixture, cancel_color_harmonizer});
        ASSERT_FALSE(rejected);
        EXPECT_TRUE(fixture.fired);
        EXPECT_EQ(rejected.error().code, ErrorCode::kCancelled);
        EXPECT_EQ(input.rgb, original.rgb);
        EXPECT_EQ(input.color_profile, original.color_profile);
        EXPECT_EQ(input.exposure_analysis, original.exposure_analysis);
        EXPECT_EQ(input.canonical_roi_scale.value(), original.canonical_roi_scale.value());
    }
}

TEST(ColorHarmonizerSmoothingTest, RawMemoryEstimateUsesS2Point2OwnerAndSaturates)
{
    DecodedRaw raw;
    raw.width = 8U;
    raw.height = 4U;
    raw.pixels.assign(32U, 0U);
    Recipe recipe;
    const auto baseline = estimate_raw_render_memory(raw, recipe, 8U, 4U);
    ColorHarmonizerParams params = frozen_color_harmonizer_0176_record13();
    params.smoothing = 0.5;
    auto serialized = color_harmonizer_to_parameters(params);
    ASSERT_TRUE(serialized) << serialized.error().message;
    recipe.operations.push_back({std::string(kColorHarmonizerOperationId),
                                 kColorHarmonizerOperationSchemaVersion, "smoothing", true,
                                 std::move(serialized).value(), std::nullopt});
    const auto estimated = estimate_raw_render_memory(raw, recipe, 8U, 4U);
    const std::uint64_t expected =
        8U * 4U * 3U * sizeof(float) + detail::recursive_gaussian_zero_2c_bytes(8U, 4U);
    EXPECT_EQ(estimated - baseline, expected);
    EXPECT_EQ(estimate_raw_render_memory(raw, recipe, std::numeric_limits<std::uint32_t>::max(),
                                         std::numeric_limits<std::uint32_t>::max()),
              std::numeric_limits<std::uint64_t>::max());
}

TEST(ColorHarmonizerTest, RowCancellationAndDisabledCopyPreserveSourceOwnership)
{
    auto input = color_harmonizer_working_fixture();
    input.width = 1024U;
    input.height = 4096U;
    input.rgb.assign(static_cast<std::size_t>(input.width) * input.height * 3U, 0.25F);
    const float first = input.rgb.front();
    const float last = input.rgb.back();
    ColorHarmonizerParams params = frozen_color_harmonizer_0176_record13();
    params.rule = ColorHarmonizerRule::kCustom;
    const auto deadline = CancellationSource::with_deadline(std::chrono::steady_clock::now() +
                                                            std::chrono::milliseconds{1});
    const auto cancelled = apply_color_harmonizer(input, params, deadline.token());
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, ErrorCode::kCancelled);
    EXPECT_FLOAT_EQ(input.rgb.front(), first);
    EXPECT_FLOAT_EQ(input.rgb.back(), last);

    const auto parameters = color_harmonizer_to_parameters(params);
    ASSERT_TRUE(parameters) << parameters.error().message;
    OperationInstance disabled{std::string(kColorHarmonizerOperationId),
                               kColorHarmonizerOperationSchemaVersion,
                               "colorharmonizer-disabled",
                               false,
                               parameters.value(),
                               std::nullopt};
    auto small = color_harmonizer_working_fixture();
    const auto copied = apply_color_harmonizer(small, disabled, CancellationToken{});
    ASSERT_TRUE(copied) << copied.error().message;
    EXPECT_EQ(copied.value().rgb, small.rgb);
    EXPECT_NE(copied.value().rgb.data(), small.rgb.data());
    EXPECT_EQ(copied.value().color_profile, small.color_profile);
    EXPECT_NE(copied.value().color_profile.icc_bytes.data(), small.color_profile.icc_bytes.data());
}

TEST(HarmonyGeometryTest, ValidatedLookupsCoverCircularSeamsAndRejectInvalidHues)
{
    const auto oracle = frozen_harmony_tables();
    const auto tables = harmony_geometry::build_harmony_hue_tables();
    const std::array hues{0.0F, 1.0F / 720.0F, 719.5F / 720.0F, std::nextafter(1.0F, 0.0F), 1.0F};
    for (const float hue : hues)
    {
        const auto forward = harmony_geometry::ucs_to_ryb_hue(tables, hue);
        ASSERT_TRUE(forward.has_value());
        EXPECT_EQ(std::bit_cast<std::uint32_t>(forward.value()),
                  std::bit_cast<std::uint32_t>(frozen_harmony_lookup(oracle.ucs_to_ryb, hue)));
        const auto inverse = harmony_geometry::ryb_to_ucs_hue(tables, hue);
        ASSERT_TRUE(inverse.has_value());
        EXPECT_EQ(std::bit_cast<std::uint32_t>(inverse.value()),
                  std::bit_cast<std::uint32_t>(frozen_harmony_lookup(oracle.ryb_to_ucs, hue)));
    }

    FrozenHarmonyHueTable seam{};
    seam.fill(0.25F);
    seam[719] = 0.99F;
    seam[0] = 0.01F;
    const harmony_geometry::HarmonyHueTables seam_tables{seam, seam};
    const auto seam_result = harmony_geometry::ucs_to_ryb_hue(seam_tables, 719.5F / 720.0F);
    ASSERT_TRUE(seam_result.has_value());
    const float seam_oracle = frozen_harmony_lookup(seam, 719.5F / 720.0F);
    EXPECT_EQ(std::bit_cast<std::uint32_t>(seam_oracle), 0x31a00000U);
    EXPECT_EQ(std::bit_cast<std::uint32_t>(seam_result.value()),
              std::bit_cast<std::uint32_t>(seam_oracle));

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();
    for (const float invalid : {-std::numeric_limits<float>::min(), std::nextafter(1.0F, infinity),
                                nan, infinity, -infinity})
    {
        const auto forward = harmony_geometry::ucs_to_ryb_hue(tables, invalid);
        ASSERT_FALSE(forward.has_value());
        EXPECT_EQ(forward.error().code, ErrorCode::kValidation);
        EXPECT_EQ(forward.error().message, "invalid_harmony_hue");
        const auto inverse = harmony_geometry::ryb_to_ucs_hue(tables, invalid);
        ASSERT_FALSE(inverse.has_value());
        EXPECT_EQ(inverse.error().code, ErrorCode::kValidation);
        EXPECT_EQ(inverse.error().message, "invalid_harmony_hue");
    }
}

TEST(HarmonyGeometryTest, InverseBuilderUsesCircularDistanceAndStrictFirstTie)
{
    FrozenHarmonyHueTable tied{};
    tied.fill(0.75F);
    tied[11] = 0.125F;
    tied[19] = 0.375F;
    const auto tied_inverse = harmony_geometry::build_ryb_to_ucs_table(tied);
    EXPECT_EQ(std::bit_cast<std::uint32_t>(tied_inverse[180]),
              std::bit_cast<std::uint32_t>(11.0F / 720.0F));

    FrozenHarmonyHueTable circular{};
    circular.fill(0.5F);
    circular[3] = 0.984375F;
    circular[4] = 0.125F;
    const auto circular_inverse = harmony_geometry::build_ryb_to_ucs_table(circular);
    EXPECT_EQ(std::bit_cast<std::uint32_t>(circular_inverse[0]),
              std::bit_cast<std::uint32_t>(3.0F / 720.0F));

    const auto oracle = frozen_harmony_inverse_table(tied);
    EXPECT_EQ(tied_inverse, oracle);
}

TEST(HarmonyGeometryTest, PredefinedRulesMatchSectorGeometryWrapAndDegreeRounding)
{
    constexpr std::array<std::size_t, 9> counts{1U, 3U, 4U, 2U, 3U, 2U, 3U, 4U, 4U};
    const auto oracle_tables = frozen_harmony_tables();
    const auto tables = harmony_geometry::build_harmony_hue_tables();
    for (std::size_t rule_index = 0U; rule_index < counts.size(); ++rule_index)
    {
        const auto rule = static_cast<harmony_geometry::StandardRule>(rule_index);
        for (const float anchor : {0.0F, 0.1F, 0.499F, 0.55F, 1.0F})
        {
            const auto oracle = frozen_predefined_harmony_nodes(rule, anchor, oracle_tables);
            const auto actual = harmony_geometry::predefined_harmony_nodes(rule, anchor, tables);
            ASSERT_TRUE(actual.has_value());
            EXPECT_EQ(actual.value().count, counts[rule_index]);
            for (std::size_t node = 0U; node < oracle.count; ++node)
            {
                EXPECT_EQ(std::bit_cast<std::uint32_t>(actual.value().hues[node]),
                          std::bit_cast<std::uint32_t>(oracle.hues[node]));
            }
        }
    }

    FrozenHarmonyHueTable identity{};
    for (std::size_t index = 0U; index < identity.size(); ++index)
    {
        identity[index] = static_cast<float>(index) / static_cast<float>(identity.size());
    }
    const harmony_geometry::HarmonyHueTables identity_tables{identity, identity};
    const auto wrapped = harmony_geometry::predefined_harmony_nodes(
        harmony_geometry::StandardRule::kAnalogous, 0.0F, identity_tables);
    ASSERT_TRUE(wrapped.has_value());
    ASSERT_EQ(wrapped.value().count, 3U);
    EXPECT_EQ(std::bit_cast<std::uint32_t>(wrapped.value().hues[0]),
              std::bit_cast<std::uint32_t>(11.0F / 12.0F));

    const float below_degree = 0.499F / 360.0F;
    const float above_degree = 0.501F / 360.0F;
    const auto below = harmony_geometry::predefined_harmony_nodes(
        harmony_geometry::StandardRule::kMonochromatic, below_degree, identity_tables);
    const auto above = harmony_geometry::predefined_harmony_nodes(
        harmony_geometry::StandardRule::kMonochromatic, above_degree, identity_tables);
    ASSERT_TRUE(below.has_value());
    ASSERT_TRUE(above.has_value());
    EXPECT_EQ(std::bit_cast<std::uint32_t>(below.value().hues[0]), 0x00000000U);
    EXPECT_EQ(std::bit_cast<std::uint32_t>(above.value().hues[0]),
              std::bit_cast<std::uint32_t>(1.0F / 360.0F));

    const auto invalid_rule = harmony_geometry::predefined_harmony_nodes(
        static_cast<harmony_geometry::StandardRule>(9U), 0.1F, tables);
    ASSERT_FALSE(invalid_rule.has_value());
    EXPECT_EQ(invalid_rule.error().code, ErrorCode::kValidation);
    EXPECT_EQ(invalid_rule.error().message, "invalid_harmony_rule");
    for (const float invalid_anchor :
         {-std::numeric_limits<float>::min(),
          std::nextafter(1.0F, std::numeric_limits<float>::infinity()),
          std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()})
    {
        const auto result = harmony_geometry::predefined_harmony_nodes(
            harmony_geometry::StandardRule::kComplementary, invalid_anchor, tables);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, ErrorCode::kValidation);
        EXPECT_EQ(result.error().message, "invalid_harmony_hue");
    }
}

TEST(HarmonyGeometryTest, AttractionMatchesWinnerCircularTieUnderflowAndPullWidth)
{
    const auto expect_oracle =
        [](const float pixel_hue, const std::span<const float> nodes, const float pull_width)
    {
        const auto oracle = frozen_harmony_attraction(pixel_hue, nodes, pull_width);
        const auto actual = harmony_geometry::harmony_attraction(pixel_hue, nodes, pull_width);
        ASSERT_TRUE(actual.has_value());
        EXPECT_EQ(actual.value().winning_index, oracle.winning_index);
        EXPECT_EQ(std::bit_cast<std::uint32_t>(actual.value().weight),
                  std::bit_cast<std::uint32_t>(oracle.weight));
        EXPECT_EQ(std::bit_cast<std::uint32_t>(actual.value().shift),
                  std::bit_cast<std::uint32_t>(oracle.shift));
    };

    const std::array exact{0.25F};
    expect_oracle(0.25F, exact, 1.0F);
    const std::array circular{0.01F};
    expect_oracle(0.99F, circular, 1.0F);
    const std::array tie{0.25F, 0.75F};
    const auto tied = harmony_geometry::harmony_attraction(0.0F, tie, 1.0F);
    ASSERT_TRUE(tied.has_value());
    EXPECT_EQ(tied.value().winning_index, 0U);
    EXPECT_GT(tied.value().shift, 0.0F);
    expect_oracle(0.0F, tie, 1.0F);

    const std::array underflow{0.5F, 0.5F, 0.5F, 0.5F};
    const auto underflowed = harmony_geometry::harmony_attraction(0.0F, underflow, 0.25F);
    ASSERT_TRUE(underflowed.has_value());
    EXPECT_EQ(underflowed.value().winning_index, 0U);
    EXPECT_EQ(std::bit_cast<std::uint32_t>(underflowed.value().weight), 0x00000000U);
    EXPECT_EQ(std::bit_cast<std::uint32_t>(underflowed.value().shift), 0x00000000U);
    for (const float width : {0.25F, 1.0F, 1.84F, 4.0F})
    {
        const std::array nodes{0.2F, 0.6F, 0.9F};
        expect_oracle(0.47F, nodes, width);
    }

    const auto expect_invalid =
        [](const Result<harmony_geometry::HarmonyAttraction> &result, const std::string_view reason)
    {
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, ErrorCode::kValidation);
        EXPECT_EQ(result.error().message, reason);
    };
    const std::array<float, 0> empty{};
    const std::array<float, 5> too_many{0.0F, 0.2F, 0.4F, 0.6F, 0.8F};
    const std::array invalid_node{-std::numeric_limits<float>::min()};
    const std::array nonfinite_node{std::numeric_limits<float>::quiet_NaN()};
    expect_invalid(harmony_geometry::harmony_attraction(0.5F, empty, 1.0F),
                   "invalid_harmony_nodes");
    expect_invalid(harmony_geometry::harmony_attraction(0.5F, too_many, 1.0F),
                   "invalid_harmony_nodes");
    expect_invalid(harmony_geometry::harmony_attraction(0.5F, invalid_node, 1.0F),
                   "invalid_harmony_nodes");
    expect_invalid(harmony_geometry::harmony_attraction(0.5F, nonfinite_node, 1.0F),
                   "invalid_harmony_nodes");
    expect_invalid(
        harmony_geometry::harmony_attraction(-std::numeric_limits<float>::min(), exact, 1.0F),
        "invalid_harmony_hue");
    expect_invalid(harmony_geometry::harmony_attraction(
                       std::nextafter(1.0F, std::numeric_limits<float>::infinity()), exact, 1.0F),
                   "invalid_harmony_hue");
    expect_invalid(
        harmony_geometry::harmony_attraction(std::numeric_limits<float>::quiet_NaN(), exact, 1.0F),
        "invalid_harmony_hue");
    expect_invalid(harmony_geometry::harmony_attraction(0.5F, exact, std::nextafter(0.25F, 0.0F)),
                   "invalid_harmony_pull_width");
    expect_invalid(harmony_geometry::harmony_attraction(
                       0.5F, exact, std::nextafter(4.0F, std::numeric_limits<float>::infinity())),
                   "invalid_harmony_pull_width");
    expect_invalid(
        harmony_geometry::harmony_attraction(0.5F, exact, std::numeric_limits<float>::infinity()),
        "invalid_harmony_pull_width");
}

TEST(ColorContrastTest, LabAffineBranchesMatchFrozenSourceBitGoldens)
{
    struct Case
    {
        ColorContrastParams params;
        FrozenD50Triplet input;
        std::array<std::uint32_t, 3> golden;
    };
    const std::array cases{
        Case{{2.5999999046325684, 0.0, 2.5, 0.0, true},
             {50.0F, -60.0F, 70.0F},
             {0x42480000U, 0xc31c0000U, 0x432f0000U}},
        Case{{1.25, -12.5, 0.5, 7.25, true},
             {37.75F, -8.125F, 2.25F},
             {0x42170000U, 0xc1b54000U, 0x41060000U}},
        Case{{5.0, 128.0, 0.0, -128.0, false},
             {-20.0F, 20.0F, -20.0F},
             {0xc1a00000U, 0x43000000U, 0xc3000000U}},
    };
    for (const auto &[params, input, golden] : cases)
    {
        const auto oracle = frozen_color_contrast_lab(params, input);
        EXPECT_EQ(d50_triplet_bits(oracle), golden);
        const auto actual = apply_color_contrast_lab(params, input, CancellationToken{});
        ASSERT_TRUE(actual) << actual.error().message;
        EXPECT_EQ(d50_triplet_bits(actual.value()), golden);
        EXPECT_EQ(actual.value()[0], input[0]);
    }

    auto perturbed = cases.front().params;
    std::swap(perturbed.a_steepness, perturbed.b_steepness);
    EXPECT_NE(d50_triplet_bits(frozen_color_contrast_lab(perturbed, cases.front().input)),
              cases.front().golden)
        << "the independent oracle must detect an a*/b* coefficient swap";

    auto invalid_params = cases.front().params;
    invalid_params.a_offset = std::numeric_limits<double>::quiet_NaN();
    const auto invalid =
        apply_color_contrast_lab(invalid_params, cases.front().input, CancellationToken{});
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().context.at("reason"), "invalid_colorcontrast_parameters");
    const auto nonfinite = apply_color_contrast_lab(
        cases.front().params, {50.0F, std::numeric_limits<float>::infinity(), 0.0F},
        CancellationToken{});
    ASSERT_FALSE(nonfinite);
    EXPECT_EQ(nonfinite.error().context.at("reason"), "nonfinite_colorcontrast_lab_input");
}

TEST(ColorContrastTest, ExplicitCanonicalDefaultRetainsTheFrozenD50LabRoundTrip)
{
    const WorkingImage input = legacy_color_balance_working_fixture();
    const WorkingImage original = input;
    const ColorContrastParams defaults;
    const auto actual = apply_color_contrast(input, defaults, CancellationToken{});
    ASSERT_TRUE(actual) << actual.error().message;
    ASSERT_EQ(actual.value().rgb.size(), input.rgb.size());
    for (std::size_t index = 0U; index < input.rgb.size(); index += 3U)
    {
        const FrozenD50Triplet source{input.rgb[index], input.rgb[index + 1U],
                                      input.rgb[index + 2U]};
        const auto expected = frozen_color_contrast_rgb(defaults, source);
        EXPECT_EQ(d50_triplet_bits({actual.value().rgb[index], actual.value().rgb[index + 1U],
                                    actual.value().rgb[index + 2U]}),
                  d50_triplet_bits(expected));
    }
    EXPECT_NE(actual.value().rgb, input.rgb)
        << "only an absent or upgraded v1-zero operation may skip the frozen Lab round-trip";
    EXPECT_EQ(input.rgb, original.rgb);
    EXPECT_EQ(input.color_profile, original.color_profile);
    EXPECT_EQ(input.exposure_analysis, original.exposure_analysis);
}

TEST(ColorContrastTest, WorkingDispatchOwnershipFailuresAndCancellationAreAtomic)
{
    const WorkingImage input = legacy_color_balance_working_fixture();
    const WorkingImage original = input;
    const ColorContrastParams params{2.5999999046325684, 0.0, 2.5, 0.0, true};
    auto direct = apply_color_contrast(input, params, CancellationToken{});
    ASSERT_TRUE(direct) << direct.error().message;
    ASSERT_EQ(direct.value().rgb.size(), input.rgb.size());
    for (std::size_t index = 0U; index < input.rgb.size(); index += 3U)
    {
        const FrozenD50Triplet source{input.rgb[index], input.rgb[index + 1U],
                                      input.rgb[index + 2U]};
        const auto expected = frozen_color_contrast_rgb(params, source);
        EXPECT_EQ(d50_triplet_bits({direct.value().rgb[index], direct.value().rgb[index + 1U],
                                    direct.value().rgb[index + 2U]}),
                  d50_triplet_bits(expected));
    }
    EXPECT_EQ(direct.value().color_profile, input.color_profile);
    EXPECT_EQ(direct.value().exposure_analysis, input.exposure_analysis);
    EXPECT_NE(direct.value().rgb.data(), input.rgb.data());
    EXPECT_NE(direct.value().color_profile.icc_bytes.data(), input.color_profile.icc_bytes.data());
    EXPECT_EQ(input.rgb, original.rgb);
    EXPECT_EQ(input.color_profile, original.color_profile);
    EXPECT_EQ(input.exposure_analysis, original.exposure_analysis);

    const auto parameters = color_contrast_to_parameters(params);
    ASSERT_TRUE(parameters) << parameters.error().message;
    Recipe canonical;
    canonical.operations.push_back({std::string(kColorContrastOperationId),
                                    kColorContrastOperationSchemaVersion, "colorcontrast-v2", true,
                                    parameters.value(), std::nullopt});
    const auto dispatched = apply_recipe_ops(input, canonical, CancellationToken{});
    ASSERT_TRUE(dispatched) << dispatched.error().message;
    EXPECT_EQ(dispatched.value().rgb, direct.value().rgb);
    direct.value().rgb.front() = 42.0F;
    direct.value().color_profile.icc_bytes.front() = 99U;
    EXPECT_EQ(input.rgb, original.rgb);
    EXPECT_EQ(input.color_profile, original.color_profile);

    Recipe compatible;
    compatible.operations.push_back({std::string(kColorContrastOperationId),
                                     1,
                                     "colorcontrast-v1",
                                     true,
                                     {{"amount", ParameterValue{0.25}}},
                                     std::nullopt});
    const auto v1 = apply_recipe_ops(input, compatible, CancellationToken{});
    const auto v1_direct = apply_color_contrast(
        input, ColorContrastParams{1.25, 0.0, 1.25, 0.0, true}, CancellationToken{});
    ASSERT_TRUE(v1) << v1.error().message;
    ASSERT_TRUE(v1_direct) << v1_direct.error().message;
    EXPECT_EQ(v1.value().rgb, v1_direct.value().rgb);
    compatible.operations.front().parameters["amount"] = ParameterValue{0.0};
    const auto v1_zero = apply_recipe_ops(input, compatible, CancellationToken{});
    ASSERT_TRUE(v1_zero) << v1_zero.error().message;
    EXPECT_EQ(v1_zero.value().rgb, input.rgb);

    OperationInstance masked{std::string(kColorContrastOperationId),
                             kColorContrastOperationSchemaVersion,
                             "colorcontrast-mask",
                             true,
                             parameters.value(),
                             "mask-1"};
    auto rejected = apply_color_contrast(input, masked, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(rejected.error().context.at("reason"), "colorcontrast_mask_graph_unavailable");
    auto future = masked;
    future.mask_id.reset();
    future.schema_version = kColorContrastOperationSchemaVersion + 1;
    rejected = apply_color_contrast(input, future, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kUnsupported);
    auto wrong_operation = masked;
    wrong_operation.mask_id.reset();
    wrong_operation.id = "ravo.color.colorcorrection";
    rejected = apply_color_contrast(input, wrong_operation, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kValidation);

    auto invalid_dimensions = input;
    invalid_dimensions.width = 0U;
    rejected = apply_color_contrast(invalid_dimensions, params, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "invalid_colorcontrast_dimensions");
    auto invalid_buffer = input;
    invalid_buffer.rgb.pop_back();
    rejected = apply_color_contrast(invalid_buffer, params, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "invalid_colorcontrast_buffer");
    auto invalid_profile = input;
    invalid_profile.color_profile.identifier = "srgb";
    rejected = apply_color_contrast(invalid_profile, params, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "unsupported_colorcontrast_working_space");
    auto invalid_model = input;
    invalid_model.color_profile.model = ColorModel::kLab;
    rejected = apply_color_contrast(invalid_model, params, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "unsupported_colorcontrast_working_space");
    auto invalid_sample = input;
    invalid_sample.rgb[2] = std::numeric_limits<float>::quiet_NaN();
    const auto invalid_source = invalid_sample.rgb;
    rejected = apply_color_contrast(invalid_sample, params, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "nonfinite_colorcontrast_input");
    for (std::size_t index = 0U; index < invalid_source.size(); ++index)
    {
        EXPECT_EQ(std::bit_cast<std::uint32_t>(invalid_sample.rgb[index]),
                  std::bit_cast<std::uint32_t>(invalid_source[index]));
    }
    auto overflowing = params;
    overflowing.a_offset = static_cast<double>(std::numeric_limits<float>::max());
    rejected = apply_color_contrast(input, overflowing, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "nonfinite_colorcontrast_output");
    CancellationSource cancelled;
    ASSERT_TRUE(cancelled.cancel("colorcontrast-pre"));
    rejected = apply_color_contrast(input, params, cancelled.token());
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kCancelled);

    WorkingImage rows;
    rows.width = 1024U;
    rows.height = 4096U;
    rows.rgb.assign(static_cast<std::size_t>(rows.width) * rows.height * 3U, 0.25F);
    rows.color_profile.model = ColorModel::kRgb;
    rows.color_profile.identifier = std::string(kInputProfileLinearRec709);
    const auto deadline = CancellationSource::with_deadline(std::chrono::steady_clock::now() +
                                                            std::chrono::milliseconds{1});
    rejected = apply_color_contrast(rows, params, deadline.token());
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kCancelled);
    EXPECT_FLOAT_EQ(rows.rgb.front(), 0.25F);
    EXPECT_FLOAT_EQ(rows.rgb.back(), 0.25F);
}

TEST(ColorCheckerTest, RgbAndD50LabBridgeMatchesTheFrozenScalarReference)
{
    const auto reference = [](const std::array<float, 3> &rgb)
    {
        constexpr std::array<float, 3> d50{0.9642F, 1.0F, 0.8249F};
        constexpr float epsilon = 216.0F / 24389.0F;
        constexpr float kappa = 24389.0F / 27.0F;
        const std::array<float, 3> xyz{
            0.4360747F * rgb[0] + 0.3850649F * rgb[1] + 0.1430804F * rgb[2],
            0.2225045F * rgb[0] + 0.7168786F * rgb[1] + 0.0606169F * rgb[2],
            0.0139322F * rgb[0] + 0.0971045F * rgb[1] + 0.7141733F * rgb[2]};
        std::array<float, 3> f{};
        for (std::size_t channel = 0U; channel < 3U; ++channel)
        {
            const float normalized = xyz[channel] / d50[channel];
            f[channel] = normalized > epsilon ? std::cbrt(normalized) :
                                                (kappa * normalized + 16.0F) / 116.0F;
        }
        const std::array<float, 3> lab{116.0F * f[1] - 16.0F, 500.0F * (f[0] - f[1]),
                                       200.0F * (f[1] - f[2])};
        const float fy = (lab[0] + 16.0F) / 116.0F;
        const std::array<float, 3> inverse_f{fy + lab[1] / 500.0F, fy, fy - lab[2] / 200.0F};
        std::array<float, 3> roundtrip_xyz{};
        for (std::size_t channel = 0U; channel < 3U; ++channel)
        {
            const float value = inverse_f[channel] > 0.20689655172413796F ?
                                    inverse_f[channel] * inverse_f[channel] * inverse_f[channel] :
                                    (116.0F * inverse_f[channel] - 16.0F) / kappa;
            roundtrip_xyz[channel] = d50[channel] * value;
        }
        return std::array<float, 3>{3.1338561F * roundtrip_xyz[0] - 1.6168667F * roundtrip_xyz[1] -
                                        0.4906146F * roundtrip_xyz[2],
                                    -0.9787684F * roundtrip_xyz[0] + 1.9161415F * roundtrip_xyz[1] +
                                        0.0334540F * roundtrip_xyz[2],
                                    0.0719453F * roundtrip_xyz[0] - 0.2289914F * roundtrip_xyz[1] +
                                        1.4052427F * roundtrip_xyz[2]};
    };
    WorkingImage input;
    input.width = 1U;
    input.height = 1U;
    input.rgb = {0.25F, 0.5F, 0.75F};
    input.color_profile.model = ColorModel::kRgb;
    input.color_profile.identifier = std::string(kInputProfileLinearRec709);
    const auto expected = reference({input.rgb[0], input.rgb[1], input.rgb[2]});
    ColorCheckerParams no_patches{{}};

    const auto actual = apply_color_checker(input, no_patches, CancellationToken{});

    ASSERT_TRUE(actual) << actual.error().message;
    for (std::size_t channel = 0U; channel < 3U; ++channel)
    {
        EXPECT_NEAR(actual.value().rgb[channel], expected[channel], 1.0e-6F) << channel;
        EXPECT_NEAR(actual.value().rgb[channel], input.rgb[channel], 2.0e-6F) << channel;
    }
}

TEST(ColorCheckerTest, DeadlineCancellationDuringRowsNeverMutatesTheSource)
{
    WorkingImage input;
    input.width = 1024U;
    input.height = 4096U;
    input.rgb.assign(static_cast<std::size_t>(input.width) * input.height * 3U, 0.25F);
    input.color_profile.model = ColorModel::kRgb;
    input.color_profile.identifier = std::string(kInputProfileLinearRec709);
    const auto first = input.rgb.front();
    const auto last = input.rgb.back();
    const auto deadline = CancellationSource::with_deadline(std::chrono::steady_clock::now() +
                                                            std::chrono::milliseconds{1});

    const auto cancelled = apply_color_checker(input, ColorCheckerParams{}, deadline.token());

    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, ErrorCode::kCancelled);
    EXPECT_FLOAT_EQ(input.rgb.front(), first);
    EXPECT_FLOAT_EQ(input.rgb.back(), last);
}

TEST(LegacyColorBalanceTest, ModeSpecificNearOneContrastThresholdIsFrozen)
{
    const auto input = legacy_color_balance_working_fixture();
    for (const std::string_view mode :
         {kColorBalanceModeSlopeOffsetPower, kColorBalanceModeLiftGammaGain})
    {
        SCOPED_TRACE(mode);
        ColorBalanceParams baseline;
        baseline.mode = std::string(mode);
        auto without_contrast = apply_color_balance(input, baseline, CancellationToken{});
        ASSERT_TRUE(without_contrast) << without_contrast.error().message;

        auto inside = baseline;
        inside.contrast = 1.0 + 0.5e-6;
        auto inside_result = apply_color_balance(input, inside, CancellationToken{});
        ASSERT_TRUE(inside_result) << inside_result.error().message;
        EXPECT_EQ(inside_result.value().rgb, without_contrast.value().rgb);

        auto outside = baseline;
        outside.contrast = 1.0 + 2.0e-6;
        auto outside_result = apply_color_balance(input, outside, CancellationToken{});
        ASSERT_TRUE(outside_result) << outside_result.error().message;
        EXPECT_NE(outside_result.value().rgb, without_contrast.value().rgb);
    }
}

TEST(LegacyColorBalanceTest, BoundaryFailuresCancellationAndMasksNeverPublishPartialPixels)
{
    const auto input = legacy_color_balance_working_fixture();
    const auto original = input;

    WorkingImage zero = input;
    zero.width = 0U;
    auto zero_result = apply_color_balance(zero, ColorBalanceParams{}, CancellationToken{});
    ASSERT_FALSE(zero_result);
    EXPECT_EQ(zero_result.error().code, ErrorCode::kValidation);

    WorkingImage wrong_size = input;
    wrong_size.width = 3U;
    auto size_result = apply_color_balance(wrong_size, ColorBalanceParams{}, CancellationToken{});
    ASSERT_FALSE(size_result);
    EXPECT_EQ(size_result.error().code, ErrorCode::kValidation);

    WorkingImage lab = input;
    lab.color_profile.model = ColorModel::kLab;
    auto model_result = apply_color_balance(lab, ColorBalanceParams{}, CancellationToken{});
    ASSERT_FALSE(model_result);
    EXPECT_EQ(model_result.error().code, ErrorCode::kUnsupported);

    WorkingImage wrong_profile = input;
    wrong_profile.color_profile.identifier = "linear_rec2020";
    auto profile_result =
        apply_color_balance(wrong_profile, ColorBalanceParams{}, CancellationToken{});
    ASSERT_FALSE(profile_result);
    EXPECT_EQ(profile_result.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(profile_result.error().context.at("reason"),
              "unsupported_colorbalance_working_space");

    for (const std::string_view mode :
         {kColorBalanceModeSlopeOffsetPower, kColorBalanceModeLiftGammaGain})
    {
        for (const float sample :
             {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
              -std::numeric_limits<float>::infinity()})
        {
            SCOPED_TRACE(mode);
            WorkingImage invalid = input;
            invalid.rgb[2] = sample;
            ColorBalanceParams params;
            params.mode = std::string(mode);
            auto result = apply_color_balance(invalid, params, CancellationToken{});
            ASSERT_FALSE(result);
            EXPECT_EQ(result.error().code, ErrorCode::kValidation);
            EXPECT_EQ(result.error().context.at("reason"), "nonfinite_colorbalance_input");
            EXPECT_EQ(invalid.rgb[0], input.rgb[0]);
            EXPECT_EQ(invalid.rgb[1], input.rgb[1]);
        }
    }

    for (const std::string_view mode :
         {kColorBalanceModeSlopeOffsetPower, kColorBalanceModeLiftGammaGain})
    {
        ColorBalanceParams invalid_denominator;
        invalid_denominator.mode = std::string(mode);
        invalid_denominator.contrast = 0.0;
        auto denominator = apply_color_balance(input, invalid_denominator, CancellationToken{});
        ASSERT_FALSE(denominator);
        EXPECT_EQ(denominator.error().code, ErrorCode::kValidation);
        EXPECT_EQ(denominator.error().context.at("parameter"), "contrast");
    }

    ColorBalanceParams invalid_power;
    invalid_power.mode = std::string(kColorBalanceModeLiftGammaGain);
    invalid_power.gamma = {0.0, 0.0, 0.0, 0.0};
    WorkingImage superwhite = input;
    superwhite.rgb.assign(superwhite.rgb.size(), 2.0F);
    auto power = apply_color_balance(superwhite, invalid_power, CancellationToken{});
    ASSERT_FALSE(power);
    EXPECT_EQ(power.error().code, ErrorCode::kValidation);
    EXPECT_EQ(power.error().context.at("reason"), "nonfinite_colorbalance_curve");

    ColorBalanceParams sop_power;
    WorkingImage negative = input;
    negative.rgb.assign(negative.rgb.size(), -2.0F);
    auto clipped_power = apply_color_balance(negative, sop_power, CancellationToken{});
    ASSERT_TRUE(clipped_power) << clipped_power.error().message;
    EXPECT_TRUE(std::all_of(clipped_power.value().rgb.begin(), clipped_power.value().rgb.end(),
                            [](const float sample) { return std::isfinite(sample); }));

    for (const std::string_view mode :
         {kColorBalanceModeSlopeOffsetPower, kColorBalanceModeLiftGammaGain})
    {
        CancellationSource cancelled;
        ASSERT_TRUE(cancelled.cancel("legacy-colorbalance-pre-cancel"));
        ColorBalanceParams params;
        params.mode = std::string(mode);
        auto pre_cancelled = apply_color_balance(input, params, cancelled.token());
        ASSERT_FALSE(pre_cancelled);
        EXPECT_EQ(pre_cancelled.error().code, ErrorCode::kCancelled);
    }

    WorkingImage large;
    large.width = 1024U;
    large.height = 2048U;
    large.color_profile = input.color_profile;
    large.rgb.assign(static_cast<std::size_t>(large.width) * large.height * 3U, 0.5F);
    for (const std::string_view mode :
         {kColorBalanceModeSlopeOffsetPower, kColorBalanceModeLiftGammaGain})
    {
        const auto deadline = CancellationSource::with_deadline(std::chrono::steady_clock::now() +
                                                                std::chrono::milliseconds{1});
        ColorBalanceParams params;
        params.mode = std::string(mode);
        auto row_cancelled = apply_color_balance(large, params, deadline.token());
        ASSERT_FALSE(row_cancelled);
        EXPECT_EQ(row_cancelled.error().code, ErrorCode::kCancelled);
    }
    EXPECT_FLOAT_EQ(large.rgb.front(), 0.5F);
    EXPECT_FLOAT_EQ(large.rgb.back(), 0.5F);

    auto masked = legacy_color_balance_operation(ColorBalanceParams{});
    masked.mask_id = "mask-1";
    auto mask_result = apply_color_balance(input, masked, CancellationToken{});
    ASSERT_FALSE(mask_result);
    EXPECT_EQ(mask_result.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(mask_result.error().context.at("reason"), "colorbalance_mask_graph_unavailable");
    EXPECT_EQ(input.rgb, original.rgb);
    EXPECT_EQ(input.color_profile, original.color_profile);
}

TEST(ColorBalanceRgbTest, FilmlightTransformsAndOpacityMasksMatchTheFrozenMath)
{
    const auto white_ych = color_balance_rgb_working_to_ych(std::array<float, 3>{1.0F, 1.0F, 1.0F});
    EXPECT_NEAR(white_ych[0], 1.0578599F, 2.0e-5F);
    EXPECT_LT(white_ych[1], 5.0e-5F);
    EXPECT_NEAR(std::hypot(white_ych[2], white_ych[3]), 1.0F, 2.0e-5F);
    const auto grading = color_balance_rgb_ych_to_grading_rgb(white_ych);
    for (const float sample : grading)
    {
        EXPECT_TRUE(std::isfinite(sample));
        EXPECT_GT(sample, 0.0F);
    }

    const ColorBalanceRgbParams params;
    const auto center =
        color_balance_rgb_opacity_masks(static_cast<float>(params.mask_grey_fulcrum), params);
    EXPECT_NEAR(center.opacity[0], 0.5F, 1.0e-6F);
    EXPECT_NEAR(center.opacity[1], 0.5F, 1.0e-6F);
    EXPECT_NEAR(center.opacity[2], 0.5F, 1.0e-6F);
    for (std::size_t index = 0; index < center.opacity.size(); ++index)
    {
        EXPECT_NEAR(center.opacity[index] + center.complement[index], 1.0F, 1.0e-6F);
    }
    const auto dark = color_balance_rgb_opacity_masks(0.001F, params);
    const auto bright = color_balance_rgb_opacity_masks(1.0F, params);
    EXPECT_GT(dark.opacity[0], dark.opacity[2]);
    EXPECT_GT(bright.opacity[2], bright.opacity[0]);
    EXPECT_GT(center.opacity[1], dark.opacity[1]);
    EXPECT_GT(center.opacity[1], bright.opacity[1]);

    bool exercised_negative_lms_clip = false;
    for (int degrees = -180; degrees < 180; degrees += 15)
    {
        auto clipped = color_balance_rgb_jzazbz_negative_lms_clip(
            0.2F, 2.0F, static_cast<float>(degrees) * std::numbers::pi_v<float> / 180.0F);
        ASSERT_TRUE(clipped) << clipped.error().message;
        if (clipped.value().clipped)
        {
            exercised_negative_lms_clip = true;
            EXPECT_LT(clipped.value().chroma, 2.0F);
        }
    }
    EXPECT_TRUE(exercised_negative_lms_clip);
}

TEST(ColorBalanceRgbTest, EveryGradingStageChangesSyntheticPixels)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    const RasterBuffer source = gradient_raster();
    Recipe identity_recipe;
    identity_recipe.asset = {"raster", "memory:raster", std::nullopt};
    declare_input(identity_recipe);
    RenderRequest identity_request;
    identity_request.asset = identity_recipe.asset;
    identity_request.recipe = identity_recipe;
    auto identity = engine.value().render_to_image(identity_request, &source);
    ASSERT_TRUE(identity) << identity.error().message;

    const auto exercise = [&](ColorBalanceRgbParams params)
    { return render_op(engine.value(), source, color_balance_rgb_operation(params)); };

    ColorBalanceRgbParams offset;
    offset.global_y = 0.15;
    offset.global_chroma = 0.08;
    offset.global_hue = 35.0;
    auto offset_result = exercise(offset);
    ASSERT_TRUE(offset_result) << offset_result.error().message;
    EXPECT_NE(offset_result.value().rgb, identity.value().rgb);

    ColorBalanceRgbParams shadows;
    shadows.shadows_y = 0.35;
    shadows.shadows_chroma = 0.1;
    shadows.shadows_hue = 220.0;
    auto shadows_result = exercise(shadows);
    ASSERT_TRUE(shadows_result) << shadows_result.error().message;
    EXPECT_NE(shadows_result.value().rgb, identity.value().rgb);

    ColorBalanceRgbParams midtones;
    midtones.midtones_y = 0.25;
    midtones.midtones_chroma = 0.08;
    midtones.midtones_hue = 300.0;
    auto midtones_result = exercise(midtones);
    ASSERT_TRUE(midtones_result) << midtones_result.error().message;
    EXPECT_NE(midtones_result.value().rgb, identity.value().rgb);

    ColorBalanceRgbParams highlights;
    highlights.highlights_y = -0.3;
    highlights.highlights_chroma = 0.08;
    highlights.highlights_hue = 70.0;
    auto highlights_result = exercise(highlights);
    ASSERT_TRUE(highlights_result) << highlights_result.error().message;
    EXPECT_NE(highlights_result.value().rgb, identity.value().rgb);

    ColorBalanceRgbParams perceptual;
    perceptual.chroma_global = 0.2;
    perceptual.saturation_global = 0.25;
    perceptual.brilliance_global = 0.15;
    perceptual.vibrance = 0.2;
    perceptual.hue_rotation = 20.0;
    perceptual.contrast = 0.15;
    auto dt_ucs = exercise(perceptual);
    ASSERT_TRUE(dt_ucs) << dt_ucs.error().message;
    EXPECT_NE(dt_ucs.value().rgb, identity.value().rgb);

    perceptual.saturation_formula = std::string(kColorBalanceRgbFormulaJzAzBz2021);
    auto jzazbz = exercise(perceptual);
    ASSERT_TRUE(jzazbz) << jzazbz.error().message;
    EXPECT_NE(jzazbz.value().rgb, identity.value().rgb);
    EXPECT_NE(jzazbz.value().rgb, dt_ucs.value().rgb);
}

TEST(ColorBalanceRgbTest, CancellationAndNonFiniteInputNeverPublishPartialPixels)
{
    ColorBalanceRgbParams params;
    params.global_y = 0.2;
    const auto operation = color_balance_rgb_operation(params);
    WorkingImage image{2, 1, {0.1F, 0.2F, 0.3F, 0.4F, 0.5F, 0.6F}, {}, {}};
    const auto original = image.rgb;
    CancellationSource cancellation;
    ASSERT_TRUE(cancellation.cancel("color_balance_cancel"));
    auto cancelled = apply_color_balance_rgb(image, operation, cancellation.token());
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, ErrorCode::kCancelled);
    EXPECT_EQ(image.rgb, original);

    image.rgb[4] = std::numeric_limits<float>::infinity();
    const auto invalid_original = image.rgb;
    auto invalid = apply_color_balance_rgb(image, operation, CancellationToken{});
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code, ErrorCode::kValidation);
    EXPECT_EQ(image.rgb, invalid_original);
}

TEST(EngineFacadeTest, PhaseOneControlsChangeSyntheticRaster)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    const auto base_raster = gradient_raster();
    Recipe identity;
    identity.asset = {"raster", "memory:raster", std::nullopt};
    declare_input(identity);
    RenderRequest identity_request;
    identity_request.asset = identity.asset;
    identity_request.recipe = identity;
    auto base = engine.value().render_to_image(identity_request, &base_raster);
    ASSERT_TRUE(base) << base.error().message;
    const auto base_mean = mean_luma(base.value());

    auto exposed = render_op(engine.value(), base_raster,
                             {"ravo.core.exposure",
                              1,
                              "exposure-1",
                              true,
                              {{"exposure_ev", ParameterValue{1.0}}},
                              std::nullopt});
    ASSERT_TRUE(exposed) << exposed.error().message;
    EXPECT_GT(mean_luma(exposed.value()), base_mean);

    auto shadowed = render_op(engine.value(), base_raster,
                              {"ravo.core.shadows",
                               1,
                               "shadows-1",
                               true,
                               {{"amount", ParameterValue{0.8}}},
                               std::nullopt});
    ASSERT_TRUE(shadowed) << shadowed.error().message;
    EXPECT_GT(mean_luma(shadowed.value()), base_mean);

    auto highlighted = render_op(engine.value(), base_raster,
                                 {"ravo.core.highlights",
                                  1,
                                  "highlights-1",
                                  true,
                                  {{"amount", ParameterValue{-0.8}}},
                                  std::nullopt});
    ASSERT_TRUE(highlighted) << highlighted.error().message;
    EXPECT_LT(mean_luma(highlighted.value()), base_mean);

    auto contrast = render_op(engine.value(), base_raster,
                              {"ravo.core.contrast",
                               1,
                               "contrast-1",
                               true,
                               {{"amount", ParameterValue{0.6}}},
                               std::nullopt});
    ASSERT_TRUE(contrast) << contrast.error().message;

    auto whites = render_op(engine.value(), base_raster,
                            {"ravo.core.whites",
                             1,
                             "whites-1",
                             true,
                             {{"amount", ParameterValue{-0.5}}},
                             std::nullopt});
    ASSERT_TRUE(whites) << whites.error().message;

    auto blacks = render_op(
        engine.value(), base_raster,
        {"ravo.core.blacks", 1, "blacks-1", true, {{"amount", ParameterValue{0.4}}}, std::nullopt});
    ASSERT_TRUE(blacks) << blacks.error().message;

    auto vibrance = render_op(engine.value(), base_raster,
                              {"ravo.color.vibrance",
                               1,
                               "vibrance-1",
                               true,
                               {{"amount", ParameterValue{0.8}}},
                               std::nullopt});
    ASSERT_TRUE(vibrance) << vibrance.error().message;

    TemperatureParams manual_wb;
    manual_wb.mode = std::string(kTemperatureModeManual);
    manual_wb.coefficients = std::array<double, kTemperatureChannelCount>{1.3, 0.9, 0.7, 1.0};
    auto wb = render_op(engine.value(), base_raster, temperature_operation(manual_wb));
    ASSERT_TRUE(wb) << wb.error().message;
    const auto mid = (8U * 16U + 8U) * 3U;
    EXPECT_NE(wb.value().rgb[mid], base.value().rgb[mid]);
    EXPECT_NE(wb.value().rgb[mid + 2U], base.value().rgb[mid + 2U]);

    auto gamma = render_op(
        engine.value(), base_raster,
        {"ravo.core.gamma", 1, "gamma-1", true, {{"gamma", ParameterValue{1.8}}}, std::nullopt});
    ASSERT_TRUE(gamma) << gamma.error().message;

    auto velvia = render_op(engine.value(), base_raster,
                            {"ravo.color.velvia",
                             1,
                             "velvia-1",
                             true,
                             {{"amount", ParameterValue{0.8}}, {"bias", ParameterValue{1.0}}},
                             std::nullopt});
    ASSERT_TRUE(velvia) << velvia.error().message;

    auto balance = render_op(engine.value(), base_raster,
                             color_balance_rgb_operation(test::color_balance_0093_params()));
    ASSERT_TRUE(balance) << balance.error().message;

    auto contrast_color = render_op(engine.value(), base_raster,
                                    {"ravo.color.colorcontrast",
                                     1,
                                     "cc-1",
                                     true,
                                     {{"amount", ParameterValue{0.7}}},
                                     std::nullopt});
    ASSERT_TRUE(contrast_color) << contrast_color.error().message;

    auto mono = render_op(engine.value(), solid_raster(8, 8, 220, 20, 20),
                          {"ravo.color.monochrome",
                           1,
                           "mono-1",
                           true,
                           {{"amount", ParameterValue{1.0}}},
                           std::nullopt});
    ASSERT_TRUE(mono) << mono.error().message;
    EXPECT_NEAR(mono.value().rgb[0], mono.value().rgb[1], 8);
    EXPECT_NEAR(mono.value().rgb[1], mono.value().rgb[2], 8);

    auto split = render_op(engine.value(), base_raster,
                           {"ravo.color.splittoning",
                            1,
                            "split-1",
                            true,
                            {{"shadows_hue", ParameterValue{0.6}},
                             {"highlights_hue", ParameterValue{0.1}},
                             {"balance", ParameterValue{0.5}},
                             {"amount", ParameterValue{0.8}}},
                            std::nullopt});
    ASSERT_TRUE(split) << split.error().message;

    auto crop = render_op(engine.value(), base_raster,
                          {"ravo.geometry.crop",
                           1,
                           "crop-1",
                           true,
                           {{"x", ParameterValue{0.25}},
                            {"y", ParameterValue{0.25}},
                            {"width", ParameterValue{0.5}},
                            {"height", ParameterValue{0.5}}},
                           std::nullopt});
    ASSERT_TRUE(crop) << crop.error().message;
    EXPECT_EQ(crop.value().width, 8U);
    EXPECT_EQ(crop.value().height, 8U);

    RasterBuffer sided;
    sided.width = 4;
    sided.height = 2;
    sided.srgb = {255, 0, 0, 255, 0, 0, 0, 255, 0, 0, 255, 0,
                  255, 0, 0, 255, 0, 0, 0, 255, 0, 0, 255, 0};
    auto flipped = render_op(engine.value(), sided,
                             {"ravo.geometry.flip",
                              1,
                              "flip-1",
                              true,
                              {{"horizontal", ParameterValue{std::int64_t{1}}},
                               {"vertical", ParameterValue{std::int64_t{0}}}},
                              std::nullopt});
    ASSERT_TRUE(flipped) << flipped.error().message;
    EXPECT_EQ(flipped.value().rgb[0], 0);
    EXPECT_EQ(flipped.value().rgb[1], 255);

    RasterBuffer unique;
    unique.width = 3;
    unique.height = 2;
    unique.srgb = {10, 20, 30, 40, 50, 60, 70, 80, 90, 11, 21, 31, 41, 51, 61, 71, 81, 91};
    declare_srgb(unique);
    const auto sample = [](const RenderedImage &image, const std::uint32_t x, const std::uint32_t y)
    {
        const std::size_t index = (static_cast<std::size_t>(y) * image.width + x) * 3U;
        return std::array<std::uint8_t, 3>{image.rgb[index], image.rgb[index + 1U],
                                           image.rgb[index + 2U]};
    };
    for (std::int32_t orientation = 0; orientation <= 7; ++orientation)
    {
        SCOPED_TRACE(orientation);
        auto geometry = leftover_flip_orientation_to_geometry(orientation);
        ASSERT_TRUE(geometry) << geometry.error().message;
        Recipe recipe;
        recipe.asset = {"raster", "memory:raster", std::nullopt};
        declare_input(recipe);
        if (geometry.value().rotate_quarters != 0)
        {
            recipe.operations.push_back(
                {"ravo.geometry.rotate",
                 1,
                 "rotate-1",
                 true,
                 {{"quarters", ParameterValue{geometry.value().rotate_quarters}}},
                 std::nullopt});
        }
        if (geometry.value().flip_horizontal != 0 || geometry.value().flip_vertical != 0)
        {
            recipe.operations.push_back(
                {"ravo.geometry.flip",
                 1,
                 "flip-1",
                 true,
                 {{"horizontal", ParameterValue{geometry.value().flip_horizontal}},
                  {"vertical", ParameterValue{geometry.value().flip_vertical}}},
                 std::nullopt});
        }
        RenderRequest request;
        request.asset = recipe.asset;
        request.recipe = recipe;
        auto rendered = engine.value().render_to_image(request, &unique);
        ASSERT_TRUE(rendered) << rendered.error().message;
        if (orientation == 0)
        {
            EXPECT_EQ(rendered.value().width, 3U);
            EXPECT_EQ(rendered.value().height, 2U);
            EXPECT_EQ(sample(rendered.value(), 0, 0), (std::array<std::uint8_t, 3>{10, 20, 30}));
            EXPECT_EQ(sample(rendered.value(), 2, 1), (std::array<std::uint8_t, 3>{71, 81, 91}));
        }
        else if (orientation == 5)
        {
            EXPECT_EQ(rendered.value().width, 2U);
            EXPECT_EQ(rendered.value().height, 3U);
            EXPECT_EQ(sample(rendered.value(), 1, 0), (std::array<std::uint8_t, 3>{10, 20, 30}));
            EXPECT_EQ(sample(rendered.value(), 0, 0), (std::array<std::uint8_t, 3>{11, 21, 31}));
        }
        else if (orientation == 4)
        {
            EXPECT_EQ(rendered.value().width, 2U);
            EXPECT_EQ(rendered.value().height, 3U);
            EXPECT_EQ(sample(rendered.value(), 0, 0), (std::array<std::uint8_t, 3>{10, 20, 30}));
            EXPECT_EQ(sample(rendered.value(), 1, 0), (std::array<std::uint8_t, 3>{11, 21, 31}));
        }
    }

    auto vignette = render_op(engine.value(), solid_raster(32, 32, 200, 200, 200),
                              {"ravo.effect.vignette",
                               1,
                               "vig-1",
                               true,
                               {{"amount", ParameterValue{1.0}},
                                {"midpoint", ParameterValue{0.3}},
                                {"falloff", ParameterValue{0.4}}},
                               std::nullopt});
    ASSERT_TRUE(vignette) << vignette.error().message;
    const auto center = (16U * 32U + 16U) * 3U;
    const auto corner = 0U;
    EXPECT_GT(vignette.value().rgb[center], vignette.value().rgb[corner]);

    auto grain_a = render_op(
        engine.value(), solid_raster(12, 12, 120, 120, 120),
        {"ravo.effect.grain", 1, "grain-1", true, {{"amount", ParameterValue{0.8}}}, std::nullopt});
    auto grain_b = render_op(
        engine.value(), solid_raster(12, 12, 120, 120, 120),
        {"ravo.effect.grain", 1, "grain-1", true, {{"amount", ParameterValue{0.8}}}, std::nullopt});
    ASSERT_TRUE(grain_a) << grain_a.error().message;
    ASSERT_TRUE(grain_b) << grain_b.error().message;
    EXPECT_EQ(grain_a.value().rgb, grain_b.value().rgb);
    EXPECT_NE(grain_a.value().rgb, solid_raster(12, 12, 120, 120, 120).srgb);

    auto sharpen = render_op(engine.value(), base_raster,
                             {"ravo.detail.sharpen",
                              1,
                              "sharp-1",
                              true,
                              {{"amount", ParameterValue{1.2}},
                               {"radius", ParameterValue{1.0}},
                               {"threshold", ParameterValue{0.0}}},
                              std::nullopt});
    ASSERT_TRUE(sharpen) << sharpen.error().message;

    auto clarity = render_op(engine.value(), base_raster,
                             {"ravo.detail.clarity",
                              1,
                              "clarity-1",
                              true,
                              {{"amount", ParameterValue{0.6}}},
                              std::nullopt});
    ASSERT_TRUE(clarity) << clarity.error().message;

    auto bloom = render_op(
        engine.value(), base_raster,
        {"ravo.effect.bloom", 1, "bloom-1", true, {{"amount", ParameterValue{0.7}}}, std::nullopt});
    ASSERT_TRUE(bloom) << bloom.error().message;

    auto soften = render_op(engine.value(), base_raster,
                            {"ravo.effect.soften",
                             1,
                             "soften-1",
                             true,
                             {{"amount", ParameterValue{0.7}}},
                             std::nullopt});
    ASSERT_TRUE(soften) << soften.error().message;

    auto dehaze = render_op(engine.value(), base_raster,
                            {"ravo.effect.dehaze",
                             1,
                             "dehaze-1",
                             true,
                             {{"amount", ParameterValue{0.5}}},
                             std::nullopt});
    ASSERT_TRUE(dehaze) << dehaze.error().message;

    auto straightened = render_op(engine.value(), solid_raster(16, 16, 200, 20, 20),
                                  {"ravo.geometry.straighten",
                                   1,
                                   "straighten-1",
                                   true,
                                   {{"degrees", ParameterValue{15.0}}},
                                   std::nullopt});
    ASSERT_TRUE(straightened) << straightened.error().message;
    EXPECT_EQ(straightened.value().width, 16U);
    EXPECT_EQ(straightened.value().height, 16U);
    EXPECT_LT(straightened.value().rgb[0], 40);
    const auto straighten_mid = (8U * 16U + 8U) * 3U;
    EXPECT_GT(straightened.value().rgb[straighten_mid], 80);
    bool saw_antialiased_edge = false;
    for (std::size_t index = 0; index + 2 < straightened.value().rgb.size(); index += 3)
    {
        const auto red = straightened.value().rgb[index];
        if (red > 20 && red < 160)
        {
            saw_antialiased_edge = true;
            break;
        }
    }
    EXPECT_TRUE(saw_antialiased_edge);

    auto graduated = render_op(engine.value(), solid_raster(32, 32, 180, 180, 180),
                               {"ravo.effect.graduatednd",
                                1,
                                "grad-1",
                                true,
                                {{"density_ev", ParameterValue{1.5}},
                                 {"hardness", ParameterValue{0.8}},
                                 {"rotation_deg", ParameterValue{0.0}},
                                 {"offset", ParameterValue{0.0}}},
                                std::nullopt});
    ASSERT_TRUE(graduated) << graduated.error().message;
    const auto top = (2U * 32U + 16U) * 3U;
    const auto bottom = (30U * 32U + 16U) * 3U;
    EXPECT_LT(graduated.value().rgb[top], graduated.value().rgb[bottom]);

    const auto dark_raster = solid_raster(16, 16, 12, 12, 12);
    auto dark_base = engine.value().render_to_image(identity_request, &dark_raster);
    ASSERT_TRUE(dark_base) << dark_base.error().message;
    auto toneeq = render_op(engine.value(), dark_raster,
                            {"ravo.core.toneequal",
                             1,
                             "toneeq-1",
                             true,
                             {{"blacks", ParameterValue{1.2}},
                              {"shadows", ParameterValue{0.0}},
                              {"midtones", ParameterValue{0.0}},
                              {"highlights", ParameterValue{0.0}},
                              {"whites", ParameterValue{0.0}}},
                             std::nullopt});
    ASSERT_TRUE(toneeq) << toneeq.error().message;
    EXPECT_GT(mean_luma(toneeq.value()), mean_luma(dark_base.value()));

    ParameterValue::Array sat_bands(8, ParameterValue{0.0});
    sat_bands[0] = ParameterValue{0.8};
    auto coloreq =
        render_op(engine.value(), solid_raster(8, 8, 220, 30, 30),
                  {"ravo.color.colorequal",
                   1,
                   "ceq-1",
                   true,
                   {{"hue_shift", ParameterValue{ParameterValue::Array(8, ParameterValue{0.0})}},
                    {"saturation", ParameterValue{sat_bands}},
                    {"lightness", ParameterValue{ParameterValue::Array(8, ParameterValue{0.0})}}},
                   std::nullopt});
    ASSERT_TRUE(coloreq) << coloreq.error().message;

    auto lens = render_op(engine.value(), solid_raster(24, 24, 200, 80, 40),
                          {"ravo.geometry.lens",
                           1,
                           "lens-1",
                           true,
                           {{"mode", ParameterValue{"manual"}},
                            {"k1", ParameterValue{-0.4}},
                            {"k2", ParameterValue{0.1}},
                            {"tca_r", ParameterValue{1.02}},
                            {"tca_b", ParameterValue{0.98}},
                            {"vignetting", ParameterValue{0.6}}},
                           std::nullopt});
    ASSERT_TRUE(lens) << lens.error().message;
    EXPECT_NE(lens.value().rgb, solid_raster(24, 24, 200, 80, 40).srgb);

    auto missing_lens = render_op(engine.value(), solid_raster(8, 8, 120, 120, 120),
                                  {"ravo.geometry.lens",
                                   1,
                                   "lens-lookup-1",
                                   true,
                                   {{"mode", ParameterValue{"lookup"}},
                                    {"camera_make", ParameterValue{"Missing"}},
                                    {"camera_model", ParameterValue{"Camera"}},
                                    {"lens", ParameterValue{"Unknown"}},
                                    {"focal_mm", ParameterValue{50.0}}},
                                   std::nullopt});
    ASSERT_FALSE(missing_lens);
    EXPECT_EQ(missing_lens.error().code, ErrorCode::kNotFound);

    auto matched_lens = render_op(engine.value(), solid_raster(16, 16, 180, 180, 180),
                                  {"ravo.geometry.lens",
                                   1,
                                   "lens-lookup-2",
                                   true,
                                   {{"mode", ParameterValue{"lookup"}},
                                    {"camera_make", ParameterValue{"RavoTest"}},
                                    {"camera_model", ParameterValue{"RavoSensor"}},
                                    {"lens", ParameterValue{"FixtureLens"}},
                                    {"focal_mm", ParameterValue{50.0}}},
                                   std::nullopt});
    ASSERT_TRUE(matched_lens) << matched_lens.error().message;

    RasterBuffer noisy = solid_raster(32, 32, 120, 120, 120);
    for (std::size_t index = 0; index < noisy.srgb.size(); ++index)
    {
        const auto delta = static_cast<int>((index * 37U) % 41U) - 20;
        noisy.srgb[index] = static_cast<std::uint8_t>(std::clamp(120 + delta, 0, 255));
    }
    auto denoised = render_op(engine.value(), noisy,
                              {"ravo.detail.denoiseprofile",
                               1,
                               "denoise-1",
                               true,
                               {{"strength", ParameterValue{0.8}},
                                {"chroma", ParameterValue{1.0}},
                                {"radius", ParameterValue{1.5}}},
                               std::nullopt});
    ASSERT_TRUE(denoised) << denoised.error().message;

    auto raw_on_raster = render_op(engine.value(), solid_raster(8, 8, 10, 10, 10),
                                   {"ravo.raw.highlights",
                                    1,
                                    "raw-hl-1",
                                    true,
                                    {{"mode", ParameterValue{"inpaint"}},
                                     {"amount", ParameterValue{1.0}},
                                     {"clip", ParameterValue{0.98}}},
                                    std::nullopt});
    ASSERT_FALSE(raw_on_raster);
    EXPECT_EQ(raw_on_raster.error().code, ErrorCode::kUnsupported);
}

TEST(EngineFacadeTest, BasicAdjustmentParametersFollowDarktableCpuResponse)
{
    const WorkingImage source{1, 1, {0.08F, 0.18F, 0.40F}, {}, {}};
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

TEST(EngineFacadeTest, EffectDefaultsAvoidSmallParameterBrightnessJumps)
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

    const WorkingImage haze_source{1, 1, {0.20F, 0.30F, 0.40F}, {}, {}};
    Recipe haze_recipe;
    haze_recipe.operations.push_back({"ravo.effect.dehaze",
                                      1,
                                      "dehaze-1",
                                      true,
                                      {{"amount", ParameterValue{-0.1}}},
                                      std::nullopt});
    auto haze = apply_recipe_ops(haze_source, haze_recipe, CancellationToken{});
    ASSERT_TRUE(haze) << haze.error().message;
    constexpr float airlight = 0.92F;
    const float transmission = 1.0F - 0.1F * 0.20F / airlight;
    for (std::size_t channel = 0; channel < 3U; ++channel)
    {
        const float expected =
            haze_source.rgb[channel] * transmission + airlight * (1.0F - transmission);
        EXPECT_NEAR(haze.value().rgb[channel], expected, 1.0e-6F);
    }
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

TEST(EngineFacadeTest, RawHighlightReconstructionChangesMire1)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    Recipe identity;
    identity.asset = {"mire1", mire1_path(), std::nullopt};
    declare_input(identity);
    RenderRequest base_request;
    base_request.asset = identity.asset;
    base_request.recipe = identity;
    base_request.output_width = 64;
    base_request.output_height = 48;
    auto base = engine.value().render_to_image(base_request);
    ASSERT_TRUE(base) << base.error().message;

    Recipe reconstructed = identity;
    reconstructed.operations.push_back({"ravo.raw.highlights",
                                        1,
                                        "raw-hl-1",
                                        true,
                                        {{"mode", ParameterValue{"opposed"}},
                                         {"amount", ParameterValue{1.0}},
                                         {"clip", ParameterValue{0.92}}},
                                        std::nullopt});
    RenderRequest request = base_request;
    request.recipe = reconstructed;
    auto rebuilt = engine.value().render_to_image(request);
    ASSERT_TRUE(rebuilt) << rebuilt.error().message;
    EXPECT_EQ(rebuilt.value().width, base.value().width);
    EXPECT_EQ(rebuilt.value().height, base.value().height);

    CancellationSource cancellation;
    ASSERT_TRUE(cancellation.cancel("highlights"));
    request.cancellation = cancellation.token();
    auto cancelled = engine.value().render_to_image(request);
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, ErrorCode::kCancelled);
}

[[nodiscard]] ParameterValue tone_curve_points(const std::vector<ToneCurvePoint> &points)
{
    return tone_curve_points_to_parameter(points);
}

[[nodiscard]] OperationInstance
sigmoid_operation(const double contrast = kSigmoidContrastDefault,
                  const double skew = kSigmoidSkewDefault,
                  const double hue_preservation = kSigmoidHuePreservationDefault)
{
    return {"ravo.display.sigmoid",
            1,
            "sigmoid-1",
            true,
            {{"working_space", ParameterValue{"linear_srgb"}},
             {"color_processing", ParameterValue{"per_channel"}},
             {"middle_grey_contrast", ParameterValue{contrast}},
             {"contrast_skewness", ParameterValue{skew}},
             {"display_white_target", ParameterValue{kSigmoidDisplayWhiteDefault}},
             {"display_black_target", ParameterValue{kSigmoidDisplayBlackDefault}},
             {"hue_preservation", ParameterValue{hue_preservation}}},
            std::nullopt};
}

TEST(EngineFacadeTest, SigmoidMapsSyntheticPixelsAndPreservesHueByPolicy)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    const auto source = gradient_raster();
    auto standard = render_op(engine.value(), source, sigmoid_operation());
    auto skewed = render_op(engine.value(), source, sigmoid_operation(1.5, -0.4, 1.0));
    ASSERT_TRUE(standard) << standard.error().message;
    ASSERT_TRUE(skewed) << skewed.error().message;
    EXPECT_NE(standard.value().rgb, source.srgb);
    EXPECT_NE(skewed.value().rgb, standard.value().rgb);

    const auto saturated = solid_raster(8, 8, 245, 100, 20);
    auto preserved = render_op(engine.value(), saturated, sigmoid_operation(1.5, 0.0, 1.0));
    auto shifted = render_op(engine.value(), saturated, sigmoid_operation(1.5, 0.0, 0.0));
    ASSERT_TRUE(preserved) << preserved.error().message;
    ASSERT_TRUE(shifted) << shifted.error().message;
    EXPECT_NE(preserved.value().rgb, shifted.value().rgb);
    EXPECT_GT(preserved.value().rgb[0], preserved.value().rgb[1]);
    EXPECT_GT(preserved.value().rgb[1], preserved.value().rgb[2]);

    auto ratio = sigmoid_operation();
    ratio.parameters["color_processing"] = ParameterValue{"rgb_ratio"};
    auto ratio_rendered = render_op(engine.value(), saturated, std::move(ratio));
    ASSERT_TRUE(ratio_rendered) << ratio_rendered.error().message;
    EXPECT_NE(ratio_rendered.value().rgb, preserved.value().rgb);

    auto boundary_operation = sigmoid_operation(kSigmoidContrastMax, kSigmoidSkewMax, 0.0);
    boundary_operation.parameters["display_white_target"] = ParameterValue{kSigmoidDisplayWhiteMax};
    boundary_operation.parameters["display_black_target"] = ParameterValue{kSigmoidDisplayBlackMin};
    auto boundary = render_op(engine.value(), source, std::move(boundary_operation));
    ASSERT_TRUE(boundary) << boundary.error().message;
    EXPECT_EQ(boundary.value().rgb.size(), source.srgb.size());
}

TEST(EngineFacadeTest, SigmoidHasARealRawReference)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    Recipe recipe;
    recipe.asset = {"mire1", mire1_path(), std::nullopt};
    declare_input(recipe);
    recipe.operations.push_back(sigmoid_operation());
    RenderRequest request;
    request.asset = recipe.asset;
    request.recipe = recipe;
    request.output_width = 64;
    request.output_height = 48;
    auto rendered = engine.value().render_to_image(request, nullptr);
    ASSERT_TRUE(rendered) << rendered.error().message;
    ASSERT_EQ(rendered.value().width, 64U);
    ASSERT_EQ(rendered.value().height, 48U);
    std::array<std::uint64_t, 3> sums{};
    std::size_t clipped_channels = 0;
    for (std::size_t index = 0; index + 2 < rendered.value().rgb.size(); index += 3)
    {
        for (std::size_t channel = 0; channel < sums.size(); ++channel)
        {
            const auto value = rendered.value().rgb[index + channel];
            sums[channel] += value;
            clipped_channels += value == 255 ? 1U : 0U;
        }
    }
    // Ravo-owned macOS reference statistics for the pinned mire1.cr2 fixture.
    // The tolerance permits platform libm/SIMD rounding without accepting a changed look.
    EXPECT_NEAR(static_cast<double>(sums[0]), 304823.0, 2000.0);
    EXPECT_NEAR(static_cast<double>(sums[1]), 281792.0, 2000.0);
    EXPECT_NEAR(static_cast<double>(sums[2]), 263085.0, 2000.0);
    EXPECT_LT(clipped_channels, rendered.value().rgb.size() / 100U);
}

TEST(EngineFacadeTest, TemperatureManualAndCameraReferenceHaveRealRawReferences)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    auto decoded = engine.value().decode_raw_frame(mire1_path(), CancellationToken{});
    ASSERT_TRUE(decoded) << decoded.error().message;
    const auto original_pixels = decoded.value().pixels;
    Recipe manual_recipe;
    manual_recipe.asset = {"mire1", mire1_path(), std::nullopt};
    declare_input(manual_recipe);
    manual_recipe.operations.push_back(temperature_operation(test::temperature_0000_params()));
    auto linear = engine.value().linear_working_from_raw(decoded.value(), manual_recipe, 64, 48,
                                                         CancellationToken{});
    ASSERT_TRUE(linear) << linear.error().message;
    EXPECT_EQ(decoded.value().pixels, original_pixels);

    const auto render_temperature = [&](TemperatureParams params)
    {
        Recipe recipe;
        recipe.asset = {"mire1", mire1_path(), std::nullopt};
        declare_input(recipe);
        recipe.operations.push_back(temperature_operation(params));
        recipe.operations.push_back(sigmoid_operation());
        RenderRequest request;
        request.asset = recipe.asset;
        request.recipe = recipe;
        request.output_width = 64;
        request.output_height = 48;
        return engine.value().render_to_image(request);
    };
    const auto sums = [](const RenderedImage &image)
    {
        std::array<std::uint64_t, 3> result{};
        for (std::size_t index = 0; index + 2 < image.rgb.size(); index += 3)
        {
            for (std::size_t channel = 0; channel < result.size(); ++channel)
            {
                result[channel] += image.rgb[index + channel];
            }
        }
        return result;
    };

    auto manual = render_temperature(test::temperature_0000_params());
    ASSERT_TRUE(manual) << manual.error().message;
    const auto manual_sums = sums(manual.value());
    EXPECT_NEAR(static_cast<double>(manual_sums[0]), 304283.0, 2000.0);
    EXPECT_NEAR(static_cast<double>(manual_sums[1]), 280917.0, 2000.0);
    EXPECT_NEAR(static_cast<double>(manual_sums[2]), 261889.0, 2000.0);

    TemperatureParams reference;
    reference.mode = std::string(kTemperatureModeCameraReference);
    auto camera = render_temperature(reference);
    ASSERT_TRUE(camera) << camera.error().message;
    const auto camera_sums = sums(camera.value());
    EXPECT_NEAR(static_cast<double>(camera_sums[0]), 363500.0, 2000.0);
    EXPECT_NEAR(static_cast<double>(camera_sums[1]), 284155.0, 2000.0);
    EXPECT_NEAR(static_cast<double>(camera_sums[2]), 241746.0, 2000.0);
    EXPECT_NE(camera_sums, manual_sums);
}

TEST(EngineFacadeTest, TemperatureLateReferenceUsesOnlyExplicitChannelMixerCat)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    ChannelMixerParams calibration;
    calibration.adaptation = std::string(kChannelMixerAdaptationCat16);
    calibration.illuminant_x = 0.3819674253463745;
    calibration.illuminant_y = 0.36998802423477173;
    calibration.gamut = 1.0;
    calibration.clip = true;
    const auto render = [&](TemperatureParams params)
    {
        Recipe recipe;
        recipe.asset = {"mire1", mire1_path(), std::nullopt};
        declare_input(recipe);
        recipe.operations.push_back(temperature_operation(params));
        recipe.operations.push_back(channel_mixer_operation(calibration));
        recipe.operations.push_back(sigmoid_operation());
        RenderRequest request;
        request.asset = recipe.asset;
        request.recipe = recipe;
        request.output_width = 64;
        request.output_height = 48;
        return engine.value().render_to_image(request);
    };

    auto manual_params = test::temperature_0000_params();
    auto manual = render(manual_params);
    ASSERT_TRUE(manual) << manual.error().message;
    auto late = render(test::temperature_0171_late_params());
    ASSERT_TRUE(late) << late.error().message;
    EXPECT_EQ(late.value().rgb, manual.value().rgb);
}

TEST(EngineFacadeTest, ChannelMixerHasARealRawReference)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    ChannelMixerParams calibration;
    calibration.adaptation = std::string(kChannelMixerAdaptationCat16);
    calibration.illuminant_x = 0.3819674253463745;
    calibration.illuminant_y = 0.36998802423477173;
    calibration.gamut = 1.0;
    calibration.clip = true;
    Recipe recipe;
    recipe.asset = {"mire1", mire1_path(), std::nullopt};
    declare_input(recipe);
    recipe.operations.push_back(channel_mixer_operation(calibration));
    recipe.operations.push_back(sigmoid_operation());
    RenderRequest request;
    request.asset = recipe.asset;
    request.recipe = recipe;
    request.output_width = 64;
    request.output_height = 48;
    auto rendered = engine.value().render_to_image(request);
    ASSERT_TRUE(rendered) << rendered.error().message;
    ASSERT_EQ(rendered.value().width, 64U);
    ASSERT_EQ(rendered.value().height, 48U);
    std::array<std::uint64_t, 3> sums{};
    for (std::size_t index = 0; index + 2 < rendered.value().rgb.size(); index += 3)
    {
        for (std::size_t channel = 0; channel < sums.size(); ++channel)
        {
            sums[channel] += rendered.value().rgb[index + channel];
        }
    }
    // Ravo-owned reference for the frozen 0085 default CAT16 parameters on mire1.cr2.
    EXPECT_NEAR(static_cast<double>(sums[0]), 253873.0, 2000.0);
    EXPECT_NEAR(static_cast<double>(sums[1]), 290768.0, 2000.0);
    EXPECT_NEAR(static_cast<double>(sums[2]), 298343.0, 2000.0);
}

TEST(EngineFacadeTest, ColorBalanceRgb0083HasARealRawReference)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    Recipe recipe;
    recipe.asset = {"mire1", mire1_path(), std::nullopt};
    declare_input(recipe);
    recipe.operations.push_back(color_balance_rgb_operation(test::color_balance_0083_params()));
    recipe.operations.push_back(sigmoid_operation());
    RenderRequest request;
    request.asset = recipe.asset;
    request.recipe = recipe;
    request.output_width = 64;
    request.output_height = 48;
    auto rendered = engine.value().render_to_image(request);
    ASSERT_TRUE(rendered) << rendered.error().message;
    ASSERT_EQ(rendered.value().width, 64U);
    ASSERT_EQ(rendered.value().height, 48U);
    std::array<std::uint64_t, 3> sums{};
    for (std::size_t index = 0; index + 2 < rendered.value().rgb.size(); index += 3)
    {
        for (std::size_t channel = 0; channel < sums.size(); ++channel)
        {
            sums[channel] += rendered.value().rgb[index + channel];
        }
    }
    // Ravo-owned macOS reference for the statically decoded 0083 schema-v4 parameters
    // in explicit linear_srgb_d50 working space. Cross-platform libm tolerance is recorded
    // without treating the unavailable legacy runner as an oracle.
    EXPECT_NEAR(static_cast<double>(sums[0]), 270856.0, 2500.0);
    EXPECT_NEAR(static_cast<double>(sums[1]), 283113.0, 2500.0);
    EXPECT_NEAR(static_cast<double>(sums[2]), 241983.0, 2500.0);
}

TEST(EngineFacadeTest, ColorChecker0098HasARealRawReferenceAndPreservesTheSource)
{
    const auto source_before = source_file_snapshot(mire1_path());
    ASSERT_TRUE(source_before.has_value());
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    ColorCheckerParams params;
    params.patches[7].target_lab = {92.74998474121094, 97.59593200683594, 82.81928253173828};
    params.patches[19].target_lab = {72.97999572753906, 43.90998840332031, 35.799983978271484};
    params.patches[22].target_lab = {45.439998626708984, -0.41999998688697815, 59.32999801635742};
    auto parameters = color_checker_to_parameters(params);
    ASSERT_TRUE(parameters) << parameters.error().message;
    Recipe recipe;
    recipe.asset = {"mire1", mire1_path(), std::nullopt};
    declare_input(recipe);
    recipe.operations.push_back({std::string(kColorCheckerOperationId),
                                 kColorCheckerOperationSchemaVersion, "colorchecker-0098", true,
                                 std::move(parameters).value(), std::nullopt});
    recipe.operations.push_back(sigmoid_operation());
    RenderRequest request;
    request.asset = recipe.asset;
    request.recipe = recipe;
    request.output_width = 64U;
    request.output_height = 48U;
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
    // Ravo-owned macOS reference for the verbatim frozen 0098 active patch set on
    // the pinned RAW fixture. The independent scalar oracle above owns fit parity.
    EXPECT_NEAR(static_cast<double>(sums[0]), 295886.0, 2500.0);
    EXPECT_NEAR(static_cast<double>(sums[1]), 283466.0, 2500.0);
    EXPECT_NEAR(static_cast<double>(sums[2]), 247458.0, 2500.0);
    const auto source_after = source_file_snapshot(mire1_path());
    ASSERT_TRUE(source_after.has_value());
    EXPECT_EQ(*source_after, *source_before);
}

TEST(EngineFacadeTest, LegacyColorBalanceV4HasARealRawReferenceAndPreservesTheSource)
{
    const auto source_before = source_file_snapshot(mire1_path());
    ASSERT_TRUE(source_before.has_value());
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    ColorBalanceParams params;
    params.mode = std::string(kColorBalanceModeLiftGammaGain);
    params.lift = {0.96, 1.03, 0.98, 1.06};
    params.gamma = {1.08, 0.91, 1.05, 0.97};
    params.gain = {1.04, 1.12, 0.95, 1.08};
    params.input_saturation = 0.84;
    params.contrast = 1.16;
    params.grey_fulcrum_percent = 18.0;
    params.output_saturation = 1.09;
    Recipe recipe;
    recipe.asset = {"mire1", mire1_path(), std::nullopt};
    declare_input(recipe);
    recipe.operations.push_back(legacy_color_balance_operation(params));
    recipe.operations.push_back(sigmoid_operation());
    RenderRequest request;
    request.asset = recipe.asset;
    request.recipe = recipe;
    request.output_width = 64U;
    request.output_height = 48U;
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
    // Ravo-owned reference for the frozen v4 LGG path on the pinned RAW fixture.
    // The tolerance permits cross-platform libm rounding without accepting a mode,
    // working-space, or channel-order change.
    EXPECT_NEAR(static_cast<double>(sums[0]), 370241.0, 2500.0);
    EXPECT_NEAR(static_cast<double>(sums[1]), 274553.0, 2500.0);
    EXPECT_NEAR(static_cast<double>(sums[2]), 346452.0, 2500.0);
    const auto source_after = source_file_snapshot(mire1_path());
    ASSERT_TRUE(source_after.has_value());
    EXPECT_EQ(*source_after, *source_before);
}

TEST(EngineFacadeTest, HotPixelsHasARealRawReferenceAndKeepsDecodedFrameImmutable)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    Recipe recipe;
    recipe.asset = {"mire1", mire1_path(), std::nullopt};
    declare_input(recipe);
    recipe.operations.push_back(hot_pixels_operation());
    recipe.operations.push_back(sigmoid_operation());

    auto decoded = engine.value().decode_raw_frame(mire1_path(), CancellationToken{});
    ASSERT_TRUE(decoded) << decoded.error().message;
    const auto original_pixels = decoded.value().pixels;
    auto working = engine.value().linear_working_from_raw(decoded.value(), recipe, 64, 48,
                                                          CancellationToken{});
    ASSERT_TRUE(working) << working.error().message;
    EXPECT_EQ(decoded.value().pixels, original_pixels);

    RenderRequest request;
    request.asset = recipe.asset;
    request.recipe = recipe;
    request.output_width = 64;
    request.output_height = 48;
    auto rendered = engine.value().render_to_image(request);
    ASSERT_TRUE(rendered) << rendered.error().message;
    std::array<std::uint64_t, 3> sums{};
    for (std::size_t index = 0; index + 2 < rendered.value().rgb.size(); index += 3)
    {
        for (std::size_t channel = 0; channel < sums.size(); ++channel)
        {
            sums[channel] += rendered.value().rgb[index + channel];
        }
    }
    // Ravo-owned reference for the frozen default Bayer neighbour contract on mire1.cr2.
    EXPECT_NEAR(static_cast<double>(sums[0]), 304270.0, 1500.0);
    EXPECT_NEAR(static_cast<double>(sums[1]), 280908.0, 1500.0);
    EXPECT_NEAR(static_cast<double>(sums[2]), 261887.0, 1500.0);
}

TEST(EngineFacadeTest, RawCaCorrectRunsFrozenDefaultOnMire1)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    Recipe recipe;
    recipe.asset = {"mire1", mire1_path(), std::nullopt};
    declare_input(recipe);
    recipe.operations.push_back(raw_ca_operation());
    recipe.operations.push_back(sigmoid_operation());
    auto decoded = engine.value().decode_raw_frame(mire1_path(), CancellationToken{});
    ASSERT_TRUE(decoded) << decoded.error().message;
    const auto original = decoded.value().pixels;
    auto working = engine.value().linear_working_from_raw(decoded.value(), recipe, 64, 48,
                                                          CancellationToken{});
    ASSERT_TRUE(working) << working.error().message;
    EXPECT_EQ(decoded.value().pixels, original);

    RenderRequest budgeted;
    budgeted.asset = recipe.asset;
    budgeted.recipe = recipe;
    budgeted.output_width = 64;
    budgeted.output_height = 48;
    budgeted.memory_budget_bytes = 64U * 1024U * 1024U;
    auto budget_failure = engine.value().render_to_image(budgeted);
    ASSERT_FALSE(budget_failure);
    EXPECT_EQ(budget_failure.error().code, ErrorCode::kValidation);

    Recipe rgb_recipe = recipe;
    for (auto &operation : rgb_recipe.operations)
    {
        if (operation.id == "ravo.raw.cacorrect")
        {
            operation.enabled = false;
        }
    }
    auto rendered =
        engine.value().render_linear_working(working.value(), rgb_recipe, CancellationToken{});
    ASSERT_TRUE(rendered) << rendered.error().message;
    EXPECT_EQ(rendered.value().width, 64U);
    EXPECT_EQ(rendered.value().height, 48U);
    std::array<std::uint64_t, 3> sums{};
    for (std::size_t index = 0; index + 2 < rendered.value().rgb.size(); index += 3)
    {
        for (std::size_t channel = 0; channel < sums.size(); ++channel)
        {
            sums[channel] += rendered.value().rgb[index + channel];
        }
    }
    EXPECT_NEAR(static_cast<double>(sums[0]), 303686.0, 2000.0);
    EXPECT_NEAR(static_cast<double>(sums[1]), 280852.0, 2000.0);
    EXPECT_NEAR(static_cast<double>(sums[2]), 262220.0, 2000.0);
}

TEST(EngineFacadeTest, RawCaCorrectCoversFrozen0084AvoidShiftParameters)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    Recipe recipe;
    recipe.asset = {"mire1", mire1_path(), std::nullopt};
    declare_input(recipe);
    recipe.operations.push_back(raw_ca_operation(5, true));
    recipe.operations.push_back(sigmoid_operation());
    auto decoded = engine.value().decode_raw_frame(mire1_path(), CancellationToken{});
    ASSERT_TRUE(decoded) << decoded.error().message;
    auto working = engine.value().linear_working_from_raw(decoded.value(), recipe, 64, 48,
                                                          CancellationToken{});
    ASSERT_TRUE(working) << working.error().message;
    Recipe rgb_recipe = recipe;
    for (auto &operation : rgb_recipe.operations)
    {
        if (operation.id == "ravo.raw.cacorrect")
        {
            operation.enabled = false;
        }
    }
    auto rendered =
        engine.value().render_linear_working(working.value(), rgb_recipe, CancellationToken{});
    ASSERT_TRUE(rendered) << rendered.error().message;
    std::array<std::uint64_t, 3> sums{};
    for (std::size_t index = 0; index + 2 < rendered.value().rgb.size(); index += 3)
    {
        for (std::size_t channel = 0; channel < sums.size(); ++channel)
        {
            sums[channel] += rendered.value().rgb[index + channel];
        }
    }
    EXPECT_NEAR(static_cast<double>(sums[0]), 304117.0, 2000.0);
    EXPECT_NEAR(static_cast<double>(sums[1]), 280976.0, 2000.0);
    EXPECT_NEAR(static_cast<double>(sums[2]), 261636.0, 2000.0);
}

TEST(EngineFacadeTest, LinearWorkingRenderMatchesDirectRawRender)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    Recipe recipe;
    recipe.asset = {"mire1", mire1_path(), std::nullopt};
    declare_input(recipe);
    recipe.operations.push_back(sigmoid_operation());
    RenderRequest request;
    request.asset = recipe.asset;
    request.recipe = recipe;
    request.output_width = 64;
    request.output_height = 48;
    auto direct = engine.value().render_to_image(request);
    ASSERT_TRUE(direct) << direct.error().message;

    auto decoded = engine.value().decode_raw_frame(mire1_path(), CancellationToken{});
    ASSERT_TRUE(decoded) << decoded.error().message;
    auto linear = engine.value().linear_working_from_raw(decoded.value(), recipe, 64, 48,
                                                         CancellationToken{});
    ASSERT_TRUE(linear) << linear.error().message;
    EXPECT_EQ(linear.value().width, 64U);
    EXPECT_EQ(linear.value().height, 48U);
    ASSERT_EQ(linear.value().rgb.size(), static_cast<std::size_t>(64U) * 48U * 3U);
    Recipe rgb_recipe = recipe;
    for (auto &operation : rgb_recipe.operations)
    {
        if (operation.id == "ravo.raw.highlights")
        {
            operation.enabled = false;
        }
    }
    auto from_working =
        engine.value().render_linear_working(linear.value(), rgb_recipe, CancellationToken{});
    ASSERT_TRUE(from_working) << from_working.error().message;
    EXPECT_EQ(from_working.value().rgb, direct.value().rgb);

    Recipe exposed = rgb_recipe;
    exposed.operations.insert(exposed.operations.begin(), {"ravo.core.exposure",
                                                           1,
                                                           "exposure-1",
                                                           true,
                                                           {{"exposure_ev", ParameterValue{1.0}}},
                                                           std::nullopt});
    auto shifted =
        engine.value().render_linear_working(linear.value(), exposed, CancellationToken{});
    ASSERT_TRUE(shifted) << shifted.error().message;
    EXPECT_NE(shifted.value().rgb, from_working.value().rgb);
    EXPECT_EQ(linear.value().rgb.size(), static_cast<std::size_t>(64U) * 48U * 3U);
}

TEST(EngineFacadeTest, RgbHistogramMatchesFrozenDisplayBinning)
{
    const auto raster = solid_raster(8, 4, 220, 20, 20);
    auto histogram = collect_rgb_histogram(raster);
    ASSERT_TRUE(histogram) << histogram.error().message;
    EXPECT_EQ(histogram.value().red[220], 32U);
    EXPECT_EQ(histogram.value().green[20], 32U);
    EXPECT_EQ(histogram.value().blue[20], 32U);
    EXPECT_EQ(histogram.value().red[0], 0U);
    EXPECT_EQ(histogram.value().max_count, 32U);

    RasterBuffer empty;
    auto rejected = collect_rgb_histogram(empty);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kInvalidArgument);
}

TEST(EngineFacadeTest, RgbParadePlacesFullWhiteAtEightNinths)
{
    const auto raster = solid_raster(12, 8, 255, 255, 255);
    auto parade = collect_rgb_parade(raster);
    ASSERT_TRUE(parade) << parade.error().message;
    EXPECT_EQ(parade.value().tones, kWaveformTones);
    ASSERT_GT(parade.value().bins, 0U);
    const std::uint32_t width = parade.value().bins * 3U;
    const std::uint32_t height = parade.value().tones;
    ASSERT_EQ(parade.value().rgb.size(), static_cast<std::size_t>(width) * height * 3U);
    const auto sample = [&](const std::uint32_t x, const std::uint32_t y, const std::uint32_t ch)
    { return parade.value().rgb[(static_cast<std::size_t>(y) * width + x) * 3U + ch]; };
    // Frozen waveform maps 1.0 to 8/9 of the tone axis: ceil((8/9)*159) = 142.
    constexpr std::uint32_t kTone = 142;
    const std::uint32_t y = kWaveformTones - 1U - kTone;
    EXPECT_GT(sample(0, y, 0), 0U);
    EXPECT_EQ(sample(0, y, 1), 0U);
    EXPECT_EQ(sample(0, height - 1U, 0), 0U);
}

TEST(EngineFacadeTest, ToneCurveMapsSyntheticRasterAndAcceptsLab)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    const auto gray = solid_raster(8, 8, 128, 128, 128);
    auto identity = render_op(engine.value(), gray,
                              {"ravo.core.tonecurve",
                               1,
                               "curve-identity",
                               true,
                               {{"working_space", ParameterValue{"rgb"}},
                                {"interpolation", ParameterValue{"monotone_hermite"}},
                                {"channel_mode", ParameterValue{"rgb"}},
                                {"preserve_colors", ParameterValue{"average"}},
                                {"points", tone_curve_points({{0.0, 0.0}, {1.0, 1.0}})}},
                               std::nullopt});
    ASSERT_TRUE(identity) << identity.error().message;
    EXPECT_EQ(identity.value().rgb[0], 128);

    const auto lifted_points = tone_curve_points({{0.0, 0.0}, {128.0 / 255.0, 0.75}, {1.0, 1.0}});
    auto rgb = render_op(engine.value(), gray,
                         {"ravo.core.tonecurve",
                          1,
                          "curve-rgb",
                          true,
                          {{"working_space", ParameterValue{"rgb"}},
                           {"interpolation", ParameterValue{"monotone_hermite"}},
                           {"channel_mode", ParameterValue{"rgb"}},
                           {"preserve_colors", ParameterValue{"average"}},
                           {"points", lifted_points}},
                          std::nullopt});
    ASSERT_TRUE(rgb) << rgb.error().message;
    EXPECT_GT(rgb.value().rgb[0], 128);

    auto lab = render_op(engine.value(), gray,
                         {"ravo.core.tonecurve",
                          1,
                          "curve-lab",
                          true,
                          {{"working_space", ParameterValue{"lab"}}, {"points", lifted_points}},
                          std::nullopt});
    ASSERT_TRUE(lab) << lab.error().message;
    EXPECT_GT(lab.value().rgb[0], 128);

    const auto red = solid_raster(8, 8, 220, 40, 30);
    auto rgb_red = render_op(engine.value(), red,
                             {"ravo.core.tonecurve",
                              1,
                              "curve-rgb-red",
                              true,
                              {{"working_space", ParameterValue{"rgb"}},
                               {"preserve_colors", ParameterValue{"average"}},
                               {"points", lifted_points}},
                              std::nullopt});
    auto lab_red = render_op(engine.value(), red,
                             {"ravo.core.tonecurve",
                              1,
                              "curve-lab-red",
                              true,
                              {{"working_space", ParameterValue{"lab"}}, {"points", lifted_points}},
                              std::nullopt});
    ASSERT_TRUE(rgb_red) << rgb_red.error().message;
    ASSERT_TRUE(lab_red) << lab_red.error().message;
    EXPECT_NE(lab_red.value().rgb, rgb_red.value().rgb);
}

TEST(EngineFacadeTest, UnknownCpuOperationFailsFast)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    auto rendered = render_op(engine.value(), solid_raster(4, 4, 10, 20, 30),
                              {"ravo.creative.unknown", 1, "x", true, {}, std::nullopt});
    ASSERT_FALSE(rendered);
    EXPECT_EQ(rendered.error().code, ErrorCode::kUnsupported);
}

} // namespace
} // namespace ravo
