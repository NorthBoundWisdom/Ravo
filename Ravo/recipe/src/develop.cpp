#include "ravo/recipe/develop.h"

#include <cmath>
#include <string>
#include <utility>

namespace ravo
{
namespace
{

constexpr double kEpsilon = 1e-6;

[[nodiscard]] bool near(const double left, const double right) noexcept
{
    return std::abs(left - right) <= kEpsilon;
}

[[nodiscard]] double as_number(const ParameterValue &value, const double fallback)
{
    if (std::holds_alternative<double>(value.value))
    {
        return std::get<double>(value.value);
    }
    if (std::holds_alternative<std::int64_t>(value.value))
    {
        return static_cast<double>(std::get<std::int64_t>(value.value));
    }
    return fallback;
}

[[nodiscard]] std::int64_t as_integer(const ParameterValue &value, const std::int64_t fallback)
{
    if (std::holds_alternative<std::int64_t>(value.value))
    {
        return std::get<std::int64_t>(value.value);
    }
    if (std::holds_alternative<double>(value.value))
    {
        return static_cast<std::int64_t>(std::llround(std::get<double>(value.value)));
    }
    return fallback;
}

void add_operation(Recipe &recipe, std::string id, std::string instance_id,
                   std::map<std::string, ParameterValue, std::less<>> parameters)
{
    recipe.operations.push_back(
        {std::move(id), 1, std::move(instance_id), true, std::move(parameters), std::nullopt});
}

} // namespace

bool DevelopParams::is_identity() const noexcept
{
    return near(temperature, kDevelopTemperatureDefault) && near(tint, 0.0) &&
           near(exposure_ev, 0.0) && near(contrast, 0.0) && near(highlights, 0.0) &&
           near(shadows, 0.0) && near(whites, 0.0) && near(blacks, 0.0) && near(vibrance, 0.0) &&
           near(saturation, 0.0) && rotate_quarters % 4 == 0 && near(crop_x, 0.0) &&
           near(crop_y, 0.0) && near(crop_width, 1.0) && near(crop_height, 1.0);
}

Result<Recipe> recipe_from_develop(AssetDescriptor asset, const DevelopParams &params)
{
    Recipe recipe;
    recipe.asset = std::move(asset);
    if (!near(params.temperature, kDevelopTemperatureDefault) || !near(params.tint, 0.0))
    {
        add_operation(recipe, "ravo.color.white_balance", "white-balance-1",
                      {{"temperature", ParameterValue{params.temperature}},
                       {"tint", ParameterValue{params.tint}}});
    }
    if (!near(params.exposure_ev, 0.0))
    {
        add_operation(recipe, "ravo.core.exposure", "exposure-1",
                      {{"exposure_ev", ParameterValue{params.exposure_ev}}});
    }
    if (!near(params.contrast, 0.0))
    {
        add_operation(recipe, "ravo.core.contrast", "contrast-1",
                      {{"amount", ParameterValue{params.contrast}}});
    }
    if (!near(params.highlights, 0.0))
    {
        add_operation(recipe, "ravo.core.highlights", "highlights-1",
                      {{"amount", ParameterValue{params.highlights}}});
    }
    if (!near(params.shadows, 0.0))
    {
        add_operation(recipe, "ravo.core.shadows", "shadows-1",
                      {{"amount", ParameterValue{params.shadows}}});
    }
    if (!near(params.whites, 0.0))
    {
        add_operation(recipe, "ravo.core.whites", "whites-1",
                      {{"amount", ParameterValue{params.whites}}});
    }
    if (!near(params.blacks, 0.0))
    {
        add_operation(recipe, "ravo.core.blacks", "blacks-1",
                      {{"amount", ParameterValue{params.blacks}}});
    }
    if (!near(params.vibrance, 0.0))
    {
        add_operation(recipe, "ravo.color.vibrance", "vibrance-1",
                      {{"amount", ParameterValue{params.vibrance}}});
    }
    if (!near(params.saturation, 0.0))
    {
        add_operation(recipe, "ravo.color.saturation", "saturation-1",
                      {{"amount", ParameterValue{params.saturation}}});
    }
    if (params.rotate_quarters % 4 != 0)
    {
        add_operation(recipe, "ravo.geometry.rotate", "rotate-1",
                      {{"quarters", ParameterValue{params.rotate_quarters % 4}}});
    }
    if (!near(params.crop_x, 0.0) || !near(params.crop_y, 0.0) || !near(params.crop_width, 1.0) ||
        !near(params.crop_height, 1.0))
    {
        add_operation(recipe, "ravo.geometry.crop", "crop-1",
                      {{"x", ParameterValue{params.crop_x}},
                       {"y", ParameterValue{params.crop_y}},
                       {"width", ParameterValue{params.crop_width}},
                       {"height", ParameterValue{params.crop_height}}});
    }
    return recipe;
}

Result<DevelopParams> develop_from_recipe(const Recipe &recipe)
{
    DevelopParams params;
    for (const auto &operation : recipe.operations)
    {
        if (!operation.enabled)
        {
            continue;
        }
        const auto number = [&](const std::string_view name, const double fallback)
        {
            const auto found = operation.parameters.find(std::string(name));
            if (found == operation.parameters.end())
            {
                return fallback;
            }
            return as_number(found->second, fallback);
        };
        if (operation.id == "ravo.color.white_balance")
        {
            params.temperature = number("temperature", params.temperature);
            params.tint = number("tint", params.tint);
        }
        else if (operation.id == "ravo.core.exposure")
        {
            params.exposure_ev = number("exposure_ev", params.exposure_ev);
        }
        else if (operation.id == "ravo.core.contrast")
        {
            params.contrast = number("amount", params.contrast);
        }
        else if (operation.id == "ravo.core.highlights")
        {
            params.highlights = number("amount", params.highlights);
        }
        else if (operation.id == "ravo.core.shadows")
        {
            params.shadows = number("amount", params.shadows);
        }
        else if (operation.id == "ravo.core.whites")
        {
            params.whites = number("amount", params.whites);
        }
        else if (operation.id == "ravo.core.blacks")
        {
            params.blacks = number("amount", params.blacks);
        }
        else if (operation.id == "ravo.color.vibrance")
        {
            params.vibrance = number("amount", params.vibrance);
        }
        else if (operation.id == "ravo.color.saturation")
        {
            params.saturation = number("amount", params.saturation);
        }
        else if (operation.id == "ravo.geometry.rotate")
        {
            const auto found = operation.parameters.find("quarters");
            params.rotate_quarters =
                found == operation.parameters.end() ? 0 : as_integer(found->second, 0) % 4;
            if (params.rotate_quarters < 0)
            {
                params.rotate_quarters += 4;
            }
        }
        else if (operation.id == "ravo.geometry.crop")
        {
            params.crop_x = number("x", params.crop_x);
            params.crop_y = number("y", params.crop_y);
            params.crop_width = number("width", params.crop_width);
            params.crop_height = number("height", params.crop_height);
        }
    }
    return params;
}

} // namespace ravo
