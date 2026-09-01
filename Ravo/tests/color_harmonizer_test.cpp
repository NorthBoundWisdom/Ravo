#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "ravo/adapters/legacy_xmp.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/recipe/color_harmonizer.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/operation.h"

#include "color_harmonizer.h"
#include "harmony_geometry.h"

namespace ravo
{
namespace
{

constexpr std::string_view kRecord12Parameters =
    "03000000cdcccc3d000000000000003f0000803f000000000000803e0000003f0000403f040000000000803f0000803f"
    "0000803f0000803f00000000";
constexpr std::string_view kRecord13Parameters =
    "04000000cdcc0c3f85eb513f0000003f1f85eb3f000000000000803e0000003f0000403f04000000ae47a13fec51383e"
    "5c8fc23f0000803f00000000";
constexpr std::string_view kFrozenBlendV14 =
    "gz08eJxjYGBgYAFiCQYYOOHEgAZY0QWAgBGLGANDgz0Ej1Q+dlAx68oBEMbFxwX+AwGIBgCbGCeh";
constexpr std::string_view kLegacyColorBalanceHistoryEntry =
    "<rdf:li darktable:operation=\"colorbalance\" darktable:num=\"15\""
    " darktable:enabled=\"1\" darktable:modversion=\"3\""
    " darktable:params=\"010000000000803f0000803f0000803f0000803ffeff7f3f0000803f0000803f"
    "0000803f0000803f0000803f0000803f0000803f0000803f8de4aa3f024ee1410000803f\""
    " darktable:multi_name=\"\" darktable:multi_priority=\"0\""
    " darktable:blendop_version=\"9\""
    " darktable:blendop_params=\"gz11eJxjYGBgkGAAgRNODGiAEV0AJ2iwh+CRyscOAAdeGQQ=\"/>";
constexpr std::size_t kColorHarmonizerV1ByteCount = 60U;

struct LegacyColorHarmonizerXmpOptions
{
    std::optional<std::string_view> history_position = "12";
    std::optional<std::string_view> version = "1";
    std::optional<std::string_view> enabled = "1";
    std::optional<std::string_view> parameters = kRecord12Parameters;
    std::optional<std::string_view> multi_name = "";
    std::optional<std::string_view> multi_priority = "0";
    std::optional<std::string_view> multi_name_hand_edited;
    std::optional<std::string_view> blend_version = "14";
    std::optional<std::string_view> blend_parameters = kFrozenBlendV14;
    std::string_view extra_attributes;
};

[[nodiscard]] std::string
legacy_color_harmonizer_xmp(const std::vector<LegacyColorHarmonizerXmpOptions> &entries,
                            const std::string_view additional_history = {})
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
        document += R"(<rdf:li darktable:operation="colorharmonizer")";
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
    document += additional_history;
    document += R"(</rdf:Seq></darktable:history></rdf:Description>
</rdf:RDF>)";
    return document;
}

[[nodiscard]] std::string
legacy_color_harmonizer_xmp(const LegacyColorHarmonizerXmpOptions &options = {})
{
    return legacy_color_harmonizer_xmp(std::vector<LegacyColorHarmonizerXmpOptions>{options});
}

[[nodiscard]] ColorHarmonizerParams frozen_record13() noexcept
{
    ColorHarmonizerParams params;
    params.rule = ColorHarmonizerRule::kSplitComplementary;
    params.anchor_hue = 0.55000001192092896;
    params.pull_strength = 0.81999999284744263;
    params.pull_width = 1.8400000333786011;
    params.node_saturation = {1.2599999904632568, 0.18000000715255737, 1.5199999809265137, 1.0};
    return params;
}

[[nodiscard]] std::uint32_t hex_u32_le(const std::string_view hex, const std::size_t byte_offset)
{
    std::uint32_t value = 0;
    for (std::size_t index = 0U; index < 4U; ++index)
    {
        const auto high = hex[(byte_offset + index) * 2U];
        const auto low = hex[(byte_offset + index) * 2U + 1U];
        const auto nibble = [](const char digit) -> std::uint32_t
        {
            if (digit >= '0' && digit <= '9')
            {
                return static_cast<std::uint32_t>(digit - '0');
            }
            if (digit >= 'a' && digit <= 'f')
            {
                return static_cast<std::uint32_t>(digit - 'a' + 10);
            }
            return static_cast<std::uint32_t>(digit - 'A' + 10);
        };
        value |= ((nibble(high) << 4U) | nibble(low)) << (8U * index);
    }
    return value;
}

[[nodiscard]] float hex_f32_le(const std::string_view hex, const std::size_t byte_offset)
{
    return std::bit_cast<float>(hex_u32_le(hex, byte_offset));
}

[[nodiscard]] const OperationInstance *find_operation(const Recipe &recipe,
                                                      const std::string_view id)
{
    for (const auto &operation : recipe.operations)
    {
        if (operation.id == id)
        {
            return &operation;
        }
    }
    return nullptr;
}

TEST(ColorHarmonizerLegacyLayoutTest, FrozenV1PayloadIsExactlySixtyLittleEndianBytes)
{
    EXPECT_EQ(kRecord12Parameters.size(), kColorHarmonizerV1ByteCount * 2U);
    EXPECT_EQ(kRecord13Parameters.size(), kColorHarmonizerV1ByteCount * 2U);
    EXPECT_EQ(hex_u32_le(kRecord12Parameters, 0U), 3U);
    EXPECT_EQ(std::bit_cast<std::uint32_t>(hex_f32_le(kRecord12Parameters, 4U)), 0x3dcccccdU);
    EXPECT_FLOAT_EQ(hex_f32_le(kRecord12Parameters, 4U), 0.1F);
    EXPECT_FLOAT_EQ(hex_f32_le(kRecord12Parameters, 8U), 0.0F);
    EXPECT_FLOAT_EQ(hex_f32_le(kRecord12Parameters, 12U), 0.5F);
    EXPECT_FLOAT_EQ(hex_f32_le(kRecord12Parameters, 16U), 1.0F);
    EXPECT_FLOAT_EQ(hex_f32_le(kRecord12Parameters, 20U), 0.0F);
    EXPECT_FLOAT_EQ(hex_f32_le(kRecord12Parameters, 24U), 0.25F);
    EXPECT_FLOAT_EQ(hex_f32_le(kRecord12Parameters, 28U), 0.5F);
    EXPECT_FLOAT_EQ(hex_f32_le(kRecord12Parameters, 32U), 0.75F);
    EXPECT_EQ(hex_u32_le(kRecord12Parameters, 36U), 4U);
    EXPECT_FLOAT_EQ(hex_f32_le(kRecord12Parameters, 40U), 1.0F);
    EXPECT_FLOAT_EQ(hex_f32_le(kRecord12Parameters, 44U), 1.0F);
    EXPECT_FLOAT_EQ(hex_f32_le(kRecord12Parameters, 48U), 1.0F);
    EXPECT_FLOAT_EQ(hex_f32_le(kRecord12Parameters, 52U), 1.0F);
    EXPECT_FLOAT_EQ(hex_f32_le(kRecord12Parameters, 56U), 0.0F);

    EXPECT_EQ(hex_u32_le(kRecord13Parameters, 0U), 4U);
    EXPECT_EQ(hex_f32_le(kRecord13Parameters, 4U), 0.55000001192092896F);
    EXPECT_EQ(hex_f32_le(kRecord13Parameters, 8U), 0.81999999284744263F);
    EXPECT_EQ(hex_f32_le(kRecord13Parameters, 16U), 1.8400000333786011F);
    EXPECT_EQ(hex_f32_le(kRecord13Parameters, 40U), 1.2599999904632568F);
    EXPECT_EQ(hex_f32_le(kRecord13Parameters, 44U), 0.18000000715255737F);
    EXPECT_EQ(hex_f32_le(kRecord13Parameters, 48U), 1.5199999809265137F);
    EXPECT_FLOAT_EQ(hex_f32_le(kRecord13Parameters, 56U), 0.0F);
}

TEST(ColorHarmonizerLegacyXmpTest, Record12AloneMapsTheExplicitCanonicalDefault)
{
    const auto imported = import_legacy_xmp(
        {legacy_color_harmonizer_xmp(), {"asset-1", "file:///fixture.raw", std::nullopt}});
    ASSERT_TRUE(imported) << imported.error().message;
    const auto *operation = find_operation(imported.value(), kColorHarmonizerOperationId);
    ASSERT_NE(operation, nullptr);
    EXPECT_EQ(operation->schema_version, kColorHarmonizerOperationSchemaVersion);
    EXPECT_EQ(operation->instance_id, "legacy-colorharmonizer-0");
    EXPECT_TRUE(operation->enabled);
    EXPECT_FALSE(operation->mask_id.has_value());
    EXPECT_EQ(operation->parameters.size(), 17U);
    auto decoded = color_harmonizer_from_parameters(operation->parameters);
    ASSERT_TRUE(decoded) << decoded.error().message;
    const ColorHarmonizerParams defaults;
    EXPECT_EQ(decoded.value().rule, defaults.rule);
    EXPECT_FLOAT_EQ(static_cast<float>(decoded.value().anchor_hue),
                    static_cast<float>(defaults.anchor_hue));
    EXPECT_FLOAT_EQ(static_cast<float>(decoded.value().pull_strength),
                    static_cast<float>(defaults.pull_strength));
    EXPECT_FLOAT_EQ(static_cast<float>(decoded.value().neutral_protection),
                    static_cast<float>(defaults.neutral_protection));
    EXPECT_FLOAT_EQ(static_cast<float>(decoded.value().pull_width),
                    static_cast<float>(defaults.pull_width));
    for (std::size_t index = 0U; index < defaults.custom_hue.size(); ++index)
    {
        EXPECT_FLOAT_EQ(static_cast<float>(decoded.value().custom_hue[index]),
                        static_cast<float>(defaults.custom_hue[index]));
        EXPECT_FLOAT_EQ(static_cast<float>(decoded.value().node_saturation[index]),
                        static_cast<float>(defaults.node_saturation[index]));
    }
    EXPECT_EQ(decoded.value().num_custom_nodes, defaults.num_custom_nodes);
    EXPECT_FLOAT_EQ(static_cast<float>(decoded.value().smoothing),
                    static_cast<float>(defaults.smoothing));
    EXPECT_EQ(std::get<std::string>(operation->parameters.at("rule").value), "complementary");
    EXPECT_EQ(std::get<std::string>(operation->parameters.at("working_space").value),
              kColorHarmonizerWorkingSpace);
    EXPECT_EQ(std::get<std::string>(operation->parameters.at("algorithm").value),
              kColorHarmonizerAlgorithm);
}

TEST(ColorHarmonizerLegacyXmpTest, GreatestHistoryPositionWinsIncludingReversedXml)
{
    LegacyColorHarmonizerXmpOptions record13;
    record13.history_position = "13";
    record13.parameters = kRecord13Parameters;

    const auto forward = import_legacy_xmp({legacy_color_harmonizer_xmp({{}, record13}),
                                            {"asset-1", "file:///fixture.raw", std::nullopt}});
    ASSERT_TRUE(forward) << forward.error().message;
    const auto *forward_op = find_operation(forward.value(), kColorHarmonizerOperationId);
    ASSERT_NE(forward_op, nullptr);
    auto forward_params = color_harmonizer_from_parameters(forward_op->parameters);
    ASSERT_TRUE(forward_params) << forward_params.error().message;
    EXPECT_EQ(forward_params.value(), frozen_record13());

    const auto reversed = import_legacy_xmp({legacy_color_harmonizer_xmp({record13, {}}),
                                             {"asset-1", "file:///fixture.raw", std::nullopt}});
    ASSERT_TRUE(reversed) << reversed.error().message;
    const auto *reversed_op = find_operation(reversed.value(), kColorHarmonizerOperationId);
    ASSERT_NE(reversed_op, nullptr);
    auto reversed_params = color_harmonizer_from_parameters(reversed_op->parameters);
    ASSERT_TRUE(reversed_params) << reversed_params.error().message;
    EXPECT_EQ(reversed_params.value(), frozen_record13());
    EXPECT_EQ(forward_op->instance_id, reversed_op->instance_id);
}

TEST(ColorHarmonizerLegacyXmpTest, InsertsAtRegistryOwnedPointBeforeColorBalance)
{
    const auto imported =
        import_legacy_xmp({legacy_color_harmonizer_xmp({LegacyColorHarmonizerXmpOptions{}},
                                                       kLegacyColorBalanceHistoryEntry),
                           {"asset-1", "file:///fixture.raw", std::nullopt}});
    ASSERT_TRUE(imported) << imported.error().message;
    const auto &operations = imported.value().operations;
    const auto harmonizer =
        std::find_if(operations.begin(), operations.end(), [](const OperationInstance &operation)
                     { return operation.id == kColorHarmonizerOperationId; });
    const auto color_balance =
        std::find_if(operations.begin(), operations.end(), [](const OperationInstance &operation)
                     { return operation.id == kColorBalanceOperationId; });
    ASSERT_NE(harmonizer, operations.end());
    ASSERT_NE(color_balance, operations.end());
    EXPECT_EQ(std::count_if(operations.begin(), operations.end(),
                            [](const OperationInstance &operation)
                            { return operation.id == kColorHarmonizerOperationId; }),
              1);
    EXPECT_LT(harmonizer, color_balance);
}

TEST(ColorHarmonizerLegacyXmpTest, RejectsEveryNonEvidencedEnvelopeAndPayload)
{
    const auto expect_rejected = [](const LegacyColorHarmonizerXmpOptions &options,
                                    const ErrorCode code, const std::string_view reason)
    {
        const auto imported = import_legacy_xmp({legacy_color_harmonizer_xmp(options),
                                                 {"asset-1", "file:///fixture.raw", std::nullopt}});
        ASSERT_FALSE(imported);
        EXPECT_EQ(imported.error().code, code);
        EXPECT_EQ(imported.error().context.at("reason"), reason);
    };

    LegacyColorHarmonizerXmpOptions unsupported_version;
    unsupported_version.version = "2";
    expect_rejected(unsupported_version, ErrorCode::kUnsupported,
                    "unsupported_legacy_colorharmonizer_version");
    LegacyColorHarmonizerXmpOptions disabled;
    disabled.enabled = "0";
    expect_rejected(disabled, ErrorCode::kUnsupported,
                    "unsupported_legacy_colorharmonizer_enabled_state");
    LegacyColorHarmonizerXmpOptions named;
    named.multi_name = "second";
    expect_rejected(named, ErrorCode::kUnsupported,
                    "unsupported_legacy_colorharmonizer_multi_state");
    LegacyColorHarmonizerXmpOptions priority;
    priority.multi_priority = "1";
    expect_rejected(priority, ErrorCode::kUnsupported,
                    "unsupported_legacy_colorharmonizer_multi_state");
    LegacyColorHarmonizerXmpOptions hand_edited;
    hand_edited.multi_name_hand_edited = "1";
    expect_rejected(hand_edited, ErrorCode::kUnsupported,
                    "unsupported_legacy_colorharmonizer_multi_state");
    LegacyColorHarmonizerXmpOptions blend;
    blend.blend_version = "13";
    expect_rejected(blend, ErrorCode::kUnsupported, "unsupported_legacy_colorharmonizer_blend");
    LegacyColorHarmonizerXmpOptions custom_blend;
    custom_blend.blend_parameters = "gz13eJxjYGBgYAJiCQYYOOHEgAYY0QVwggZ7CB6pfNoAAExgGQY=";
    expect_rejected(custom_blend, ErrorCode::kUnsupported,
                    "unsupported_legacy_colorharmonizer_blend");
    LegacyColorHarmonizerXmpOptions mask;
    mask.extra_attributes = R"( darktable:mask_id="1")";
    expect_rejected(mask, ErrorCode::kUnsupported, "unsupported_legacy_colorharmonizer_mask");
    LegacyColorHarmonizerXmpOptions unknown;
    unknown.extra_attributes = R"( darktable:unproven="1")";
    expect_rejected(unknown, ErrorCode::kUnsupported,
                    "unsupported_legacy_colorharmonizer_attribute");
    LegacyColorHarmonizerXmpOptions missing_params;
    missing_params.parameters.reset();
    expect_rejected(missing_params, ErrorCode::kUnsupported, "unsupported_legacy_operation");
    LegacyColorHarmonizerXmpOptions short_payload;
    short_payload.parameters = "03000000";
    expect_rejected(short_payload, ErrorCode::kValidation,
                    "invalid_legacy_colorharmonizer_parameters");
    static const std::string long_parameters = std::string(kRecord12Parameters) + "00000000";
    LegacyColorHarmonizerXmpOptions long_payload;
    long_payload.parameters = long_parameters;
    expect_rejected(long_payload, ErrorCode::kValidation,
                    "invalid_legacy_colorharmonizer_parameters");
    LegacyColorHarmonizerXmpOptions nonfinite;
    nonfinite.parameters =
        "030000000000c07f000000000000003f0000803f000000000000803e0000003f0000403f040000000000803f"
        "0000803f0000803f0000803f00000000";
    expect_rejected(nonfinite, ErrorCode::kValidation, "invalid_legacy_colorharmonizer_parameters");
    LegacyColorHarmonizerXmpOptions invalid_rule;
    invalid_rule.parameters =
        "0a000000cdcccc3d000000000000003f0000803f000000000000803e0000003f0000403f040000000000803f"
        "0000803f0000803f0000803f00000000";
    expect_rejected(invalid_rule, ErrorCode::kValidation,
                    "invalid_legacy_colorharmonizer_parameters");
    LegacyColorHarmonizerXmpOptions invalid_nodes;
    invalid_nodes.parameters =
        "03000000cdcccc3d000000000000003f0000803f000000000000803e0000003f0000403f010000000000803f"
        "0000803f0000803f0000803f00000000";
    expect_rejected(invalid_nodes, ErrorCode::kValidation,
                    "invalid_legacy_colorharmonizer_parameters");
    LegacyColorHarmonizerXmpOptions positive_smoothing;
    positive_smoothing.parameters =
        "03000000cdcccc3d000000000000003f0000803f000000000000803e0000003f0000403f040000000000803f"
        "0000803f0000803f0000803f0000803f";
    expect_rejected(positive_smoothing, ErrorCode::kUnsupported,
                    "unsupported_legacy_colorharmonizer_unevidenced_smoothing");
    LegacyColorHarmonizerXmpOptions malformed_position;
    malformed_position.history_position = "12a";
    expect_rejected(malformed_position, ErrorCode::kValidation, "invalid_legacy_history_position");

    const auto duplicate = import_legacy_xmp(
        {legacy_color_harmonizer_xmp(std::vector<LegacyColorHarmonizerXmpOptions>{{}, {}}),
         {"asset-1", "file:///fixture.raw", std::nullopt}});
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code, ErrorCode::kConflict);
    EXPECT_EQ(duplicate.error().context.at("reason"), "duplicate_legacy_colorharmonizer_revision");

    LegacyColorHarmonizerXmpOptions winner = {};
    winner.history_position = "13";
    winner.parameters = kRecord13Parameters;
    LegacyColorHarmonizerXmpOptions stale_mask = {};
    stale_mask.history_position = "12";
    stale_mask.extra_attributes = R"( darktable:mask_id="9")";
    const auto stale = import_legacy_xmp({legacy_color_harmonizer_xmp({winner, stale_mask}),
                                          {"asset-1", "file:///fixture.raw", std::nullopt}});
    ASSERT_FALSE(stale);
    EXPECT_EQ(stale.error().context.at("reason"), "unsupported_legacy_colorharmonizer_mask");
}

TEST(ColorHarmonizerLegacyXmpTest,
     Frozen0176BytesMatchHardCodedRecordPayloadsWithoutImportingTheDocument)
{
    const auto path = std::filesystem::path(RAVO_REPOSITORY_ROOT) / "Ravo" / "tests" / "fixtures" / "frozen" /
                      "0176-color-harmonizer" / "color-harmonizer.xmp";
    std::ifstream stream(path);
    ASSERT_TRUE(stream) << path;
    const std::string document((std::istreambuf_iterator<char>(stream)),
                               std::istreambuf_iterator<char>());
    EXPECT_NE(document.find(std::string(kRecord12Parameters)), std::string::npos);
    EXPECT_NE(document.find(std::string(kRecord13Parameters)), std::string::npos);
    EXPECT_NE(document.find(std::string(kFrozenBlendV14)), std::string::npos);
    const auto imported =
        import_legacy_xmp({document, {"asset-1", "file:///fixture.raw", std::nullopt}});
    ASSERT_FALSE(imported);
    const auto reason = imported.error().context.find("reason");
    ASSERT_NE(reason, imported.error().context.end());
    EXPECT_NE(reason->second.find("unsupported_"), std::string::npos);
}

TEST(ColorHarmonizerLegacyXmpTest, ImportedRecord13MatchesDirectEngine)
{
    LegacyColorHarmonizerXmpOptions record13;
    record13.history_position = "13";
    record13.parameters = kRecord13Parameters;
    const auto imported = import_legacy_xmp(
        {legacy_color_harmonizer_xmp(record13), {"asset-1", "file:///fixture.raw", std::nullopt}});
    ASSERT_TRUE(imported) << imported.error().message;
    const auto *operation = find_operation(imported.value(), kColorHarmonizerOperationId);
    ASSERT_NE(operation, nullptr);
    WorkingImage input;
    input.width = 2U;
    input.height = 1U;
    input.rgb = {0.03F, 0.18F, 0.72F, 0.91F, 0.42F, 0.07F};
    input.color_profile.kind = ColorProfileKind::kIcc;
    input.color_profile.model = ColorModel::kRgb;
    input.color_profile.identifier = "linear_rec709";
    input.color_profile.has_matrix = true;
    input.color_profile.matrix_to_xyz_d50 = {0.4360747F, 0.3850649F, 0.1430804F,
                                             0.2225045F, 0.7168786F, 0.0606169F,
                                             0.0139322F, 0.0971045F, 0.7141733F};
    const auto imported_pixels = apply_color_harmonizer(input, *operation, CancellationToken{});
    const auto direct_pixels =
        apply_color_harmonizer(input, frozen_record13(), CancellationToken{});
    ASSERT_TRUE(imported_pixels) << imported_pixels.error().message;
    ASSERT_TRUE(direct_pixels) << direct_pixels.error().message;
    EXPECT_EQ(imported_pixels.value().rgb, direct_pixels.value().rgb);

    ColorHarmonizerParams positive = frozen_record13();
    positive.smoothing = 0.25;
    input.canonical_roi_scale = CanonicalRoiScale::from_scaled_dimensions(2U, 1U, 2U, 1U);
    const auto smoothed = apply_color_harmonizer(input, positive, CancellationToken{});
    ASSERT_TRUE(smoothed) << smoothed.error().message;
    EXPECT_NE(smoothed.value().rgb, direct_pixels.value().rgb);
}

TEST(ColorHarmonizerPresentationTest, RecipeNodeCountsMatchEnginePredefinedGeometry)
{
    const auto tables = harmony_geometry::build_harmony_hue_tables();
    for (std::size_t index = 0U; index < kColorHarmonizerPredefinedNodeCounts.size(); ++index)
    {
        ColorHarmonizerParams params;
        params.rule = static_cast<ColorHarmonizerRule>(index);
        params.num_custom_nodes = 2;
        const auto nodes = harmony_geometry::predefined_harmony_nodes(
            static_cast<harmony_geometry::StandardRule>(index), 0.1F, tables);
        ASSERT_TRUE(nodes) << static_cast<int>(index);
        EXPECT_EQ(static_cast<std::int64_t>(nodes.value().count),
                  kColorHarmonizerPredefinedNodeCounts[index]);
        EXPECT_EQ(color_harmonizer_active_node_count(params),
                  kColorHarmonizerPredefinedNodeCounts[index]);
    }
}

} // namespace
} // namespace ravo
