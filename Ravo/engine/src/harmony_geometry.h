#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "ravo/foundation/error.h"

namespace ravo::harmony_geometry
{

inline constexpr std::size_t kHueTableSteps = 720U;
inline constexpr std::size_t kMaxHarmonyNodes = 4U;

using HueTable = std::array<float, kHueTableSteps>;

enum class StandardRule : std::uint8_t
{
    kMonochromatic = 0U,
    kAnalogous,
    kAnalogousComplementary,
    kComplementary,
    kSplitComplementary,
    kDyad,
    kTriad,
    kTetrad,
    kSquare,
};

struct HarmonyHueTables
{
    HueTable ucs_to_ryb;
    HueTable ryb_to_ucs;

    bool operator==(const HarmonyHueTables &) const = default;
};

struct HarmonyNodes
{
    std::array<float, kMaxHarmonyNodes> hues{};
    std::size_t count = 0U;

    bool operator==(const HarmonyNodes &) const = default;
};

struct HarmonyAttraction
{
    float shift = 0.0F;
    std::size_t winning_index = 0U;
    float weight = 0.0F;

    bool operator==(const HarmonyAttraction &) const = default;
};

// Engine-private, value-only geometry boundary. Tables are built and returned
// by value; no profile/parser handles or mutable process-global state cross it.
[[nodiscard]] HueTable build_ryb_to_ucs_table(const HueTable &ucs_to_ryb) noexcept;
[[nodiscard]] HarmonyHueTables build_harmony_hue_tables() noexcept;
[[nodiscard]] Result<float> ucs_to_ryb_hue(const HarmonyHueTables &tables, float hue);
[[nodiscard]] Result<float> ryb_to_ucs_hue(const HarmonyHueTables &tables, float hue);
[[nodiscard]] Result<HarmonyNodes> predefined_harmony_nodes(StandardRule rule, float anchor_hue,
                                                            const HarmonyHueTables &tables);
[[nodiscard]] Result<HarmonyAttraction>
harmony_attraction(float pixel_hue, std::span<const float> nodes, float pull_width);

} // namespace ravo::harmony_geometry
