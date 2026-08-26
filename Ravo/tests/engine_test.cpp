#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
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

#include "ravo/engine/engine.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/operation.h"

#include "color_balance_fixture.h"
#include "color_balance_rgb.h"
#include "image_ops.h"
#include "input_color.h"
#include "primaries.h"
#include "raw_pipeline.h"
#include "raw_temperature.h"
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
    const WorkingImage source{1, 1, {0.08F, 0.18F, 0.40F}, {}, {}};
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
