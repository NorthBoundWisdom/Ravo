#pragma once

#include <array>
#include <cstddef>
#include <string_view>

#include "ravo/recipe/recipe.h"

namespace ravo
{

inline constexpr double kDevelopTemperatureDefault = 6500.0;
inline constexpr double kDevelopTemperatureMin = 2000.0;
inline constexpr double kDevelopTemperatureMax = 12000.0;
inline constexpr double kDevelopGammaDefault = 1.0;
inline constexpr double kDevelopStraightenMin = -45.0;
inline constexpr double kDevelopStraightenMax = 45.0;

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
    std::int64_t flip_horizontal = 0;
    std::int64_t flip_vertical = 0;
    double straighten_degrees = 0.0;
    double crop_x = 0.0;
    double crop_y = 0.0;
    double crop_width = 1.0;
    double crop_height = 1.0;
    double sharpen = 0.0;
    double sharpen_radius = 1.0;
    double clarity = 0.0;
    double vignette = 0.0;
    double grain = 0.0;
    double bloom = 0.0;
    double soften = 0.0;
    double dehaze = 0.0;
    double velvia = 0.0;
    double lift = 0.0;
    double color_gamma = 0.0;
    double gain = 0.0;
    double color_contrast = 0.0;
    double monochrome = 0.0;
    double split_shadows_hue = 0.55;
    double split_highlights_hue = 0.08;
    double split_balance = 0.5;
    double split_amount = 0.0;
    double gamma = kDevelopGammaDefault;

    [[nodiscard]] bool is_identity() const noexcept;
    [[nodiscard]] bool operator==(const DevelopParams &) const noexcept = default;
};

void clamp_develop(DevelopParams &params) noexcept;
[[nodiscard]] bool apply_develop_field(DevelopParams &params, std::string_view name, double value);
[[nodiscard]] bool reset_develop_field(DevelopParams &params, std::string_view name);
[[nodiscard]] bool reset_develop_section(DevelopParams &params, std::string_view section);
[[nodiscard]] bool apply_crop_aspect(DevelopParams &params, std::string_view aspect);
void transform_crop_for_quarter_turns(DevelopParams &params, int turns_cw) noexcept;
void transform_crop_for_flip(DevelopParams &params, bool horizontal, bool vertical) noexcept;
[[nodiscard]] double working_image_aspect(std::int64_t rotate_quarters,
                                          double source_aspect) noexcept;
void map_straighten_normalized(double x, double y, double straighten_degrees, double working_aspect,
                               bool inverse, double &ox, double &oy) noexcept;
void straightened_source_quad(double straighten_degrees, double working_aspect,
                              std::array<double, 8> &corners) noexcept;
void inscribed_crop_for_straighten(double straighten_degrees, double working_aspect,
                                   double crop_aspect_norm, double &x, double &y, double &width,
                                   double &height) noexcept;
void constrain_crop_to_straighten(DevelopParams &params, double working_aspect) noexcept;
void fit_crop_to_straighten(DevelopParams &params, double working_aspect) noexcept;
void strip_crop_operations(Recipe &recipe);
void strip_straighten_operations(Recipe &recipe);

[[nodiscard]] Result<Recipe> recipe_from_develop(AssetDescriptor asset,
                                                 const DevelopParams &params);
[[nodiscard]] Result<DevelopParams> develop_from_recipe(const Recipe &recipe);

} // namespace ravo
