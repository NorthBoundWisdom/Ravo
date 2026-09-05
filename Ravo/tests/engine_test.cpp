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
#include "ravo/recipe/rapidraw_tone.h"
#include "ravo/recipe/rapidraw_tone_controls.h"

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
#include "engine_test_support.h"
#include "test_support.h"

namespace ravo
{
namespace
{
using namespace engine_test_support;

class RecordingProgressSink final : public ProgressSink
{
public:
    void on_progress(const ProgressEvent &event) override
    {
        events.push_back(event);
    }

    std::vector<ProgressEvent> events;
};

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
    EXPECT_NE(engine.value().operations().end(),
              std::find_if(engine.value().operations().begin(), engine.value().operations().end(),
                           [](const OperationDescriptor &item)
                           { return item.id == kRapidRawBasicToneOperationId; }));
    EXPECT_NE(engine.value().operations().end(),
              std::find_if(engine.value().operations().begin(), engine.value().operations().end(),
                           [](const OperationDescriptor &item)
                           { return item.id == kRapidRawToneControlsOperationId; }));
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
    const WorkingImage input{2, 1, {-0.5F, 0.0F, 0.25F, 0.5F, 1.0F, 2.0F}, profile, {}, {}, {}};
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
    WorkingImage input{1, 1, {0.25F, 0.5F, 0.75F}, {}, {}, {}, {}};
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
    WorkingImage input{1, 1, {0.25F, 0.5F, 0.75F}, {}, {}, {}, {}};
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
    WorkingImage input{1, 1, {0.25F, 0.5F, 0.75F}, {}, {}, {}, {}};
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
    WorkingImage input{1, 1, {0.25F, 0.5F, 0.75F}, {}, {}, {}, {}};
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
    WorkingImage input{1, 1, {0.25F, 0.5F, 0.75F}, {}, {}, {}, {}};
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
    WorkingImage input{1, 1, {0.25F, 0.5F, 0.75F}, {}, {}, {}, {}};
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
              raw.pixels.size() * (sizeof(std::uint16_t) + sizeof(float)) + failure_capacity + 1U);
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
    EXPECT_FLOAT_EQ(full.reference_short_edge(), 6.0F);
    EXPECT_FLOAT_EQ(downscaled.reference_short_edge(), 3.0F);
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
    EXPECT_FLOAT_EQ(raster_working.value().canonical_roi_scale.reference_short_edge(), 3.0F);
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
    EXPECT_FLOAT_EQ(raw_working.value().canonical_roi_scale.reference_short_edge(), 1.0F);
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

TEST(EngineFacadeTest, ReadsExifLensMakeAndLensModelFromSyntheticTiff)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    const auto root =
        std::filesystem::temp_directory_path() / ("ravo-engine-lens-" + generate_catalog_id());
    std::filesystem::create_directories(root);
    test_support::CaptureExifProfile profile;
    profile.lens_make = "RavoOptics";
    profile.lens_model = "Prime 35mm f/1.8 MACRO";
    const auto path = root / "lens-name.tif";
    const auto bytes = test_support::make_capture_exif_tiff(profile);
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.close();
    auto extracted =
        engine.value().read_embedded_capture_metadata(path.string(), CancellationToken{});
    ASSERT_TRUE(extracted) << extracted.error().message;
    ASSERT_TRUE(extracted.value().lens_make);
    ASSERT_TRUE(extracted.value().lens_model);
    EXPECT_EQ(*extracted.value().lens_make, "RavoOptics");
    EXPECT_EQ(*extracted.value().lens_model, "Prime 35mm f/1.8 MACRO");
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
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
    profile.lens_make = std::string("Canon");
    profile.lens_make->append(2, '\0');
    auto padded = read_bytes("padded-ascii.tif", test_support::make_capture_exif_tiff(profile));
    ASSERT_TRUE(padded) << padded.error().message;
    ASSERT_TRUE(padded.value().lens_make);
    EXPECT_EQ(*padded.value().lens_make, "Canon");
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
    EXPECT_TRUE(inspected.value().has_as_shot_white_balance);
    EXPECT_GT(inspected.value().as_shot_white_balance[0], 0.0);
    EXPECT_NEAR(inspected.value().as_shot_white_balance[1], 1.0, 1.0e-6);
}

} // namespace
} // namespace ravo
