#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ravo/adapters/legacy_xmp.h"
#include "ravo/recipe/color_correction.h"
#include "ravo/recipe/color_input.h"
#include "ravo/recipe/operation.h"

#include "color_correction.h"
#include "d50_lab.h"

namespace ravo
{
namespace
{

using Lab = std::array<float, 3>;

struct FrozenColorCorrectionData
{
    float a_scale = 0.0F;
    float a_base = 0.0F;
    float b_scale = 0.0F;
    float b_base = 0.0F;
    float saturation = 1.0F;
};

// Independent scalar oracle transcribed from frozen colorcorrection.c v1
// commit_params() and process(). It calls no production Color Correction helper
// and preserves every float narrowing and parenthesized addition in source order.
[[nodiscard]] FrozenColorCorrectionData
frozen_color_correction_commit(const ColorCorrectionParams &params) noexcept
{
    const float highlight_a = static_cast<float>(params.highlight_a);
    const float highlight_b = static_cast<float>(params.highlight_b);
    const float shadow_a = static_cast<float>(params.shadow_a);
    const float shadow_b = static_cast<float>(params.shadow_b);
    return {(highlight_a - shadow_a) / 100.0F, shadow_a, (highlight_b - shadow_b) / 100.0F,
            shadow_b, static_cast<float>(params.saturation)};
}

[[nodiscard]] Lab frozen_color_correction_lab(const ColorCorrectionParams &params,
                                              const Lab &input) noexcept
{
    const auto data = frozen_color_correction_commit(params);
    return {input[0], data.saturation * (input[1] + input[0] * data.a_scale + data.a_base),
            data.saturation * (input[2] + input[0] * data.b_scale + data.b_base)};
}

// Independent S1.1 bridge transcription from frozen
// common/colorspaces_inline_conversions.h. C12 production must call the shared
// owner; this oracle remains separate so a bridge or operation-order drift is
// visible in the fixed RGB golden.
[[nodiscard]] Lab frozen_linear_rec709_to_xyz(const Lab &rgb) noexcept
{
    return {0.4360747F * rgb[0] + 0.3850649F * rgb[1] + 0.1430804F * rgb[2],
            0.2225045F * rgb[0] + 0.7168786F * rgb[1] + 0.0606169F * rgb[2],
            0.0139322F * rgb[0] + 0.0971045F * rgb[1] + 0.7141733F * rgb[2]};
}

[[nodiscard]] Lab frozen_xyz_to_linear_rec709(const Lab &xyz) noexcept
{
    return {3.1338561F * xyz[0] + (-1.6168667F) * xyz[1] + (-0.4906146F) * xyz[2],
            (-0.9787684F) * xyz[0] + 1.9161415F * xyz[1] + 0.0334540F * xyz[2],
            0.0719453F * xyz[0] + (-0.2289914F) * xyz[1] + 1.4052427F * xyz[2]};
}

[[nodiscard]] Lab frozen_xyz_to_lab(const Lab &xyz) noexcept
{
    constexpr Lab d50_inverse{1.0F / 0.9642F, 1.0F, 1.0F / 0.8249F};
    constexpr float epsilon = 216.0F / 24389.0F;
    constexpr float kappa = 24389.0F / 27.0F;
    Lab transformed{};
    for (std::size_t channel = 0U; channel < transformed.size(); ++channel)
    {
        const float normalized = xyz[channel] * d50_inverse[channel];
        transformed[channel] =
            normalized > epsilon ? std::cbrt(normalized) : (kappa * normalized + 16.0F) / 116.0F;
    }
    return {116.0F * transformed[1] - 16.0F, 500.0F * (transformed[0] - transformed[1]),
            -200.0F * (transformed[2] - transformed[1])};
}

[[nodiscard]] Lab frozen_lab_to_xyz(const Lab &lab) noexcept
{
    constexpr Lab d50{0.9642F, 1.0F, 0.8249F};
    constexpr Lab offset{0.0F, 16.0F, 0.0F};
    constexpr Lab coefficient{1.0F / 500.0F, 1.0F / 116.0F, -1.0F / 200.0F};
    constexpr Lab add_coefficient{1.0F, 0.0F, 1.0F};
    constexpr float epsilon = 0.20689655172413796F;
    constexpr float kappa = 24389.0F / 27.0F;
    const Lab reordered{lab[1], lab[0], lab[2]};
    Lab scaled{};
    for (std::size_t channel = 0U; channel < scaled.size(); ++channel)
    {
        scaled[channel] = (reordered[channel] + offset[channel]) * coefficient[channel];
    }
    Lab xyz{};
    for (std::size_t channel = 0U; channel < xyz.size(); ++channel)
    {
        const float value = scaled[channel] + scaled[1] * add_coefficient[channel];
        const float inverse =
            value > epsilon ? value * value * value : (116.0F * value - 16.0F) / kappa;
        xyz[channel] = d50[channel] * inverse;
    }
    return xyz;
}

[[nodiscard]] Lab frozen_color_correction_rgb(const ColorCorrectionParams &params,
                                              const Lab &rgb) noexcept
{
    const auto lab = frozen_xyz_to_lab(frozen_linear_rec709_to_xyz(rgb));
    return frozen_xyz_to_linear_rec709(frozen_lab_to_xyz(frozen_color_correction_lab(params, lab)));
}

[[nodiscard]] std::array<std::uint32_t, 3> bits(const Lab &value) noexcept
{
    return {std::bit_cast<std::uint32_t>(value[0]), std::bit_cast<std::uint32_t>(value[1]),
            std::bit_cast<std::uint32_t>(value[2])};
}

[[nodiscard]] ColorCorrectionParams fixture_0029_params() noexcept
{
    return {static_cast<double>(std::bit_cast<float>(0x4218c225U)),
            static_cast<double>(std::bit_cast<float>(0xc21209daU)),
            static_cast<double>(std::bit_cast<float>(0xc2143641U)),
            static_cast<double>(std::bit_cast<float>(0x42118cb6U)),
            static_cast<double>(std::bit_cast<float>(0x3fc7ae14U))};
}

[[nodiscard]] WorkingImage working_fixture()
{
    ColorProfileState profile;
    profile.kind = ColorProfileKind::kIcc;
    profile.model = ColorModel::kRgb;
    profile.identifier = std::string(kInputProfileLinearRec709);
    profile.icc_bytes = {1U, 2U, 3U, 4U};
    return {2U, 1U, {0.25F, 0.5F, 0.75F, -0.2F, 1.1F, 2.0F}, std::move(profile), nullptr, {}, {}};
}

inline constexpr std::string_view kLegacy0029Parameters =
    "25c21842da0912c2413614c2b68c114214aec73f";
inline constexpr std::string_view kLegacy0092Parameters =
    "0000000000000000000000000000000000004040";
inline constexpr std::string_view kLegacyBlendV9 =
    "gz11eJxjYGBgkGAAgRNODGiAEV0AJ2iwh+CRyscOAAdeGQQ=";
inline constexpr std::string_view kLegacyBlendV11 =
    "gz13eJxjYGBgYAJiCQYYOOHEgAZY0QVwggZ7CB6pfNoAAFJgGQo=";

struct LegacyColorCorrectionXmpOptions
{
    std::optional<std::string_view> history_position = "11";
    std::optional<std::string_view> version = "1";
    std::optional<std::string_view> enabled = "1";
    std::optional<std::string_view> parameters = kLegacy0029Parameters;
    std::optional<std::string_view> multi_name = "";
    std::optional<std::string_view> multi_priority = "0";
    std::optional<std::string_view> multi_name_hand_edited;
    std::optional<std::string_view> blend_version = "9";
    std::optional<std::string_view> blend_parameters = kLegacyBlendV9;
    std::string_view extra_attributes;
};

[[nodiscard]] std::string
legacy_color_correction_xmp(const std::vector<LegacyColorCorrectionXmpOptions> &entries)
{
    std::string document = R"(<?xml version="1.0"?>
<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"
         xmlns:darktable="http://darktable.sf.net/">
  <rdf:Description darktable:xmp_version="6"><darktable:history><rdf:Seq>)";
    const auto append_attribute =
        [&](const std::string_view name, const std::optional<std::string_view> value)
    {
        if (value)
        {
            document += " darktable:";
            document += name;
            document += "=\"";
            document += *value;
            document += '"';
        }
    };
    for (const auto &entry : entries)
    {
        document += R"(<rdf:li darktable:operation="colorcorrection")";
        append_attribute("num", entry.history_position);
        append_attribute("enabled", entry.enabled);
        append_attribute("modversion", entry.version);
        append_attribute("params", entry.parameters);
        append_attribute("multi_name", entry.multi_name);
        append_attribute("multi_priority", entry.multi_priority);
        append_attribute("multi_name_hand_edited", entry.multi_name_hand_edited);
        append_attribute("blendop_version", entry.blend_version);
        append_attribute("blendop_params", entry.blend_parameters);
        document += entry.extra_attributes;
        document += "/>";
    }
    document += R"(</rdf:Seq></darktable:history></rdf:Description>
</rdf:RDF>)";
    return document;
}

[[nodiscard]] std::string
legacy_color_correction_xmp(const LegacyColorCorrectionXmpOptions &options = {})
{
    return legacy_color_correction_xmp(std::vector<LegacyColorCorrectionXmpOptions>{options});
}

TEST(ColorCorrectionRecipeTest, V1SchemaRoundTripsAllFiveFieldsAndRejectsInvalidSurface)
{
    const auto params = fixture_0029_params();

    const auto encoded = color_correction_to_parameters(params);

    ASSERT_TRUE(encoded) << encoded.error().message;
    EXPECT_EQ(encoded.value().size(), 7U);
    EXPECT_EQ(std::get<std::string>(encoded.value().at("working_space").value),
              kColorCorrectionWorkingSpaceLabD50);
    EXPECT_EQ(std::get<std::string>(encoded.value().at("algorithm").value),
              kColorCorrectionAlgorithmAffineLabV1);
    const auto decoded = color_correction_from_parameters(encoded.value());
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded.value(), params);
    EXPECT_TRUE(validate_color_correction_parameters(encoded.value()));

    const auto registry = make_phase1_registry();
    ASSERT_TRUE(registry) << registry.error().message;
    EXPECT_EQ(kPhase1OperationCount, 62U);
    EXPECT_EQ(registry.value().descriptors().size(), kPhase1OperationCount);
    const auto *descriptor = registry.value().find(kColorCorrectionOperationId);
    ASSERT_NE(descriptor, nullptr);
    EXPECT_EQ(descriptor->parameter_schema_version, kColorCorrectionOperationSchemaVersion);
    EXPECT_EQ(descriptor->parameters.size(), 7U);
    EXPECT_FALSE(descriptor->supports_mask);
    EXPECT_TRUE(descriptor->cpu_reference_available);

    const auto expect_invalid = [](auto parameters)
    {
        const auto result = color_correction_from_parameters(parameters);
        EXPECT_FALSE(result);
        if (!result)
        {
            EXPECT_EQ(result.error().code, ErrorCode::kValidation);
            EXPECT_EQ(result.error().context.at("reason"), "invalid_colorcorrection_parameters");
        }
    };
    auto invalid = encoded.value();
    invalid.emplace("unknown", ParameterValue{0.0});
    expect_invalid(invalid);
    invalid = encoded.value();
    invalid.erase("highlight_a");
    expect_invalid(invalid);
    invalid = encoded.value();
    invalid["shadow_b"] = ParameterValue{std::string("zero")};
    expect_invalid(invalid);
    for (const auto &[field, value] : std::array<std::pair<std::string_view, double>, 7>{{
             {"highlight_a", kColorCorrectionEndpointMin - 0.01},
             {"highlight_b", kColorCorrectionEndpointMax + 0.01},
             {"shadow_a", kColorCorrectionEndpointMin - 0.01},
             {"shadow_b", kColorCorrectionEndpointMax + 0.01},
             {"saturation", kColorCorrectionSaturationMin - 0.01},
             {"saturation", kColorCorrectionSaturationMax + 0.01},
             {"highlight_a", std::numeric_limits<double>::quiet_NaN()},
         }})
    {
        invalid = encoded.value();
        invalid[std::string(field)] = ParameterValue{value};
        expect_invalid(invalid);
    }
}

TEST(ColorCorrectionTest, AffineLabMathMatchesFrozenSourceOrderAndBitGoldens)
{
    struct Case
    {
        ColorCorrectionParams params;
        Lab input;
        std::array<std::uint32_t, 3> golden;
    };
    const std::array cases{
        Case{
            fixture_0029_params(), {50.0F, -12.25F, 4.5F}, {0x42480000U, 0xc191c99cU, 0x40dd96f6U}},
        Case{fixture_0029_params(),
             {-20.0F, 130.0F, -140.0F},
             {0xc1a00000U, 0x42f30b16U, 0xc30ae44aU}},
        Case{fixture_0029_params(),
             {125.0F, -200.0F, 300.0F},
             {0x42fa0000U, 0xc35f1461U, 0x43bf4ebcU}},
        Case{{0.0, 0.0, 0.0, 0.0, 3.0},
             {42.0F, -5.0F, 7.0F},
             {0x42280000U, 0xc1700000U, 0x41a80000U}},
        Case{{40.0, -40.0, -40.0, 40.0, -3.0},
             {100.0F, -20.0F, 30.0F},
             {0x42c80000U, 0xc2700000U, 0x41f00000U}},
    };
    for (const auto &[params, input, golden] : cases)
    {
        SCOPED_TRACE(testing::Message() << bits(input)[0]);
        const auto oracle = frozen_color_correction_lab(params, input);
        EXPECT_EQ(bits(oracle), golden);
        const auto actual = apply_color_correction_lab(params, input, CancellationToken{});
        ASSERT_TRUE(actual) << actual.error().message;
        EXPECT_EQ(bits(actual.value()), golden);
        EXPECT_EQ(actual.value()[0], input[0]);
    }

    auto field_order_perturbation = fixture_0029_params();
    std::swap(field_order_perturbation.highlight_a, field_order_perturbation.shadow_a);
    EXPECT_NE(bits(frozen_color_correction_lab(field_order_perturbation, cases[1].input)),
              cases[1].golden)
        << "the independent oracle must detect swapped legacy highlight/shadow endpoints";
}

TEST(ColorCorrectionTest, RgbBridgeMatchesIndependentFrozenOracleWithoutClampingExtendedValues)
{
    WorkingImage input;
    input.width = 1U;
    input.height = 1U;
    input.rgb = {0.25F, 0.5F, 0.75F};
    input.color_profile.kind = ColorProfileKind::kIcc;
    input.color_profile.model = ColorModel::kRgb;
    input.color_profile.identifier = std::string(kInputProfileLinearRec709);
    input.color_profile.icc_bytes = {1U, 2U, 3U, 4U};
    auto analysis = std::make_shared<ExposureAnalysisContext>();
    analysis->raw_pixel_count = 3U;
    input.exposure_analysis = std::move(analysis);
    const auto original = input;
    const auto params = fixture_0029_params();
    const std::array<std::uint32_t, 3> golden{0x3e820d07U, 0x3ee0ecc4U, 0x3fb9fe91U};
    const auto oracle = frozen_color_correction_rgb(params, {0.25F, 0.5F, 0.75F});
    for (std::size_t channel = 0U; channel < golden.size(); ++channel)
    {
        // cbrtf is supplied by each supported platform's libm. The source-order
        // oracle must agree bit-for-bit with production on that host, while the
        // recorded reference allows the bounded cross-platform libm variation.
        EXPECT_NEAR(oracle[channel], std::bit_cast<float>(golden[channel]), 1.0e-5F);
    }

    const auto output = apply_color_correction(input, params, CancellationToken{});

    ASSERT_TRUE(output) << output.error().message;
    ASSERT_EQ(output.value().rgb.size(), 3U);
    const auto output_bits =
        bits({output.value().rgb[0], output.value().rgb[1], output.value().rgb[2]});
    EXPECT_EQ(output_bits, bits(oracle));
    EXPECT_GT(output.value().rgb[2], 1.0F);
    EXPECT_EQ(output.value().color_profile, input.color_profile);
    EXPECT_EQ(output.value().exposure_analysis, input.exposure_analysis);
    EXPECT_EQ(input.rgb, original.rgb);
    EXPECT_NE(output.value().rgb.data(), input.rgb.data());
    EXPECT_NE(output.value().color_profile.icc_bytes.data(), input.color_profile.icc_bytes.data());

    const auto default_oracle = frozen_color_correction_rgb({}, {0.25F, 0.5F, 0.75F});
    const std::array<std::uint32_t, 3> default_golden{0x3e800006U, 0x3efffffcU, 0x3f400000U};
    for (std::size_t channel = 0U; channel < default_golden.size(); ++channel)
    {
        EXPECT_NEAR(default_oracle[channel], std::bit_cast<float>(default_golden[channel]),
                    1.0e-5F);
    }
    const auto explicit_defaults =
        apply_color_correction(input, ColorCorrectionParams{}, CancellationToken{});
    ASSERT_TRUE(explicit_defaults) << explicit_defaults.error().message;
    const auto explicit_default_bits =
        bits({explicit_defaults.value().rgb[0], explicit_defaults.value().rgb[1],
              explicit_defaults.value().rgb[2]});
    EXPECT_EQ(explicit_default_bits, bits(default_oracle));
    EXPECT_NE(bits({input.rgb[0], input.rgb[1], input.rgb[2]}), default_golden)
        << "an explicitly present default operation still crosses the frozen D50 Lab bridge";
}

TEST(ColorCorrectionTest, CanonicalRecipeDispatchMatchesTheDirectFrozenOperation)
{
    WorkingImage input;
    input.width = 1U;
    input.height = 1U;
    input.rgb = {0.25F, 0.5F, 0.75F};
    input.color_profile.kind = ColorProfileKind::kBuiltin;
    input.color_profile.model = ColorModel::kRgb;
    input.color_profile.identifier = std::string(kInputProfileLinearRec709);
    const auto params = fixture_0029_params();
    const auto expected = frozen_color_correction_rgb(params, {0.25F, 0.5F, 0.75F});
    const auto encoded = color_correction_to_parameters(params);
    ASSERT_TRUE(encoded) << encoded.error().message;
    Recipe recipe;
    recipe.asset.id = "colorcorrection-asset";
    recipe.asset.input_uri = "file:///colorcorrection-fixture.raw";
    recipe.operations.push_back({std::string(kColorCorrectionOperationId),
                                 kColorCorrectionOperationSchemaVersion, "colorcorrection-dispatch",
                                 true, encoded.value(), std::nullopt});
    const auto registry = make_phase1_registry();
    ASSERT_TRUE(registry) << registry.error().message;
    const auto valid_recipe = validate_recipe(recipe, registry.value());
    ASSERT_TRUE(valid_recipe) << valid_recipe.error().message;

    const auto direct = apply_color_correction(input, params, CancellationToken{});
    const auto dispatched = apply_recipe_ops(input, recipe, CancellationToken{});

    ASSERT_TRUE(direct) << direct.error().message;
    ASSERT_TRUE(dispatched) << dispatched.error().message;
    ASSERT_EQ(direct.value().rgb.size(), expected.size());
    ASSERT_EQ(dispatched.value().rgb.size(), expected.size());
    const Lab direct_rgb{direct.value().rgb[0], direct.value().rgb[1], direct.value().rgb[2]};
    const Lab dispatched_rgb{dispatched.value().rgb[0], dispatched.value().rgb[1],
                             dispatched.value().rgb[2]};
    EXPECT_EQ(bits(direct_rgb), bits(expected));
    EXPECT_EQ(bits(dispatched_rgb), bits(expected));
    EXPECT_EQ(bits(dispatched_rgb), bits(direct_rgb));
    EXPECT_EQ(input.rgb, (std::vector<float>{0.25F, 0.5F, 0.75F}));
}

TEST(ColorCorrectionTest, CancellationNonFiniteMasksAndPublicationFailuresAreAtomic)
{
    const auto input = working_fixture();
    const auto original = input;
    CancellationSource cancelled;
    ASSERT_TRUE(cancelled.cancel("colorcorrection-pre"));
    auto rejected = apply_color_correction(input, fixture_0029_params(), cancelled.token());
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kCancelled);

    for (const float invalid :
         {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
          -std::numeric_limits<float>::infinity()})
    {
        auto nonfinite = input;
        nonfinite.rgb[1] = invalid;
        const auto source = nonfinite.rgb;
        rejected = apply_color_correction(nonfinite, fixture_0029_params(), CancellationToken{});
        ASSERT_FALSE(rejected);
        EXPECT_EQ(rejected.error().context.at("reason"), "nonfinite_colorcorrection_input");
        for (std::size_t index = 0U; index < source.size(); ++index)
        {
            EXPECT_EQ(std::bit_cast<std::uint32_t>(nonfinite.rgb[index]),
                      std::bit_cast<std::uint32_t>(source[index]));
        }
    }
    auto bad_params = fixture_0029_params();
    bad_params.shadow_b = std::numeric_limits<double>::quiet_NaN();
    const auto invalid_params =
        apply_color_correction_lab(bad_params, {50.0F, 0.0F, 0.0F}, CancellationToken{});
    ASSERT_FALSE(invalid_params);
    EXPECT_EQ(invalid_params.error().context.at("reason"), "invalid_colorcorrection_parameters");

    auto canonical = color_correction_to_parameters(fixture_0029_params());
    ASSERT_TRUE(canonical) << canonical.error().message;
    OperationInstance masked{std::string(kColorCorrectionOperationId),
                             kColorCorrectionOperationSchemaVersion,
                             "colorcorrection-mask",
                             true,
                             std::move(canonical).value(),
                             "mask-1"};
    rejected = apply_color_correction(input, masked, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(rejected.error().context.at("reason"), "colorcorrection_mask_graph_unavailable");
    auto wrong_id = masked;
    wrong_id.mask_id.reset();
    wrong_id.id = "ravo.color.colorchecker";
    rejected = apply_color_correction(input, wrong_id, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kValidation);
    auto wrong_schema = masked;
    wrong_schema.mask_id.reset();
    wrong_schema.schema_version = kColorCorrectionOperationSchemaVersion + 1;
    rejected = apply_color_correction(input, wrong_schema, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kUnsupported);
    masked.mask_id.reset();
    masked.enabled = false;
    const auto disabled = apply_color_correction(input, masked, CancellationToken{});
    ASSERT_TRUE(disabled) << disabled.error().message;
    EXPECT_EQ(disabled.value().rgb, input.rgb);
    EXPECT_NE(disabled.value().rgb.data(), input.rgb.data());

    auto zero = input;
    zero.width = 0U;
    rejected = apply_color_correction(zero, fixture_0029_params(), CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "invalid_colorcorrection_dimensions");
    auto wrong_size = input;
    wrong_size.rgb.pop_back();
    rejected = apply_color_correction(wrong_size, fixture_0029_params(), CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "invalid_colorcorrection_buffer");
    auto wrong_model = input;
    wrong_model.color_profile.model = ColorModel::kLab;
    rejected = apply_color_correction(wrong_model, fixture_0029_params(), CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "unsupported_colorcorrection_working_space");
    auto wrong_profile = input;
    wrong_profile.color_profile.identifier = "srgb";
    rejected = apply_color_correction(wrong_profile, fixture_0029_params(), CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "unsupported_colorcorrection_working_space");

    EXPECT_EQ(input.width, original.width);
    EXPECT_EQ(input.height, original.height);
    EXPECT_EQ(input.rgb, original.rgb);
    EXPECT_EQ(input.color_profile, original.color_profile);
}

TEST(ColorCorrectionTest, DeadlineCancellationDuringRowsNeverMutatesTheSource)
{
    WorkingImage input;
    input.width = 1024U;
    input.height = 4096U;
    input.rgb.assign(static_cast<std::size_t>(input.width) * input.height * 3U, 0.25F);
    input.color_profile.model = ColorModel::kRgb;
    input.color_profile.identifier = std::string(kInputProfileLinearRec709);
    const auto deadline = CancellationSource::with_deadline(std::chrono::steady_clock::now() +
                                                            std::chrono::milliseconds{1});

    const auto cancelled = apply_color_correction(input, fixture_0029_params(), deadline.token());

    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, ErrorCode::kCancelled);
    EXPECT_FLOAT_EQ(input.rgb.front(), 0.25F);
    EXPECT_FLOAT_EQ(input.rgb.back(), 0.25F);
}

TEST(ColorCorrectionLegacyXmpTest, ImportsBothEvidencedDefaultUnmaskedV1SingletonEnvelopes)
{
    LegacyColorCorrectionXmpOptions options_0029;
    const auto imported_0029 =
        import_legacy_xmp({legacy_color_correction_xmp(options_0029),
                           {"asset-1", "file:///fixture.raw", std::nullopt}});
    ASSERT_TRUE(imported_0029) << imported_0029.error().message;
    ASSERT_EQ(imported_0029.value().operations.size(), 3U);
    const auto &operation_0029 = imported_0029.value().operations[1];
    EXPECT_EQ(operation_0029.id, kColorCorrectionOperationId);
    EXPECT_EQ(operation_0029.schema_version, kColorCorrectionOperationSchemaVersion);
    EXPECT_EQ(operation_0029.instance_id, "legacy-colorcorrection-11");
    EXPECT_TRUE(operation_0029.enabled);
    EXPECT_FALSE(operation_0029.mask_id.has_value());
    const auto params_0029 = color_correction_from_parameters(operation_0029.parameters);
    ASSERT_TRUE(params_0029) << params_0029.error().message;
    EXPECT_EQ(params_0029.value(), fixture_0029_params());

    LegacyColorCorrectionXmpOptions options_0092;
    options_0092.history_position = "037";
    options_0092.parameters = kLegacy0092Parameters;
    options_0092.blend_version = "11";
    options_0092.blend_parameters = kLegacyBlendV11;
    const auto imported_0092 =
        import_legacy_xmp({legacy_color_correction_xmp(options_0092),
                           {"asset-1", "file:///fixture.raw", std::nullopt}});
    ASSERT_TRUE(imported_0092) << imported_0092.error().message;
    ASSERT_EQ(imported_0092.value().operations.size(), 3U);
    const auto &operation_0092 = imported_0092.value().operations[1];
    EXPECT_EQ(operation_0092.id, kColorCorrectionOperationId);
    EXPECT_EQ(operation_0092.instance_id, "legacy-colorcorrection-37");
    const auto params_0092 = color_correction_from_parameters(operation_0092.parameters);
    ASSERT_TRUE(params_0092) << params_0092.error().message;
    EXPECT_EQ(params_0092.value(), (ColorCorrectionParams{0.0, 0.0, 0.0, 0.0, 3.0}));
}

TEST(ColorCorrectionLegacyXmpTest, RejectsEveryNonEvidencedPresentationStateStructurally)
{
    const auto expect_rejected = [](const LegacyColorCorrectionXmpOptions &options,
                                    const ErrorCode code, const std::string_view reason)
    {
        const auto imported = import_legacy_xmp({legacy_color_correction_xmp(options),
                                                 {"asset-1", "file:///fixture.raw", std::nullopt}});
        ASSERT_FALSE(imported);
        EXPECT_EQ(imported.error().code, code);
        EXPECT_EQ(imported.error().context.at("reason"), reason);
    };

    LegacyColorCorrectionXmpOptions unsupported_version;
    unsupported_version.version = "2";
    expect_rejected(unsupported_version, ErrorCode::kUnsupported,
                    "unsupported_legacy_colorcorrection_version");
    LegacyColorCorrectionXmpOptions disabled;
    disabled.enabled = "0";
    expect_rejected(disabled, ErrorCode::kUnsupported,
                    "unsupported_legacy_colorcorrection_enabled_state");
    LegacyColorCorrectionXmpOptions invalid_enabled;
    invalid_enabled.enabled = "2";
    expect_rejected(invalid_enabled, ErrorCode::kUnsupported,
                    "unsupported_legacy_colorcorrection_enabled_state");
    for (const std::string_view priority : {"1", "00"})
    {
        LegacyColorCorrectionXmpOptions multi;
        multi.multi_priority = priority;
        expect_rejected(multi, ErrorCode::kUnsupported,
                        "unsupported_legacy_colorcorrection_multi_state");
    }
    LegacyColorCorrectionXmpOptions named;
    named.multi_name = "second";
    expect_rejected(named, ErrorCode::kUnsupported,
                    "unsupported_legacy_colorcorrection_multi_state");
    LegacyColorCorrectionXmpOptions hand_edited;
    hand_edited.multi_name_hand_edited = "1";
    expect_rejected(hand_edited, ErrorCode::kUnsupported,
                    "unsupported_legacy_colorcorrection_multi_state");
    LegacyColorCorrectionXmpOptions mismatched_blend;
    mismatched_blend.blend_version = "11";
    expect_rejected(mismatched_blend, ErrorCode::kUnsupported,
                    "unsupported_legacy_colorcorrection_blend");
    LegacyColorCorrectionXmpOptions custom_blend;
    custom_blend.blend_parameters = "gz11eJxjZGBgYGYAgQVODGiAEV0AJ2iwh+CRyscOALejGMg=";
    expect_rejected(custom_blend, ErrorCode::kUnsupported,
                    "unsupported_legacy_colorcorrection_blend");
    LegacyColorCorrectionXmpOptions explicit_mask;
    explicit_mask.extra_attributes = R"( darktable:mask_id="42")";
    expect_rejected(explicit_mask, ErrorCode::kUnsupported,
                    "unsupported_legacy_colorcorrection_mask");
    LegacyColorCorrectionXmpOptions unknown;
    unknown.extra_attributes = R"( darktable:unproven="1")";
    expect_rejected(unknown, ErrorCode::kUnsupported,
                    "unsupported_legacy_colorcorrection_attribute");

    LegacyColorCorrectionXmpOptions wrong_length;
    wrong_length.parameters = "00000000";
    expect_rejected(wrong_length, ErrorCode::kValidation,
                    "invalid_legacy_colorcorrection_parameters");
    LegacyColorCorrectionXmpOptions nonfinite;
    nonfinite.parameters = "0000c07f0000000000000000000000000000803f";
    expect_rejected(nonfinite, ErrorCode::kValidation, "invalid_legacy_colorcorrection_parameters");

    LegacyColorCorrectionXmpOptions duplicate;
    const auto duplicate_result =
        import_legacy_xmp({legacy_color_correction_xmp({duplicate, duplicate}),
                           {"asset-1", "file:///fixture.raw", std::nullopt}});
    ASSERT_FALSE(duplicate_result);
    EXPECT_EQ(duplicate_result.error().code, ErrorCode::kConflict);
    EXPECT_EQ(duplicate_result.error().context.at("reason"), "duplicate_legacy_colorcorrection");
}

} // namespace
} // namespace ravo
