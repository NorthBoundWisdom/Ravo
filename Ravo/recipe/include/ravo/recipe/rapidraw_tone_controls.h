#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

#include "ravo/foundation/error.h"
#include "ravo/recipe/recipe.h"

namespace ravo
{

inline constexpr std::string_view kRapidRawToneControlsOperationId =
    "ravo.core.rapidraw-tone-controls";
inline constexpr std::int64_t kRapidRawToneControlsSchemaVersion = 1;
inline constexpr std::string_view kRapidRawToneControlsWorkingSpace = "linear_srgb";
inline constexpr double kRapidRawExposureMin = -5.0;
inline constexpr double kRapidRawExposureMax = 5.0;
inline constexpr double kRapidRawToneMin = -100.0;
inline constexpr double kRapidRawToneMax = 100.0;

struct RapidRawToneControlsParams
{
    double ev_shift = 0.0;
    double exposure = 0.0;
    double contrast = 0.0;
    double highlights = 0.0;
    double shadows = 0.0;
    double whites = 0.0;
    double blacks = 0.0;

    [[nodiscard]] bool is_identity() const noexcept;
    [[nodiscard]] bool operator==(const RapidRawToneControlsParams &) const noexcept = default;
};

[[nodiscard]] Result<RapidRawToneControlsParams> rapidraw_tone_controls_from_parameters(
    const std::map<std::string, ParameterValue, std::less<>> &parameters);
[[nodiscard]] Result<std::map<std::string, ParameterValue, std::less<>>>
rapidraw_tone_controls_to_parameters(const RapidRawToneControlsParams &params);
[[nodiscard]] Result<void> validate_rapidraw_tone_controls_parameters(
    const std::map<std::string, ParameterValue, std::less<>> &parameters);

} // namespace ravo
