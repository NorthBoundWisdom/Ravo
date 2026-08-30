#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
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
#include "ravo/recipe/color_reconstruction.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/operation.h"

#include "color_reconstruction.h"
#include "d50_lab.h"
#include "raw_pipeline.h"

namespace ravo
{
namespace
{

constexpr std::string_view kFrozenParameters = "000070420000964300002041c3f5283f01000000";
constexpr std::string_view kFrozenBlendV10 = "gz13eJxjYGBgYAJiCQYYOOHEgAYY0QVwggZ7CB6pfNoAAExgGQY=";

[[nodiscard]] std::filesystem::path repository_root()
{
    return std::filesystem::path(RAVO_REPOSITORY_ROOT);
}

[[nodiscard]] std::string mire1_path()
{
    return (repository_root() / "legacy" / "tests" / "images" / "mire1.cr2").string();
}

[[nodiscard]] std::string fixture_xmp_path()
{
    return (repository_root() / "legacy" / "tests" / "0052-color-reconstruction" /
            "color-reconstruction.xmp")
        .string();
}

[[nodiscard]] std::optional<std::string> read_file(const std::string &path)
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
    std::uint64_t content_hash = 1469598103934665603ULL;

    [[nodiscard]] bool operator==(const SourceFileSnapshot &) const = default;
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
        const auto count = input.gcount();
        for (std::streamsize index = 0; index < count; ++index)
        {
            result.content_hash ^=
                static_cast<std::uint8_t>(block[static_cast<std::size_t>(index)]);
            result.content_hash *= 1099511628211ULL;
        }
    }
    return input.eof() ? std::optional<SourceFileSnapshot>{result} : std::nullopt;
}

[[nodiscard]] ColorProfileState linear_rec709_profile()
{
    ColorProfileState profile;
    profile.kind = ColorProfileKind::kBuiltin;
    profile.model = ColorModel::kRgb;
    profile.identifier = std::string(kInputProfileLinearRec709);
    profile.icc_bytes = {1U, 2U, 3U, 4U};
    return profile;
}

[[nodiscard]] WorkingImage synthetic_highlight_fixture()
{
    constexpr std::uint32_t width = 8U;
    constexpr std::uint32_t height = 6U;
    WorkingImage image;
    image.width = width;
    image.height = height;
    image.color_profile = linear_rec709_profile();
    image.exposure_analysis = std::make_shared<ExposureAnalysisContext>();
    image.canonical_roi_scale =
        CanonicalRoiScale::from_scaled_dimensions(width, height, width, height);
    image.rgb.reserve(static_cast<std::size_t>(width) * height * 3U);
    for (std::uint32_t row = 0U; row < height; ++row)
    {
        for (std::uint32_t column = 0U; column < width; ++column)
        {
            std::array<float, 3> lab{42.0F + 2.0F * static_cast<float>(row),
                                     column < width / 2U ?
                                         8.0F + 9.0F * static_cast<float>(column) :
                                         -8.0F - 5.0F * static_cast<float>(column),
                                     row < height / 2U ? 6.0F + 10.0F * static_cast<float>(row) :
                                                         -8.0F - 6.0F * static_cast<float>(row)};
            if ((row == 2U || row == 3U) && (column == 3U || column == 4U))
            {
                lab = {65.0F, 0.0F, 0.0F};
            }
            const auto rgb = d50_lab::xyz_to_linear_rec709(d50_lab::lab_to_xyz(lab));
            image.rgb.insert(image.rgb.end(), rgb.begin(), rgb.end());
        }
    }
    return image;
}

[[nodiscard]] std::array<std::uint32_t, 3> rgb_bits(const WorkingImage &image,
                                                    const std::uint32_t x, const std::uint32_t y)
{
    const std::size_t index = (static_cast<std::size_t>(y) * image.width + x) * 3U;
    return {std::bit_cast<std::uint32_t>(image.rgb[index]),
            std::bit_cast<std::uint32_t>(image.rgb[index + 1U]),
            std::bit_cast<std::uint32_t>(image.rgb[index + 2U])};
}

void expect_rgb_reference(const WorkingImage &image, const std::uint32_t x, const std::uint32_t y,
                          const std::array<std::uint32_t, 3> &reference)
{
    const auto actual = rgb_bits(image, x, y);
    for (std::size_t channel = 0U; channel < reference.size(); ++channel)
    {
        EXPECT_NEAR(std::bit_cast<float>(actual[channel]), std::bit_cast<float>(reference[channel]),
                    1.0e-5F);
    }
}

[[nodiscard]] std::array<double, 3> channel_sums(const WorkingImage &image)
{
    std::array<double, 3> sums{};
    for (std::size_t index = 0U; index + 2U < image.rgb.size(); index += 3U)
    {
        for (std::size_t channel = 0U; channel < sums.size(); ++channel)
        {
            sums[channel] += image.rgb[index + channel];
        }
    }
    return sums;
}

void expect_working_unchanged(const WorkingImage &actual, const WorkingImage &expected)
{
    EXPECT_EQ(actual.width, expected.width);
    EXPECT_EQ(actual.height, expected.height);
    EXPECT_EQ(actual.rgb, expected.rgb);
    EXPECT_EQ(actual.color_profile, expected.color_profile);
    EXPECT_EQ(actual.exposure_analysis, expected.exposure_analysis);
    EXPECT_FLOAT_EQ(actual.canonical_roi_scale.value(), expected.canonical_roi_scale.value());
}

[[nodiscard]] std::string minimal_legacy_xmp(const std::string_view version = "3",
                                             const std::string_view enabled = "1",
                                             const std::string_view parameters = kFrozenParameters,
                                             const std::string_view blend = kFrozenBlendV10,
                                             const std::string_view extra = {},
                                             const bool duplicate = false)
{
    const auto entry = [&]
    {
        std::string value =
            R"(<rdf:li darktable:operation="colorreconstruct" darktable:num="8" darktable:enabled=")";
        value += enabled;
        value += R"(" darktable:modversion=")";
        value += version;
        value += R"(" darktable:params=")";
        value += parameters;
        value +=
            R"(" darktable:multi_name="" darktable:multi_priority="0" darktable:blendop_version="10" darktable:blendop_params=")";
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
    detail::ColorReconstructionCheckpoint target =
        detail::ColorReconstructionCheckpoint::kBeforeValidation;
    bool fired = false;
};

void cancel_at_checkpoint(void *const context,
                          const detail::ColorReconstructionCheckpoint checkpoint,
                          std::uint32_t) noexcept
{
    auto &fixture = *static_cast<CancellationFixture *>(context);
    if (!fixture.fired && checkpoint == fixture.target)
    {
        fixture.fired = fixture.source->cancel("colorreconstruct-checkpoint");
    }
}

TEST(ColorReconstructionRecipeTest, SchemaDevelopAndRegistryOwnTheCompleteV3Surface)
{
    ColorReconstructionParams params;
    auto encoded = color_reconstruction_to_parameters(params);
    ASSERT_TRUE(encoded) << encoded.error().message;
    EXPECT_EQ(encoded.value().size(), 7U);
    auto decoded = color_reconstruction_from_parameters(encoded.value());
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded.value(), params);

    const auto registry = make_phase1_registry();
    ASSERT_TRUE(registry) << registry.error().message;
    EXPECT_EQ(registry.value().descriptors().size(), kPhase1OperationCount);
    const auto *descriptor = registry.value().find(kColorReconstructionOperationId);
    ASSERT_NE(descriptor, nullptr);
    EXPECT_EQ(descriptor->parameter_schema_version, kColorReconstructionOperationSchemaVersion);
    EXPECT_EQ(descriptor->parameters.size(), 7U);
    EXPECT_FALSE(descriptor->supports_mask);
    EXPECT_TRUE(descriptor->cpu_reference_available);

    DevelopParams develop;
    ASSERT_TRUE(apply_develop_field_strict(develop, "colorReconstructionThreshold", 60.0));
    ASSERT_TRUE(apply_develop_field_strict(develop, "colorReconstructionSpatial", 300.0));
    ASSERT_TRUE(apply_develop_field_strict(develop, "colorReconstructionRange", 10.0));
    ASSERT_TRUE(apply_develop_field_strict(develop, "colorReconstructionHueDegrees", 237.6));
    ASSERT_TRUE(apply_develop_field_strict(develop, "colorReconstructionPrecedenceIndex", 1.0));
    EXPECT_TRUE(develop.color_reconstruction_enabled);
    EXPECT_EQ(develop.color_reconstruction.precedence, ColorReconstructionPrecedence::kChroma);
    EXPECT_NEAR(develop.color_reconstruction.hue, 0.66, 1.0e-12);
    auto recipe = recipe_from_develop({"asset", "file:///fixture.raw", std::nullopt}, develop);
    ASSERT_TRUE(recipe) << recipe.error().message;
    const auto operation = std::find_if(
        recipe.value().operations.begin(), recipe.value().operations.end(),
        [](const OperationInstance &item) { return item.id == kColorReconstructionOperationId; });
    ASSERT_NE(operation, recipe.value().operations.end());
    ASSERT_NE(std::next(operation), recipe.value().operations.end());
    EXPECT_EQ(std::next(operation)->id, "ravo.color.output");
    auto round_trip = develop_from_recipe(recipe.value());
    ASSERT_TRUE(round_trip) << round_trip.error().message;
    EXPECT_EQ(round_trip.value().color_reconstruction, develop.color_reconstruction);
    EXPECT_TRUE(round_trip.value().color_reconstruction_enabled);
    EXPECT_TRUE(reset_develop_field(round_trip.value(), "colorReconstruction"));
    EXPECT_FALSE(round_trip.value().color_reconstruction_enabled);

    auto invalid = encoded.value();
    invalid["threshold"] = ParameterValue{49.999};
    EXPECT_FALSE(color_reconstruction_from_parameters(invalid));
    invalid = encoded.value();
    invalid["precedence"] = ParameterValue{"saturated"};
    EXPECT_FALSE(color_reconstruction_from_parameters(invalid));
    invalid = encoded.value();
    invalid["unknown"] = ParameterValue{0.0};
    EXPECT_FALSE(color_reconstruction_from_parameters(invalid));
    const DevelopParams unchanged = round_trip.value();
    EXPECT_FALSE(
        apply_develop_field_strict(round_trip.value(), "colorReconstructionPrecedenceIndex", 1.5));
    EXPECT_EQ(round_trip.value(), unchanged);
}

TEST(ColorReconstructionLegacyXmpTest, ImportsOnlyTheVerbatim0052V3SingletonBoundary)
{
    const auto fixture = read_file(fixture_xmp_path());
    ASSERT_TRUE(fixture);
    const auto imported = import_legacy_xmp({*fixture, {"mire1", mire1_path(), std::nullopt}});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_EQ(imported.value().operations.size(), 3U);
    EXPECT_EQ(imported.value().operations.front().id, "ravo.color.input");
    EXPECT_EQ(imported.value().operations.back().id, "ravo.color.output");
    const auto &operation = imported.value().operations[1];
    EXPECT_EQ(operation.id, kColorReconstructionOperationId);
    EXPECT_EQ(operation.instance_id, "legacy-colorreconstruct-8");
    auto params = color_reconstruction_from_parameters(operation.parameters);
    ASSERT_TRUE(params) << params.error().message;
    EXPECT_DOUBLE_EQ(params.value().threshold, 60.0);
    EXPECT_DOUBLE_EQ(params.value().spatial, 300.0);
    EXPECT_DOUBLE_EQ(params.value().range, 10.0);
    EXPECT_EQ(std::bit_cast<std::uint64_t>(params.value().hue),
              std::bit_cast<std::uint64_t>(static_cast<double>(std::bit_cast<float>(0x3f28f5c3U))));
    EXPECT_EQ(params.value().precedence, ColorReconstructionPrecedence::kChroma);

    auto unproven_builtin = *fixture;
    const auto temperature = unproven_builtin.find("operation=\"temperature\"");
    ASSERT_NE(temperature, std::string::npos);
    const auto builtin_blend_version = unproven_builtin.find("blendop_version=\"10\"", temperature);
    ASSERT_NE(builtin_blend_version, std::string::npos);
    unproven_builtin.replace(builtin_blend_version,
                             std::string_view{"blendop_version=\"10\""}.size(),
                             "blendop_version=\"11\"");
    const auto rejected_builtin =
        import_legacy_xmp({unproven_builtin, {"mire1", mire1_path(), std::nullopt}});
    ASSERT_FALSE(rejected_builtin);
    EXPECT_EQ(rejected_builtin.error().context.at("reason"),
              "unsupported_legacy_builtin_parameters");

    const auto import_minimal = [](const std::string &xmp)
    { return import_legacy_xmp({xmp, {"asset", "file:///fixture.raw", std::nullopt}}); };
    EXPECT_TRUE(import_minimal(minimal_legacy_xmp()));
    for (const auto &xmp : {minimal_legacy_xmp("2"), minimal_legacy_xmp("3", "0"),
                            minimal_legacy_xmp("3", "1", kFrozenParameters, "invalid"),
                            minimal_legacy_xmp("3", "1", kFrozenParameters, kFrozenBlendV10,
                                               " darktable:mask_id=\"1\"")})
    {
        const auto rejected = import_minimal(xmp);
        ASSERT_FALSE(rejected);
        EXPECT_EQ(rejected.error().code, ErrorCode::kUnsupported);
    }
    const auto duplicate =
        import_minimal(minimal_legacy_xmp("3", "1", kFrozenParameters, kFrozenBlendV10, {}, true));
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code, ErrorCode::kConflict);

    std::size_t record_count = 0U;
    std::filesystem::path record_path;
    for (const auto &entry :
         std::filesystem::recursive_directory_iterator(repository_root() / "legacy" / "tests"))
    {
        if (!entry.is_regular_file() || entry.path().extension() != ".xmp")
        {
            continue;
        }
        const auto text = read_file(entry.path().string());
        ASSERT_TRUE(text);
        std::size_t offset = 0U;
        while ((offset = text->find("operation=\"colorreconstruct\"", offset)) != std::string::npos)
        {
            ++record_count;
            record_path = entry.path();
            ++offset;
        }
    }
    EXPECT_EQ(record_count, 1U);
    EXPECT_EQ(record_path.filename(), "color-reconstruction.xmp");
}

TEST(ColorReconstructionTest, FrozenBilateralGridBranchesAndRoiScaleHaveFixedReferences)
{
    const WorkingImage input = synthetic_highlight_fixture();
    const WorkingImage original = input;
    ColorReconstructionParams params;
    params.threshold = 60.0;
    params.spatial = 3.0;
    params.range = 10.0;
    params.hue = 0.66;

    params.precedence = ColorReconstructionPrecedence::kNone;
    auto none = apply_color_reconstruction(input, params, CancellationToken{});
    ASSERT_TRUE(none) << none.error().message;
    params.precedence = ColorReconstructionPrecedence::kChroma;
    auto chroma = apply_color_reconstruction(input, params, CancellationToken{});
    ASSERT_TRUE(chroma) << chroma.error().message;
    params.precedence = ColorReconstructionPrecedence::kHue;
    auto hue = apply_color_reconstruction(input, params, CancellationToken{});
    ASSERT_TRUE(hue) << hue.error().message;

    // Fixed source-order references for the complete bilateral grid and D50
    // bridge. cbrtf/atan2f/expf are allowed only the recorded cross-platform
    // libm envelope; the three precedence modes must remain distinguishable.
    expect_rgb_reference(none.value(), 3U, 2U, {1051358717U, 1051401774U, 1055023431U});
    expect_rgb_reference(chroma.value(), 3U, 2U, {1050094782U, 1051711392U, 1056001263U});
    expect_rgb_reference(hue.value(), 3U, 2U, {1052893531U, 1050034898U, 1061259721U});
    EXPECT_NE(none.value().rgb, chroma.value().rgb);
    EXPECT_NE(chroma.value().rgb, hue.value().rgb);
    expect_working_unchanged(input, original);
    EXPECT_NE(none.value().rgb.data(), input.rgb.data());
    EXPECT_EQ(none.value().color_profile, input.color_profile);
    EXPECT_NE(none.value().color_profile.icc_bytes.data(), input.color_profile.icc_bytes.data());
    EXPECT_EQ(none.value().exposure_analysis, input.exposure_analysis);
    EXPECT_FLOAT_EQ(none.value().canonical_roi_scale.value(), input.canonical_roi_scale.value());

    const auto none_sums = channel_sums(none.value());
    const auto chroma_sums = channel_sums(chroma.value());
    const auto hue_sums = channel_sums(hue.value());
    const std::array references{
        std::array{5.6657437868416309, 8.9558440670371056, 13.145878050476313},
        std::array{5.5231806673109531, 8.9905063733458519, 13.259249839931726},
        std::array{6.0033054985105991, 8.7570667192339897, 14.25761741772294},
    };
    const std::array actual_sums{none_sums, chroma_sums, hue_sums};
    for (std::size_t mode = 0U; mode < references.size(); ++mode)
    {
        for (std::size_t channel = 0U; channel < references[mode].size(); ++channel)
        {
            EXPECT_NEAR(actual_sums[mode][channel], references[mode][channel], 1.0e-4);
        }
    }

    EXPECT_EQ(detail::color_reconstruction_grid_bytes(8U, 6U, 1.0F, params), 4400U);
    EXPECT_EQ(detail::color_reconstruction_grid_bytes(8U, 6U, 0.5F, params), 5280U);
    auto invalid = params;
    invalid.spatial = std::numeric_limits<double>::quiet_NaN();
    EXPECT_EQ(detail::color_reconstruction_grid_bytes(8U, 6U, 1.0F, invalid),
              std::numeric_limits<std::uint64_t>::max());
}

TEST(ColorReconstructionTest, FailuresAndEveryControlledCancellationPublishNothing)
{
    const WorkingImage input = synthetic_highlight_fixture();
    const WorkingImage original = input;
    ColorReconstructionParams params;
    params.threshold = 60.0;
    params.spatial = 3.0;

    const std::array checkpoints{
        detail::ColorReconstructionCheckpoint::kBeforeValidation,
        detail::ColorReconstructionCheckpoint::kSplatRow,
        detail::ColorReconstructionCheckpoint::kBlurLine,
        detail::ColorReconstructionCheckpoint::kSliceRow,
        detail::ColorReconstructionCheckpoint::kBeforePublication,
    };
    for (const auto checkpoint : checkpoints)
    {
        CancellationSource cancellation;
        CancellationFixture fixture{&cancellation, checkpoint};
        const auto rejected = detail::apply_color_reconstruction_controlled(
            input, params, cancellation.token(), {&fixture, cancel_at_checkpoint});
        ASSERT_FALSE(rejected);
        EXPECT_TRUE(fixture.fired);
        EXPECT_EQ(rejected.error().code, ErrorCode::kCancelled);
        expect_working_unchanged(input, original);
    }

    auto rejected_input = input;
    rejected_input.width = 0U;
    auto rejected = apply_color_reconstruction(rejected_input, params, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "invalid_colorreconstruct_dimensions");
    rejected_input = input;
    rejected_input.rgb.pop_back();
    rejected = apply_color_reconstruction(rejected_input, params, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "invalid_colorreconstruct_buffer");
    rejected_input = input;
    rejected_input.color_profile.identifier = "srgb";
    rejected = apply_color_reconstruction(rejected_input, params, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "unsupported_colorreconstruct_working_space");
    rejected_input = input;
    rejected_input.canonical_roi_scale = {};
    rejected = apply_color_reconstruction(rejected_input, params, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "invalid_colorreconstruct_roi_scale");
    rejected_input = input;
    rejected_input.rgb.front() = std::numeric_limits<float>::infinity();
    rejected = apply_color_reconstruction(rejected_input, params, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "nonfinite_colorreconstruct_input");

    auto parameters = color_reconstruction_to_parameters(params);
    ASSERT_TRUE(parameters) << parameters.error().message;
    OperationInstance operation{std::string(kColorReconstructionOperationId),
                                kColorReconstructionOperationSchemaVersion,
                                "colorreconstruct-test",
                                true,
                                parameters.value(),
                                std::nullopt};
    auto direct = apply_color_reconstruction(input, operation, CancellationToken{});
    ASSERT_TRUE(direct) << direct.error().message;
    operation.mask_id = "mask";
    rejected = apply_color_reconstruction(input, operation, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "colorreconstruct_mask_graph_unavailable");
    operation.mask_id.reset();
    operation.schema_version += 1;
    rejected = apply_color_reconstruction(input, operation, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kUnsupported);
    operation.schema_version = kColorReconstructionOperationSchemaVersion;
    operation.id = "ravo.color.colorcontrast";
    rejected = apply_color_reconstruction(input, operation, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kValidation);
    expect_working_unchanged(input, original);
}

TEST(ColorReconstructionTest, Frozen0052HasARealRawReferenceAndBudgetAccounting)
{
    const auto source_before = source_file_snapshot(mire1_path());
    ASSERT_TRUE(source_before);
    const auto xmp = read_file(fixture_xmp_path());
    ASSERT_TRUE(xmp);
    auto recipe = import_legacy_xmp({*xmp, {"mire1", mire1_path(), std::nullopt}});
    ASSERT_TRUE(recipe) << recipe.error().message;
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;

    RenderRequest request;
    request.asset = recipe.value().asset;
    request.recipe = recipe.value();
    request.output_width = 42U;
    request.output_height = 64U;
    auto rendered = engine.value().render_to_image(request);
    ASSERT_TRUE(rendered) << rendered.error().message;
    ASSERT_EQ(rendered.value().width, 42U);
    ASSERT_EQ(rendered.value().height, 64U);
    std::array<std::uint64_t, 3> sums{};
    for (std::size_t index = 0U; index + 2U < rendered.value().rgb.size(); index += 3U)
    {
        for (std::size_t channel = 0U; channel < sums.size(); ++channel)
        {
            sums[channel] += rendered.value().rgb[index + channel];
        }
    }
    // Ravo-owned macOS reference for the verbatim 0052 v3 payload on the
    // committed RAW with the default RCD demosaic. The envelope accounts for
    // supported-platform libm and decoder variation without using the
    // prohibited old runner as an oracle.
    EXPECT_NEAR(static_cast<double>(sums[0]), 284425.0, 2500.0);
    EXPECT_NEAR(static_cast<double>(sums[1]), 261073.0, 2500.0);
    EXPECT_NEAR(static_cast<double>(sums[2]), 247429.0, 2500.0);

    Recipe without_reconstruction = recipe.value();
    std::erase_if(without_reconstruction.operations, [](const OperationInstance &operation)
                  { return operation.id == kColorReconstructionOperationId; });
    request.recipe = without_reconstruction;
    auto baseline = engine.value().render_to_image(request);
    ASSERT_TRUE(baseline) << baseline.error().message;
    EXPECT_NE(baseline.value().rgb, rendered.value().rgb);

    DecodedRaw raw;
    raw.width = 2602U;
    raw.height = 3908U;
    const std::uint64_t baseline_bytes =
        estimate_raw_render_memory(raw, without_reconstruction, 42U, 64U);
    const std::uint64_t reconstruction_bytes =
        estimate_raw_render_memory(raw, recipe.value(), 42U, 64U);
    ASSERT_GT(reconstruction_bytes, baseline_bytes);
    const auto &operation = recipe.value().operations[1];
    auto params = color_reconstruction_from_parameters(operation.parameters);
    ASSERT_TRUE(params) << params.error().message;
    const auto scale = CanonicalRoiScale::from_scaled_dimensions(42U, 64U, 2602U, 3908U);
    EXPECT_EQ(reconstruction_bytes - baseline_bytes,
              detail::color_reconstruction_grid_bytes(42U, 64U, scale.value(), params.value()));

    const auto source_after = source_file_snapshot(mire1_path());
    ASSERT_TRUE(source_after);
    EXPECT_EQ(*source_after, *source_before);
}

} // namespace
} // namespace ravo
