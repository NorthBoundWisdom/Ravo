#pragma once

#include <cstdint>
#include <map>

#include "ravo/foundation/error.h"
#include "ravo/recipe/recipe.h"

namespace ravo
{

inline constexpr std::string_view kVelviaOperationId = "ravo.color.velvia";
inline constexpr std::int64_t kVelviaOperationSchemaVersion = 2;
inline constexpr std::string_view kVelviaWorkingSpace = "linear_rec709";
inline constexpr std::string_view kVelviaAlgorithm = "frozen_velvia_v2";

struct VelviaParams
{
    double strength = 25.0;
    double bias = 1.0;

    [[nodiscard]] bool operator==(const VelviaParams &) const noexcept = default;
};

[[nodiscard]] Result<VelviaParams>
velvia_from_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters);
[[nodiscard]] Result<std::map<std::string, ParameterValue, std::less<>>>
velvia_to_parameters(const VelviaParams &params);
[[nodiscard]] Result<void> upgrade_velvia_operation(OperationInstance &operation);

} // namespace ravo
