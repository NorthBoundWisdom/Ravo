#pragma once

#include <algorithm>

namespace ravo::hsl
{

inline void rgb_to_hsl(const float red, const float green, const float blue, float &hue,
                       float &saturation, float &lightness) noexcept
{
    const float maximum = std::max(red, std::max(green, blue));
    const float minimum = std::min(red, std::min(green, blue));
    lightness = (maximum + minimum) * 0.5F;
    if (maximum <= minimum + 1.0e-6F)
    {
        hue = 0.0F;
        saturation = 0.0F;
        return;
    }
    const float delta = maximum - minimum;
    saturation =
        lightness > 0.5F ? delta / (2.0F - maximum - minimum) : delta / (maximum + minimum);
    if (maximum == red)
        hue = (green - blue) / delta + (green < blue ? 6.0F : 0.0F);
    else if (maximum == green)
        hue = (blue - red) / delta + 2.0F;
    else
        hue = (red - green) / delta + 4.0F;
    hue /= 6.0F;
}

[[nodiscard]] inline float hue_to_rgb(const float p, const float q, float value) noexcept
{
    if (value < 0.0F)
        value += 1.0F;
    if (value > 1.0F)
        value -= 1.0F;
    if (value < 1.0F / 6.0F)
        return p + (q - p) * 6.0F * value;
    if (value < 0.5F)
        return q;
    if (value < 2.0F / 3.0F)
        return p + (q - p) * (2.0F / 3.0F - value) * 6.0F;
    return p;
}

inline void hsl_to_rgb(const float hue, const float saturation, const float lightness, float &red,
                       float &green, float &blue) noexcept
{
    if (saturation <= 1.0e-6F)
    {
        red = green = blue = lightness;
        return;
    }
    const float q = lightness < 0.5F ? lightness * (1.0F + saturation) :
                                       lightness + saturation - lightness * saturation;
    const float p = 2.0F * lightness - q;
    red = hue_to_rgb(p, q, hue + 1.0F / 3.0F);
    green = hue_to_rgb(p, q, hue);
    blue = hue_to_rgb(p, q, hue - 1.0F / 3.0F);
}

} // namespace ravo::hsl
