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
#include "engine_harmony_test_support.h"
#include "test_support.h"

namespace ravo
{
namespace
{
using namespace engine_harmony_test_support;

TEST(HarmonyGeometryTest, ValidatedLookupsCoverCircularSeamsAndRejectInvalidHues)
{
    const auto oracle = frozen_harmony_tables();
    const auto tables = harmony_geometry::build_harmony_hue_tables();
    const std::array hues{0.0F, 1.0F / 720.0F, 719.5F / 720.0F, std::nextafter(1.0F, 0.0F), 1.0F};
    for (const float hue : hues)
    {
        const auto forward = harmony_geometry::ucs_to_ryb_hue(tables, hue);
        ASSERT_TRUE(forward.has_value());
        EXPECT_EQ(std::bit_cast<std::uint32_t>(forward.value()),
                  std::bit_cast<std::uint32_t>(frozen_harmony_lookup(oracle.ucs_to_ryb, hue)));
        const auto inverse = harmony_geometry::ryb_to_ucs_hue(tables, hue);
        ASSERT_TRUE(inverse.has_value());
        EXPECT_EQ(std::bit_cast<std::uint32_t>(inverse.value()),
                  std::bit_cast<std::uint32_t>(frozen_harmony_lookup(oracle.ryb_to_ucs, hue)));
    }

    FrozenHarmonyHueTable seam{};
    seam.fill(0.25F);
    seam[719] = 0.99F;
    seam[0] = 0.01F;
    const harmony_geometry::HarmonyHueTables seam_tables{seam, seam};
    const auto seam_result = harmony_geometry::ucs_to_ryb_hue(seam_tables, 719.5F / 720.0F);
    ASSERT_TRUE(seam_result.has_value());
    const float seam_oracle = frozen_harmony_lookup(seam, 719.5F / 720.0F);
    EXPECT_EQ(std::bit_cast<std::uint32_t>(seam_oracle), 0x31a00000U);
    EXPECT_EQ(std::bit_cast<std::uint32_t>(seam_result.value()),
              std::bit_cast<std::uint32_t>(seam_oracle));

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();
    for (const float invalid : {-std::numeric_limits<float>::min(), std::nextafter(1.0F, infinity),
                                nan, infinity, -infinity})
    {
        const auto forward = harmony_geometry::ucs_to_ryb_hue(tables, invalid);
        ASSERT_FALSE(forward.has_value());
        EXPECT_EQ(forward.error().code, ErrorCode::kValidation);
        EXPECT_EQ(forward.error().message, "invalid_harmony_hue");
        const auto inverse = harmony_geometry::ryb_to_ucs_hue(tables, invalid);
        ASSERT_FALSE(inverse.has_value());
        EXPECT_EQ(inverse.error().code, ErrorCode::kValidation);
        EXPECT_EQ(inverse.error().message, "invalid_harmony_hue");
    }
}

TEST(HarmonyGeometryTest, InverseBuilderUsesCircularDistanceAndStrictFirstTie)
{
    FrozenHarmonyHueTable tied{};
    tied.fill(0.75F);
    tied[11] = 0.125F;
    tied[19] = 0.375F;
    const auto tied_inverse = harmony_geometry::build_ryb_to_ucs_table(tied);
    EXPECT_EQ(std::bit_cast<std::uint32_t>(tied_inverse[180]),
              std::bit_cast<std::uint32_t>(11.0F / 720.0F));

    FrozenHarmonyHueTable circular{};
    circular.fill(0.5F);
    circular[3] = 0.984375F;
    circular[4] = 0.125F;
    const auto circular_inverse = harmony_geometry::build_ryb_to_ucs_table(circular);
    EXPECT_EQ(std::bit_cast<std::uint32_t>(circular_inverse[0]),
              std::bit_cast<std::uint32_t>(3.0F / 720.0F));

    const auto oracle = frozen_harmony_inverse_table(tied);
    EXPECT_EQ(tied_inverse, oracle);
}

TEST(HarmonyGeometryTest, PredefinedRulesMatchSectorGeometryWrapAndDegreeRounding)
{
    constexpr std::array<std::size_t, 9> counts{1U, 3U, 4U, 2U, 3U, 2U, 3U, 4U, 4U};
    const auto oracle_tables = frozen_harmony_tables();
    const auto tables = harmony_geometry::build_harmony_hue_tables();
    for (std::size_t rule_index = 0U; rule_index < counts.size(); ++rule_index)
    {
        const auto rule = static_cast<harmony_geometry::StandardRule>(rule_index);
        for (const float anchor : {0.0F, 0.1F, 0.499F, 0.55F, 1.0F})
        {
            const auto oracle = frozen_predefined_harmony_nodes(rule, anchor, oracle_tables);
            const auto actual = harmony_geometry::predefined_harmony_nodes(rule, anchor, tables);
            ASSERT_TRUE(actual.has_value());
            EXPECT_EQ(actual.value().count, counts[rule_index]);
            for (std::size_t node = 0U; node < oracle.count; ++node)
            {
                EXPECT_EQ(std::bit_cast<std::uint32_t>(actual.value().hues[node]),
                          std::bit_cast<std::uint32_t>(oracle.hues[node]));
            }
        }
    }

    FrozenHarmonyHueTable identity{};
    for (std::size_t index = 0U; index < identity.size(); ++index)
    {
        identity[index] = static_cast<float>(index) / static_cast<float>(identity.size());
    }
    const harmony_geometry::HarmonyHueTables identity_tables{identity, identity};
    const auto wrapped = harmony_geometry::predefined_harmony_nodes(
        harmony_geometry::StandardRule::kAnalogous, 0.0F, identity_tables);
    ASSERT_TRUE(wrapped.has_value());
    ASSERT_EQ(wrapped.value().count, 3U);
    EXPECT_EQ(std::bit_cast<std::uint32_t>(wrapped.value().hues[0]),
              std::bit_cast<std::uint32_t>(11.0F / 12.0F));

    const float below_degree = 0.499F / 360.0F;
    const float above_degree = 0.501F / 360.0F;
    const auto below = harmony_geometry::predefined_harmony_nodes(
        harmony_geometry::StandardRule::kMonochromatic, below_degree, identity_tables);
    const auto above = harmony_geometry::predefined_harmony_nodes(
        harmony_geometry::StandardRule::kMonochromatic, above_degree, identity_tables);
    ASSERT_TRUE(below.has_value());
    ASSERT_TRUE(above.has_value());
    EXPECT_EQ(std::bit_cast<std::uint32_t>(below.value().hues[0]), 0x00000000U);
    EXPECT_EQ(std::bit_cast<std::uint32_t>(above.value().hues[0]),
              std::bit_cast<std::uint32_t>(1.0F / 360.0F));

    const auto invalid_rule = harmony_geometry::predefined_harmony_nodes(
        static_cast<harmony_geometry::StandardRule>(9U), 0.1F, tables);
    ASSERT_FALSE(invalid_rule.has_value());
    EXPECT_EQ(invalid_rule.error().code, ErrorCode::kValidation);
    EXPECT_EQ(invalid_rule.error().message, "invalid_harmony_rule");
    for (const float invalid_anchor :
         {-std::numeric_limits<float>::min(),
          std::nextafter(1.0F, std::numeric_limits<float>::infinity()),
          std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()})
    {
        const auto result = harmony_geometry::predefined_harmony_nodes(
            harmony_geometry::StandardRule::kComplementary, invalid_anchor, tables);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, ErrorCode::kValidation);
        EXPECT_EQ(result.error().message, "invalid_harmony_hue");
    }
}

TEST(HarmonyGeometryTest, AttractionMatchesWinnerCircularTieUnderflowAndPullWidth)
{
    const auto expect_oracle =
        [](const float pixel_hue, const std::span<const float> nodes, const float pull_width)
    {
        const auto oracle = frozen_harmony_attraction(pixel_hue, nodes, pull_width);
        const auto actual = harmony_geometry::harmony_attraction(pixel_hue, nodes, pull_width);
        ASSERT_TRUE(actual.has_value());
        EXPECT_EQ(actual.value().winning_index, oracle.winning_index);
        EXPECT_EQ(std::bit_cast<std::uint32_t>(actual.value().weight),
                  std::bit_cast<std::uint32_t>(oracle.weight));
        EXPECT_EQ(std::bit_cast<std::uint32_t>(actual.value().shift),
                  std::bit_cast<std::uint32_t>(oracle.shift));
    };

    const std::array exact{0.25F};
    expect_oracle(0.25F, exact, 1.0F);
    const std::array circular{0.01F};
    expect_oracle(0.99F, circular, 1.0F);
    const std::array tie{0.25F, 0.75F};
    const auto tied = harmony_geometry::harmony_attraction(0.0F, tie, 1.0F);
    ASSERT_TRUE(tied.has_value());
    EXPECT_EQ(tied.value().winning_index, 0U);
    EXPECT_GT(tied.value().shift, 0.0F);
    expect_oracle(0.0F, tie, 1.0F);

    const std::array underflow{0.5F, 0.5F, 0.5F, 0.5F};
    const auto underflowed = harmony_geometry::harmony_attraction(0.0F, underflow, 0.25F);
    ASSERT_TRUE(underflowed.has_value());
    EXPECT_EQ(underflowed.value().winning_index, 0U);
    EXPECT_EQ(std::bit_cast<std::uint32_t>(underflowed.value().weight), 0x00000000U);
    EXPECT_EQ(std::bit_cast<std::uint32_t>(underflowed.value().shift), 0x00000000U);
    for (const float width : {0.25F, 1.0F, 1.84F, 4.0F})
    {
        const std::array nodes{0.2F, 0.6F, 0.9F};
        expect_oracle(0.47F, nodes, width);
    }

    const auto expect_invalid =
        [](const Result<harmony_geometry::HarmonyAttraction> &result, const std::string_view reason)
    {
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, ErrorCode::kValidation);
        EXPECT_EQ(result.error().message, reason);
    };
    const std::array<float, 0> empty{};
    const std::array<float, 5> too_many{0.0F, 0.2F, 0.4F, 0.6F, 0.8F};
    const std::array invalid_node{-std::numeric_limits<float>::min()};
    const std::array nonfinite_node{std::numeric_limits<float>::quiet_NaN()};
    expect_invalid(harmony_geometry::harmony_attraction(0.5F, empty, 1.0F),
                   "invalid_harmony_nodes");
    expect_invalid(harmony_geometry::harmony_attraction(0.5F, too_many, 1.0F),
                   "invalid_harmony_nodes");
    expect_invalid(harmony_geometry::harmony_attraction(0.5F, invalid_node, 1.0F),
                   "invalid_harmony_nodes");
    expect_invalid(harmony_geometry::harmony_attraction(0.5F, nonfinite_node, 1.0F),
                   "invalid_harmony_nodes");
    expect_invalid(
        harmony_geometry::harmony_attraction(-std::numeric_limits<float>::min(), exact, 1.0F),
        "invalid_harmony_hue");
    expect_invalid(harmony_geometry::harmony_attraction(
                       std::nextafter(1.0F, std::numeric_limits<float>::infinity()), exact, 1.0F),
                   "invalid_harmony_hue");
    expect_invalid(
        harmony_geometry::harmony_attraction(std::numeric_limits<float>::quiet_NaN(), exact, 1.0F),
        "invalid_harmony_hue");
    expect_invalid(harmony_geometry::harmony_attraction(0.5F, exact, std::nextafter(0.25F, 0.0F)),
                   "invalid_harmony_pull_width");
    expect_invalid(harmony_geometry::harmony_attraction(
                       0.5F, exact, std::nextafter(4.0F, std::numeric_limits<float>::infinity())),
                   "invalid_harmony_pull_width");
    expect_invalid(
        harmony_geometry::harmony_attraction(0.5F, exact, std::numeric_limits<float>::infinity()),
        "invalid_harmony_pull_width");
}

} // namespace
} // namespace ravo
