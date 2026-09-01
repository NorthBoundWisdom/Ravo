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

TEST(HarmonyGeometryTest, FullTablesMatchIndependentOracleAndReferenceInvariants)
{
    // The oracle is a scalar transcription of colorharmonizer.c's 16-step
    // gamut search, D65 sRGB bridge, HCV conversion, Gossett knots, and strict
    // nearest inverse scan. It never calls a production harmony helper.
    const auto oracle = frozen_harmony_tables();
    const std::uint64_t forward_hash = frozen_harmony_table_hash(oracle.ucs_to_ryb);
    const std::uint64_t inverse_hash = frozen_harmony_table_hash(oracle.ryb_to_ucs);

    const auto actual = harmony_geometry::build_harmony_hue_tables();
    EXPECT_EQ(frozen_harmony_table_hash(actual.ucs_to_ryb), forward_hash);
    EXPECT_EQ(frozen_harmony_table_hash(actual.ryb_to_ucs), inverse_hash);
    for (std::size_t index = 0U; index < harmony_geometry::kHueTableSteps; ++index)
    {
        EXPECT_EQ(std::bit_cast<std::uint32_t>(actual.ucs_to_ryb[index]),
                  std::bit_cast<std::uint32_t>(oracle.ucs_to_ryb[index]));
        EXPECT_EQ(std::bit_cast<std::uint32_t>(actual.ryb_to_ucs[index]),
                  std::bit_cast<std::uint32_t>(oracle.ryb_to_ucs[index]));
    }

    // These deliberate oracle perturbations prove that the table hash catches
    // the frozen search count, transposed matrix orientation, and RYB knot
    // constants rather than merely hashing an arbitrary smooth curve.
    EXPECT_NE(frozen_harmony_table_hash(frozen_harmony_forward_table(15)), forward_hash);
    EXPECT_NE(frozen_harmony_table_hash(frozen_harmony_forward_table(16, true)), forward_hash);
    EXPECT_NE(frozen_harmony_table_hash(
                  frozen_harmony_forward_table(16, false, std::nextafter(0.472217F, 1.0F))),
              forward_hash);

    // Frozen CLAMP routes NaN to zero even though valid table construction does
    // not normally feed non-finite swatch samples.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(std::bit_cast<std::uint32_t>(frozen_harmony_clamp01(nan)), 0x00000000U);

    // The legacy conditional transfer curve is lazy: negative linear sRGB
    // takes the toe and must not evaluate powf on the discarded branch.
    const float white_lightness = frozen_dt_ucs_y_to_lightness(1.0F);
    constexpr FrozenD50Triplet extended_jch{0.65F, 2.0F, 0.0F};
    const auto extended_linear = frozen_harmony_xyz_d65_to_linear_rec709(
        frozen_dt_ucs_xyy_to_xyz_d65(frozen_dt_ucs_jch_to_xyy(extended_jch, white_lightness)));
    ASSERT_TRUE(
        std::ranges::any_of(extended_linear, [](const float value) { return value < 0.0F; }));
    std::feclearexcept(FE_ALL_EXCEPT);
    static_cast<void>(frozen_harmony_jch_to_srgb(extended_jch, white_lightness));
    EXPECT_EQ(std::fetestexcept(FE_INVALID), 0);
    std::feclearexcept(FE_ALL_EXCEPT);
    static_cast<void>(harmony_geometry::build_harmony_hue_tables());
    EXPECT_EQ(std::fetestexcept(FE_INVALID), 0);

    EXPECT_EQ(std::bit_cast<std::uint32_t>(frozen_harmony_rgb_hue_to_ryb(0.0F)), 0x00000000U);
    EXPECT_EQ(std::bit_cast<std::uint32_t>(frozen_harmony_rgb_hue_to_ryb(1.0F / 6.0F)),
              std::bit_cast<std::uint32_t>(1.0F / 3.0F));
    EXPECT_EQ(std::bit_cast<std::uint32_t>(frozen_harmony_rgb_hue_to_ryb(2.0F / 6.0F)),
              std::bit_cast<std::uint32_t>(0.472217F));
    EXPECT_EQ(std::bit_cast<std::uint32_t>(frozen_harmony_rgb_hue_to_ryb(5.0F / 6.0F)),
              std::bit_cast<std::uint32_t>(5.0F / 6.0F));
    EXPECT_EQ(std::bit_cast<std::uint32_t>(frozen_harmony_rgb_hue_to_ryb(1.0F)), 0x00000000U);

    const float transfer_input = 0.045F;
    EXPECT_NE(std::bit_cast<std::uint32_t>(frozen_harmony_srgb_to_linear(transfer_input)),
              std::bit_cast<std::uint32_t>(frozen_harmony_srgb_to_linear(transfer_input, 0.05F)));
}

} // namespace
} // namespace ravo
