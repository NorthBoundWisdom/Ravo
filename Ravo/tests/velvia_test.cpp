#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "ravo/adapters/legacy_xmp.h"
#include "ravo/recipe/color_input.h"
#include "ravo/recipe/color_output.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/velvia.h"
#include "velvia.h"

namespace ravo
{
namespace
{

[[nodiscard]] WorkingImage image(const std::vector<float> &rgb, const std::uint32_t width = 0U)
{
    WorkingImage result;
    result.width = width == 0U ? static_cast<std::uint32_t>(rgb.size() / 3U) : width;
    result.height =
        result.width == 0U ? 0U : static_cast<std::uint32_t>(rgb.size() / 3U) / result.width;
    result.rgb = rgb;
    result.color_profile.kind = ColorProfileKind::kBuiltin;
    result.color_profile.model = ColorModel::kRgb;
    result.color_profile.identifier = std::string(kInputProfileLinearRec709);
    result.color_profile.has_matrix = true;
    result.color_profile.matrix_to_xyz_d50 = {0.4360747F, 0.3850649F, 0.1430804F,
                                              0.2225045F, 0.7168786F, 0.0606169F,
                                              0.0139322F, 0.0971045F, 0.7141733F};
    result.canonical_roi_scale = CanonicalRoiScale::from_scaled_dimensions(
        result.width, result.height, result.width, result.height);
    return result;
}

[[nodiscard]] std::array<float, 3> scalar_velvia(const std::array<float, 3> rgb,
                                                 const VelviaParams &params)
{
    const float maximum = std::max(rgb[0], std::max(rgb[1], rgb[2]));
    const float minimum = std::min(rgb[0], std::min(rgb[1], rgb[2]));
    const float luminance = (maximum + minimum) * 0.5F;
    const float saturation =
        luminance <= 0.5F ?
            (maximum - minimum) / (1.0e-5F + maximum + minimum) :
            (maximum - minimum) / (1.0e-5F + std::max(0.0F, 2.0F - maximum - minimum));
    const float bias = static_cast<float>(params.bias);
    const float weight = std::clamp(
        ((1.0F - 1.5F * saturation) + (1.0F + std::abs(luminance - 0.5F) * 2.0F) * (1.0F - bias)) /
            (1.0F + (1.0F - bias)),
        0.0F, 1.0F);
    const float amount = static_cast<float>(params.strength / 100.0) * weight;
    return {
        std::clamp(rgb[0] + amount * (rgb[0] - 0.5F * (rgb[1] + rgb[2])), 0.0F, 1.0F),
        std::clamp(rgb[1] + amount * (rgb[1] - 0.5F * (rgb[0] + rgb[2])), 0.0F, 1.0F),
        std::clamp(rgb[2] + amount * (rgb[2] - 0.5F * (rgb[0] + rgb[1])), 0.0F, 1.0F),
    };
}

[[nodiscard]] std::string read_fixture()
{
    const auto path = std::filesystem::path(RAVO_REPOSITORY_ROOT) / "legacy" / "tests" /
                      "0063-velvia" / "velvia.xmp";
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

TEST(VelviaRecipeTest, SchemaLegacyUpgradeAndDevelopRoundTripAreStrict)
{
    const VelviaParams expected{62.5, 0.35};
    auto parameters = velvia_to_parameters(expected);
    ASSERT_TRUE(parameters) << parameters.error().message;
    EXPECT_EQ(velvia_from_parameters(parameters.value()).value(), expected);

    auto unknown = parameters.value();
    unknown.emplace("future", ParameterValue{1.0});
    EXPECT_FALSE(velvia_from_parameters(unknown));
    EXPECT_FALSE(velvia_to_parameters({100.1, 0.5}));
    EXPECT_FALSE(velvia_to_parameters({50.0, std::numeric_limits<double>::quiet_NaN()}));

    OperationInstance v1{std::string(kVelviaOperationId),
                         1,
                         "old",
                         true,
                         {{"amount", ParameterValue{0.4}}, {"bias", ParameterValue{0.2}}},
                         std::nullopt};
    ASSERT_TRUE(upgrade_velvia_operation(v1));
    EXPECT_EQ(v1.schema_version, kVelviaOperationSchemaVersion);
    EXPECT_EQ(velvia_from_parameters(v1.parameters).value(), (VelviaParams{40.0, 0.2}));

    DevelopParams develop;
    ASSERT_TRUE(apply_develop_field_strict(develop, "velviaEnabled", 1.0));
    ASSERT_TRUE(apply_develop_field_strict(develop, "velviaStrength", 62.5));
    ASSERT_TRUE(apply_develop_field_strict(develop, "velviaBias", 0.35));
    Mask all{"all", kCanonicalMaskSchemaVersion, MaskKind::kAll};
    all.payload = AllMask{};
    develop.masks.push_back(std::move(all));
    develop.velvia_mask_id = "all";
    auto recipe = recipe_from_develop({"asset", "file:///fixture.raw", std::nullopt}, develop);
    ASSERT_TRUE(recipe) << recipe.error().message;
    const auto operation =
        std::find_if(recipe.value().operations.begin(), recipe.value().operations.end(),
                     [](const auto &item) { return item.id == kVelviaOperationId; });
    ASSERT_NE(operation, recipe.value().operations.end());
    EXPECT_EQ(operation->schema_version, kVelviaOperationSchemaVersion);
    EXPECT_EQ(operation->mask_id, "all");
    auto roundtrip = develop_from_recipe(recipe.value());
    ASSERT_TRUE(roundtrip) << roundtrip.error().message;
    EXPECT_TRUE(roundtrip.value().velvia_present);
    EXPECT_TRUE(roundtrip.value().velvia_enabled);
    EXPECT_EQ(roundtrip.value().velvia, expected);
    EXPECT_EQ(roundtrip.value().velvia_mask_id, "all");
    ASSERT_EQ(roundtrip.value().masks.size(), 1U);
    EXPECT_EQ(roundtrip.value().masks.front().id, "all");

    DevelopParams compatibility;
    ASSERT_TRUE(apply_develop_field_strict(compatibility, "velvia", 0.4));
    EXPECT_EQ(compatibility.velvia, (VelviaParams{40.0, 1.0}));
    ASSERT_TRUE(apply_develop_field_strict(compatibility, "velvia", 0.0));
    EXPECT_TRUE(compatibility.is_identity());
}

TEST(VelviaTest, DarkMidtoneLightAndBiasBranchesMatchFrozenScalarFormula)
{
    const std::vector<float> source{0.12F, 0.08F, 0.05F, 0.65F, 0.42F, 0.28F, 0.94F, 0.83F, 0.72F};
    for (const double bias : {0.0, 0.15, 1.0})
    {
        const VelviaParams params{100.0, bias};
        auto result = apply_velvia(image(source), params, {});
        ASSERT_TRUE(result) << result.error().message;
        for (std::size_t pixel = 0U; pixel < 3U; ++pixel)
        {
            const auto expected = scalar_velvia(
                {source[pixel * 3U], source[pixel * 3U + 1U], source[pixel * 3U + 2U]}, params);
            for (std::size_t channel = 0U; channel < 3U; ++channel)
                EXPECT_FLOAT_EQ(result.value().rgb[pixel * 3U + channel], expected[channel]);
        }
    }

    auto identity = apply_velvia(image(source), {0.0, 0.0}, {});
    ASSERT_TRUE(identity);
    EXPECT_EQ(identity.value().rgb, source);
}

TEST(VelviaTest, CanonicalAllMaskMatchesUnmaskedOutput)
{
    auto parameters = velvia_to_parameters({100.0, 0.15});
    ASSERT_TRUE(parameters);
    Recipe plain;
    plain.asset = {"asset", "file:///fixture.raw", std::nullopt};
    plain.operations = {
        {"ravo.color.input", 1, "input", true, input_color_to_parameters({}), std::nullopt},
        {std::string(kVelviaOperationId), kVelviaOperationSchemaVersion, "velvia", true,
         parameters.value(), std::nullopt},
        {"ravo.color.output", 1, "output", true, output_color_to_parameters({}), std::nullopt},
    };
    Recipe masked = plain;
    Mask all{"all", kCanonicalMaskSchemaVersion, MaskKind::kAll};
    all.payload = AllMask{};
    masked.masks.push_back(std::move(all));
    masked.operations[1].mask_id = "all";
    auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine);
    const auto source = image({0.12F, 0.08F, 0.05F, 0.65F, 0.42F, 0.28F});
    auto unmasked = engine.value().render_linear_working(source, plain, {});
    auto attached = engine.value().render_linear_working(source, masked, {});
    ASSERT_TRUE(unmasked) << unmasked.error().message;
    ASSERT_TRUE(attached) << attached.error().message;
    EXPECT_EQ(attached.value().rgb, unmasked.value().rgb);
}

struct CancelState
{
    CancellationSource *source = nullptr;
};

void cancel_velvia(void *context, const detail::VelviaCheckpoint checkpoint,
                   const std::uint32_t progress) noexcept
{
    auto &state = *static_cast<CancelState *>(context);
    if (checkpoint == detail::VelviaCheckpoint::kProcessRow && progress == 1U)
        static_cast<void>(state.source->cancel("velvia-checkpoint"));
}

TEST(VelviaTest, InvalidInputAndCancellationKeepCallerSourceImmutable)
{
    auto source_image = image(std::vector<float>(8U * 4U * 3U, 0.25F), 8U);
    const auto original = source_image.rgb;
    CancellationSource source;
    CancelState state{&source};
    auto cancelled = detail::apply_velvia_controlled(source_image, {100.0, 0.15}, source.token(),
                                                     {&state, cancel_velvia});
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, ErrorCode::kCancelled);
    EXPECT_EQ(source_image.rgb, original);

    auto nonfinite_image = source_image;
    nonfinite_image.rgb[2U] = std::numeric_limits<float>::quiet_NaN();
    auto nonfinite = apply_velvia(nonfinite_image, {100.0, 0.15}, {});
    ASSERT_FALSE(nonfinite);
    EXPECT_EQ(nonfinite.error().context.at("reason"), "nonfinite_velvia_input");

    auto wrong_profile = source_image;
    wrong_profile.color_profile.identifier = "srgb";
    auto unsupported = apply_velvia(wrong_profile, {100.0, 0.15}, {});
    ASSERT_FALSE(unsupported);
    EXPECT_EQ(unsupported.error().context.at("reason"), "unsupported_velvia_working_space");

    auto bad_dimensions = source_image;
    bad_dimensions.width = 7U;
    auto invalid = apply_velvia(bad_dimensions, {100.0, 0.15}, {});
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().context.at("reason"), "invalid_velvia_input");
}

TEST(VelviaLegacyXmpTest, Frozen0063SingletonMapsAndUnprovenStateRejects)
{
    const std::string fixture = read_fixture();
    ASSERT_FALSE(fixture.empty());
    auto imported = import_legacy_xmp({fixture, {"mire1", "file:///fixture.cr2", std::nullopt}});
    ASSERT_TRUE(imported) << imported.error().message;
    const auto found =
        std::find_if(imported.value().operations.begin(), imported.value().operations.end(),
                     [](const auto &operation) { return operation.id == kVelviaOperationId; });
    ASSERT_NE(found, imported.value().operations.end());
    EXPECT_EQ(found->schema_version, kVelviaOperationSchemaVersion);
    auto parsed = velvia_from_parameters(found->parameters);
    ASSERT_TRUE(parsed);
    EXPECT_FLOAT_EQ(static_cast<float>(parsed.value().strength), 100.0F);
    EXPECT_FLOAT_EQ(static_cast<float>(parsed.value().bias), 0.15F);

    std::string modified = fixture;
    const auto payload = modified.find("0000c8429a99193e");
    ASSERT_NE(payload, std::string::npos);
    modified[payload] = '1';
    auto rejected = import_legacy_xmp({modified, {"mire1", "file:///fixture.cr2", std::nullopt}});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "unsupported_legacy_velvia_parameters");

    std::string disabled = fixture;
    const auto record = disabled.find("darktable:operation=\"velvia\"");
    ASSERT_NE(record, std::string::npos);
    const auto enabled = disabled.find("darktable:enabled=\"1\"", record);
    ASSERT_NE(enabled, std::string::npos);
    disabled[enabled + std::string_view("darktable:enabled=\"").size()] = '0';
    auto rejected_disabled =
        import_legacy_xmp({disabled, {"mire1", "file:///fixture.cr2", std::nullopt}});
    ASSERT_FALSE(rejected_disabled);
    EXPECT_EQ(rejected_disabled.error().context.at("reason"),
              "unsupported_legacy_velvia_enabled_state");
}

} // namespace
} // namespace ravo
