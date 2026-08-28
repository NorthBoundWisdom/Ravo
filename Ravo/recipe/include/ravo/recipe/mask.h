#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "ravo/foundation/error.h"
#include "ravo/foundation/json.h"

namespace ravo
{

// Mask schema v1 was the original, intentionally small `{ id, kind: all }`
// contract.  Schema v2 is the first canonical graph contract.  Recipe schema
// remains independent: recipes own a collection of versioned mask nodes.
inline constexpr std::int64_t kCanonicalMaskSchemaVersion = 2;
inline constexpr std::size_t kCanonicalMaskMaxNodes = 128U;
inline constexpr std::size_t kCanonicalMaskMaxGroupChildren = 32U;
inline constexpr std::size_t kCanonicalMaskMaxDepth = 32U;
// A DAG may reach the same node through several parents. The depth-first
// evaluator deliberately does not cache full alpha planes, so bound the
// expanded work of every possible root as well as the stored node count.
inline constexpr std::size_t kCanonicalMaskMaxExpandedNodes = 256U;
inline constexpr std::size_t kCanonicalMaskMinPathPoints = 3U;
inline constexpr std::size_t kCanonicalMaskMinBrushPoints = 2U;
inline constexpr std::size_t kCanonicalMaskMaxPathPoints = 32U;
inline constexpr std::size_t kCanonicalMaskMaxTessellatedSamples = 65536U;

// These are the canonical validator bounds. Presentation may choose a coarser
// interaction step, but it must not invent another accepted range.
inline constexpr double kCanonicalMaskUnitMin = 0.0;
inline constexpr double kCanonicalMaskUnitMax = 1.0;
inline constexpr double kCanonicalMaskAngleMin = -180.0;
inline constexpr double kCanonicalMaskAngleMax = 180.0;
// Radius values are strictly positive. denorm_min is the smallest positive
// representable double, so this preserves the validator's existing `> 0`
// contract without silently narrowing persisted canonical recipes.
inline constexpr double kCanonicalMaskPositiveMin = std::numeric_limits<double>::denorm_min();

enum class MaskKind
{
    kAll,
    kLinearGradient,
    kCircle,
    kEllipse,
    kParametric,
    kGroup,
    kPath,
    kBrush,
};

enum class ParametricMaskSource
{
    kInput,
    kOperationOutput,
};

enum class ParametricMaskChannel
{
    kLuminance,
    kRed,
    kGreen,
    kBlue,
};

enum class MaskGroupOperator
{
    // Every group has one ordered accumulator.  Its first child replaces the
    // empty accumulator; every later child must use one of the compositions.
    kReplace,
    kUnion,
    kIntersection,
    kDifference,
    kExclusion,
};

struct MaskCommon
{
    double opacity = 1.0;
    bool inverted = false;

    [[nodiscard]] bool operator==(const MaskCommon &) const noexcept = default;
};

struct AllMask
{
    [[nodiscard]] bool operator==(const AllMask &) const noexcept = default;
};

// `transition` is the normalized half-width of the source-derived linear
// ramp. Zero is an explicit hard edge at the anchor line; positive values use
// the frozen 0.001 minimum effective width.
struct LinearGradientMask
{
    double anchor_x = 0.5;
    double anchor_y = 0.5;
    double rotation_degrees = 0.0;
    double transition = 0.1;

    [[nodiscard]] bool operator==(const LinearGradientMask &) const noexcept = default;
};

// Radius and feather are normalized against min(full_width, full_height), as
// in the frozen circle/ellipse CPU owners.  Coordinates remain normalized to
// the attached operation input frame.
struct CircleMask
{
    double center_x = 0.5;
    double center_y = 0.5;
    double radius = 0.25;
    double feather = 0.0;

    [[nodiscard]] bool operator==(const CircleMask &) const noexcept = default;
};

struct EllipseMask
{
    double center_x = 0.5;
    double center_y = 0.5;
    double radius_x = 0.25;
    double radius_y = 0.25;
    double rotation_degrees = 0.0;
    double feather = 0.0;

    [[nodiscard]] bool operator==(const EllipseMask &) const noexcept = default;
};

struct ParametricMask
{
    ParametricMaskSource source = ParametricMaskSource::kInput;
    ParametricMaskChannel channel = ParametricMaskChannel::kLuminance;
    // Source-order four-keyframe 0 -> 1 -> 1 -> 0 ramp. Equal adjacent
    // thresholds are valid and create a documented hard edge; positive slopes
    // retain the frozen 0.001 denominator floor.
    std::array<double, 4> thresholds{0.0, 0.0, 1.0, 1.0};

    [[nodiscard]] bool operator==(const ParametricMask &) const noexcept = default;
};

struct MaskGroupChild
{
    std::string mask_id;
    MaskGroupOperator operation = MaskGroupOperator::kReplace;
    double opacity = 1.0;
    bool inverted = false;

    [[nodiscard]] bool operator==(const MaskGroupChild &) const noexcept = default;
};

struct MaskGroup
{
    std::vector<MaskGroupChild> children;

    [[nodiscard]] bool operator==(const MaskGroup &) const noexcept = default;
};

struct PathMaskPoint
{
    double x = 0.5;
    double y = 0.5;
    double ctrl1_x = 0.5;
    double ctrl1_y = 0.5;
    double ctrl2_x = 0.5;
    double ctrl2_y = 0.5;

    [[nodiscard]] bool operator==(const PathMaskPoint &) const noexcept = default;
};

struct PathMask
{
    std::vector<PathMaskPoint> points;
    double feather = 0.0;

    [[nodiscard]] bool operator==(const PathMask &) const noexcept = default;
};

struct BrushMaskPoint
{
    double x = 0.5;
    double y = 0.5;
    double ctrl1_x = 0.5;
    double ctrl1_y = 0.5;
    double ctrl2_x = 0.5;
    double ctrl2_y = 0.5;
    double radius = 0.05;
    double hardness = 0.5;
    double density = 1.0;

    [[nodiscard]] bool operator==(const BrushMaskPoint &) const noexcept = default;
};

struct BrushMask
{
    std::vector<BrushMaskPoint> points;

    [[nodiscard]] bool operator==(const BrushMask &) const noexcept = default;
};

using MaskPayload = std::variant<AllMask, LinearGradientMask, CircleMask, EllipseMask,
                                 ParametricMask, MaskGroup, PathMask, BrushMask>;

struct Mask
{
    Mask() = default;
    Mask(std::string id, std::int64_t schema_version, MaskKind kind);

    std::string id;
    std::int64_t schema_version = kCanonicalMaskSchemaVersion;
    MaskKind kind = MaskKind::kAll;
    MaskCommon common;
    MaskPayload payload{AllMask{}};

    [[nodiscard]] bool operator==(const Mask &) const noexcept = default;
};

[[nodiscard]] Result<Mask> parse_canonical_mask(const JsonValue &value, std::string_view path);
[[nodiscard]] Result<void> upgrade_mask_graph(std::vector<Mask> &masks);
[[nodiscard]] Result<void> validate_mask_graph(const std::vector<Mask> &masks);
[[nodiscard]] Result<JsonValue> canonical_mask_to_json(const Mask &mask);
[[nodiscard]] std::string_view mask_kind_name(MaskKind kind) noexcept;

} // namespace ravo
