#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string_view>

#include "ravo/foundation/error.h"
#include "ravo/recipe/recipe.h"

namespace ravo
{

inline constexpr std::string_view kCanvasOperationId = "ravo.geometry.canvas";
inline constexpr std::int64_t kCanvasOperationSchemaVersion = 1;
inline constexpr std::string_view kCanvasWorkingSpace = "linear_rec709";
inline constexpr std::string_view kCanvasAlgorithm = "frozen_enlargecanvas_v1";
inline constexpr double kCanvasPercentMin = 0.0;
inline constexpr double kCanvasPercentMax = 100.0;

enum class CanvasColor : std::uint8_t
{
    kGreen = 0,
    kRed = 1,
    kBlue = 2,
    kBlack = 3,
    kWhite = 4,
};

struct CanvasParams
{
    double percent_left = 0.0;
    double percent_right = 0.0;
    double percent_top = 0.0;
    double percent_bottom = 0.0;
    CanvasColor color = CanvasColor::kGreen;

    [[nodiscard]] bool is_identity() const noexcept;
    [[nodiscard]] bool operator==(const CanvasParams &) const noexcept = default;
};

inline constexpr std::string_view kFrameOperationId = "ravo.output.frame";
inline constexpr std::int64_t kFrameOperationSchemaVersion = 1;
inline constexpr std::string_view kFrameWorkingSpace = "encoded_output_rgb";
inline constexpr std::string_view kFrameAlgorithm = "frozen_borders_v4";

enum class FrameOrientation : std::uint8_t
{
    kAuto = 0,
    kPortrait = 1,
    kLandscape = 2,
};

enum class FrameBasis : std::uint8_t
{
    kAuto = 0,
    kWidth = 1,
    kHeight = 2,
    kShorter = 3,
    kLonger = 4,
};

struct FrameParams
{
    std::array<double, 3> border_color{1.0, 1.0, 1.0};
    // -1 is constant border; 0 follows the image; positive is outer w/h.
    double aspect = -1.0;
    FrameOrientation orientation = FrameOrientation::kAuto;
    double size = 0.1;
    double position_h = 0.5;
    double position_v = 0.5;
    double frame_size = 0.0;
    double frame_offset = 0.5;
    std::array<double, 3> frame_color{0.0, 0.0, 0.0};
    FrameBasis basis = FrameBasis::kAuto;

    [[nodiscard]] bool operator==(const FrameParams &) const noexcept = default;
};

[[nodiscard]] std::string_view canvas_color_name(CanvasColor color) noexcept;
[[nodiscard]] Result<CanvasColor> parse_canvas_color(std::string_view name);
[[nodiscard]] std::string_view frame_orientation_name(FrameOrientation orientation) noexcept;
[[nodiscard]] Result<FrameOrientation> parse_frame_orientation(std::string_view name);
[[nodiscard]] std::string_view frame_basis_name(FrameBasis basis) noexcept;
[[nodiscard]] Result<FrameBasis> parse_frame_basis(std::string_view name);

[[nodiscard]] Result<CanvasParams>
canvas_from_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters);
[[nodiscard]] Result<std::map<std::string, ParameterValue, std::less<>>>
canvas_to_parameters(const CanvasParams &params);
[[nodiscard]] Result<FrameParams>
frame_from_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters);
[[nodiscard]] Result<std::map<std::string, ParameterValue, std::less<>>>
frame_to_parameters(const FrameParams &params);

} // namespace ravo
