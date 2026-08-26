#pragma once

#include <array>
#include <cmath>
#include <cstddef>

namespace ravo::d50_lab
{

using Triplet = std::array<float, 3>;

// Engine-private colour-science boundary. These helpers intentionally expose
// only value types; profile selection and publication remain callers'
// responsibilities.
[[nodiscard]] inline Triplet linear_rec709_to_xyz(const Triplet &rgb) noexcept
{
    return {0.4360747F * rgb[0] + 0.3850649F * rgb[1] + 0.1430804F * rgb[2],
            0.2225045F * rgb[0] + 0.7168786F * rgb[1] + 0.0606169F * rgb[2],
            0.0139322F * rgb[0] + 0.0971045F * rgb[1] + 0.7141733F * rgb[2]};
}

[[nodiscard]] inline Triplet xyz_to_linear_rec709(const Triplet &xyz) noexcept
{
    return {3.1338561F * xyz[0] + (-1.6168667F) * xyz[1] + (-0.4906146F) * xyz[2],
            (-0.9787684F) * xyz[0] + 1.9161415F * xyz[1] + 0.0334540F * xyz[2],
            0.0719453F * xyz[0] + (-0.2289914F) * xyz[1] + 1.4052427F * xyz[2]};
}

[[nodiscard]] inline Triplet xyz_to_lab(const Triplet &xyz) noexcept
{
    constexpr Triplet d50_inverse{1.0F / 0.9642F, 1.0F, 1.0F / 0.8249F};
    constexpr float epsilon = 216.0F / 24389.0F;
    constexpr float kappa = 24389.0F / 27.0F;
    Triplet transformed{};
    for (std::size_t channel = 0U; channel < transformed.size(); ++channel)
    {
        const float normalized = xyz[channel] * d50_inverse[channel];
        transformed[channel] =
            normalized > epsilon ? std::cbrt(normalized) : (kappa * normalized + 16.0F) / 116.0F;
    }
    return {116.0F * transformed[1] - 16.0F, 500.0F * (transformed[0] - transformed[1]),
            -200.0F * (transformed[2] - transformed[1])};
}

[[nodiscard]] inline Triplet lab_to_xyz(const Triplet &lab) noexcept
{
    constexpr Triplet d50{0.9642F, 1.0F, 0.8249F};
    constexpr Triplet offset{0.0F, 16.0F, 0.0F};
    constexpr Triplet coefficient{1.0F / 500.0F, 1.0F / 116.0F, -1.0F / 200.0F};
    constexpr Triplet add_coefficient{1.0F, 0.0F, 1.0F};
    constexpr float epsilon = 0.20689655172413796F;
    constexpr float kappa = 24389.0F / 27.0F;
    const Triplet reordered{lab[1], lab[0], lab[2]};
    Triplet scaled{};
    for (std::size_t channel = 0U; channel < scaled.size(); ++channel)
    {
        scaled[channel] = (reordered[channel] + offset[channel]) * coefficient[channel];
    }
    Triplet xyz{};
    for (std::size_t channel = 0U; channel < xyz.size(); ++channel)
    {
        const float value = scaled[channel] + scaled[1] * add_coefficient[channel];
        const float inverse =
            value > epsilon ? value * value * value : (116.0F * value - 16.0F) / kappa;
        xyz[channel] = d50[channel] * inverse;
    }
    return xyz;
}

} // namespace ravo::d50_lab
