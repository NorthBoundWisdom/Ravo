#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ravo/engine/engine.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/recipe/color_input.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/operation.h"
#include "ravo/recipe/recipe.h"
#include "ravo/recipe/sharpen.h"
#include "ravo/recipe/texture.h"

#include "texture.h"

namespace ravo
{
namespace
{

[[nodiscard]] WorkingImage synthetic_working(const std::uint32_t width, const std::uint32_t height,
                                             const std::uint32_t original_width = 0U,
                                             const std::uint32_t original_height = 0U,
                                             const bool include_highlight = true)
{
    WorkingImage image;
    image.width = width;
    image.height = height;
    image.color_profile.kind = ColorProfileKind::kBuiltin;
    image.color_profile.model = ColorModel::kRgb;
    image.color_profile.identifier = std::string(kInputProfileLinearRec709);
    image.color_profile.icc_bytes = {1U, 2U, 3U};
    image.exposure_analysis = std::make_shared<ExposureAnalysisContext>();
    image.canonical_roi_scale = CanonicalRoiScale::from_scaled_dimensions(
        width, height, original_width == 0U ? width : original_width,
        original_height == 0U ? height : original_height);
    image.rgb.resize(static_cast<std::size_t>(width) * height * 3U);
    for (std::uint32_t row = 0U; row < height; ++row)
    {
        for (std::uint32_t column = 0U; column < width; ++column)
        {
            const float checker = ((column / 2U + row / 2U) & 1U) == 0U ? -0.018F : 0.018F;
            const float wave = 0.012F * std::sin(static_cast<float>(column) * 0.19F) *
                               std::cos(static_cast<float>(row) * 0.13F);
            float level = 0.28F + checker + wave;
            if (include_highlight && column > width * 3U / 4U && row < height / 4U)
            {
                level += 1.5F;
            }
            const std::size_t index = (static_cast<std::size_t>(row) * width + column) * 3U;
            image.rgb[index] = level * 1.2F;
            image.rgb[index + 1U] = level * 0.8F;
            image.rgb[index + 2U] = level * 0.4F;
        }
    }
    return image;
}

[[nodiscard]] WorkingImage half_sample(const WorkingImage &input)
{
    WorkingImage output = input;
    output.width = input.width / 2U;
    output.height = input.height / 2U;
    output.canonical_roi_scale = CanonicalRoiScale::from_scaled_dimensions(
        output.width, output.height, input.width, input.height);
    output.rgb.resize(static_cast<std::size_t>(output.width) * output.height * 3U);
    for (std::uint32_t row = 0U; row < output.height; ++row)
    {
        for (std::uint32_t column = 0U; column < output.width; ++column)
        {
            const std::size_t source =
                (static_cast<std::size_t>(row * 2U) * input.width + column * 2U) * 3U;
            const std::size_t target = (static_cast<std::size_t>(row) * output.width + column) * 3U;
            std::copy_n(input.rgb.begin() + static_cast<std::ptrdiff_t>(source), 3U,
                        output.rgb.begin() + static_cast<std::ptrdiff_t>(target));
        }
    }
    return output;
}

[[nodiscard]] double luminance_standard_deviation(const WorkingImage &image)
{
    std::vector<double> values(static_cast<std::size_t>(image.width) * image.height);
    for (std::size_t pixel = 0U; pixel < values.size(); ++pixel)
    {
        values[pixel] = 0.2126 * image.rgb[pixel * 3U] + 0.7152 * image.rgb[pixel * 3U + 1U] +
                        0.0722 * image.rgb[pixel * 3U + 2U];
    }
    double mean = 0.0;
    for (const double value : values)
    {
        mean += value;
    }
    mean /= static_cast<double>(values.size());
    double squared = 0.0;
    for (const double value : values)
    {
        squared += (value - mean) * (value - mean);
    }
    return std::sqrt(squared / static_cast<double>(values.size()));
}

struct CancellationFixture
{
    CancellationSource *source = nullptr;
    detail::TextureCheckpoint target = detail::TextureCheckpoint::kBeforeValidation;
    std::atomic<bool> fired{false};
};

void cancel_at_checkpoint(void *const context, const detail::TextureCheckpoint checkpoint,
                          std::uint32_t) noexcept
{
    auto &fixture = *static_cast<CancellationFixture *>(context);
    bool expected = false;
    if (checkpoint == fixture.target &&
        fixture.fired.compare_exchange_strong(expected, true, std::memory_order_relaxed))
    {
        static_cast<void>(fixture.source->cancel("texture-checkpoint"));
    }
}

[[nodiscard]] std::string raw_fixture_path()
{
    return (std::filesystem::path(RAVO_REPOSITORY_ROOT) / "Ravo" / "tests" / "fixtures" / "frozen" / "images" /
            "mire1.cr2")
        .string();
}

} // namespace

TEST(TextureRecipeTest, StrictSchemaDevelopRoundTripAndRegistryAreCanonical)
{
    const TextureParams authored{0.75, 4.0, 2};
    auto encoded = texture_to_parameters(authored);
    ASSERT_TRUE(encoded) << encoded.error().message;
    ASSERT_EQ(encoded.value().size(), 5U);
    auto decoded = texture_from_parameters(encoded.value());
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded.value(), authored);

    auto unknown = encoded.value();
    unknown.emplace("future", ParameterValue{1.0});
    EXPECT_FALSE(texture_from_parameters(unknown));
    auto fractional_iterations = encoded.value();
    fractional_iterations["iterations"] = ParameterValue{2.0};
    EXPECT_FALSE(texture_from_parameters(fractional_iterations));

    const auto registry = make_phase1_registry();
    ASSERT_TRUE(registry) << registry.error().message;
    const auto *descriptor = registry.value().find(kTextureOperationId);
    ASSERT_NE(descriptor, nullptr);
    EXPECT_EQ(descriptor->parameter_schema_version, kTextureOperationSchemaVersion);
    EXPECT_EQ(descriptor->parameters.size(), 5U);
    EXPECT_TRUE(descriptor->cpu_reference_available);
    EXPECT_FALSE(descriptor->supports_mask);

    DevelopParams develop;
    ASSERT_TRUE(apply_develop_field_strict(develop, "texture", authored.strength));
    ASSERT_TRUE(
        apply_develop_field_strict(develop, "textureDetailThreshold", authored.detail_threshold));
    ASSERT_TRUE(apply_develop_field_strict(develop, "textureIterations",
                                           static_cast<double>(authored.iterations)));
    ASSERT_TRUE(apply_develop_field_strict(develop, "sharpen", 0.5));
    auto recipe = recipe_from_develop({"asset", "file:///fixture.raw", std::nullopt}, develop);
    ASSERT_TRUE(recipe) << recipe.error().message;
    const auto operation =
        std::find_if(recipe.value().operations.begin(), recipe.value().operations.end(),
                     [](const OperationInstance &item) { return item.id == kTextureOperationId; });
    ASSERT_NE(operation, recipe.value().operations.end());
    const auto sharpen =
        std::find_if(recipe.value().operations.begin(), recipe.value().operations.end(),
                     [](const OperationInstance &item) { return item.id == kSharpenOperationId; });
    ASSERT_NE(sharpen, recipe.value().operations.end());
    EXPECT_LT(operation, sharpen);
    auto serialized = serialize_recipe(recipe.value());
    ASSERT_TRUE(serialized) << serialized.error().message;
    auto parsed = parse_recipe_json(serialized.value());
    ASSERT_TRUE(parsed) << parsed.error().message;
    auto round_trip = develop_from_recipe(parsed.value());
    ASSERT_TRUE(round_trip) << round_trip.error().message;
    EXPECT_EQ(round_trip.value().texture, authored);

    EXPECT_FALSE(apply_develop_field_strict(develop, "texture", 2.01));
    EXPECT_FALSE(apply_develop_field_strict(develop, "textureDetailThreshold", 0.0));
    EXPECT_FALSE(apply_develop_field_strict(develop, "textureIterations", 1.5));
    EXPECT_FALSE(apply_develop_field_strict(develop, "textureIterations", 6.0));
    ASSERT_TRUE(reset_develop_field(round_trip.value(), "texture"));
    EXPECT_EQ(round_trip.value().texture, TextureParams{});
}

TEST(TextureTest, EdgeAwareLuminanceScalingPreservesHueAndUnboundedHighlights)
{
    const WorkingImage input = synthetic_working(128U, 96U);
    const WorkingImage original = input;
    const TextureParams params{0.75, 4.0, 1};
    auto output = apply_texture(input, params, CancellationToken{});
    ASSERT_TRUE(output) << output.error().message;
    EXPECT_EQ(input.rgb, original.rgb);
    EXPECT_NE(output.value().rgb, input.rgb);
    EXPECT_NE(output.value().rgb.data(), input.rgb.data());
    EXPECT_EQ(output.value().color_profile, input.color_profile);
    EXPECT_EQ(output.value().exposure_analysis, input.exposure_analysis);
    const WorkingImage detail_input = synthetic_working(128U, 96U, 0U, 0U, false);
    auto detail_output = apply_texture(detail_input, params, CancellationToken{});
    ASSERT_TRUE(detail_output) << detail_output.error().message;
    EXPECT_GT(luminance_standard_deviation(detail_output.value()),
              luminance_standard_deviation(detail_input) * 1.02);

    float maximum = 0.0F;
    for (std::size_t pixel = 0U; pixel < input.rgb.size() / 3U; ++pixel)
    {
        const std::size_t index = pixel * 3U;
        const double red_scale = output.value().rgb[index] / input.rgb[index];
        const double green_scale = output.value().rgb[index + 1U] / input.rgb[index + 1U];
        const double blue_scale = output.value().rgb[index + 2U] / input.rgb[index + 2U];
        EXPECT_NEAR(red_scale, green_scale, 2.0e-5) << pixel;
        EXPECT_NEAR(red_scale, blue_scale, 2.0e-5) << pixel;
        maximum = std::max(maximum, output.value().rgb[index]);
    }
    EXPECT_GT(maximum, 1.0F);

    auto softened = apply_texture(input, TextureParams{-1.0, 4.0, 1}, CancellationToken{});
    ASSERT_TRUE(softened) << softened.error().message;
    EXPECT_LT(luminance_standard_deviation(softened.value()), luminance_standard_deviation(input));

    auto identity = apply_texture(input, TextureParams{}, CancellationToken{});
    ASSERT_TRUE(identity) << identity.error().message;
    EXPECT_EQ(identity.value().rgb, input.rgb);
}

TEST(TextureTest, CanonicalScaleTracksTheSameSourceRadius)
{
    const WorkingImage full = synthetic_working(192U, 128U);
    const WorkingImage preview = half_sample(full);
    const TextureParams params{0.5, 3.0, 1};
    auto full_output = apply_texture(full, params, CancellationToken{});
    auto preview_output = apply_texture(preview, params, CancellationToken{});
    ASSERT_TRUE(full_output) << full_output.error().message;
    ASSERT_TRUE(preview_output) << preview_output.error().message;

    double squared = 0.0;
    std::size_t count = 0U;
    for (std::uint32_t row = 8U; row + 8U < preview.height; ++row)
    {
        for (std::uint32_t column = 8U; column + 8U < preview.width; ++column)
        {
            const std::size_t full_index =
                (static_cast<std::size_t>(row * 2U) * full.width + column * 2U) * 3U;
            const std::size_t preview_index =
                (static_cast<std::size_t>(row) * preview.width + column) * 3U;
            const double delta = static_cast<double>(full_output.value().rgb[full_index + 1U]) -
                                 preview_output.value().rgb[preview_index + 1U];
            squared += delta * delta;
            ++count;
        }
    }
    EXPECT_LT(std::sqrt(squared / static_cast<double>(count)), 0.02);
}

TEST(TextureTest, ValidationCancellationAndMemoryAccountingFailBeforePublication)
{
    const WorkingImage input = synthetic_working(64U, 48U);
    const WorkingImage original = input;
    const TextureParams params{0.75, 4.0, 2};
    const std::array checkpoints{detail::TextureCheckpoint::kBeforeValidation,
                                 detail::TextureCheckpoint::kInputRow,
                                 detail::TextureCheckpoint::kBeforeFineFilter,
                                 detail::TextureCheckpoint::kBeforeCoarseFilter,
                                 detail::TextureCheckpoint::kOutputRow,
                                 detail::TextureCheckpoint::kBeforePublication};
    for (const auto checkpoint : checkpoints)
    {
        CancellationSource cancellation;
        CancellationFixture fixture;
        fixture.source = &cancellation;
        fixture.target = checkpoint;
        const auto rejected = detail::apply_texture_controlled(input, params, cancellation.token(),
                                                               {&fixture, cancel_at_checkpoint});
        ASSERT_FALSE(rejected);
        EXPECT_TRUE(fixture.fired.load(std::memory_order_relaxed));
        EXPECT_EQ(rejected.error().code, ErrorCode::kCancelled);
        EXPECT_EQ(input.rgb, original.rgb);
    }

    auto invalid = input;
    invalid.width = 0U;
    auto rejected = apply_texture(invalid, params, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "invalid_texture_dimensions");
    invalid = input;
    invalid.rgb.pop_back();
    rejected = apply_texture(invalid, params, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "invalid_texture_buffer");
    invalid = input;
    invalid.color_profile.identifier = "srgb";
    rejected = apply_texture(invalid, params, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "unsupported_texture_working_space");
    invalid = input;
    invalid.canonical_roi_scale = {};
    rejected = apply_texture(invalid, params, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "invalid_texture_roi_scale");
    invalid = input;
    invalid.rgb.front() = std::numeric_limits<float>::infinity();
    rejected = apply_texture(invalid, params, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "nonfinite_texture_input");

    auto parameters = texture_to_parameters(params);
    ASSERT_TRUE(parameters) << parameters.error().message;
    OperationInstance operation{std::string(kTextureOperationId),
                                kTextureOperationSchemaVersion,
                                "texture-test",
                                true,
                                parameters.value(),
                                std::string("mask")};
    rejected = apply_texture(input, operation, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "texture_mask_graph_unavailable");
    operation.mask_id.reset();
    ++operation.schema_version;
    rejected = apply_texture(input, operation, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kUnsupported);

    EXPECT_EQ(detail::texture_working_bytes(64U, 48U, params), 64U * 48U * 6U * sizeof(float));
    EXPECT_EQ(detail::texture_working_bytes(64U, 48U, TextureParams{}), 0U);
    auto invalid_params = params;
    invalid_params.iterations = 6;
    EXPECT_EQ(detail::texture_working_bytes(64U, 48U, invalid_params),
              std::numeric_limits<std::uint64_t>::max());
}

TEST(TextureTest, CommittedRawFixtureIsFiniteDeterministicAndSourceOwned)
{
    auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    const std::string path = raw_fixture_path();
    auto decoded = engine.value().decode_raw_frame(path, CancellationToken{});
    ASSERT_TRUE(decoded) << decoded.error().message;
    Recipe recipe;
    recipe.asset = {"mire1", path, std::nullopt};
    recipe.operations.push_back({"ravo.color.input", 1, "input", true,
                                 input_color_to_parameters(InputColorParams{}), std::nullopt});
    auto working = engine.value().linear_working_from_raw(decoded.value(), recipe, 320U, 214U,
                                                          CancellationToken{});
    ASSERT_TRUE(working) << working.error().message;
    working.value().canonical_roi_scale =
        CanonicalRoiScale::from_scaled_dimensions(working.value().width, working.value().height,
                                                  working.value().width, working.value().height);
    const auto original = working.value().rgb;
    const TextureParams params{0.75, 4.0, 1};
    auto first = apply_texture(working.value(), params, CancellationToken{});
    auto second = apply_texture(working.value(), params, CancellationToken{});
    ASSERT_TRUE(first) << first.error().message;
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_EQ(first.value().rgb, second.value().rgb);
    EXPECT_NE(first.value().rgb, original);
    EXPECT_EQ(working.value().rgb, original);
    EXPECT_TRUE(std::all_of(first.value().rgb.begin(), first.value().rgb.end(),
                            [](const float value) { return std::isfinite(value); }));
}

} // namespace ravo
