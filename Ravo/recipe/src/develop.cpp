#include "ravo/recipe/develop.h"

#include <algorithm>
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

[[nodiscard]] double clamp_value(const double value, const double lo, const double hi) noexcept
{
    return std::clamp(value, lo, hi);
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
    if (std::holds_alternative<bool>(value.value))
    {
        return std::get<bool>(value.value) ? 1.0 : 0.0;
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
    if (std::holds_alternative<bool>(value.value))
    {
        return std::get<bool>(value.value) ? 1 : 0;
    }
    return fallback;
}

void add_operation(Recipe &recipe, std::string id, std::string instance_id,
                   std::map<std::string, ParameterValue, std::less<>> parameters)
{
    recipe.operations.push_back(
        {std::move(id), 1, std::move(instance_id), true, std::move(parameters), std::nullopt});
}

[[nodiscard]] std::int64_t flag01(const std::int64_t value) noexcept
{
    return value != 0 ? 1 : 0;
}

} // namespace

void clamp_develop(DevelopParams &params) noexcept
{
    params.temperature =
        clamp_value(params.temperature, kDevelopTemperatureMin, kDevelopTemperatureMax);
    params.tint = clamp_value(params.tint, -150.0, 150.0);
    params.exposure_ev = clamp_value(params.exposure_ev, -10.0, 10.0);
    params.contrast = clamp_value(params.contrast, -1.0, 1.0);
    params.highlights = clamp_value(params.highlights, -1.0, 1.0);
    params.shadows = clamp_value(params.shadows, -1.0, 1.0);
    params.whites = clamp_value(params.whites, -1.0, 1.0);
    params.blacks = clamp_value(params.blacks, -1.0, 1.0);
    params.vibrance = clamp_value(params.vibrance, -1.0, 1.0);
    params.saturation = clamp_value(params.saturation, -1.0, 1.0);
    params.rotate_quarters = ((params.rotate_quarters % 4) + 4) % 4;
    params.flip_horizontal = flag01(params.flip_horizontal);
    params.flip_vertical = flag01(params.flip_vertical);
    params.crop_width = clamp_value(params.crop_width, 0.01, 1.0);
    params.crop_height = clamp_value(params.crop_height, 0.01, 1.0);
    params.crop_x = clamp_value(params.crop_x, 0.0, 1.0 - params.crop_width);
    params.crop_y = clamp_value(params.crop_y, 0.0, 1.0 - params.crop_height);
    params.sharpen = clamp_value(params.sharpen, 0.0, 2.0);
    params.sharpen_radius = clamp_value(params.sharpen_radius, 0.0, 12.0);
    params.clarity = clamp_value(params.clarity, -1.0, 1.0);
    params.vignette = clamp_value(params.vignette, 0.0, 1.0);
    params.grain = clamp_value(params.grain, 0.0, 1.0);
    params.bloom = clamp_value(params.bloom, 0.0, 1.0);
    params.soften = clamp_value(params.soften, 0.0, 1.0);
    params.dehaze = clamp_value(params.dehaze, -1.0, 1.0);
    params.velvia = clamp_value(params.velvia, 0.0, 1.0);
    params.lift = clamp_value(params.lift, -1.0, 1.0);
    params.color_gamma = clamp_value(params.color_gamma, -1.0, 1.0);
    params.gain = clamp_value(params.gain, -1.0, 1.0);
    params.color_contrast = clamp_value(params.color_contrast, -1.0, 1.0);
    params.monochrome = clamp_value(params.monochrome, 0.0, 1.0);
    params.split_shadows_hue = clamp_value(params.split_shadows_hue, 0.0, 1.0);
    params.split_highlights_hue = clamp_value(params.split_highlights_hue, 0.0, 1.0);
    params.split_balance = clamp_value(params.split_balance, 0.0, 1.0);
    params.split_amount = clamp_value(params.split_amount, 0.0, 1.0);
    params.gamma = clamp_value(params.gamma, 0.2, 3.0);
}

bool DevelopParams::is_identity() const noexcept
{
    return near(temperature, kDevelopTemperatureDefault) && near(tint, 0.0) &&
           near(exposure_ev, 0.0) && near(contrast, 0.0) && near(highlights, 0.0) &&
           near(shadows, 0.0) && near(whites, 0.0) && near(blacks, 0.0) && near(vibrance, 0.0) &&
           near(saturation, 0.0) && rotate_quarters % 4 == 0 && flip_horizontal == 0 &&
           flip_vertical == 0 && near(crop_x, 0.0) && near(crop_y, 0.0) && near(crop_width, 1.0) &&
           near(crop_height, 1.0) && near(sharpen, 0.0) && near(clarity, 0.0) &&
           near(vignette, 0.0) && near(grain, 0.0) && near(bloom, 0.0) && near(soften, 0.0) &&
           near(dehaze, 0.0) && near(velvia, 0.0) && near(lift, 0.0) && near(color_gamma, 0.0) &&
           near(gain, 0.0) && near(color_contrast, 0.0) && near(monochrome, 0.0) &&
           near(split_amount, 0.0) && near(gamma, kDevelopGammaDefault);
}

bool apply_develop_field(DevelopParams &params, const std::string_view name, const double value)
{
    if (name == "temperature")
    {
        params.temperature = value;
    }
    else if (name == "tint")
    {
        params.tint = value;
    }
    else if (name == "exposure")
    {
        params.exposure_ev = value;
    }
    else if (name == "contrast")
    {
        params.contrast = value;
    }
    else if (name == "highlights")
    {
        params.highlights = value;
    }
    else if (name == "shadows")
    {
        params.shadows = value;
    }
    else if (name == "whites")
    {
        params.whites = value;
    }
    else if (name == "blacks")
    {
        params.blacks = value;
    }
    else if (name == "vibrance")
    {
        params.vibrance = value;
    }
    else if (name == "saturation")
    {
        params.saturation = value;
    }
    else if (name == "cropX")
    {
        params.crop_x = value;
    }
    else if (name == "cropY")
    {
        params.crop_y = value;
    }
    else if (name == "cropWidth")
    {
        params.crop_width = value;
    }
    else if (name == "cropHeight")
    {
        params.crop_height = value;
    }
    else if (name == "sharpen")
    {
        params.sharpen = value;
    }
    else if (name == "sharpenRadius")
    {
        params.sharpen_radius = value;
    }
    else if (name == "clarity")
    {
        params.clarity = value;
    }
    else if (name == "vignette")
    {
        params.vignette = value;
    }
    else if (name == "grain")
    {
        params.grain = value;
    }
    else if (name == "bloom")
    {
        params.bloom = value;
    }
    else if (name == "soften")
    {
        params.soften = value;
    }
    else if (name == "dehaze")
    {
        params.dehaze = value;
    }
    else if (name == "velvia")
    {
        params.velvia = value;
    }
    else if (name == "lift")
    {
        params.lift = value;
    }
    else if (name == "colorGamma")
    {
        params.color_gamma = value;
    }
    else if (name == "gain")
    {
        params.gain = value;
    }
    else if (name == "colorContrast")
    {
        params.color_contrast = value;
    }
    else if (name == "monochrome")
    {
        params.monochrome = value;
    }
    else if (name == "splitShadowsHue")
    {
        params.split_shadows_hue = value;
    }
    else if (name == "splitHighlightsHue")
    {
        params.split_highlights_hue = value;
    }
    else if (name == "splitBalance")
    {
        params.split_balance = value;
    }
    else if (name == "splitAmount")
    {
        params.split_amount = value;
    }
    else if (name == "gamma")
    {
        params.gamma = value;
    }
    else
    {
        return false;
    }
    clamp_develop(params);
    return true;
}

bool reset_develop_field(DevelopParams &params, const std::string_view name)
{
    DevelopParams identity;
    if (name == "temperature")
    {
        params.temperature = identity.temperature;
    }
    else if (name == "tint")
    {
        params.tint = identity.tint;
    }
    else if (name == "exposure")
    {
        params.exposure_ev = identity.exposure_ev;
    }
    else if (name == "contrast")
    {
        params.contrast = identity.contrast;
    }
    else if (name == "highlights")
    {
        params.highlights = identity.highlights;
    }
    else if (name == "shadows")
    {
        params.shadows = identity.shadows;
    }
    else if (name == "whites")
    {
        params.whites = identity.whites;
    }
    else if (name == "blacks")
    {
        params.blacks = identity.blacks;
    }
    else if (name == "vibrance")
    {
        params.vibrance = identity.vibrance;
    }
    else if (name == "saturation")
    {
        params.saturation = identity.saturation;
    }
    else if (name == "rotate")
    {
        params.rotate_quarters = 0;
    }
    else if (name == "flip")
    {
        params.flip_horizontal = 0;
        params.flip_vertical = 0;
    }
    else if (name == "crop" || name == "cropX" || name == "cropY" || name == "cropWidth" ||
             name == "cropHeight")
    {
        params.crop_x = 0.0;
        params.crop_y = 0.0;
        params.crop_width = 1.0;
        params.crop_height = 1.0;
    }
    else if (name == "sharpen" || name == "sharpenRadius")
    {
        params.sharpen = identity.sharpen;
        if (name == "sharpenRadius")
        {
            params.sharpen_radius = identity.sharpen_radius;
        }
    }
    else if (name == "clarity")
    {
        params.clarity = identity.clarity;
    }
    else if (name == "vignette")
    {
        params.vignette = identity.vignette;
    }
    else if (name == "grain")
    {
        params.grain = identity.grain;
    }
    else if (name == "bloom")
    {
        params.bloom = identity.bloom;
    }
    else if (name == "soften")
    {
        params.soften = identity.soften;
    }
    else if (name == "dehaze")
    {
        params.dehaze = identity.dehaze;
    }
    else if (name == "velvia")
    {
        params.velvia = identity.velvia;
    }
    else if (name == "lift")
    {
        params.lift = identity.lift;
    }
    else if (name == "colorGamma")
    {
        params.color_gamma = identity.color_gamma;
    }
    else if (name == "gain")
    {
        params.gain = identity.gain;
    }
    else if (name == "colorContrast")
    {
        params.color_contrast = identity.color_contrast;
    }
    else if (name == "monochrome")
    {
        params.monochrome = identity.monochrome;
    }
    else if (name == "splitShadowsHue")
    {
        params.split_shadows_hue = identity.split_shadows_hue;
    }
    else if (name == "splitHighlightsHue")
    {
        params.split_highlights_hue = identity.split_highlights_hue;
    }
    else if (name == "splitBalance")
    {
        params.split_balance = identity.split_balance;
    }
    else if (name == "splitAmount")
    {
        params.split_amount = identity.split_amount;
    }
    else if (name == "gamma")
    {
        params.gamma = identity.gamma;
    }
    else
    {
        return false;
    }
    clamp_develop(params);
    return true;
}

bool reset_develop_section(DevelopParams &params, const std::string_view section)
{
    DevelopParams identity;
    if (section == "geometry")
    {
        params.rotate_quarters = 0;
        params.flip_horizontal = 0;
        params.flip_vertical = 0;
        params.crop_x = 0.0;
        params.crop_y = 0.0;
        params.crop_width = 1.0;
        params.crop_height = 1.0;
    }
    else if (section == "whiteBalance")
    {
        params.temperature = identity.temperature;
        params.tint = identity.tint;
    }
    else if (section == "light")
    {
        params.exposure_ev = identity.exposure_ev;
        params.contrast = identity.contrast;
        params.highlights = identity.highlights;
        params.shadows = identity.shadows;
        params.whites = identity.whites;
        params.blacks = identity.blacks;
        params.gamma = identity.gamma;
    }
    else if (section == "color")
    {
        params.vibrance = identity.vibrance;
        params.saturation = identity.saturation;
        params.velvia = identity.velvia;
        params.lift = identity.lift;
        params.color_gamma = identity.color_gamma;
        params.gain = identity.gain;
        params.color_contrast = identity.color_contrast;
        params.monochrome = identity.monochrome;
        params.split_shadows_hue = identity.split_shadows_hue;
        params.split_highlights_hue = identity.split_highlights_hue;
        params.split_balance = identity.split_balance;
        params.split_amount = identity.split_amount;
    }
    else if (section == "detail")
    {
        params.sharpen = identity.sharpen;
        params.sharpen_radius = identity.sharpen_radius;
        params.clarity = identity.clarity;
        params.grain = identity.grain;
    }
    else if (section == "effects")
    {
        params.vignette = identity.vignette;
        params.bloom = identity.bloom;
        params.soften = identity.soften;
        params.dehaze = identity.dehaze;
    }
    else
    {
        return false;
    }
    clamp_develop(params);
    return true;
}

bool apply_crop_aspect(DevelopParams &params, const std::string_view aspect)
{
    if (aspect == "free")
    {
        return true;
    }
    double ratio = 0.0;
    if (aspect == "1:1")
    {
        ratio = 1.0;
    }
    else if (aspect == "3:2")
    {
        ratio = 3.0 / 2.0;
    }
    else if (aspect == "4:3")
    {
        ratio = 4.0 / 3.0;
    }
    else if (aspect == "5:4")
    {
        ratio = 5.0 / 4.0;
    }
    else if (aspect == "16:9")
    {
        ratio = 16.0 / 9.0;
    }
    else
    {
        return false;
    }

    const double box_w = params.crop_width;
    const double box_h = params.crop_height;
    const double box_x = params.crop_x;
    const double box_y = params.crop_y;
    double new_w = box_w;
    double new_h = box_h;
    if (box_w / std::max(box_h, kEpsilon) > ratio)
    {
        new_w = box_h * ratio;
        new_h = box_h;
    }
    else
    {
        new_w = box_w;
        new_h = box_w / ratio;
    }
    params.crop_width = new_w;
    params.crop_height = new_h;
    params.crop_x = box_x + (box_w - new_w) * 0.5;
    params.crop_y = box_y + (box_h - new_h) * 0.5;
    clamp_develop(params);
    return true;
}

void strip_crop_operations(Recipe &recipe)
{
    recipe.operations.erase(std::remove_if(recipe.operations.begin(), recipe.operations.end(),
                                           [](const OperationInstance &operation)
                                           { return operation.id == "ravo.geometry.crop"; }),
                            recipe.operations.end());
}

Result<Recipe> recipe_from_develop(AssetDescriptor asset, const DevelopParams &params)
{
    DevelopParams clamped = params;
    clamp_develop(clamped);
    Recipe recipe;
    recipe.asset = std::move(asset);
    if (!near(clamped.temperature, kDevelopTemperatureDefault) || !near(clamped.tint, 0.0))
    {
        add_operation(recipe, "ravo.color.white_balance", "white-balance-1",
                      {{"temperature", ParameterValue{clamped.temperature}},
                       {"tint", ParameterValue{clamped.tint}}});
    }
    if (!near(clamped.exposure_ev, 0.0))
    {
        add_operation(recipe, "ravo.core.exposure", "exposure-1",
                      {{"exposure_ev", ParameterValue{clamped.exposure_ev}}});
    }
    if (!near(clamped.highlights, 0.0))
    {
        add_operation(recipe, "ravo.core.highlights", "highlights-1",
                      {{"amount", ParameterValue{clamped.highlights}}});
    }
    if (!near(clamped.shadows, 0.0))
    {
        add_operation(recipe, "ravo.core.shadows", "shadows-1",
                      {{"amount", ParameterValue{clamped.shadows}}});
    }
    if (!near(clamped.whites, 0.0))
    {
        add_operation(recipe, "ravo.core.whites", "whites-1",
                      {{"amount", ParameterValue{clamped.whites}}});
    }
    if (!near(clamped.blacks, 0.0))
    {
        add_operation(recipe, "ravo.core.blacks", "blacks-1",
                      {{"amount", ParameterValue{clamped.blacks}}});
    }
    if (!near(clamped.contrast, 0.0))
    {
        add_operation(recipe, "ravo.core.contrast", "contrast-1",
                      {{"amount", ParameterValue{clamped.contrast}}});
    }
    if (!near(clamped.gamma, kDevelopGammaDefault))
    {
        add_operation(recipe, "ravo.core.gamma", "gamma-1",
                      {{"gamma", ParameterValue{clamped.gamma}}});
    }
    if (!near(clamped.lift, 0.0) || !near(clamped.color_gamma, 0.0) || !near(clamped.gain, 0.0))
    {
        add_operation(recipe, "ravo.color.colorbalance", "colorbalance-1",
                      {{"lift", ParameterValue{clamped.lift}},
                       {"gamma", ParameterValue{clamped.color_gamma}},
                       {"gain", ParameterValue{clamped.gain}}});
    }
    if (!near(clamped.color_contrast, 0.0))
    {
        add_operation(recipe, "ravo.color.colorcontrast", "colorcontrast-1",
                      {{"amount", ParameterValue{clamped.color_contrast}}});
    }
    if (!near(clamped.velvia, 0.0))
    {
        add_operation(recipe, "ravo.color.velvia", "velvia-1",
                      {{"amount", ParameterValue{clamped.velvia}}, {"bias", ParameterValue{1.0}}});
    }
    if (!near(clamped.vibrance, 0.0))
    {
        add_operation(recipe, "ravo.color.vibrance", "vibrance-1",
                      {{"amount", ParameterValue{clamped.vibrance}}});
    }
    if (!near(clamped.saturation, 0.0))
    {
        add_operation(recipe, "ravo.color.saturation", "saturation-1",
                      {{"amount", ParameterValue{clamped.saturation}}});
    }
    if (!near(clamped.monochrome, 0.0))
    {
        add_operation(recipe, "ravo.color.monochrome", "monochrome-1",
                      {{"amount", ParameterValue{clamped.monochrome}}});
    }
    if (!near(clamped.split_amount, 0.0))
    {
        add_operation(recipe, "ravo.color.splittoning", "splittoning-1",
                      {{"shadows_hue", ParameterValue{clamped.split_shadows_hue}},
                       {"highlights_hue", ParameterValue{clamped.split_highlights_hue}},
                       {"balance", ParameterValue{clamped.split_balance}},
                       {"amount", ParameterValue{clamped.split_amount}}});
    }
    if (!near(clamped.sharpen, 0.0))
    {
        add_operation(recipe, "ravo.detail.sharpen", "sharpen-1",
                      {{"amount", ParameterValue{clamped.sharpen}},
                       {"radius", ParameterValue{clamped.sharpen_radius}},
                       {"threshold", ParameterValue{0.0}}});
    }
    if (!near(clamped.clarity, 0.0))
    {
        add_operation(recipe, "ravo.detail.clarity", "clarity-1",
                      {{"amount", ParameterValue{clamped.clarity}}});
    }
    if (!near(clamped.bloom, 0.0))
    {
        add_operation(recipe, "ravo.effect.bloom", "bloom-1",
                      {{"amount", ParameterValue{clamped.bloom}}});
    }
    if (!near(clamped.soften, 0.0))
    {
        add_operation(recipe, "ravo.effect.soften", "soften-1",
                      {{"amount", ParameterValue{clamped.soften}}});
    }
    if (!near(clamped.dehaze, 0.0))
    {
        add_operation(recipe, "ravo.effect.dehaze", "dehaze-1",
                      {{"amount", ParameterValue{clamped.dehaze}}});
    }
    if (!near(clamped.vignette, 0.0))
    {
        add_operation(recipe, "ravo.effect.vignette", "vignette-1",
                      {{"amount", ParameterValue{clamped.vignette}},
                       {"midpoint", ParameterValue{0.5}},
                       {"falloff", ParameterValue{0.5}}});
    }
    if (!near(clamped.grain, 0.0))
    {
        add_operation(recipe, "ravo.effect.grain", "grain-1",
                      {{"amount", ParameterValue{clamped.grain}}});
    }
    if (clamped.rotate_quarters % 4 != 0)
    {
        add_operation(recipe, "ravo.geometry.rotate", "rotate-1",
                      {{"quarters", ParameterValue{clamped.rotate_quarters % 4}}});
    }
    if (clamped.flip_horizontal != 0 || clamped.flip_vertical != 0)
    {
        add_operation(recipe, "ravo.geometry.flip", "flip-1",
                      {{"horizontal", ParameterValue{clamped.flip_horizontal}},
                       {"vertical", ParameterValue{clamped.flip_vertical}}});
    }
    if (!near(clamped.crop_x, 0.0) || !near(clamped.crop_y, 0.0) ||
        !near(clamped.crop_width, 1.0) || !near(clamped.crop_height, 1.0))
    {
        add_operation(recipe, "ravo.geometry.crop", "crop-1",
                      {{"x", ParameterValue{clamped.crop_x}},
                       {"y", ParameterValue{clamped.crop_y}},
                       {"width", ParameterValue{clamped.crop_width}},
                       {"height", ParameterValue{clamped.crop_height}}});
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
        const auto integer = [&](const std::string_view name, const std::int64_t fallback)
        {
            const auto found = operation.parameters.find(std::string(name));
            if (found == operation.parameters.end())
            {
                return fallback;
            }
            return as_integer(found->second, fallback);
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
        else if (operation.id == "ravo.core.gamma")
        {
            params.gamma = number("gamma", params.gamma);
        }
        else if (operation.id == "ravo.color.vibrance")
        {
            params.vibrance = number("amount", params.vibrance);
        }
        else if (operation.id == "ravo.color.saturation")
        {
            params.saturation = number("amount", params.saturation);
        }
        else if (operation.id == "ravo.color.velvia")
        {
            params.velvia = number("amount", params.velvia);
        }
        else if (operation.id == "ravo.color.colorbalance")
        {
            params.lift = number("lift", params.lift);
            params.color_gamma = number("gamma", params.color_gamma);
            params.gain = number("gain", params.gain);
        }
        else if (operation.id == "ravo.color.colorcontrast")
        {
            params.color_contrast = number("amount", params.color_contrast);
        }
        else if (operation.id == "ravo.color.monochrome")
        {
            params.monochrome = number("amount", params.monochrome);
        }
        else if (operation.id == "ravo.color.splittoning")
        {
            params.split_shadows_hue = number("shadows_hue", params.split_shadows_hue);
            params.split_highlights_hue = number("highlights_hue", params.split_highlights_hue);
            params.split_balance = number("balance", params.split_balance);
            params.split_amount = number("amount", params.split_amount);
        }
        else if (operation.id == "ravo.detail.sharpen")
        {
            params.sharpen = number("amount", params.sharpen);
            params.sharpen_radius = number("radius", params.sharpen_radius);
        }
        else if (operation.id == "ravo.detail.clarity")
        {
            params.clarity = number("amount", params.clarity);
        }
        else if (operation.id == "ravo.effect.vignette")
        {
            params.vignette = number("amount", params.vignette);
        }
        else if (operation.id == "ravo.effect.grain")
        {
            params.grain = number("amount", params.grain);
        }
        else if (operation.id == "ravo.effect.bloom")
        {
            params.bloom = number("amount", params.bloom);
        }
        else if (operation.id == "ravo.effect.soften")
        {
            params.soften = number("amount", params.soften);
        }
        else if (operation.id == "ravo.effect.dehaze")
        {
            params.dehaze = number("amount", params.dehaze);
        }
        else if (operation.id == "ravo.geometry.rotate")
        {
            params.rotate_quarters = integer("quarters", 0) % 4;
            if (params.rotate_quarters < 0)
            {
                params.rotate_quarters += 4;
            }
        }
        else if (operation.id == "ravo.geometry.flip")
        {
            params.flip_horizontal = flag01(integer("horizontal", 0));
            params.flip_vertical = flag01(integer("vertical", 0));
        }
        else if (operation.id == "ravo.geometry.crop")
        {
            params.crop_x = number("x", params.crop_x);
            params.crop_y = number("y", params.crop_y);
            params.crop_width = number("width", params.crop_width);
            params.crop_height = number("height", params.crop_height);
        }
    }
    clamp_develop(params);
    return params;
}

} // namespace ravo
