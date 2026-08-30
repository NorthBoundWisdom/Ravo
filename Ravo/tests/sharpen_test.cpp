#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ravo/adapters/legacy_xmp.h"
#include "ravo/engine/engine.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/recipe/color_input.h"
#include "ravo/recipe/color_output.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/operation.h"
#include "ravo/recipe/sharpen.h"

#include "d50_lab.h"
#include "raw_pipeline.h"
#include "sharpen.h"

namespace ravo
{
namespace
{

using LabPixel = detail::SharpenLabPixel;

constexpr std::string_view kFrozenParameters = "000000400000003f0000003f";
constexpr std::string_view kFrozenBlendV9 = "gz11eJxjYGBgkGAAgRNODGiAEV0AJ2iwh+CRyscOAAdeGQQ=";
constexpr std::string_view kFrozenBlendV11 = "gz13eJxjYGBgYAJiCQYYOOHEgAZY0QVwggZ7CB6pfNoAAFJgGQo=";

[[nodiscard]] std::filesystem::path repository_root()
{
    return std::filesystem::path(RAVO_REPOSITORY_ROOT);
}

[[nodiscard]] std::string mire1_path()
{
    return (repository_root() / "legacy" / "tests" / "images" / "mire1.cr2").string();
}

[[nodiscard]] std::optional<std::string> read_file(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        return std::nullopt;
    }
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

struct SourceFileSnapshot
{
    std::uintmax_t size = 0U;
    std::filesystem::file_time_type modified;
    std::uint64_t hash = 1469598103934665603ULL;

    [[nodiscard]] bool operator==(const SourceFileSnapshot &) const = default;
};

[[nodiscard]] std::optional<SourceFileSnapshot> source_snapshot(const std::string &path)
{
    std::error_code error;
    SourceFileSnapshot snapshot;
    snapshot.size = std::filesystem::file_size(path, error);
    if (error)
    {
        return std::nullopt;
    }
    snapshot.modified = std::filesystem::last_write_time(path, error);
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
        for (std::streamsize index = 0; index < input.gcount(); ++index)
        {
            snapshot.hash ^= static_cast<std::uint8_t>(block[static_cast<std::size_t>(index)]);
            snapshot.hash *= 1099511628211ULL;
        }
    }
    return input.eof() ? std::optional<SourceFileSnapshot>{snapshot} : std::nullopt;
}

[[nodiscard]] std::vector<LabPixel> synthetic_lab(const std::uint32_t width,
                                                  const std::uint32_t height)
{
    std::vector<LabPixel> pixels(static_cast<std::size_t>(width) * height);
    for (std::uint32_t row = 0U; row < height; ++row)
    {
        for (std::uint32_t column = 0U; column < width; ++column)
        {
            const std::size_t index = static_cast<std::size_t>(row) * width + column;
            pixels[index] = {25.0F + 0.5F * static_cast<float>(column) +
                                 0.25F * static_cast<float>(row),
                             -18.0F + static_cast<float>(column), 12.0F - static_cast<float>(row)};
        }
    }
    const std::size_t center = static_cast<std::size_t>(height / 2U) * width + width / 2U;
    pixels[center][0] += 24.0F;
    pixels[center - 1U][0] -= 8.0F;
    pixels[center + 1U][0] += 5.0F;
    return pixels;
}

[[nodiscard]] std::vector<LabPixel>
frozen_sharpen_oracle(const std::vector<LabPixel> &input, const std::uint32_t width,
                      const std::uint32_t height, const float scale, const SharpenParams &params)
{
    const float committed_radius = 2.5F * static_cast<float>(params.radius);
    const float amount = static_cast<float>(params.amount);
    const float threshold = static_cast<float>(params.threshold);
    std::vector<LabPixel> output = input;
    if (committed_radius == 0.0F)
    {
        return output;
    }
    const float scaled_radius = committed_radius * scale;
    const int radius = std::min(12, static_cast<int>(std::ceil(scaled_radius)));
    if (radius == 0 || width < static_cast<std::uint32_t>(2 * radius + 1) ||
        height < static_cast<std::uint32_t>(2 * radius + 1))
    {
        return output;
    }
    const float sigma2 = (1.0F / (2.5F * 2.5F)) * scaled_radius * scaled_radius;
    std::vector<float> kernel(static_cast<std::size_t>(2 * radius + 1));
    float weight = 0.0F;
    for (int offset = -radius; offset <= radius; ++offset)
    {
        const float value = std::exp(-static_cast<float>(offset * offset) / (2.0F * sigma2));
        kernel[static_cast<std::size_t>(offset + radius)] = value;
        weight += value;
    }
    for (float &value : kernel)
    {
        value /= weight;
    }
    std::vector<float> vertical(width);
    for (std::uint32_t row = static_cast<std::uint32_t>(radius);
         row < height - static_cast<std::uint32_t>(radius); ++row)
    {
        const std::uint32_t first_row = row - static_cast<std::uint32_t>(radius);
        const std::uint32_t last_row = row + static_cast<std::uint32_t>(radius);
        for (std::uint32_t column = 0U; column < width; ++column)
        {
            float sum = 0.0F;
            for (std::uint32_t source_row = first_row; source_row <= last_row; ++source_row)
            {
                sum += kernel[source_row - first_row] *
                       input[static_cast<std::size_t>(source_row) * width + column][0];
            }
            vertical[column] = sum;
        }
        for (std::uint32_t column = static_cast<std::uint32_t>(radius);
             column < width - static_cast<std::uint32_t>(radius); ++column)
        {
            const std::uint32_t first_column = column - static_cast<std::uint32_t>(radius);
            const std::uint32_t last_column = column + static_cast<std::uint32_t>(radius);
            float sum = 0.0F;
            for (std::uint32_t source_column = first_column; source_column <= last_column;
                 ++source_column)
            {
                sum += kernel[source_column - first_column] * vertical[source_column];
            }
            const std::size_t index = static_cast<std::size_t>(row) * width + column;
            const float difference = input[index][0] - sum;
            const float absolute = std::fabs(difference);
            const float detail =
                absolute > threshold ?
                    std::copysign(std::fmax(absolute - threshold, 0.0F), difference) :
                    0.0F;
            output[index][0] = input[index][0] + detail * amount;
        }
    }
    return output;
}

[[nodiscard]] WorkingImage working_from_lab(const std::vector<LabPixel> &lab,
                                            const std::uint32_t width, const std::uint32_t height)
{
    WorkingImage image;
    image.width = width;
    image.height = height;
    image.color_profile.kind = ColorProfileKind::kBuiltin;
    image.color_profile.model = ColorModel::kRgb;
    image.color_profile.identifier = std::string(kInputProfileLinearRec709);
    image.color_profile.icc_bytes = {1U, 2U, 3U};
    image.exposure_analysis = std::make_shared<ExposureAnalysisContext>();
    image.canonical_roi_scale =
        CanonicalRoiScale::from_scaled_dimensions(width, height, width, height);
    image.rgb.reserve(lab.size() * 3U);
    for (const auto &pixel : lab)
    {
        const auto rgb = d50_lab::xyz_to_linear_rec709(d50_lab::lab_to_xyz(pixel));
        image.rgb.insert(image.rgb.end(), rgb.begin(), rgb.end());
    }
    return image;
}

[[nodiscard]] std::array<std::uint32_t, 3> bits(const LabPixel &pixel) noexcept
{
    return {std::bit_cast<std::uint32_t>(pixel[0]), std::bit_cast<std::uint32_t>(pixel[1]),
            std::bit_cast<std::uint32_t>(pixel[2])};
}

[[nodiscard]] std::string minimal_sharpen_xmp(const std::string_view version = "1",
                                              const std::string_view enabled = "1",
                                              const std::string_view parameters = kFrozenParameters,
                                              const std::string_view blend_version = "9",
                                              const std::string_view blend = kFrozenBlendV9,
                                              const std::string_view extra = {},
                                              const bool duplicate = false)
{
    const auto entry = [&]
    {
        std::string value =
            R"(<rdf:li darktable:operation="sharpen" darktable:num="7" darktable:enabled=")";
        value += enabled;
        value += R"(" darktable:modversion=")";
        value += version;
        value += R"(" darktable:params=")";
        value += parameters;
        value +=
            R"(" darktable:multi_name="" darktable:multi_priority="0" darktable:blendop_version=")";
        value += blend_version;
        value += R"(" darktable:blendop_params=")";
        value += blend;
        value += '"';
        value += extra;
        value += "/>";
        return value;
    }();
    std::string document = R"(<?xml version="1.0"?>
<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"
         xmlns:darktable="http://darktable.sf.net/">
  <rdf:Description darktable:xmp_version="6"><darktable:history><rdf:Seq>)";
    document += entry;
    if (duplicate)
    {
        document += entry;
    }
    document += R"(</rdf:Seq></darktable:history></rdf:Description></rdf:RDF>)";
    return document;
}

struct CancellationFixture
{
    CancellationSource *source = nullptr;
    detail::SharpenCheckpoint target = detail::SharpenCheckpoint::kBeforeValidation;
    bool fired = false;
};

void cancel_at_checkpoint(void *const context, const detail::SharpenCheckpoint checkpoint,
                          std::uint32_t) noexcept
{
    auto &fixture = *static_cast<CancellationFixture *>(context);
    if (!fixture.fired && checkpoint == fixture.target)
    {
        fixture.fired = fixture.source->cancel("sharpen-checkpoint");
    }
}

TEST(SharpenRecipeTest, V2SchemaUpgradesCurrentRavoV1AndDevelopOwnsThreshold)
{
    SharpenParams params;
    auto encoded = sharpen_to_parameters(params);
    ASSERT_TRUE(encoded) << encoded.error().message;
    EXPECT_EQ(encoded.value().size(), 5U);
    auto decoded = sharpen_from_parameters(encoded.value());
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded.value(), params);

    OperationInstance v1{std::string(kSharpenOperationId),
                         1,
                         "sharpen-v1",
                         true,
                         {{"amount", ParameterValue{0.4}},
                          {"radius", ParameterValue{1.5}},
                          {"threshold", ParameterValue{0.25}}},
                         std::nullopt};
    ASSERT_TRUE(upgrade_sharpen_operation(v1));
    EXPECT_EQ(v1.schema_version, kSharpenOperationSchemaVersion);
    auto upgraded = sharpen_from_parameters(v1.parameters);
    ASSERT_TRUE(upgraded) << upgraded.error().message;
    EXPECT_EQ(upgraded.value(), (SharpenParams{1.5, 0.4, 0.25}));

    const auto registry = make_phase1_registry();
    ASSERT_TRUE(registry) << registry.error().message;
    const auto *descriptor = registry.value().find(kSharpenOperationId);
    ASSERT_NE(descriptor, nullptr);
    EXPECT_EQ(descriptor->parameter_schema_version, kSharpenOperationSchemaVersion);
    EXPECT_EQ(descriptor->parameters.size(), 5U);
    EXPECT_TRUE(descriptor->cpu_reference_available);
    EXPECT_FALSE(descriptor->supports_mask);

    DevelopParams develop;
    ASSERT_TRUE(apply_develop_field_strict(develop, "sharpen", 0.5));
    ASSERT_TRUE(apply_develop_field_strict(develop, "sharpenRadius", 8.0));
    ASSERT_TRUE(apply_develop_field_strict(develop, "sharpenThreshold", 3.25));
    auto recipe = recipe_from_develop({"asset", "file:///fixture.raw", std::nullopt}, develop);
    ASSERT_TRUE(recipe) << recipe.error().message;
    const auto operation =
        std::find_if(recipe.value().operations.begin(), recipe.value().operations.end(),
                     [](const OperationInstance &item) { return item.id == kSharpenOperationId; });
    ASSERT_NE(operation, recipe.value().operations.end());
    EXPECT_EQ(operation->schema_version, kSharpenOperationSchemaVersion);
    auto round_trip = develop_from_recipe(recipe.value());
    ASSERT_TRUE(round_trip) << round_trip.error().message;
    EXPECT_DOUBLE_EQ(round_trip.value().sharpen, 0.5);
    EXPECT_DOUBLE_EQ(round_trip.value().sharpen_radius, 8.0);
    EXPECT_DOUBLE_EQ(round_trip.value().sharpen_threshold, 3.25);
    EXPECT_FALSE(apply_develop_field_strict(round_trip.value(), "sharpenRadius", 99.01));
    EXPECT_TRUE(apply_develop_field_strict(round_trip.value(), "sharpenRadius", 99.0));
    EXPECT_FALSE(apply_develop_field_strict(round_trip.value(), "sharpenThreshold", -0.01));
}

TEST(SharpenLegacyXmpTest, ThreeFrozenRecordsMapOnlyTheirDefaultUnmaskedSingletons)
{
    const auto import = [](const std::string &xmp)
    { return import_legacy_xmp({xmp, {"asset", "file:///fixture.raw", std::nullopt}}); };
    auto v9 = import(minimal_sharpen_xmp());
    ASSERT_TRUE(v9) << v9.error().message;
    ASSERT_EQ(v9.value().operations.size(), 3U);
    auto params = sharpen_from_parameters(v9.value().operations[1].parameters);
    ASSERT_TRUE(params) << params.error().message;
    EXPECT_EQ(params.value(), SharpenParams{});
    auto v11 = import(minimal_sharpen_xmp("1", "1", kFrozenParameters, "11", kFrozenBlendV11));
    ASSERT_TRUE(v11) << v11.error().message;

    for (const auto &xmp : {minimal_sharpen_xmp("2"), minimal_sharpen_xmp("1", "0"),
                            minimal_sharpen_xmp("1", "1", kFrozenParameters, "10", kFrozenBlendV9),
                            minimal_sharpen_xmp("1", "1", kFrozenParameters, "9", kFrozenBlendV9,
                                                " darktable:mask_id=\"1\"")})
    {
        const auto rejected = import(xmp);
        ASSERT_FALSE(rejected);
        EXPECT_EQ(rejected.error().code, ErrorCode::kUnsupported);
    }
    const auto duplicate =
        import(minimal_sharpen_xmp("1", "1", kFrozenParameters, "9", kFrozenBlendV9, {}, true));
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code, ErrorCode::kConflict);

    std::vector<std::filesystem::path> records;
    for (const auto &entry :
         std::filesystem::recursive_directory_iterator(repository_root() / "legacy" / "tests"))
    {
        if (!entry.is_regular_file() || entry.path().extension() != ".xmp")
        {
            continue;
        }
        const auto text = read_file(entry.path());
        ASSERT_TRUE(text);
        if (text->find("operation=\"sharpen\"") != std::string::npos)
        {
            records.push_back(entry.path());
            EXPECT_NE(text->find(std::string(kFrozenParameters)), std::string::npos);
        }
    }
    std::sort(records.begin(), records.end());
    ASSERT_EQ(records.size(), 3U);
    EXPECT_EQ(records[0].parent_path().filename(), "0028-highpass-overlay");
    EXPECT_EQ(records[1].parent_path().filename(), "0029-color-correction");
    EXPECT_EQ(records[2].parent_path().filename(), "0087-blendif-and-or");

    const auto full = read_file(records[1]);
    ASSERT_TRUE(full);
    const auto full_rejected = import_legacy_xmp({*full, {"mire1", mire1_path(), std::nullopt}});
    ASSERT_FALSE(full_rejected);
    EXPECT_EQ(full_rejected.error().context.at("legacy_operation"), "filmicrgb");
}

TEST(SharpenTest, SeparableGaussianThresholdScaleAndRadiusCapMatchIndependentOracle)
{
    const auto input = synthetic_lab(25U, 25U);
    const std::array cases{
        std::pair{1.0F, SharpenParams{2.0, 0.5, 0.5}},
        std::pair{0.5F, SharpenParams{2.0, 1.25, 0.0}},
        std::pair{1.0F, SharpenParams{99.0, 2.0, 100.0}},
    };
    for (const auto &[scale, params] : cases)
    {
        const auto expected = frozen_sharpen_oracle(input, 25U, 25U, scale, params);
        const auto actual =
            detail::apply_sharpen_lab(input, 25U, 25U, scale, params, CancellationToken{});
        ASSERT_TRUE(actual) << actual.error().message;
        ASSERT_EQ(actual.value().size(), expected.size());
        for (std::size_t index = 0U; index < expected.size(); ++index)
        {
            EXPECT_EQ(bits(actual.value()[index]), bits(expected[index])) << index;
        }
    }

    const auto sharpened =
        detail::apply_sharpen_lab(input, 25U, 25U, 1.0F, {2.0, 0.5, 0.5}, CancellationToken{});
    ASSERT_TRUE(sharpened) << sharpened.error().message;
    const std::size_t center = 12U * 25U + 12U;
    EXPECT_NE(bits(sharpened.value()[center]), bits(input[center]));
    EXPECT_EQ(bits(sharpened.value()[0]), bits(input[0]));
    EXPECT_EQ(sharpened.value()[center][1], input[center][1]);
    EXPECT_EQ(sharpened.value()[center][2], input[center][2]);

    const auto small = synthetic_lab(4U, 4U);
    const auto unchanged =
        detail::apply_sharpen_lab(small, 4U, 4U, 1.0F, {2.0, 0.5, 0.5}, CancellationToken{});
    ASSERT_TRUE(unchanged) << unchanged.error().message;
    EXPECT_EQ(unchanged.value(), small);
}

TEST(SharpenTest, WorkingOwnershipFailuresAndControlledCancellationAreAtomic)
{
    const auto lab = synthetic_lab(15U, 15U);
    const WorkingImage input = working_from_lab(lab, 15U, 15U);
    const WorkingImage original = input;
    const SharpenParams params{2.0, 0.5, 0.5};
    auto output = apply_sharpen(input, params, CancellationToken{});
    ASSERT_TRUE(output) << output.error().message;
    EXPECT_NE(output.value().rgb, input.rgb);
    EXPECT_NE(output.value().rgb.data(), input.rgb.data());
    EXPECT_EQ(output.value().color_profile, input.color_profile);
    EXPECT_NE(output.value().color_profile.icc_bytes.data(), input.color_profile.icc_bytes.data());
    EXPECT_EQ(output.value().exposure_analysis, input.exposure_analysis);
    EXPECT_EQ(input.rgb, original.rgb);
    EXPECT_EQ(input.color_profile, original.color_profile);

    const std::array checkpoints{detail::SharpenCheckpoint::kBeforeValidation,
                                 detail::SharpenCheckpoint::kConvertInputRow,
                                 detail::SharpenCheckpoint::kVerticalRow,
                                 detail::SharpenCheckpoint::kHorizontalRow,
                                 detail::SharpenCheckpoint::kConvertOutputRow,
                                 detail::SharpenCheckpoint::kBeforePublication};
    for (const auto checkpoint : checkpoints)
    {
        CancellationSource cancellation;
        CancellationFixture fixture{&cancellation, checkpoint};
        const auto rejected = detail::apply_sharpen_controlled(input, params, cancellation.token(),
                                                               {&fixture, cancel_at_checkpoint});
        ASSERT_FALSE(rejected);
        EXPECT_TRUE(fixture.fired);
        EXPECT_EQ(rejected.error().code, ErrorCode::kCancelled);
        EXPECT_EQ(input.rgb, original.rgb);
    }

    auto invalid = input;
    invalid.width = 0U;
    auto rejected = apply_sharpen(invalid, params, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "invalid_sharpen_dimensions");
    invalid = input;
    invalid.rgb.pop_back();
    rejected = apply_sharpen(invalid, params, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "invalid_sharpen_buffer");
    invalid = input;
    invalid.color_profile.identifier = "srgb";
    rejected = apply_sharpen(invalid, params, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "unsupported_sharpen_working_space");
    invalid = input;
    invalid.canonical_roi_scale = {};
    rejected = apply_sharpen(invalid, params, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "invalid_sharpen_roi_scale");
    invalid = input;
    invalid.rgb.front() = std::numeric_limits<float>::infinity();
    rejected = apply_sharpen(invalid, params, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "nonfinite_sharpen_input");

    auto parameters = sharpen_to_parameters(params);
    ASSERT_TRUE(parameters) << parameters.error().message;
    OperationInstance operation{std::string(kSharpenOperationId),
                                kSharpenOperationSchemaVersion,
                                "sharpen-test",
                                true,
                                parameters.value(),
                                std::nullopt};
    operation.mask_id = "mask";
    rejected = apply_sharpen(input, operation, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "sharpen_mask_graph_unavailable");
    operation.mask_id.reset();
    operation.schema_version += 1;
    rejected = apply_sharpen(input, operation, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kUnsupported);

    EXPECT_EQ(detail::sharpen_working_bytes(15U, 15U, params), 2860U);
    auto invalid_params = params;
    invalid_params.radius = 100.0;
    EXPECT_EQ(detail::sharpen_working_bytes(15U, 15U, invalid_params),
              std::numeric_limits<std::uint64_t>::max());
}

TEST(SharpenTest, FrozenDefaultHasARealRawReferenceAndBudgetAccounting)
{
    const auto before = source_snapshot(mire1_path());
    ASSERT_TRUE(before);
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    Recipe recipe;
    recipe.asset = {"mire1", mire1_path(), std::nullopt};
    recipe.operations.push_back({"ravo.color.input", 1, "input", true,
                                 input_color_to_parameters(InputColorParams{}), std::nullopt});
    auto parameters = sharpen_to_parameters(SharpenParams{});
    ASSERT_TRUE(parameters) << parameters.error().message;
    recipe.operations.push_back({std::string(kSharpenOperationId), kSharpenOperationSchemaVersion,
                                 "sharpen", true, parameters.value(), std::nullopt});
    recipe.operations.push_back(
        {"ravo.display.sigmoid",
         1,
         "sigmoid",
         true,
         {{"working_space", ParameterValue{std::string(kSigmoidWorkingSpaceLinearSrgb)}},
          {"color_processing", ParameterValue{std::string(kSigmoidColorProcessingPerChannel)}},
          {"middle_grey_contrast", ParameterValue{kSigmoidContrastDefault}},
          {"contrast_skewness", ParameterValue{kSigmoidSkewDefault}},
          {"display_white_target", ParameterValue{kSigmoidDisplayWhiteDefault}},
          {"display_black_target", ParameterValue{kSigmoidDisplayBlackDefault}},
          {"hue_preservation", ParameterValue{kSigmoidHuePreservationDefault}}},
         std::nullopt});
    recipe.operations.push_back({"ravo.color.output", 1, "output", true,
                                 output_color_to_parameters(OutputColorParams{}), std::nullopt});
    RenderRequest request;
    request.asset = recipe.asset;
    request.recipe = recipe;
    request.output_width = 340U;
    request.output_height = 512U;
    auto rendered = engine.value().render_to_image(request);
    ASSERT_TRUE(rendered) << rendered.error().message;
    std::array<std::uint64_t, 3> sums{};
    for (std::size_t index = 0U; index + 2U < rendered.value().rgb.size(); index += 3U)
    {
        sums[0] += rendered.value().rgb[index];
        sums[1] += rendered.value().rgb[index + 1U];
        sums[2] += rendered.value().rgb[index + 2U];
    }
    // Ravo-owned reference for the frozen default v1 payload after the default
    // RCD demosaic, at a scale where its source radius is observable. The
    // scalar oracle above owns exact CPU parity; this envelope permits
    // supported-platform decoder/libm variation.
    EXPECT_NEAR(static_cast<double>(sums[0]), 17266767.0, 15000.0);
    EXPECT_NEAR(static_cast<double>(sums[1]), 15817540.0, 15000.0);
    EXPECT_NEAR(static_cast<double>(sums[2]), 14818995.0, 15000.0);

    Recipe baseline = recipe;
    std::erase_if(baseline.operations, [](const OperationInstance &operation)
                  { return operation.id == kSharpenOperationId; });
    request.recipe = baseline;
    auto unsharpened = engine.value().render_to_image(request);
    ASSERT_TRUE(unsharpened) << unsharpened.error().message;
    EXPECT_NE(unsharpened.value().rgb, rendered.value().rgb);

    DecodedRaw raw;
    raw.width = 2602U;
    raw.height = 3908U;
    const auto baseline_bytes = estimate_raw_render_memory(raw, baseline, 340U, 512U);
    const auto sharpen_bytes = estimate_raw_render_memory(raw, recipe, 340U, 512U);
    EXPECT_EQ(sharpen_bytes - baseline_bytes,
              detail::sharpen_working_bytes(340U, 512U, SharpenParams{}));
    EXPECT_EQ(source_snapshot(mire1_path()), before);
}

} // namespace
} // namespace ravo
