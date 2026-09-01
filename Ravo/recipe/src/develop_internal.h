#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>

#include <string_view>
#include <vector>

#include "ravo/recipe/develop.h"

namespace ravo::develop_internal
{

extern const double kEpsilon;

[[nodiscard]] bool near(double left, double right) noexcept;
[[nodiscard]] double clamp_value(double value, double minimum, double maximum) noexcept;
[[nodiscard]] double as_number(const ParameterValue &value, double fallback);
[[nodiscard]] std::int64_t as_integer(const ParameterValue &value, std::int64_t fallback);
[[nodiscard]] const std::string *as_string_if(const ParameterValue &value);
[[nodiscard]] std::int64_t flag01(std::int64_t value) noexcept;

void add_operation(Recipe &recipe, std::string id, std::string instance_id,
                   std::map<std::string, ParameterValue, std::less<>> parameters,
                   std::int64_t schema_version = 1,
                   std::optional<std::string> mask_id = std::nullopt, bool enabled = true);

[[nodiscard]] Result<std::array<double, kColorEqualizerBandCount>>
parse_band_array(const ParameterValue &value, std::string_view name);
[[nodiscard]] bool parse_band_field(std::string_view name, std::string_view prefix,
                                    std::size_t &index) noexcept;

[[nodiscard]] bool apply_temperature_field(TemperatureParams &params, std::string_view name,
                                           double value) noexcept;
[[nodiscard]] bool reset_temperature_field(TemperatureParams &params,
                                           std::string_view name) noexcept;
void clamp_temperature(TemperatureParams &params) noexcept;

[[nodiscard]] bool apply_legacy_color_balance_field(ColorBalanceParams &params,
                                                    std::string_view name, double value) noexcept;
[[nodiscard]] bool reset_legacy_color_balance_field(ColorBalanceParams &params,
                                                    std::string_view name) noexcept;
void clamp_legacy_color_balance(ColorBalanceParams &params) noexcept;
void append_legacy_color_balance_develop_names(std::vector<std::string> &names);

[[nodiscard]] bool apply_color_balance_field(ColorBalanceRgbParams &params, std::string_view name,
                                             double value) noexcept;
[[nodiscard]] bool reset_color_balance_field(ColorBalanceRgbParams &params,
                                             std::string_view name) noexcept;
void clamp_color_balance(ColorBalanceRgbParams &params) noexcept;
void append_color_balance_develop_names(std::vector<std::string> &names);

[[nodiscard]] bool
bands_near_zero(const std::array<double, kColorEqualizerBandCount> &values) noexcept;
void make_studio_color_zones_curves(ColorZonesParams &params);
[[nodiscard]] bool studio_color_zones_curves(const ColorZonesParams &params) noexcept;
[[nodiscard]] ParameterValue
band_array_parameter(const std::array<double, kColorEqualizerBandCount> &values);
[[nodiscard]] std::array<std::string_view, 7> rgb_levels_preserve_names() noexcept;

[[nodiscard]] bool apply_color_checker_field(DevelopParams &params, std::string_view name,
                                             double value);
[[nodiscard]] bool reset_color_checker_field(DevelopParams &params, std::string_view name);
[[nodiscard]] bool apply_color_correction_field(DevelopParams &params, std::string_view name,
                                                double value) noexcept;
[[nodiscard]] bool reset_color_correction_field(DevelopParams &params,
                                                std::string_view name) noexcept;
void clamp_color_correction(ColorCorrectionParams &params) noexcept;
[[nodiscard]] bool apply_color_contrast_field(DevelopParams &params, std::string_view name,
                                              double value) noexcept;
[[nodiscard]] bool reset_color_contrast_field(DevelopParams &params,
                                              std::string_view name) noexcept;
void clamp_color_contrast(ColorContrastParams &params) noexcept;
[[nodiscard]] bool apply_color_reconstruction_field(DevelopParams &params, std::string_view name,
                                                    double value) noexcept;
[[nodiscard]] bool reset_color_reconstruction_field(DevelopParams &params,
                                                    std::string_view name) noexcept;
void clamp_color_reconstruction(ColorReconstructionParams &params) noexcept;
void clamp_rgb_levels(RgbLevelsParams &params) noexcept;
[[nodiscard]] bool apply_rgb_levels_field(DevelopParams &params, std::string_view name,
                                          double value) noexcept;
[[nodiscard]] bool apply_rgb_curve_field(DevelopParams &params, std::string_view name,
                                         double value) noexcept;
[[nodiscard]] bool reset_rgb_curve_field(DevelopParams &params, std::string_view name) noexcept;
[[nodiscard]] bool reset_rgb_levels_field(DevelopParams &params, std::string_view name) noexcept;
[[nodiscard]] bool exact_develop_integer(double value, std::int64_t minimum,
                                         std::int64_t maximum, std::int64_t &parsed) noexcept;
[[nodiscard]] bool assign_color_harmonizer_hue_turns(double &target, double degrees) noexcept;
[[nodiscard]] bool apply_color_harmonizer_field(DevelopParams &params, std::string_view name,
                                                double value) noexcept;
[[nodiscard]] bool reset_color_harmonizer_field(DevelopParams &params,
                                                std::string_view name) noexcept;
void clamp_color_harmonizer(ColorHarmonizerParams &params) noexcept;
void append_develop_numeric_field_names(std::vector<std::string> &names);
[[nodiscard]] bool assign_develop_field(DevelopParams &params, std::string_view name,
                                        double value);
[[nodiscard]] bool develop_set_field_accepts(std::string_view name, double value);
[[nodiscard]] std::optional<double> first_accepted_develop_set_value(std::string_view name);
[[nodiscard]] double develop_set_field_extreme(std::string_view name, double seed,
                                               double direction, bool integer);
[[nodiscard]] std::vector<std::string> candidate_develop_set_names();

} // namespace ravo::develop_internal
