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
#include "ravo/recipe/dehaze.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/operation.h"

#include "dehaze.h"
#include "raw_pipeline.h"

namespace ravo
{
namespace
{

constexpr std::string_view kLegacyV1Parameters = "cdcc4c3ecdcc4c3e";
constexpr std::string_view kLegacyV2Parameters = "6666663fcdcc4c3f00000000";
constexpr std::string_view kBlendV9 = "gz11eJxjYGBgkGAAgRNODGiAEV0AJ2iwh+CRyscOAAdeGQQ=";
constexpr std::string_view kBlendV13 =
    "gz08eJxjYGBgYAFiCQYYOOHEgAZY0QWAgBGLGANDgz0Ej1Q+dlAx68oBEMbFxwX+AwGIBgCbGCeh";

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

struct SourceSnapshot
{
    std::uintmax_t size = 0U;
    std::filesystem::file_time_type modified;
    std::uint64_t hash = 1469598103934665603ULL;

    [[nodiscard]] bool operator==(const SourceSnapshot &) const = default;
};

[[nodiscard]] std::optional<SourceSnapshot> source_snapshot(const std::string &path)
{
    std::error_code error;
    SourceSnapshot result;
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
        for (std::streamsize index = 0; index < input.gcount(); ++index)
        {
            result.hash ^= static_cast<std::uint8_t>(block[static_cast<std::size_t>(index)]);
            result.hash *= 1099511628211ULL;
        }
    }
    return input.eof() ? std::optional<SourceSnapshot>{result} : std::nullopt;
}

[[nodiscard]] WorkingImage hazy_fixture(const std::uint32_t width, const std::uint32_t height)
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
    image.rgb.resize(static_cast<std::size_t>(width) * height * 3U);
    for (std::uint32_t row = 0U; row < height; ++row)
    {
        for (std::uint32_t column = 0U; column < width; ++column)
        {
            const float depth =
                static_cast<float>(row + column) / static_cast<float>(width + height - 2U);
            const float transmission = 0.35F + 0.55F * (1.0F - depth);
            const std::array<float, 3> object{
                0.08F + 0.5F * static_cast<float>(column) / static_cast<float>(width - 1U),
                0.12F + 0.4F * static_cast<float>(row) / static_cast<float>(height - 1U),
                0.05F + 0.3F * depth};
            constexpr std::array<float, 3> ambient{0.82F, 0.88F, 0.94F};
            const std::size_t pixel = static_cast<std::size_t>(row) * width + column;
            for (std::size_t channel = 0U; channel < 3U; ++channel)
            {
                image.rgb[pixel * 3U + channel] =
                    object[channel] * transmission + ambient[channel] * (1.0F - transmission);
            }
        }
    }
    return image;
}

template <typename Compare>
[[nodiscard]] std::vector<float>
box_extreme_oracle(const std::vector<float> &input, const std::uint32_t width,
                   const std::uint32_t height, const std::uint32_t radius, Compare compare)
{
    std::vector<float> output(input.size());
    for (std::uint32_t row = 0U; row < height; ++row)
    {
        for (std::uint32_t column = 0U; column < width; ++column)
        {
            const std::uint32_t first_row = row > radius ? row - radius : 0U;
            const std::uint32_t last_row = std::min(height - 1U, row + radius);
            const std::uint32_t first_column = column > radius ? column - radius : 0U;
            const std::uint32_t last_column = std::min(width - 1U, column + radius);
            float value = input[static_cast<std::size_t>(first_row) * width + first_column];
            for (std::uint32_t y = first_row; y <= last_row; ++y)
            {
                for (std::uint32_t x = first_column; x <= last_column; ++x)
                {
                    value = compare(value, input[static_cast<std::size_t>(y) * width + x]);
                }
            }
            output[static_cast<std::size_t>(row) * width + column] = value;
        }
    }
    return output;
}

[[nodiscard]] std::vector<float> box_mean_oracle(const std::vector<float> &input,
                                                 const std::uint32_t width,
                                                 const std::uint32_t height,
                                                 const std::uint32_t radius)
{
    std::vector<float> output(input.size());
    for (std::uint32_t row = 0U; row < height; ++row)
    {
        for (std::uint32_t column = 0U; column < width; ++column)
        {
            const std::uint32_t first_row = row > radius ? row - radius : 0U;
            const std::uint32_t last_row = std::min(height - 1U, row + radius);
            const std::uint32_t first_column = column > radius ? column - radius : 0U;
            const std::uint32_t last_column = std::min(width - 1U, column + radius);
            double sum = 0.0;
            std::uint64_t count = 0U;
            for (std::uint32_t y = first_row; y <= last_row; ++y)
            {
                for (std::uint32_t x = first_column; x <= last_column; ++x)
                {
                    sum += input[static_cast<std::size_t>(y) * width + x];
                    ++count;
                }
            }
            output[static_cast<std::size_t>(row) * width + column] =
                static_cast<float>(sum / static_cast<double>(count));
        }
    }
    return output;
}

[[nodiscard]] WorkingImage dehaze_oracle(const WorkingImage &input, const DehazeParams &params,
                                         detail::DehazeAnalysis &analysis)
{
    const float scale = params.adaptive ? input.canonical_roi_scale.value() : 1.0F;
    const std::uint32_t w1 = 2U + static_cast<std::uint32_t>(std::ceil(4.0F * scale));
    const std::uint32_t w2 = 3U + static_cast<std::uint32_t>(std::ceil(6.0F * scale));
    const std::size_t pixels = static_cast<std::size_t>(input.width) * input.height;
    std::vector<float> dark(pixels);
    for (std::size_t pixel = 0U; pixel < pixels; ++pixel)
    {
        dark[pixel] = std::min(input.rgb[pixel * 3U],
                               std::min(input.rgb[pixel * 3U + 1U], input.rgb[pixel * 3U + 2U]));
    }
    dark = box_extreme_oracle(dark, input.width, input.height, w1,
                              [](const float a, const float b) { return std::min(a, b); });
    auto ordered = dark;
    std::sort(ordered.begin(), ordered.end());
    const float critical_haze =
        ordered[static_cast<std::size_t>(static_cast<float>(pixels) * 0.95F)];
    std::vector<float> brightness;
    for (std::size_t pixel = 0U; pixel < pixels; ++pixel)
    {
        if (dark[pixel] >= critical_haze)
        {
            brightness.push_back(input.rgb[pixel * 3U] + input.rgb[pixel * 3U + 1U] +
                                 input.rgb[pixel * 3U + 2U]);
        }
    }
    std::sort(brightness.begin(), brightness.end());
    const float critical_brightness =
        brightness[static_cast<std::size_t>(static_cast<float>(brightness.size()) * 0.95F)];
    std::size_t selected = 0U;
    std::array<float, 3> ambient{};
    for (std::size_t pixel = 0U; pixel < pixels; ++pixel)
    {
        const float value =
            input.rgb[pixel * 3U] + input.rgb[pixel * 3U + 1U] + input.rgb[pixel * 3U + 2U];
        if (dark[pixel] >= critical_haze && value >= critical_brightness)
        {
            ambient[0] += input.rgb[pixel * 3U];
            ambient[1] += input.rgb[pixel * 3U + 1U];
            ambient[2] += input.rgb[pixel * 3U + 2U];
            ++selected;
        }
    }
    for (float &channel : ambient)
    {
        channel /= static_cast<float>(selected);
    }
    const float distance_max = critical_haze > 0.0F ?
                                   -1.125F * std::log(critical_haze) :
                                   std::log(std::numeric_limits<float>::max()) / 2.0F;
    analysis = {ambient, distance_max, static_cast<int>(w1), static_cast<int>(w2)};

    std::vector<float> transition(pixels);
    for (std::size_t pixel = 0U; pixel < pixels; ++pixel)
    {
        const float minimum = std::min(input.rgb[pixel * 3U] / ambient[0],
                                       std::min(input.rgb[pixel * 3U + 1U] / ambient[1],
                                                input.rgb[pixel * 3U + 2U] / ambient[2]));
        transition[pixel] = 1.0F - minimum * static_cast<float>(params.strength);
    }
    transition = box_extreme_oracle(transition, input.width, input.height, w1,
                                    [](const float a, const float b) { return std::max(a, b); });
    transition = box_extreme_oracle(transition, input.width, input.height, w1,
                                    [](const float a, const float b) { return std::min(a, b); });

    std::array<std::vector<float>, 4> means;
    means[0] = box_mean_oracle(transition, input.width, input.height, w2);
    for (std::size_t channel = 0U; channel < 3U; ++channel)
    {
        std::vector<float> guide(pixels);
        for (std::size_t pixel = 0U; pixel < pixels; ++pixel)
        {
            guide[pixel] = input.rgb[pixel * 3U + channel];
        }
        means[channel + 1U] = box_mean_oracle(guide, input.width, input.height, w2);
    }
    std::array<std::vector<float>, 9> products;
    for (auto &plane : products)
    {
        plane.resize(pixels);
    }
    for (std::size_t pixel = 0U; pixel < pixels; ++pixel)
    {
        const float r = input.rgb[pixel * 3U];
        const float g = input.rgb[pixel * 3U + 1U];
        const float b = input.rgb[pixel * 3U + 2U];
        products[0][pixel] = r * transition[pixel];
        products[1][pixel] = g * transition[pixel];
        products[2][pixel] = b * transition[pixel];
        products[3][pixel] = r * r;
        products[4][pixel] = r * g;
        products[5][pixel] = r * b;
        products[6][pixel] = g * g;
        products[7][pixel] = g * b;
        products[8][pixel] = b * b;
    }
    for (auto &plane : products)
    {
        plane = box_mean_oracle(plane, input.width, input.height, w2);
    }
    std::array<std::vector<float>, 4> coefficients;
    for (auto &plane : coefficients)
    {
        plane.resize(pixels);
    }
    const float root_epsilon = std::sqrt(0.025F);
    const float epsilon = root_epsilon * root_epsilon;
    for (std::size_t pixel = 0U; pixel < pixels; ++pixel)
    {
        const float p = means[0][pixel];
        const float r = means[1][pixel];
        const float g = means[2][pixel];
        const float b = means[3][pixel];
        const float s00 = products[3][pixel] - r * r + epsilon;
        const float s01 = products[4][pixel] - r * g;
        const float s02 = products[5][pixel] - r * b;
        const float s11 = products[6][pixel] - g * g + epsilon;
        const float s12 = products[7][pixel] - g * b;
        const float s22 = products[8][pixel] - b * b + epsilon;
        const float det0 = s00 * (s11 * s22 - s12 * s12) - s01 * (s01 * s22 - s02 * s12) +
                           s02 * (s01 * s12 - s02 * s11);
        if (std::fabs(det0) > 4.0F * std::numeric_limits<float>::epsilon())
        {
            const float cr = products[0][pixel] - r * p;
            const float cg = products[1][pixel] - g * p;
            const float cb = products[2][pixel] - b * p;
            coefficients[0][pixel] = (cr * (s11 * s22 - s12 * s12) - s01 * (cg * s22 - cb * s12) +
                                      s02 * (cg * s12 - cb * s11)) /
                                     det0;
            coefficients[1][pixel] = (s00 * (cg * s22 - cb * s12) - cr * (s01 * s22 - s02 * s12) +
                                      s02 * (s01 * cb - s02 * cg)) /
                                     det0;
            coefficients[2][pixel] = (s00 * (s11 * cb - s12 * cg) - s01 * (s01 * cb - s02 * cg) +
                                      cr * (s01 * s12 - s02 * s11)) /
                                     det0;
        }
        coefficients[3][pixel] = p - coefficients[0][pixel] * r - coefficients[1][pixel] * g -
                                 coefficients[2][pixel] * b;
    }
    for (auto &plane : coefficients)
    {
        plane = box_mean_oracle(plane, input.width, input.height, w2);
    }
    std::vector<float> filtered(pixels);
    for (std::size_t pixel = 0U; pixel < pixels; ++pixel)
    {
        filtered[pixel] = coefficients[0][pixel] * input.rgb[pixel * 3U] +
                          coefficients[1][pixel] * input.rgb[pixel * 3U + 1U] +
                          coefficients[2][pixel] * input.rgb[pixel * 3U + 2U] +
                          coefficients[3][pixel];
    }
    const float minimum_transition = std::clamp(
        std::exp(-static_cast<float>(params.distance) * distance_max), 1.0F / 1024.0F, 1.0F);
    WorkingImage output = input;
    for (std::size_t pixel = 0U; pixel < pixels; ++pixel)
    {
        const float transmission = std::max(filtered[pixel], minimum_transition);
        for (std::size_t channel = 0U; channel < 3U; ++channel)
        {
            output.rgb[pixel * 3U + channel] =
                (input.rgb[pixel * 3U + channel] - ambient[channel]) / transmission +
                ambient[channel];
        }
    }
    return output;
}

[[nodiscard]] std::string
minimal_dehaze_xmp(const std::string_view version, const std::string_view parameters,
                   const std::string_view blend_version, const std::string_view blend,
                   const std::string_view enabled = "1", const std::string_view extra = {},
                   const bool duplicate = false)
{
    const auto entry = [&]
    {
        std::string value =
            R"(<rdf:li darktable:operation="hazeremoval" darktable:num="9" darktable:enabled=")";
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
    detail::DehazeCheckpoint target = detail::DehazeCheckpoint::kBeforeValidation;
    bool fired = false;
};

void cancel_at_checkpoint(void *const context, const detail::DehazeCheckpoint checkpoint,
                          std::uint32_t) noexcept
{
    auto &fixture = *static_cast<CancellationFixture *>(context);
    if (!fixture.fired && checkpoint == fixture.target)
    {
        fixture.fired = fixture.source->cancel("dehaze-checkpoint");
    }
}

TEST(DehazeRecipeTest, V2SchemaUpgradesCurrentRavoV1AndDevelopOwnsDistanceAdaptive)
{
    auto encoded = dehaze_to_parameters(DehazeParams{});
    ASSERT_TRUE(encoded) << encoded.error().message;
    EXPECT_EQ(encoded.value().size(), 5U);
    auto decoded = dehaze_from_parameters(encoded.value());
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded.value(), DehazeParams{});

    OperationInstance v1{std::string(kDehazeOperationId),    1,           "dehaze-v1", true,
                         {{"amount", ParameterValue{-0.4}}}, std::nullopt};
    ASSERT_TRUE(upgrade_dehaze_operation(v1));
    auto upgraded = dehaze_from_parameters(v1.parameters);
    ASSERT_TRUE(upgraded) << upgraded.error().message;
    EXPECT_EQ(upgraded.value(), (DehazeParams{-0.4, 0.2, true}));

    const auto registry = make_phase1_registry();
    ASSERT_TRUE(registry) << registry.error().message;
    const auto *descriptor = registry.value().find(kDehazeOperationId);
    ASSERT_NE(descriptor, nullptr);
    EXPECT_EQ(descriptor->parameter_schema_version, kDehazeOperationSchemaVersion);
    EXPECT_EQ(descriptor->parameters.size(), 5U);

    DevelopParams develop;
    ASSERT_TRUE(apply_develop_field_strict(develop, "dehaze", 0.9));
    ASSERT_TRUE(apply_develop_field_strict(develop, "dehazeDistance", 0.8));
    ASSERT_TRUE(apply_develop_field_strict(develop, "dehazeAdaptive", 0.0));
    auto recipe = recipe_from_develop({"asset", "file:///fixture.raw", std::nullopt}, develop);
    ASSERT_TRUE(recipe) << recipe.error().message;
    const auto operation =
        std::find_if(recipe.value().operations.begin(), recipe.value().operations.end(),
                     [](const OperationInstance &item) { return item.id == kDehazeOperationId; });
    ASSERT_NE(operation, recipe.value().operations.end());
    auto round_trip = develop_from_recipe(recipe.value());
    ASSERT_TRUE(round_trip) << round_trip.error().message;
    EXPECT_DOUBLE_EQ(round_trip.value().dehaze, 0.9);
    EXPECT_DOUBLE_EQ(round_trip.value().dehaze_distance, 0.8);
    EXPECT_FALSE(round_trip.value().dehaze_adaptive);
    EXPECT_FALSE(apply_develop_field_strict(round_trip.value(), "dehazeAdaptive", 0.5));
}

TEST(DehazeLegacyXmpTest, V1AndV2MapOnlyTheirFrozenUnmaskedSingletons)
{
    const auto import = [](const std::string &xmp)
    { return import_legacy_xmp({xmp, {"asset", "file:///fixture.raw", std::nullopt}}); };
    auto v1 = import(minimal_dehaze_xmp("1", kLegacyV1Parameters, "9", kBlendV9));
    ASSERT_TRUE(v1) << v1.error().message;
    auto v1_params = dehaze_from_parameters(v1.value().operations[1].parameters);
    ASSERT_TRUE(v1_params) << v1_params.error().message;
    EXPECT_EQ(v1_params.value(), (DehazeParams{0.20000000298023224, 0.20000000298023224, true}));
    auto v2 = import(minimal_dehaze_xmp("2", kLegacyV2Parameters, "13", kBlendV13));
    ASSERT_TRUE(v2) << v2.error().message;
    auto v2_params = dehaze_from_parameters(v2.value().operations[1].parameters);
    ASSERT_TRUE(v2_params) << v2_params.error().message;
    EXPECT_EQ(v2_params.value(), (DehazeParams{0.8999999761581421, 0.800000011920929, false}));

    for (const auto &xmp : {minimal_dehaze_xmp("3", kLegacyV2Parameters, "13", kBlendV13),
                            minimal_dehaze_xmp("1", kLegacyV1Parameters, "9", kBlendV9, "0"),
                            minimal_dehaze_xmp("1", kLegacyV1Parameters, "10", kBlendV9),
                            minimal_dehaze_xmp("1", kLegacyV1Parameters, "9", kBlendV9, "1",
                                               " darktable:mask_id=\"1\"")})
    {
        const auto rejected = import(xmp);
        ASSERT_FALSE(rejected);
        EXPECT_EQ(rejected.error().code, ErrorCode::kUnsupported);
    }
    const auto duplicate =
        import(minimal_dehaze_xmp("1", kLegacyV1Parameters, "9", kBlendV9, "1", {}, true));
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
        if (text->find("operation=\"hazeremoval\"") != std::string::npos)
        {
            records.push_back(entry.path());
        }
    }
    std::sort(records.begin(), records.end());
    ASSERT_EQ(records.size(), 2U);
    EXPECT_EQ(records[0].parent_path().filename(), "0026-haze-removal");
    EXPECT_EQ(records[1].parent_path().filename(), "0166-haze-removal-v2");
}

TEST(DehazeTest, DarkChannelAmbientGuidedFilterAndAdaptiveWindowsMatchIndependentOracle)
{
    const WorkingImage input = hazy_fixture(16U, 12U);
    const DehazeParams params{0.7, 0.65, true};
    detail::DehazeAnalysis production_analysis;
    auto production = detail::apply_dehaze_controlled(input, params, CancellationToken{}, {},
                                                      &production_analysis);
    ASSERT_TRUE(production) << production.error().message;
    detail::DehazeAnalysis oracle_analysis;
    const WorkingImage oracle = dehaze_oracle(input, params, oracle_analysis);
    EXPECT_EQ(production_analysis.dark_channel_radius, 6);
    EXPECT_EQ(production_analysis.guided_filter_radius, 9);
    for (std::size_t channel = 0U; channel < 3U; ++channel)
    {
        EXPECT_NEAR(production_analysis.ambient[channel], oracle_analysis.ambient[channel],
                    1.0e-6F);
    }
    EXPECT_NEAR(production_analysis.distance_max, oracle_analysis.distance_max, 1.0e-6F);
    ASSERT_EQ(production.value().rgb.size(), oracle.rgb.size());
    for (std::size_t index = 0U; index < oracle.rgb.size(); ++index)
    {
        EXPECT_NEAR(production.value().rgb[index], oracle.rgb[index], 3.0e-4F) << index;
    }
    EXPECT_NE(production.value().rgb, input.rgb);

    auto downscaled = input;
    downscaled.canonical_roi_scale = CanonicalRoiScale::from_scaled_dimensions(16U, 12U, 32U, 24U);
    detail::DehazeAnalysis scaled_analysis;
    auto scaled = detail::apply_dehaze_controlled(downscaled, params, CancellationToken{}, {},
                                                  &scaled_analysis);
    ASSERT_TRUE(scaled) << scaled.error().message;
    EXPECT_EQ(scaled_analysis.dark_channel_radius, 4);
    EXPECT_EQ(scaled_analysis.guided_filter_radius, 6);
    EXPECT_NE(scaled.value().rgb, production.value().rgb);
}

TEST(DehazeTest, OwnershipFailuresMemoryAndControlledCancellationAreAtomic)
{
    const WorkingImage input = hazy_fixture(16U, 12U);
    const WorkingImage original = input;
    const DehazeParams params{0.7, 0.65, true};
    auto output = apply_dehaze(input, params, CancellationToken{});
    ASSERT_TRUE(output) << output.error().message;
    EXPECT_NE(output.value().rgb.data(), input.rgb.data());
    EXPECT_EQ(output.value().color_profile, input.color_profile);
    EXPECT_EQ(output.value().exposure_analysis, input.exposure_analysis);
    EXPECT_EQ(input.rgb, original.rgb);

    const std::array checkpoints{
        detail::DehazeCheckpoint::kBeforeValidation, detail::DehazeCheckpoint::kDarkChannelRow,
        detail::DehazeCheckpoint::kAmbientSelection, detail::DehazeCheckpoint::kTransitionRow,
        detail::DehazeCheckpoint::kGuidedTile,       detail::DehazeCheckpoint::kGuidedStatisticsRow,
        detail::DehazeCheckpoint::kGuidedSolveRow,   detail::DehazeCheckpoint::kOutputRow,
        detail::DehazeCheckpoint::kBeforePublication};
    for (const auto checkpoint : checkpoints)
    {
        CancellationSource cancellation;
        CancellationFixture fixture{&cancellation, checkpoint};
        const auto rejected = detail::apply_dehaze_controlled(
            input, params, cancellation.token(), {&fixture, cancel_at_checkpoint}, nullptr);
        ASSERT_FALSE(rejected);
        EXPECT_TRUE(fixture.fired);
        EXPECT_EQ(rejected.error().code, ErrorCode::kCancelled);
        EXPECT_EQ(input.rgb, original.rgb);
    }

    auto invalid = input;
    invalid.width = 0U;
    auto rejected = apply_dehaze(invalid, params, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "invalid_dehaze_dimensions");
    invalid = input;
    invalid.rgb.pop_back();
    rejected = apply_dehaze(invalid, params, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "invalid_dehaze_buffer");
    invalid = input;
    invalid.color_profile.identifier = "srgb";
    rejected = apply_dehaze(invalid, params, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "unsupported_dehaze_working_space");
    invalid = input;
    invalid.canonical_roi_scale = {};
    rejected = apply_dehaze(invalid, params, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "invalid_dehaze_roi_scale");
    auto fixed = params;
    fixed.adaptive = false;
    EXPECT_TRUE(apply_dehaze(invalid, fixed, CancellationToken{}));
    invalid = input;
    invalid.rgb.front() = std::numeric_limits<float>::infinity();
    rejected = apply_dehaze(invalid, params, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "nonfinite_dehaze_input");

    auto parameters = dehaze_to_parameters(params);
    ASSERT_TRUE(parameters) << parameters.error().message;
    OperationInstance operation{std::string(kDehazeOperationId),
                                kDehazeOperationSchemaVersion,
                                "dehaze",
                                true,
                                parameters.value(),
                                "mask"};
    rejected = apply_dehaze(input, operation, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "dehaze_mask_graph_unavailable");
    EXPECT_GT(detail::dehaze_working_bytes(16U, 12U, 1.0F, params), 0U);
    auto bad_params = params;
    bad_params.distance = 2.0;
    EXPECT_EQ(detail::dehaze_working_bytes(16U, 12U, 1.0F, bad_params),
              std::numeric_limits<std::uint64_t>::max());
}

TEST(DehazeTest, FrozenV1HasARealRawReferenceAndBudgetAccounting)
{
    const auto before = source_snapshot(mire1_path());
    ASSERT_TRUE(before);
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    Recipe recipe;
    recipe.asset = {"mire1", mire1_path(), std::nullopt};
    recipe.operations.push_back({"ravo.color.input", 1, "input", true,
                                 input_color_to_parameters(InputColorParams{}), std::nullopt});
    auto parameters = dehaze_to_parameters({0.20000000298023224, 0.20000000298023224, true});
    ASSERT_TRUE(parameters) << parameters.error().message;
    recipe.operations.push_back({std::string(kDehazeOperationId), kDehazeOperationSchemaVersion,
                                 "dehaze", true, parameters.value(), std::nullopt});
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
    request.output_width = 85U;
    request.output_height = 128U;
    auto rendered = engine.value().render_to_image(request);
    ASSERT_TRUE(rendered) << rendered.error().message;
    std::array<std::uint64_t, 3> sums{};
    for (std::size_t index = 0U; index + 2U < rendered.value().rgb.size(); index += 3U)
    {
        sums[0] += rendered.value().rgb[index];
        sums[1] += rendered.value().rgb[index + 1U];
        sums[2] += rendered.value().rgb[index + 2U];
    }
    // Ravo-owned reference for the frozen v1 parameters. The independent
    // dark-channel/guided-filter oracle above owns mathematical parity.
    EXPECT_NEAR(static_cast<double>(sums[0]), 1037037.0, 5000.0);
    EXPECT_NEAR(static_cast<double>(sums[1]), 953562.0, 5000.0);
    EXPECT_NEAR(static_cast<double>(sums[2]), 885367.0, 5000.0);

    Recipe baseline = recipe;
    std::erase_if(baseline.operations, [](const OperationInstance &operation)
                  { return operation.id == kDehazeOperationId; });
    request.recipe = baseline;
    auto hazy = engine.value().render_to_image(request);
    ASSERT_TRUE(hazy) << hazy.error().message;
    EXPECT_NE(hazy.value().rgb, rendered.value().rgb);

    DecodedRaw raw;
    raw.width = 2602U;
    raw.height = 3908U;
    const auto baseline_bytes = estimate_raw_render_memory(raw, baseline, 85U, 128U);
    const auto dehaze_bytes = estimate_raw_render_memory(raw, recipe, 85U, 128U);
    const auto scale = CanonicalRoiScale::from_scaled_dimensions(85U, 128U, 2602U, 3908U);
    EXPECT_EQ(dehaze_bytes - baseline_bytes,
              detail::dehaze_working_bytes(85U, 128U, scale.value(),
                                           {0.20000000298023224, 0.20000000298023224, true}));
    EXPECT_EQ(source_snapshot(mire1_path()), before);
}

} // namespace
} // namespace ravo
