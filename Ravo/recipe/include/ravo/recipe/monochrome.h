#pragma once

#include <cstdint>
#include <map>

#include "ravo/foundation/error.h"
#include "ravo/recipe/recipe.h"

namespace ravo
{

inline constexpr std::string_view kMonochromeOperationId = "ravo.color.monochrome";
inline constexpr std::int64_t kMonochromeOperationSchemaVersion = 2;
inline constexpr std::string_view kMonochromeWorkingSpace = "lab_d50";
inline constexpr std::string_view kMonochromeAlgorithm = "frozen_monochrome_v2";

struct MonochromeParams
{
    double filter_a = 0.0;
    double filter_b = 0.0;
    double size = 2.0;
    double highlights = 0.0;
    double mix = 1.0;

    [[nodiscard]] bool operator==(const MonochromeParams &) const noexcept = default;
};

[[nodiscard]] Result<MonochromeParams>
monochrome_from_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters);
[[nodiscard]] Result<std::map<std::string, ParameterValue, std::less<>>>
monochrome_to_parameters(const MonochromeParams &params);
[[nodiscard]] Result<void> upgrade_monochrome_operation(OperationInstance &operation);

} // namespace ravo
