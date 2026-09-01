#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QColor>
#include <QColorSpace>
#include <QCoreApplication>
#include <QImage>
#include <QProcess>
#include <QXmlStreamReader>
#include <gtest/gtest.h>
#include <png.h>

#include "ravo/adapters/legacy_xmp.h"
#include "ravo/adapters/text_file.h"
#include "ravo/cli/application.h"
#include "ravo/domain/types.h"
#include "ravo/domain/uri.h"
#include "ravo/foundation/json.h"
#include "ravo/foundation/log.h"
#include "ravo/recipe/color_checker.h"
#include "ravo/recipe/color_contrast.h"
#include "ravo/recipe/color_correction.h"
#include "ravo/recipe/color_harmonizer.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/operation.h"
#include "ravo/recipe/profile_gamma.h"
#include "ravo/recipe/primaries.h"
#include "ravo/recipe/recipe.h"
#include "ravo/recipe/style.h"

#include "capture_metadata_test_support.h"
#include "cli_test_support.h"
#include "test_support.h"

namespace ravo
{
namespace
{

TEST_F(CliTest, LegacyXmpMapsRgbLevelsOntoCanonicalToneOp)
{
    constexpr std::string_view kIdentity =
        "0000000001000000000000000000003f0000803f000000000000003f0000803f000000000000003f0000803f";
    constexpr std::string_view kFixtureLinked =
        "0000000001000000a6a53e3c9ff9b53e17f21e3f121d813da63fe43e40823f3f70c56a3bfba0d73e2f2c2c3f";
    constexpr std::string_view kFixtureIndependent =
        "0100000001000000a6a53e3c9ff9b53e17f21e3f121d813da63fe43e40823f3f70c56a3bfba0d73e2f2c2c3f";

    auto identity = import_legacy_xmp(
        {legacy_rgblevels_xmp(kIdentity), {"asset-1", "file:///fixture.raw", std::nullopt}});
    ASSERT_TRUE(identity) << identity.error().message;
    ASSERT_EQ(identity.value().operations.size(), 2U);

    auto linked = import_legacy_xmp(
        {legacy_rgblevels_xmp(kFixtureLinked), {"asset-1", "file:///fixture.raw", std::nullopt}});
    ASSERT_TRUE(linked) << linked.error().message;
    ASSERT_EQ(linked.value().operations.size(), 3U);
    EXPECT_EQ(linked.value().operations[1].id, "ravo.color.rgblevels");
    EXPECT_EQ(std::get<std::string>(linked.value().operations[1].parameters.at("mode").value),
              kRgbLevelsModeLinked);
    EXPECT_NEAR(std::get<double>(linked.value().operations[1].parameters.at("black").value),
                0.011636173352599144, 1e-7);
    EXPECT_NEAR(std::get<double>(linked.value().operations[1].parameters.at("white").value),
                0.6208814978599548, 1e-7);

    auto independent = import_legacy_xmp({legacy_rgblevels_xmp(kFixtureIndependent),
                                          {"asset-1", "file:///fixture.raw", std::nullopt}});
    ASSERT_TRUE(independent) << independent.error().message;
    EXPECT_EQ(independent.value().operations[1].id, "ravo.color.rgblevels");
    EXPECT_EQ(std::get<std::string>(independent.value().operations[1].parameters.at("mode").value),
              kRgbLevelsModeIndependent);

    std::string last_write = R"(<?xml version="1.0"?>
<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"
         xmlns:darktable="http://darktable.sf.net/">
  <rdf:Description darktable:xmp_version="6"><darktable:history><rdf:Seq>)";
    last_write +=
        R"(<rdf:li darktable:num="8" darktable:operation="rgblevels" darktable:modversion="1" darktable:enabled="1" darktable:params=")";
    last_write += kFixtureLinked;
    last_write +=
        R"(" darktable:multi_name="" darktable:multi_priority="0" darktable:blendop_version="10" darktable:blendop_params="gz13eJxjYGBgYAZiCQYYOOHEgAYY0QVwggZ7CB6pfNoAAE4AGQc="/>)";
    last_write +=
        R"(<rdf:li darktable:num="9" darktable:operation="rgblevels" darktable:modversion="1" darktable:enabled="1" darktable:params=")";
    last_write += kFixtureIndependent;
    last_write +=
        R"(" darktable:multi_name="" darktable:multi_priority="0" darktable:blendop_version="10" darktable:blendop_params="gz13eJxjYGBgYAZiCQYYOOHEgAYY0QVwggZ7CB6pfNoAAE4AGQc="/>)";
    last_write += R"(</rdf:Seq></darktable:history></rdf:Description></rdf:RDF>)";
    auto superseded =
        import_legacy_xmp({last_write, {"asset-1", "file:///fixture.raw", std::nullopt}});
    ASSERT_TRUE(superseded) << superseded.error().message;
    ASSERT_EQ(superseded.value().operations.size(), 3U);
    EXPECT_EQ(std::get<std::string>(superseded.value().operations[1].parameters.at("mode").value),
              kRgbLevelsModeIndependent);

    auto default_blend =
        import_legacy_xmp({legacy_rgblevels_xmp(kFixtureLinked, kLegacyGammaBlendV9),
                           {"asset-1", "file:///fixture.raw", std::nullopt}});
    ASSERT_TRUE(default_blend) << default_blend.error().message;
    EXPECT_EQ(default_blend.value().operations[1].id, "ravo.color.rgblevels");

    const auto fixture_linked_path = std::filesystem::path(RAVO_REPOSITORY_ROOT) / "Ravo" /
                                     "tests" / "fixtures" / "frozen" / "0054-rgblevels-linked" /
                                     "rgblevels-linked.xmp";
    const auto fixture_linked = read_utf8_text_file(fixture_linked_path.generic_string());
    ASSERT_TRUE(fixture_linked) << fixture_linked.error().message;
    EXPECT_NE(fixture_linked.value().find(kFixtureLinked), std::string::npos);

    const auto fixture_indep_path = std::filesystem::path(RAVO_REPOSITORY_ROOT) / "Ravo" / "tests" /
                                    "fixtures" / "frozen" / "0055-rgblevels-indep" /
                                    "rgblevels-indep.xmp";
    const auto fixture_indep = read_utf8_text_file(fixture_indep_path.generic_string());
    ASSERT_TRUE(fixture_indep) << fixture_indep.error().message;
    EXPECT_NE(fixture_indep.value().find(kFixtureIndependent), std::string::npos);
    EXPECT_NE(fixture_indep.value().find(kFixtureLinked), std::string::npos);
}

[[nodiscard]] std::string legacy_rgbcurve_xmp(const std::string_view parameters)
{
    std::string document = R"(<?xml version="1.0"?>
<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"
         xmlns:darktable="http://darktable.sf.net/">
  <rdf:Description darktable:xmp_version="6"><darktable:history><rdf:Seq>
<rdf:li darktable:num="8" darktable:operation="rgbcurve" darktable:modversion="1" darktable:enabled="1" darktable:params=")";
    document += parameters;
    document +=
        R"(" darktable:multi_name="" darktable:multi_priority="0" darktable:blendop_version="10" darktable:blendop_params="gz13eJxjYGBgYAZiCQYYOOHEgAYY0QVwggZ7CB6pfNoAAE4AGQc="/>
</rdf:Seq></darktable:history></rdf:Description>
</rdf:RDF>)";
    return document;
}

TEST_F(CliTest, LegacyXmpMapsRgbCurveIncludingMiddleGrey)
{
    constexpr std::string_view kIdentity = "gz04eNpjYICBBnsIHqxg1H3kACYcGAYYgRgABhAEiA==";
    constexpr std::string_view kLifted = "gz04eNpjYIADewYGByBugOLBBgaruwa3+5iBmAkLRgYAb2EFRg==";
    constexpr std::string_view kFixture0060 =
        "gz04eJzjuZFqywAEbjMZ7MKmc9g5+1nbzfs+xe7O2167nXWn7aS7L9rtdRWwjwgSt9+4Vt1+ibmPfeY5Z/"
        "v7vxPtT6pn2DMwNKBh2gDdL6l2qkZedirTeeyFfz6xE7Azs59T42wvJpVOU3uJBQvS5O0Efh2yCw1hsG+6LGmfLWdif74nlebh"
        "QghwAjErFDMhYUYkDABTiDBy";

    auto identity = import_legacy_xmp(
        {legacy_rgbcurve_xmp(kIdentity), {"asset-1", "file:///fixture.raw", std::nullopt}});
    ASSERT_TRUE(identity) << identity.error().message;
    ASSERT_EQ(identity.value().operations.size(), 2U);

    auto lifted = import_legacy_xmp(
        {legacy_rgbcurve_xmp(kLifted), {"asset-1", "file:///fixture.raw", std::nullopt}});
    ASSERT_TRUE(lifted) << lifted.error().message;
    ASSERT_EQ(lifted.value().operations.size(), 3U);
    EXPECT_EQ(lifted.value().operations[1].id, "ravo.color.rgbcurve");
    EXPECT_EQ(std::get<std::string>(lifted.value().operations[1].parameters.at("mode").value),
              kRgbLevelsModeLinked);

    auto fixture = import_legacy_xmp(
        {legacy_rgbcurve_xmp(kFixture0060), {"asset-1", "file:///fixture.raw", std::nullopt}});
    ASSERT_TRUE(fixture) << fixture.error().message;
    ASSERT_EQ(fixture.value().operations.size(), 3U);
    EXPECT_EQ(fixture.value().operations[1].id, "ravo.color.rgbcurve");
    EXPECT_EQ(std::get<std::string>(fixture.value().operations[1].parameters.at("mode").value),
              kRgbLevelsModeIndependent);
    EXPECT_TRUE(std::get<bool>(
        fixture.value().operations[1].parameters.at("compensate_middle_grey").value));
    const auto &red_points = std::get<ParameterValue::Array>(
        fixture.value().operations[1].parameters.at("points").value);
    EXPECT_EQ(red_points.size(), 9U);
    auto developed = develop_from_recipe(fixture.value());
    ASSERT_TRUE(developed) << developed.error().message;
    EXPECT_TRUE(developed.value().rgb_curve.compensate_middle_grey);
    ASSERT_EQ(developed.value().rgb_curve.channels[0].size(), 9U);
    EXPECT_NEAR(developed.value().rgb_curve.channels[0].front().x, 0.056114, 1e-5);

    const auto fixture_path = std::filesystem::path(RAVO_REPOSITORY_ROOT) / "Ravo" / "tests" /
                              "fixtures" / "frozen" / "0060-rgbcurve-indep" / "rgbcurve-indep.xmp";
    const auto fixture_text = read_utf8_text_file(fixture_path.generic_string());
    ASSERT_TRUE(fixture_text) << fixture_text.error().message;
    EXPECT_NE(fixture_text.value().find("gz04eJzjuZFqywAEbjMZ7MKmc9g5"), std::string::npos);
}

[[nodiscard]] std::string legacy_rawdenoise_xmp(const std::string_view parameters)
{
    std::string document = R"(<?xml version="1.0"?>
<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"
         xmlns:darktable="http://darktable.sf.net/">
  <rdf:Description darktable:xmp_version="6"><darktable:history><rdf:Seq>
<rdf:li darktable:num="8" darktable:operation="rawdenoise" darktable:modversion="2" darktable:enabled="1" darktable:params=")";
    document += parameters;
    document +=
        R"(" darktable:multi_name="" darktable:multi_priority="0" darktable:blendop_version="10" darktable:blendop_params="gz13eJxjYGBgYARiCQYYOOHEgAYY0QVwggZ7CB6pfNoAAErAGQU="/>
</rdf:Seq></darktable:history></rdf:Description>
</rdf:RDF>)";
    return document;
}

TEST_F(CliTest, LegacyXmpMapsRawDenoiseV2)
{
    constexpr std::string_view kFixture0049 =
        "gz02eJw7e8bHlgEMGuyAhD0DgwMQN9hTIvb3YKW9yPL/ds87btl812K0lz1aAFJnz+zNYB+5qtFew4LBHqIegpP4Ge3nNdfZZ/"
        "Chire2Mdp3LU21BwBmCx/+";
    auto imported = import_legacy_xmp(
        {legacy_rawdenoise_xmp(kFixture0049), {"asset-1", "file:///fixture.raw", std::nullopt}});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_EQ(imported.value().operations.size(), 3U);
    EXPECT_EQ(imported.value().operations[1].id, "ravo.raw.denoise");
    EXPECT_NEAR(std::get<double>(imported.value().operations[1].parameters.at("threshold").value),
                0.05, 1e-6);
    EXPECT_NEAR(std::get<double>(imported.value().operations[1].parameters.at("y_all0").value),
                0.9756162762641907, 1e-6);

    const auto fixture_path = std::filesystem::path(RAVO_REPOSITORY_ROOT) / "Ravo" / "tests" /
                              "fixtures" / "frozen" / "0049-rawdenoise" / "rawdenoise.xmp";
    const auto fixture_text = read_utf8_text_file(fixture_path.generic_string());
    ASSERT_TRUE(fixture_text) << fixture_text.error().message;
    EXPECT_NE(fixture_text.value().find("gz02eJw7e8bHlgEMGuyAhD0DgwMQ"), std::string::npos);
}

TEST_F(CliTest, LegacyXmpGammaFixtureCensusPinsEveryFrozenMandatoryBoundary)
{
    const auto fixture_root =
        std::filesystem::path(RAVO_REPOSITORY_ROOT) / "Ravo" / "tests" / "fixtures" / "frozen";
    std::vector<std::filesystem::path> xmp_paths;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(fixture_root))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".xmp")
        {
            xmp_paths.push_back(entry.path());
        }
    }
    std::sort(xmp_paths.begin(), xmp_paths.end());
    ASSERT_EQ(xmp_paths.size(), 158U);

    std::array<std::size_t, kFrozenLegacyGammaBlendTuples.size()> tuple_counts{};
    std::size_t gamma_count = 0U;
    std::size_t missing_hand_edited_count = 0U;
    std::size_t zero_hand_edited_count = 0U;
    for (const auto &path : xmp_paths)
    {
        SCOPED_TRACE(path.generic_string());
        const auto path_u8 = path.generic_u8string();
        const std::string path_utf8(path_u8.begin(), path_u8.end());
        const auto content = read_utf8_text_file(path_utf8);
        ASSERT_TRUE(content) << content.error().message;
        const QByteArray bytes(content.value().data(),
                               static_cast<qsizetype>(content.value().size()));
        QXmlStreamReader reader(bytes);
        std::size_t file_gamma_count = 0U;
        while (!reader.atEnd())
        {
            reader.readNext();
            if (!reader.isStartElement() || reader.name() != u"li" ||
                xml_attribute_value(reader.attributes(), u"operation") != "gamma")
            {
                continue;
            }

            ++file_gamma_count;
            ++gamma_count;
            const auto version = xml_attribute_value(reader.attributes(), u"modversion");
            const auto enabled = xml_attribute_value(reader.attributes(), u"enabled");
            const auto parameters = xml_attribute_value(reader.attributes(), u"params");
            const auto multi_priority = xml_attribute_value(reader.attributes(), u"multi_priority");
            const auto multi_name = xml_attribute_value(reader.attributes(), u"multi_name");
            const auto multi_name_hand_edited =
                xml_attribute_value(reader.attributes(), u"multi_name_hand_edited");
            const auto blend_version = xml_attribute_value(reader.attributes(), u"blendop_version");
            const auto blend_parameters =
                xml_attribute_value(reader.attributes(), u"blendop_params");
            const auto history_position = xml_attribute_value(reader.attributes(), u"num");

            ASSERT_TRUE(version);
            EXPECT_EQ(*version, "1");
            ASSERT_TRUE(enabled);
            EXPECT_EQ(*enabled, "1");
            ASSERT_TRUE(parameters);
            EXPECT_EQ(*parameters, "0000000000000000");
            ASSERT_TRUE(multi_priority);
            EXPECT_EQ(*multi_priority, "0");
            ASSERT_TRUE(multi_name);
            EXPECT_TRUE(multi_name->empty());
            ASSERT_TRUE(history_position);
            EXPECT_FALSE(history_position->empty());
            EXPECT_TRUE(std::all_of(history_position->begin(), history_position->end(),
                                    [](const char value) { return value >= '0' && value <= '9'; }));
            if (multi_name_hand_edited)
            {
                EXPECT_EQ(*multi_name_hand_edited, "0");
                ++zero_hand_edited_count;
            }
            else
            {
                ++missing_hand_edited_count;
            }

            for (const auto &attribute : reader.attributes())
            {
                const auto name = attribute.name();
                EXPECT_FALSE(name.contains(u"mask"));
                EXPECT_TRUE(name == u"num" || name == u"operation" || name == u"enabled" ||
                            name == u"modversion" || name == u"params" || name == u"multi_name" ||
                            name == u"multi_priority" || name == u"multi_name_hand_edited" ||
                            name == u"blendop_version" || name == u"blendop_params")
                    << name.toString().toStdString();
            }

            ASSERT_TRUE(blend_version);
            ASSERT_TRUE(blend_parameters);
            const auto tuple = std::find_if(kFrozenLegacyGammaBlendTuples.begin(),
                                            kFrozenLegacyGammaBlendTuples.end(),
                                            [&](const FrozenLegacyGammaBlendTuple &candidate)
                                            {
                                                return candidate.version == *blend_version &&
                                                       candidate.parameters == *blend_parameters;
                                            });
            ASSERT_NE(tuple, kFrozenLegacyGammaBlendTuples.end());
            ++tuple_counts[static_cast<std::size_t>(
                std::distance(kFrozenLegacyGammaBlendTuples.begin(), tuple))];
        }
        ASSERT_FALSE(reader.hasError()) << reader.errorString().toStdString();
        EXPECT_EQ(file_gamma_count, 1U);
    }

    EXPECT_EQ(gamma_count, 158U);
    EXPECT_EQ(missing_hand_edited_count, 99U);
    EXPECT_EQ(zero_hand_edited_count, 59U);
    for (std::size_t index = 0; index < kFrozenLegacyGammaBlendTuples.size(); ++index)
    {
        SCOPED_TRACE(std::string(kFrozenLegacyGammaBlendTuples[index].version));
        EXPECT_EQ(tuple_counts[index], kFrozenLegacyGammaBlendTuples[index].fixture_count);
    }
}

TEST_F(CliTest, LegacyXmpGammaAbsorbsOnlyTheTwelveExactFrozenBlendTuples)
{
    for (std::size_t index = 0; index < kFrozenLegacyGammaBlendTuples.size(); ++index)
    {
        const auto &tuple = kFrozenLegacyGammaBlendTuples[index];
        SCOPED_TRACE(std::string(tuple.version) + ":" + std::to_string(index));
        LegacyGammaXmpOptions options;
        options.blend_version = tuple.version;
        options.blend_parameters = tuple.parameters;
        if ((index % 2U) == 0U)
        {
            options.multi_name_hand_edited = "0";
        }
        const auto imported = import_legacy_xmp(
            {legacy_gamma_xmp(options), {"asset-1", "file:///fixture.raw", std::nullopt}});

        ASSERT_TRUE(imported) << imported.error().message;
        ASSERT_EQ(imported.value().operations.size(), 2U);
        EXPECT_EQ(imported.value().operations.front().id, "ravo.color.input");
        EXPECT_EQ(imported.value().operations.back().id, "ravo.color.output");
        EXPECT_TRUE(std::none_of(imported.value().operations.begin(),
                                 imported.value().operations.end(),
                                 [](const OperationInstance &operation)
                                 {
                                     return operation.id == "ravo.core.gamma" ||
                                            operation.id == kProfileGammaOperationId;
                                 }));
        EXPECT_TRUE(imported.value().masks.empty());
    }
}

TEST_F(CliTest, LegacyXmpGammaRejectsEveryUnprovenHistoryState)
{
    const auto expect_rejected = [](const LegacyGammaXmpOptions &options,
                                    const ErrorCode expected_code,
                                    const std::string_view expected_reason)
    {
        const auto imported = import_legacy_xmp(
            {legacy_gamma_xmp(options), {"asset-1", "file:///fixture.raw", std::nullopt}});
        ASSERT_FALSE(imported);
        EXPECT_EQ(imported.error().code, expected_code);
        EXPECT_EQ(imported.error().context.at("legacy_operation"), "gamma");
        EXPECT_EQ(imported.error().context.at("reason"), expected_reason);
    };

    LegacyGammaXmpOptions options;
    options.version = "2";
    expect_rejected(options, ErrorCode::kUnsupported, "unsupported_legacy_gamma_version");

    options = {};
    options.version.reset();
    expect_rejected(options, ErrorCode::kUnsupported, "unsupported_legacy_gamma_version");

    options = {};
    options.enabled = "0";
    expect_rejected(options, ErrorCode::kUnsupported, "unsupported_legacy_gamma_disabled");

    options = {};
    options.enabled.reset();
    expect_rejected(options, ErrorCode::kUnsupported, "unsupported_legacy_gamma_disabled");

    options = {};
    options.parameters = "0000000000000001";
    expect_rejected(options, ErrorCode::kUnsupported, "unsupported_legacy_gamma_parameters");

    options = {};
    options.parameters.reset();
    expect_rejected(options, ErrorCode::kUnsupported, "unsupported_legacy_gamma_parameters");

    options = {};
    options.instances = 2U;
    expect_rejected(options, ErrorCode::kConflict, "duplicate_legacy_gamma");

    options = {};
    options.blend_version = "15";
    expect_rejected(options, ErrorCode::kUnsupported, "unsupported_legacy_gamma_blend");

    options = {};
    options.blend_version.reset();
    expect_rejected(options, ErrorCode::kUnsupported, "unsupported_legacy_gamma_blend");

    options = {};
    options.blend_parameters = kLegacyGammaBlendGz14GuideOne;
    expect_rejected(options, ErrorCode::kUnsupported, "unsupported_legacy_gamma_blend");

    options = {};
    options.blend_parameters.reset();
    expect_rejected(options, ErrorCode::kUnsupported, "unsupported_legacy_gamma_blend");

    options = {};
    options.extra_attributes = R"( darktable:mask_id="42")";
    expect_rejected(options, ErrorCode::kUnsupported, "unsupported_legacy_gamma_mask");

    options = {};
    options.extra_attributes = R"( darktable:blendop_mask_id="42")";
    expect_rejected(options, ErrorCode::kUnsupported, "unsupported_legacy_gamma_mask");

    options = {};
    options.multi_priority = "1";
    expect_rejected(options, ErrorCode::kUnsupported, "unsupported_legacy_gamma_multi_state");

    options = {};
    options.multi_priority.reset();
    expect_rejected(options, ErrorCode::kUnsupported, "unsupported_legacy_gamma_multi_state");

    options = {};
    options.multi_name = "edited";
    expect_rejected(options, ErrorCode::kUnsupported, "unsupported_legacy_gamma_multi_state");

    options = {};
    options.multi_name.reset();
    expect_rejected(options, ErrorCode::kUnsupported, "unsupported_legacy_gamma_multi_state");

    options = {};
    options.multi_name_hand_edited = "1";
    expect_rejected(options, ErrorCode::kUnsupported, "unsupported_legacy_gamma_multi_state");

    options = {};
    options.extra_attributes = R"( darktable:unproven_state="1")";
    expect_rejected(options, ErrorCode::kUnsupported, "unsupported_legacy_gamma_attribute");
}

TEST_F(CliTest, LegacyXmpDecodesAndImportsTheProvenRgbPrimariesSingleton)
{
    const auto decoded = decode_legacy_primaries_v1_parameters(kLegacyPrimariesPayload);
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_NEAR(decoded.value().achromatic_tint_hue, 0.5794494152, 1e-7);
    EXPECT_NEAR(decoded.value().achromatic_tint_purity, 0.1340000033, 1e-7);
    EXPECT_NEAR(decoded.value().red_hue, 0.1658063233, 1e-7);
    EXPECT_NEAR(decoded.value().red_purity, 0.8429999948, 1e-7);
    EXPECT_NEAR(decoded.value().green_hue, -0.1431170106, 1e-7);
    EXPECT_NEAR(decoded.value().green_purity, 1.2350000143, 1e-7);
    EXPECT_NEAR(decoded.value().blue_hue, 0.2286381423, 1e-7);
    EXPECT_NEAR(decoded.value().blue_purity, 0.8199999928, 1e-7);
    auto canonical = primaries_from_parameters(primaries_to_parameters(decoded.value()));
    ASSERT_TRUE(canonical) << canonical.error().message;
    EXPECT_EQ(canonical.value(), decoded.value());

    const auto short_payload = decode_legacy_primaries_v1_parameters("00000000");
    ASSERT_FALSE(short_payload);
    EXPECT_EQ(short_payload.error().code, ErrorCode::kValidation);
    const auto non_finite_payload = decode_legacy_primaries_v1_parameters(
        "0000c07f00000000000000000000803f000000000000803f000000000000803f");
    ASSERT_FALSE(non_finite_payload);
    EXPECT_EQ(non_finite_payload.error().code, ErrorCode::kValidation);

    const auto document = legacy_primaries_xmp();
    const LegacyXmpImportRequest request{document,
                                         {"asset-1", "file:///fixture.raw", std::nullopt}};
    const auto imported = import_legacy_xmp(request);
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_EQ(imported.value().operations.size(), 3U);
    EXPECT_EQ(imported.value().operations.front().id, "ravo.color.input");
    const auto &primaries = imported.value().operations[1];
    EXPECT_EQ(primaries.id, kPrimariesOperationId);
    EXPECT_EQ(primaries.instance_id, "legacy-primaries-0");
    EXPECT_TRUE(primaries.enabled);
    auto imported_params = primaries_from_parameters(primaries.parameters);
    ASSERT_TRUE(imported_params) << imported_params.error().message;
    EXPECT_EQ(imported_params.value(), decoded.value());
    EXPECT_EQ(imported.value().operations.back().id, "ravo.color.output");
    auto registry = make_phase1_registry();
    ASSERT_TRUE(registry) << registry.error().message;
    ASSERT_TRUE(validate_recipe(imported.value(), registry.value()));
}

TEST_F(CliTest, LegacyXmpRejectsUnprovenRgbPrimariesHistoryStates)
{
    const auto import_document = [](std::string document)
    { return import_legacy_xmp({document, {"asset-1", "file:///fixture.raw", std::nullopt}}); };

    auto disabled = import_document(legacy_primaries_xmp("1", "0"));
    ASSERT_FALSE(disabled);
    EXPECT_EQ(disabled.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(disabled.error().context.at("reason"), "unsupported_legacy_primaries_disabled");

    auto version = import_document(legacy_primaries_xmp("2"));
    ASSERT_FALSE(version);
    EXPECT_EQ(version.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(version.error().context.at("reason"), "unsupported_legacy_primaries_version");

    auto blend = import_document(
        legacy_primaries_xmp("1", "1", kLegacyPrimariesPayload, "unsupported-blend"));
    ASSERT_FALSE(blend);
    EXPECT_EQ(blend.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(blend.error().context.at("reason"), "unsupported_legacy_blend");

    auto mask = import_document(legacy_primaries_xmp("1", "1", kLegacyPrimariesPayload,
                                                     kLegacyPrimariesDefaultBlend, 1U, "13",
                                                     R"( darktable:mask_id="mask-1")"));
    ASSERT_FALSE(mask);
    EXPECT_EQ(mask.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(mask.error().context.at("reason"), "unsupported_legacy_mask");

    auto duplicate = import_document(
        legacy_primaries_xmp("1", "1", kLegacyPrimariesPayload, kLegacyPrimariesDefaultBlend, 2U));
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code, ErrorCode::kConflict);
    EXPECT_EQ(duplicate.error().context.at("reason"), "duplicate_legacy_primaries");
}

TEST_F(CliTest, LegacyXmpPrimariesImportCommandWritesCanonicalRecipe)
{
    const auto directory = std::filesystem::temp_directory_path();
    const auto xmp_path = directory / "ravo-primaries-v1.xmp";
    const auto recipe_path = directory / "ravo-primaries-v1.recipe.json";
    std::error_code ignored;
    std::filesystem::remove(xmp_path, ignored);
    std::filesystem::remove(recipe_path, ignored);
    {
        std::ofstream output(xmp_path, std::ios::binary);
        ASSERT_TRUE(output);
        output << legacy_primaries_xmp();
    }
    const auto xmp_u8 = xmp_path.generic_u8string();
    const std::string xmp_argument(xmp_u8.begin(), xmp_u8.end());
    const auto recipe_u8 = recipe_path.generic_u8string();
    const std::string recipe_argument(recipe_u8.begin(), recipe_u8.end());
    std::ostringstream stdout_stream;
    std::ostringstream stderr_stream;
    const CliApplication application(engine, stdout_stream, stderr_stream);
    const std::vector<std::string_view> arguments{
        "recipe",  "import-xmp",          xmp_argument, "--asset-id",    "asset-1",
        "--input", "file:///fixture.raw", "--output",   recipe_argument, "--json"};

    const auto exit_code = application.run(std::span{arguments});
    const auto recipe = read_utf8_text_file(recipe_argument);
    std::filesystem::remove(xmp_path, ignored);
    std::filesystem::remove(recipe_path, ignored);

    EXPECT_EQ(exit_code, 0);
    ASSERT_TRUE(recipe) << recipe.error().message;
    const auto parsed = parse_recipe_json(recipe.value());
    ASSERT_TRUE(parsed) << parsed.error().message;
    ASSERT_EQ(parsed.value().operations.size(), 3U);
    EXPECT_EQ(parsed.value().operations[1].id, kPrimariesOperationId);
    EXPECT_EQ(parsed.value().operations.back().id, "ravo.color.output");
    EXPECT_TRUE(stderr_stream.str().empty());
}

TEST_F(CliTest, LegacyXmpOperationsRemainExplicitlyUnsupported)
{
    constexpr std::string_view xmp = R"(<?xml version="1.0"?>
<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"
         xmlns:darktable="http://darktable.sf.net/">
  <rdf:Description><darktable:history><rdf:Seq><rdf:li darktable:operation="exposure"/></rdf:Seq></darktable:history></rdf:Description>
</rdf:RDF>)";
    const LegacyXmpImportRequest request{xmp, {"asset-1", "file:///fixture.raw", std::nullopt}};

    const auto imported = import_legacy_xmp(request);

    ASSERT_FALSE(imported);
    EXPECT_EQ(imported.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(imported.error().context.at("legacy_operation"), "exposure");
}

TEST_F(CliTest, LegacyXmpProfileGammaRejectsMissingFrozenPayloadEvidence)
{
    constexpr std::string_view xmp = R"(<?xml version="1.0"?>
<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"
         xmlns:darktable="http://darktable.sf.net/">
  <rdf:Description darktable:xmp_version="6"><darktable:history><rdf:Seq>
    <rdf:li darktable:operation="profile_gamma" darktable:modversion="2" darktable:enabled="1"/>
  </rdf:Seq></darktable:history></rdf:Description>
</rdf:RDF>)";
    const LegacyXmpImportRequest request{xmp, {"asset-1", "file:///fixture.raw", std::nullopt}};

    const auto imported = import_legacy_xmp(request);

    ASSERT_FALSE(imported);
    EXPECT_EQ(imported.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(imported.error().context.at("legacy_operation"), "profile_gamma");
    EXPECT_EQ(imported.error().context.at("reason"), "unsupported_legacy_profile_gamma_no_fixture");
}

TEST_F(CliTest, LegacyXmpMapsStrictExposureV5V6AndV7Payloads)
{
    struct Case
    {
        std::string_view version;
        std::string_view parameters;
        bool enabled;
        bool exposure_bias;
        bool highlight_preservation;
    };
    const std::array cases{
        Case{"5", kLegacyExposureV5ManualOne, true, false, false},
        Case{"6", kLegacyExposureV6ManualOneBias, true, true, false},
        Case{"7", kLegacyExposureV7ManualOneBothCompensations, false, true, true},
    };
    for (const auto &test_case : cases)
    {
        LegacyExposureXmpOptions options;
        options.version = test_case.version;
        options.parameters = test_case.parameters;
        options.enabled = test_case.enabled ? "1" : "0";
        const auto xmp = legacy_exposure_xmp(options);
        const LegacyXmpImportRequest request{xmp, {"asset-1", "file:///fixture.raw", std::nullopt}};

        const auto imported = import_legacy_xmp(request);

        ASSERT_TRUE(imported) << test_case.version << ": " << imported.error().message;
        ASSERT_EQ(imported.value().operations.size(), 3U);
        EXPECT_EQ(imported.value().operations.front().id, "ravo.color.input");
        const auto &operation = imported.value().operations[1];
        EXPECT_EQ(operation.id, kExposureOperationId);
        EXPECT_EQ(operation.schema_version, kExposureOperationSchemaVersion);
        EXPECT_EQ(operation.instance_id, "legacy-exposure-8");
        EXPECT_EQ(operation.enabled, test_case.enabled);
        auto params = exposure_from_parameters(operation.parameters);
        ASSERT_TRUE(params) << params.error().message;
        EXPECT_EQ(params.value().mode, kExposureModeManual);
        EXPECT_DOUBLE_EQ(params.value().black, 0.0);
        EXPECT_DOUBLE_EQ(params.value().exposure_ev, 1.0);
        EXPECT_DOUBLE_EQ(params.value().deflicker_percentile, 50.0);
        EXPECT_DOUBLE_EQ(params.value().deflicker_target_ev, -4.0);
        EXPECT_EQ(params.value().compensate_exposure_bias, test_case.exposure_bias);
        EXPECT_EQ(params.value().compensate_highlight_preservation,
                  test_case.highlight_preservation);
    }
}

} // namespace
} // namespace ravo
