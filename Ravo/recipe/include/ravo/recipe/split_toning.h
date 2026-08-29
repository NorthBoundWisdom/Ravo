#pragma once

#include <cstdint>
#include <map>

#include "ravo/foundation/error.h"
#include "ravo/recipe/recipe.h"

namespace ravo
{

inline constexpr std::string_view kSplitToningOperationId = "ravo.color.splittoning";
inline constexpr std::int64_t kSplitToningOperationSchemaVersion = 2;
inline constexpr std::string_view kSplitToningWorkingSpace = "linear_rec709";
inline constexpr std::string_view kSplitToningAlgorithm = "frozen_splittoning_v1";

struct SplitToningParams
{
    double shadow_hue = 0.0;
    double shadow_saturation = 0.5;
    double highlight_hue = 0.2;
    double highlight_saturation = 0.5;
    double balance = 0.5;
    double compress = 33.0;
    double mix = 1.0;

    [[nodiscard]] bool operator==(const SplitToningParams &) const noexcept = default;
};

[[nodiscard]] Result<SplitToningParams>
split_toning_from_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters);
[[nodiscard]] Result<std::map<std::string, ParameterValue, std::less<>>>
split_toning_to_parameters(const SplitToningParams &params);
[[nodiscard]] Result<void> upgrade_split_toning_operation(OperationInstance &operation);

} // namespace ravo
