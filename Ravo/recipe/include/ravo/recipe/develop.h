#pragma once

#include "ravo/recipe/recipe.h"

namespace ravo
{

inline constexpr double kDevelopTemperatureDefault = 6500.0;
inline constexpr double kDevelopTemperatureMin = 2000.0;
inline constexpr double kDevelopTemperatureMax = 12000.0;

struct DevelopParams
{
    double temperature = kDevelopTemperatureDefault;
    double tint = 0.0;
    double exposure_ev = 0.0;
    double contrast = 0.0;
    double highlights = 0.0;
    double shadows = 0.0;
    double whites = 0.0;
    double blacks = 0.0;
    double vibrance = 0.0;
    double saturation = 0.0;
    std::int64_t rotate_quarters = 0;
    double crop_x = 0.0;
    double crop_y = 0.0;
    double crop_width = 1.0;
    double crop_height = 1.0;

    [[nodiscard]] bool is_identity() const noexcept;
};

[[nodiscard]] Result<Recipe> recipe_from_develop(AssetDescriptor asset,
                                                 const DevelopParams &params);
[[nodiscard]] Result<DevelopParams> develop_from_recipe(const Recipe &recipe);

} // namespace ravo
