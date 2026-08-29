#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

#include "ravo/foundation/error.h"
#include "ravo/recipe/recipe.h"

namespace ravo
{

inline constexpr std::string_view kWatermarkOperationId = "ravo.output.watermark";
inline constexpr std::int64_t kWatermarkOperationSchemaVersion = 1;
inline constexpr std::string_view kWatermarkWorkingSpace = "encoded_output_rgb";
inline constexpr std::string_view kWatermarkAlgorithm = "ravo_mono_5x7_v1";
inline constexpr std::size_t kWatermarkTextMaxBytes = 256U;
inline constexpr std::size_t kWatermarkLineMaxCharacters = 64U;
inline constexpr std::size_t kWatermarkMaxLines = 8U;
inline constexpr double kWatermarkScaleMin = 0.5;
inline constexpr double kWatermarkScaleMax = 50.0;

enum class WatermarkAlignment : std::uint8_t
{
    kTopLeft = 0,
    kTopCenter,
    kTopRight,
    kCenterLeft,
    kCenter,
    kCenterRight,
    kBottomLeft,
    kBottomCenter,
    kBottomRight,
};

struct WatermarkParams
{
    std::string text = "RAVO";
    std::array<double, 3> color{1.0, 1.0, 1.0};
    double opacity = 0.5;
    double scale_percent = 8.0;
    double x_offset = 0.0;
    double y_offset = 0.0;
    WatermarkAlignment alignment = WatermarkAlignment::kBottomRight;
    double rotation_degrees = 0.0;

    [[nodiscard]] bool operator==(const WatermarkParams &) const noexcept = default;
};

[[nodiscard]] std::string_view watermark_alignment_name(WatermarkAlignment alignment) noexcept;
[[nodiscard]] Result<WatermarkAlignment> parse_watermark_alignment(std::string_view name);
[[nodiscard]] bool watermark_character_supported(char character) noexcept;
[[nodiscard]] Result<std::string> expand_watermark_text(std::string_view text,
                                                        const AssetDescriptor &asset);
[[nodiscard]] Result<WatermarkParams>
watermark_from_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters);
[[nodiscard]] Result<std::map<std::string, ParameterValue, std::less<>>>
watermark_to_parameters(const WatermarkParams &params);

} // namespace ravo
