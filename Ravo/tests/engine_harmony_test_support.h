#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ravo/foundation/cancellation.h"
#include "ravo/recipe/color_harmonizer.h"
#include "ravo/recipe/develop.h"

#include "color_harmonizer.h"
#include "harmony_geometry.h"
#include "image_ops.h"

namespace ravo::engine_harmony_test_support
{

using FrozenD50Triplet = std::array<float, 3>;
inline constexpr float kPlatformLibmReferenceTolerance = 1.0e-5F;

[[nodiscard]] std::array<std::uint32_t, 3>
d50_triplet_bits(const FrozenD50Triplet &triplet) noexcept;
void expect_frozen_d50_bits(const FrozenD50Triplet &actual, const FrozenD50Triplet &oracle,
                            const std::array<std::uint32_t, 3> &golden);
void expect_frozen_d50_cbrt_reference(const FrozenD50Triplet &actual,
                                      const FrozenD50Triplet &oracle,
                                      const std::array<std::uint32_t, 3> &reference);
[[nodiscard]] float frozen_dt_ucs_matrix_row(float coefficient0, float value0, float coefficient1,
                                             float value1, float coefficient2,
                                             float value2) noexcept;
[[nodiscard]] FrozenD50Triplet frozen_dt_ucs_xyz_d50_to_d65(FrozenD50Triplet xyz) noexcept;
[[nodiscard]] FrozenD50Triplet frozen_dt_ucs_xyz_d65_to_d50(FrozenD50Triplet xyz) noexcept;
[[nodiscard]] FrozenD50Triplet frozen_dt_ucs_xyz_d65_to_xyy(FrozenD50Triplet xyz) noexcept;
[[nodiscard]] FrozenD50Triplet frozen_dt_ucs_xyy_to_xyz_d65(FrozenD50Triplet xyy) noexcept;
[[nodiscard]] float frozen_dt_ucs_y_to_lightness(float luminance) noexcept;
[[nodiscard]] float frozen_dt_ucs_lightness_to_y(float lightness) noexcept;

struct FrozenDtUcsJchOracle
{
    FrozenD50Triplet jch{};
    float lightness = 0.0F;
    float squared_colorfulness = 0.0F;
    float source_order_chroma = 0.0F;
    float reassociated_chroma = 0.0F;
};

[[nodiscard]] FrozenDtUcsJchOracle frozen_dt_ucs_xyy_to_jch_oracle(FrozenD50Triplet xyy,
                                                                   float white_lightness) noexcept;
[[nodiscard]] FrozenD50Triplet frozen_dt_ucs_jch_to_xyy(FrozenD50Triplet jch,
                                                        float white_lightness) noexcept;
[[nodiscard]] FrozenD50Triplet frozen_dt_ucs_xyz_d50_to_jch(FrozenD50Triplet xyz,
                                                            float white_lightness) noexcept;
[[nodiscard]] FrozenD50Triplet frozen_dt_ucs_jch_to_xyz_d50(FrozenD50Triplet jch,
                                                            float white_lightness) noexcept;
void expect_dt_ucs_local_oracle(FrozenD50Triplet actual, FrozenD50Triplet oracle);

using FrozenHarmonyHueTable = std::array<float, harmony_geometry::kHueTableSteps>;

struct FrozenHarmonyHueTables
{
    FrozenHarmonyHueTable ucs_to_ryb{};
    FrozenHarmonyHueTable ryb_to_ucs{};
};

struct FrozenHarmonyNodes
{
    std::array<float, harmony_geometry::kMaxHarmonyNodes> hues{};
    std::size_t count = 0U;
};

struct FrozenHarmonyAttraction
{
    float shift = 0.0F;
    std::size_t winning_index = 0U;
    float weight = 0.0F;
};

[[nodiscard]] float frozen_harmony_clamp01(float value) noexcept;
[[nodiscard]] FrozenD50Triplet
frozen_harmony_xyz_d65_to_linear_rec709(FrozenD50Triplet xyz,
                                        bool transpose_discriminator = false) noexcept;
[[nodiscard]] float frozen_harmony_srgb_to_linear(float srgb, float threshold = 0.04045F) noexcept;
[[nodiscard]] FrozenD50Triplet
frozen_harmony_jch_to_srgb(FrozenD50Triplet jch, float white_lightness,
                           bool transpose_discriminator = false) noexcept;
[[nodiscard]] float frozen_harmony_max_chroma(float hue, int iterations = 16,
                                              bool transpose_discriminator = false) noexcept;
[[nodiscard]] float frozen_harmony_rgb_hue_to_ryb(float hue,
                                                  float middle_knot = 0.472217F) noexcept;
[[nodiscard]] float frozen_harmony_ucs_to_ryb(float hue, int iterations = 16,
                                              bool transpose_discriminator = false,
                                              float middle_knot = 0.472217F) noexcept;
[[nodiscard]] FrozenHarmonyHueTable
frozen_harmony_forward_table(int iterations = 16, bool transpose_discriminator = false,
                             float middle_knot = 0.472217F) noexcept;
[[nodiscard]] FrozenHarmonyHueTable
frozen_harmony_inverse_table(const FrozenHarmonyHueTable &forward) noexcept;
[[nodiscard]] FrozenHarmonyHueTables frozen_harmony_tables() noexcept;
[[nodiscard]] float frozen_harmony_lookup(const FrozenHarmonyHueTable &table, float hue) noexcept;
[[nodiscard]] FrozenHarmonyNodes
frozen_predefined_harmony_nodes(harmony_geometry::StandardRule rule, float anchor_hue,
                                const FrozenHarmonyHueTables &tables) noexcept;
[[nodiscard]] FrozenHarmonyAttraction
frozen_harmony_attraction(float pixel_hue, std::span<const float> nodes, float pull_width) noexcept;
[[nodiscard]] std::uint64_t frozen_harmony_table_hash(const FrozenHarmonyHueTable &table) noexcept;

[[nodiscard]] std::array<float, 9>
frozen_color_harmonizer_inverse(const std::array<float, 9> &matrix) noexcept;
[[nodiscard]] FrozenD50Triplet frozen_color_harmonizer_matrix(const std::array<float, 9> &matrix,
                                                              FrozenD50Triplet value) noexcept;
[[nodiscard]] FrozenHarmonyNodes
frozen_color_harmonizer_nodes(const ColorHarmonizerParams &params,
                              const FrozenHarmonyHueTables &tables) noexcept;
[[nodiscard]] FrozenD50Triplet
frozen_color_harmonizer_rgb(const ColorHarmonizerParams &params, FrozenD50Triplet input,
                            const std::array<float, 9> &working_to_xyz_d50,
                            const FrozenHarmonyHueTables &tables, bool skip_negative_clip = false,
                            bool linear_neutral_protection = false) noexcept;
[[nodiscard]] ColorHarmonizerParams frozen_color_harmonizer_0176_record13() noexcept;
[[nodiscard]] WorkingImage color_harmonizer_working_fixture();
[[nodiscard]] std::vector<float>
frozen_color_harmonizer_two_pass(const WorkingImage &input, const ColorHarmonizerParams &params,
                                 float roi_scale, const FrozenHarmonyHueTables &tables);

struct ColorHarmonizerCancellationFixture
{
    CancellationSource *source = nullptr;
    detail::ColorHarmonizerCheckpoint target = detail::ColorHarmonizerCheckpoint::kBeforeValidation;
    bool fired = false;
};

void cancel_color_harmonizer(void *context, detail::ColorHarmonizerCheckpoint checkpoint,
                             std::uint32_t row) noexcept;
[[nodiscard]] std::vector<float>
frozen_legacy_color_balance_reference(const WorkingImage &input, const ColorBalanceParams &params);

} // namespace ravo::engine_harmony_test_support
