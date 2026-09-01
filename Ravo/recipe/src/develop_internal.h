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

} // namespace ravo::develop_internal
