#include "color_balance_rgb.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <new>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

#include "parallel_rows.h"

namespace ravo
{
namespace
{

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kHalfPi = 0.5F * kPi;
constexpr float kTwoPi = 2.0F * kPi;
constexpr float kMaskPower = 0.4101205819200422F;
constexpr float kUcsLStarRange = 2.098883786377F;
constexpr float kUcsLStarUpper = 2.09885F;
constexpr std::size_t kGamutLutSize = 512;
constexpr int kJzGamutSteps = 92;
constexpr float kD65X = 0.3127F;
constexpr float kD65Y = 0.3290F;
constexpr float kFilmlightWhiteR = 0.21902143F;
constexpr float kFilmlightWhiteG = 0.54371398F;

using Vec3 = std::array<float, 3>;
using Vec4 = std::array<float, 4>;
using Mat3 = std::array<Vec3, 3>;
using GamutLut = std::array<float, kGamutLutSize>;

constexpr Mat3 kLinearSrgbToXyzD65 = {
    Vec3{0.4124564F, 0.3575761F, 0.1804375F},
    Vec3{0.2126729F, 0.7151522F, 0.0721750F},
    Vec3{0.0193339F, 0.1191920F, 0.9503041F},
};
constexpr Mat3 kXyzD50ToD65Cat16 = {
    Vec3{0.989466254F, -0.0400304626F, 0.0440530317F},
    Vec3{-0.00540518733F, 1.00666069F, -0.00175551955F},
    Vec3{-0.000403920992F, 0.0150768030F, 1.30210211F},
};
constexpr Mat3 kXyzD65ToD50Cat16 = {
    Vec3{1.01085433F, 0.0407086103F, -0.0341445825F},
    Vec3{0.00542814201F, 0.993581926F, 0.00115592039F},
    Vec3{0.000250722468F, -0.0114918759F, 0.767964947F},
};
constexpr Mat3 kXyzD65ToLms2006 = {
    Vec3{0.257085F, 0.859943F, -0.031061F},
    Vec3{-0.394427F, 1.175800F, 0.106423F},
    Vec3{0.064856F, -0.076250F, 0.559067F},
};
constexpr Mat3 kLms2006ToXyzD65 = {
    Vec3{1.80794659F, -1.29971660F, 0.34785879F},
    Vec3{0.61783960F, 0.39595453F, -0.04104687F},
    Vec3{-0.12546960F, 0.20478038F, 1.74274183F},
};
constexpr Mat3 kGradingRgbToLms = {
    Vec3{0.95F, 0.38F, 0.00F},
    Vec3{0.05F, 0.62F, 0.03F},
    Vec3{0.00F, 0.00F, 0.97F},
};
constexpr Mat3 kLmsToGradingRgb = {
    Vec3{1.08771930F, -0.66666667F, 0.02061856F},
    Vec3{-0.0877193F, 1.66666667F, -0.05154639F},
    Vec3{0.0F, 0.0F, 1.03092784F},
};
constexpr Mat3 kJzAi = {
    Vec3{1.0F, 0.1386050432715393F, 0.0580473161561189F},
    Vec3{1.0F, -0.1386050432715393F, -0.0580473161561189F},
    Vec3{1.0F, -0.0960192420263190F, -0.8118918960560390F},
};

[[nodiscard]] Vec3 mat_apply(const Mat3 &matrix, const Vec3 &input) noexcept
{
    Vec3 output{};
    for (std::size_t row = 0; row < output.size(); ++row)
    {
        output[row] = std::fma(matrix[row][0], input[0],
                               std::fma(matrix[row][1], input[1], matrix[row][2] * input[2]));
    }
    return output;
}

[[nodiscard]] Mat3 mat_multiply(const Mat3 &left, const Mat3 &right) noexcept
{
    Mat3 output{};
    for (std::size_t row = 0; row < output.size(); ++row)
    {
        for (std::size_t column = 0; column < output[row].size(); ++column)
        {
            output[row][column] =
                std::fma(left[row][0], right[0][column],
                         std::fma(left[row][1], right[1][column], left[row][2] * right[2][column]));
        }
    }
    return output;
}

[[nodiscard]] bool mat_inverse(const Mat3 &input, Mat3 &output) noexcept
{
    const float determinant =
        input[0][0] * (input[1][1] * input[2][2] - input[1][2] * input[2][1]) -
        input[0][1] * (input[1][0] * input[2][2] - input[1][2] * input[2][0]) +
        input[0][2] * (input[1][0] * input[2][1] - input[1][1] * input[2][0]);
    if (!std::isfinite(determinant) || std::abs(determinant) <= 1.0e-8F)
    {
        return false;
    }
    const float inverse = 1.0F / determinant;
    output = {{
        {(input[1][1] * input[2][2] - input[1][2] * input[2][1]) * inverse,
         (input[0][2] * input[2][1] - input[0][1] * input[2][2]) * inverse,
         (input[0][1] * input[1][2] - input[0][2] * input[1][1]) * inverse},
        {(input[1][2] * input[2][0] - input[1][0] * input[2][2]) * inverse,
         (input[0][0] * input[2][2] - input[0][2] * input[2][0]) * inverse,
         (input[0][2] * input[1][0] - input[0][0] * input[1][2]) * inverse},
        {(input[1][0] * input[2][1] - input[1][1] * input[2][0]) * inverse,
         (input[0][1] * input[2][0] - input[0][0] * input[2][1]) * inverse,
         (input[0][0] * input[1][1] - input[0][1] * input[1][0]) * inverse},
    }};
    return true;
}

[[nodiscard]] bool finite(const Vec3 &value) noexcept
{
    return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
}

[[nodiscard]] Mat3 working_rgb_to_xyz_d50() noexcept
{
    return mat_multiply(kXyzD65ToD50Cat16, kLinearSrgbToXyzD65);
}

[[nodiscard]] Mat3 working_rgb_to_xyz_d65() noexcept
{
    return mat_multiply(kXyzD50ToD65Cat16, working_rgb_to_xyz_d50());
}

[[nodiscard]] Mat3 working_rgb_to_lms() noexcept
{
    return mat_multiply(kXyzD65ToLms2006, working_rgb_to_xyz_d65());
}

[[nodiscard]] bool output_matrix(Mat3 &output) noexcept
{
    Mat3 xyz_d50_to_working{};
    if (!mat_inverse(working_rgb_to_xyz_d50(), xyz_d50_to_working))
    {
        return false;
    }
    output = mat_multiply(xyz_d50_to_working, kXyzD65ToD50Cat16);
    return true;
}

[[nodiscard]] Vec3 lms_to_yrg(const Vec3 &lms) noexcept
{
    const float luminance = std::fma(0.68990272F, lms[0], 0.34832189F * lms[1]);
    const float sum = lms[0] + lms[1] + lms[2];
    const Vec3 normalized = sum == 0.0F ? Vec3{} : Vec3{lms[0] / sum, lms[1] / sum, lms[2] / sum};
    const Vec3 grading = mat_apply(kLmsToGradingRgb, normalized);
    return {luminance, grading[0], grading[1]};
}

[[nodiscard]] Vec3 yrg_to_lms(const Vec3 &yrg) noexcept
{
    const Vec3 grading{yrg[1], yrg[2], 1.0F - yrg[1] - yrg[2]};
    const Vec3 normalized = mat_apply(kGradingRgbToLms, grading);
    const float denominator = std::fma(0.68990272F, normalized[0], 0.34832189F * normalized[1]);
    const float scale = denominator == 0.0F ? 0.0F : yrg[0] / denominator;
    return {normalized[0] * scale, normalized[1] * scale, normalized[2] * scale};
}

[[nodiscard]] Vec4 yrg_to_ych(const Vec3 &yrg) noexcept
{
    const float red = yrg[1] - kFilmlightWhiteR;
    const float green = yrg[2] - kFilmlightWhiteG;
    const float chroma = std::hypot(red, green);
    return {yrg[0], chroma, chroma != 0.0F ? red / chroma : 1.0F,
            chroma != 0.0F ? green / chroma : 0.0F};
}

[[nodiscard]] Vec3 ych_to_yrg(const Vec4 &ych) noexcept
{
    return {ych[0], std::fma(ych[1], ych[2], kFilmlightWhiteR),
            std::fma(ych[1], ych[3], kFilmlightWhiteG)};
}

[[nodiscard]] Vec3 ych_to_grading_rgb(const Vec4 &ych) noexcept
{
    return mat_apply(kLmsToGradingRgb, yrg_to_lms(ych_to_yrg(ych)));
}

void gamut_check_yrg(Vec4 &ych) noexcept
{
    const Vec3 yrg = ych_to_yrg(ych);
    float maximum = ych[1];
    if (yrg[1] < 0.0F)
    {
        maximum = std::min(-kFilmlightWhiteR / ych[2], maximum);
    }
    if (yrg[2] < 0.0F)
    {
        maximum = std::min(-kFilmlightWhiteG / ych[3], maximum);
    }
    if (yrg[1] + yrg[2] > 1.0F)
    {
        maximum =
            std::min((1.0F - kFilmlightWhiteR - kFilmlightWhiteG) / (ych[2] + ych[3]), maximum);
    }
    ych[1] = maximum;
}

[[nodiscard]] Vec4 make_ych(const float luminance, const float chroma,
                            const float conventional_hue_degrees) noexcept
{
    const float hue = (conventional_hue_degrees - 30.0F) * kPi / 180.0F;
    return {luminance, chroma, std::cos(hue), std::sin(hue)};
}

[[nodiscard]] float y_to_ucs_l_star(const float luminance) noexcept
{
    const float y_hat = std::pow(std::max(luminance, 0.0F), 0.631651345306265F);
    return kUcsLStarRange * y_hat / (y_hat + 1.12426773749357F);
}

[[nodiscard]] float ucs_l_star_to_y(const float l_star) noexcept
{
    return std::pow(1.12426773749357F * l_star / (kUcsLStarRange - l_star), 1.5831518565279648F);
}

void xyz_to_xyy(Vec3 xyz, Vec3 &xyy) noexcept
{
    for (float &sample : xyz)
    {
        sample = std::max(sample, 0.0F);
    }
    const float sum = xyz[0] + xyz[1] + xyz[2];
    xyy = {sum > 0.0F ? xyz[0] / sum : kD65X, sum > 0.0F ? xyz[1] / sum : kD65Y, xyz[1]};
}

[[nodiscard]] Vec3 xyy_to_xyz(const Vec3 &xyy) noexcept
{
    if (xyy[1] == 0.0F)
    {
        return {};
    }
    return {xyy[2] * xyy[0] / xyy[1], xyy[2], xyy[2] * (1.0F - xyy[0] - xyy[1]) / xyy[1]};
}

[[nodiscard]] std::array<float, 2> xyy_to_ucs_uv(const Vec3 &xyy) noexcept
{
    const Vec3 x_factors{-0.783941002840055F, 0.745273540913283F, 0.318707282433486F};
    const Vec3 y_factors{0.277512987809202F, -0.205375866083878F, 2.16743692732158F};
    const Vec3 offsets{0.153836578598858F, -0.165478376301988F, 0.291320554395942F};
    Vec3 uvd{};
    for (std::size_t channel = 0; channel < uvd.size(); ++channel)
    {
        uvd[channel] = std::fma(x_factors[channel], xyy[0],
                                std::fma(y_factors[channel], xyy[1], offsets[channel]));
    }
    const float divisor = uvd[2] >= 0.0F ? std::max(uvd[2], std::numeric_limits<float>::min()) :
                                           std::min(uvd[2], -std::numeric_limits<float>::min());
    uvd[0] /= divisor;
    uvd[1] /= divisor;
    const float u_star = 1.39656225667F * uvd[0] / (std::abs(uvd[0]) + 1.49217352929F);
    const float v_star = 1.4513954287F * uvd[1] / (std::abs(uvd[1]) + 1.52488637914F);
    return {-1.124983854323892F * u_star - 0.980483721769325F * v_star,
            1.86323315098672F * u_star + 1.971853092390862F * v_star};
}

[[nodiscard]] Vec3 ucs_luv_to_jch(const float l_star, const float l_white,
                                  const std::array<float, 2> &uv) noexcept
{
    const float colorfulness_squared = uv[0] * uv[0] + uv[1] * uv[1];
    return {l_star / l_white,
            15.932993652962535F * std::pow(l_star, 0.6523997524738018F) *
                std::pow(colorfulness_squared, 0.6007557017508491F) / l_white,
            std::atan2(uv[1], uv[0])};
}

[[nodiscard]] Vec3 xyy_to_ucs_jch(const Vec3 &xyy, const float l_white) noexcept
{
    return ucs_luv_to_jch(y_to_ucs_l_star(xyy[2]), l_white, xyy_to_ucs_uv(xyy));
}

[[nodiscard]] Vec3 ucs_jch_to_xyy(const Vec3 &jch, const float l_white) noexcept
{
    const float l_star = std::clamp(jch[0] * l_white, 0.0F, kUcsLStarUpper);
    const float colorfulness =
        l_star != 0.0F ? std::pow(jch[1] * l_white /
                                      (15.932993652962535F * std::pow(l_star, 0.6523997524738018F)),
                                  0.8322850678616855F) :
                         0.0F;
    const float u_prime = colorfulness * std::cos(jch[2]);
    const float v_prime = colorfulness * std::sin(jch[2]);
    const float u_star = -5.037522385190711F * u_prime - 2.504856328185843F * v_prime;
    const float v_star = 4.760029407436461F * u_prime + 2.874012963239247F * v_prime;
    const float u = -1.49217352929F * u_star / (std::abs(u_star) - 1.39656225667F);
    const float v = -1.52488637914F * v_star / (std::abs(v_star) - 1.4513954287F);
    const Vec3 xyd{
        0.167171472114775F * u + 0.141299802443708F * v - 0.00801531300850582F,
        -0.150959086409163F * u - 0.155185060382272F * v - 0.00843312433578007F,
        0.940254742367256F * u + v - 0.0256325967652889F,
    };
    const float divisor = xyd[2] >= 0.0F ? std::max(xyd[2], std::numeric_limits<float>::min()) :
                                           std::min(xyd[2], -std::numeric_limits<float>::min());
    return {xyd[0] / divisor, xyd[1] / divisor, ucs_l_star_to_y(l_star)};
}

[[nodiscard]] Vec3 ucs_jch_to_hcb(const Vec3 &jch) noexcept
{
    return {jch[2], jch[1], jch[0] * (std::pow(jch[1], 1.33654221029386F) + 1.0F)};
}

[[nodiscard]] Vec3 ucs_hcb_to_jch(const Vec3 &hcb) noexcept
{
    return {hcb[2] / (std::pow(hcb[1], 1.33654221029386F) + 1.0F), hcb[1], hcb[0]};
}

[[nodiscard]] Vec3 ucs_jch_to_hsb(const Vec3 &jch) noexcept
{
    const float brightness = jch[0] * (std::pow(jch[1], 1.33654221029386F) + 1.0F);
    return {jch[2], brightness > 0.0F ? jch[1] / brightness : 0.0F, brightness};
}

[[nodiscard]] Vec3 ucs_hsb_to_jch(const Vec3 &hsb) noexcept
{
    const float chroma = hsb[1] * hsb[2];
    return {hsb[2] / (std::pow(chroma, 1.33654221029386F) + 1.0F), chroma, hsb[0]};
}

[[nodiscard]] float delta_h(float left, const float right) noexcept
{
    left -= right;
    left += left < -kPi ? kTwoPi : 0.0F;
    left -= left > kPi ? kTwoPi : 0.0F;
    return left;
}

[[nodiscard]] float soft_clip(const float value, const float soft_threshold,
                              const float hard_threshold) noexcept
{
    const float range = hard_threshold - soft_threshold;
    if (range <= std::numeric_limits<float>::epsilon())
    {
        return std::min(value, hard_threshold);
    }
    return value > soft_threshold ?
               soft_threshold + (1.0F - std::exp(-(value - soft_threshold) / range)) * range :
               value;
}

[[nodiscard]] float lookup_gamut(const GamutLut &lut, const float hue) noexcept
{
    const float coordinate = static_cast<float>(kGamutLutSize) * (hue + kPi) / kTwoPi;
    const float previous = std::floor(coordinate);
    const float next = std::ceil(coordinate);
    const int first = static_cast<int>(previous) & (static_cast<int>(kGamutLutSize) - 1);
    const int second = static_cast<int>(next) & (static_cast<int>(kGamutLutSize) - 1);
    const float value = lut[static_cast<std::size_t>(first)];
    return value + (first != second ?
                        (coordinate - previous) * (lut[static_cast<std::size_t>(second)] - value) :
                        0.0F);
}

[[nodiscard]] Result<GamutLut> build_dt_ucs_gamut_lut(const Mat3 &rgb_to_xyz_d65,
                                                      const CancellationToken &cancellation)
{
    GamutLut lut{};
    GamutLut samples{};
    Vec3 red_xyy{};
    Vec3 green_xyy{};
    Vec3 blue_xyy{};
    xyz_to_xyy(mat_apply(rgb_to_xyz_d65, {1.0F, 0.0F, 0.0F}), red_xyy);
    xyz_to_xyy(mat_apply(rgb_to_xyz_d65, {0.0F, 1.0F, 0.0F}), green_xyy);
    xyz_to_xyy(mat_apply(rgb_to_xyz_d65, {0.0F, 0.0F, 1.0F}), blue_xyy);
    const float red_hue = std::atan2(red_xyy[1] - kD65Y, red_xyy[0] - kD65X);
    const float green_hue = std::atan2(green_xyy[1] - kD65Y, green_xyy[0] - kD65X);
    const float blue_hue = std::atan2(blue_xyy[1] - kD65Y, blue_xyy[0] - kD65X);

    constexpr int sample_count = 50 * static_cast<int>(kGamutLutSize);
    for (int index = 0; index < sample_count; ++index)
    {
        if ((index & 511) == 0)
        {
            auto active = cancellation.check();
            if (!active)
            {
                return active.error();
            }
        }
        const float angle =
            -kPi + static_cast<float>(index) / static_cast<float>(sample_count) * kTwoPi;
        const float tangent = std::tan(angle);
        const float t1 = delta_h(angle, blue_hue) / delta_h(red_hue, blue_hue);
        const float t2 = delta_h(angle, red_hue) / delta_h(green_hue, red_hue);
        const float t3 = delta_h(angle, green_hue) / delta_h(blue_hue, green_hue);
        float x = 0.0F;
        float y = 0.0F;
        if (t1 >= 0.0F && t1 <= 1.0F)
        {
            const float t = (kD65Y - blue_xyy[1] + tangent * (blue_xyy[0] - kD65X)) /
                            (red_xyy[1] - blue_xyy[1] + tangent * (blue_xyy[0] - red_xyy[0]));
            x = std::fma(t, red_xyy[0] - blue_xyy[0], blue_xyy[0]);
            y = std::fma(t, red_xyy[1] - blue_xyy[1], blue_xyy[1]);
        }
        else if (t2 >= 0.0F && t2 <= 1.0F)
        {
            const float t = (kD65Y - red_xyy[1] + tangent * (red_xyy[0] - kD65X)) /
                            (green_xyy[1] - red_xyy[1] + tangent * (red_xyy[0] - green_xyy[0]));
            x = std::fma(t, green_xyy[0] - red_xyy[0], red_xyy[0]);
            y = std::fma(t, green_xyy[1] - red_xyy[1], red_xyy[1]);
        }
        else if (t3 >= 0.0F && t3 <= 1.0F)
        {
            const float t = (kD65Y - green_xyy[1] + tangent * (green_xyy[0] - kD65X)) /
                            (blue_xyy[1] - green_xyy[1] + tangent * (green_xyy[0] - blue_xyy[0]));
            x = std::fma(t, blue_xyy[0] - green_xyy[0], green_xyy[0]);
            y = std::fma(t, blue_xyy[1] - green_xyy[1], green_xyy[1]);
        }
        const auto uv = xyy_to_ucs_uv({x, y, 1.0F});
        const float hue = std::atan2(uv[1], uv[0]);
        int lut_index = static_cast<int>(
            std::lround(static_cast<float>(kGamutLutSize - 1U) * (hue + kPi) / kTwoPi));
        lut_index += lut_index < 0 ? static_cast<int>(kGamutLutSize) : 0;
        lut_index -=
            lut_index >= static_cast<int>(kGamutLutSize) ? static_cast<int>(kGamutLutSize) : 0;
        const auto position = static_cast<std::size_t>(lut_index);
        lut[position] += uv[0] * uv[0] + uv[1] * uv[1];
        samples[position] += 1.0F;
    }
    for (std::size_t index = 0; index < lut.size(); ++index)
    {
        lut[index] /= std::max(1.0F, samples[index]);
        if (!std::isfinite(lut[index]))
        {
            return make_error(ErrorCode::kInternal,
                              "Color Balance RGB DT UCS gamut LUT is non-finite",
                              {{"index", std::to_string(index)}});
        }
    }
    return lut;
}

[[nodiscard]] Vec3 xyz_to_jzazbz(const Vec3 &xyz_d65) noexcept
{
    constexpr float b = 1.15F;
    constexpr float g = 0.66F;
    constexpr float c1 = 0.8359375F;
    constexpr float c2 = 18.8515625F;
    constexpr float c3 = 18.6875F;
    constexpr float n = 0.159301758F;
    constexpr float p = 134.034375F;
    constexpr float d = -0.56F;
    constexpr float d0 = 1.6295499532821566e-11F;
    constexpr Mat3 m = {
        Vec3{0.41478972F, 0.57999900F, 0.01464800F},
        Vec3{-0.2015100F, 1.1206490F, 0.0531008F},
        Vec3{-0.0166008F, 0.2648000F, 0.6684799F},
    };
    constexpr Mat3 a = {
        Vec3{0.5F, 0.5F, 0.0F},
        Vec3{3.524000F, -4.066708F, 0.542708F},
        Vec3{0.199076F, 1.096799F, -1.295875F},
    };
    const Vec3 adjusted{b * xyz_d65[0] - (b - 1.0F) * xyz_d65[2],
                        g * xyz_d65[1] - (g - 1.0F) * xyz_d65[0], xyz_d65[2]};
    Vec3 lms = mat_apply(m, adjusted);
    for (float &sample : lms)
    {
        sample = std::pow(std::max(sample / 10000.0F, 0.0F), n);
        sample = std::pow((c1 + c2 * sample) / (1.0F + c3 * sample), p);
    }
    Vec3 result = mat_apply(a, lms);
    result[0] = std::max((1.0F + d) * result[0] / (1.0F + d * result[0]) - d0, 0.0F);
    return result;
}

[[nodiscard]] Vec3 jzazbz_to_xyz(const Vec3 &jzazbz) noexcept
{
    constexpr float b = 1.15F;
    constexpr float g = 0.66F;
    constexpr float c1 = 0.8359375F;
    constexpr float c2 = 18.8515625F;
    constexpr float c3 = 18.6875F;
    constexpr float inverse_n = 1.0F / 0.159301758F;
    constexpr float inverse_p = 1.0F / 134.034375F;
    constexpr float d = -0.56F;
    constexpr float d0 = 1.6295499532821566e-11F;
    constexpr Mat3 mi = {
        Vec3{1.9242264357876067F, -1.0047923125953657F, 0.0376514040306180F},
        Vec3{0.3503167620949991F, 0.7264811939316552F, -0.0653844229480850F},
        Vec3{-0.0909828109828475F, -0.3127282905230739F, 1.5227665613052603F},
    };
    Vec3 iz{jzazbz[0] + d0, jzazbz[1], jzazbz[2]};
    iz[0] = std::max(iz[0] / (1.0F + d - d * iz[0]), 0.0F);
    Vec3 lms = mat_apply(kJzAi, iz);
    for (float &sample : lms)
    {
        sample = std::pow(std::max(sample, 0.0F), inverse_p);
        sample = (c1 - sample) / (c3 * sample - c2);
        sample = 10000.0F * std::pow(std::max(sample, 0.0F), inverse_n);
    }
    const Vec3 adjusted = mat_apply(mi, lms);
    Vec3 xyz{};
    xyz[0] = (adjusted[0] + (b - 1.0F) * adjusted[2]) / b;
    xyz[1] = (adjusted[1] + (g - 1.0F) * xyz[0]) / g;
    xyz[2] = adjusted[2];
    return xyz;
}

[[nodiscard]] Result<GamutLut> build_jzazbz_gamut_lut(const Mat3 &rgb_to_xyz_d65,
                                                      const CancellationToken &cancellation)
{
    GamutLut sampler{};
    for (int red = 0; red < kJzGamutSteps; ++red)
    {
        auto active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
        for (int green = 0; green < kJzGamutSteps; ++green)
        {
            for (int blue = 0; blue < kJzGamutSteps; ++blue)
            {
                const float denominator = static_cast<float>(kJzGamutSteps - 1);
                const Vec3 rgb{static_cast<float>(red) / denominator,
                               static_cast<float>(green) / denominator,
                               static_cast<float>(blue) / denominator};
                const Vec3 jab = xyz_to_jzazbz(mat_apply(rgb_to_xyz_d65, rgb));
                const float chroma = std::hypot(jab[1], jab[2]);
                const float saturation = jab[0] > 0.0F ? chroma / jab[0] : 0.0F;
                const float hue = std::atan2(jab[2], jab[1]);
                int index = static_cast<int>(
                    std::lround(static_cast<float>(kGamutLutSize - 1U) * (hue + kPi) / kTwoPi));
                index += index < 0 ? static_cast<int>(kGamutLutSize) : 0;
                index -=
                    index >= static_cast<int>(kGamutLutSize) ? static_cast<int>(kGamutLutSize) : 0;
                sampler[static_cast<std::size_t>(index)] =
                    std::max(sampler[static_cast<std::size_t>(index)], saturation);
            }
        }
    }
    GamutLut lut{};
    for (std::size_t index = 0; index < lut.size(); ++index)
    {
        float sum = 0.0F;
        for (int offset = -2; offset <= 2; ++offset)
        {
            const auto wrapped =
                (static_cast<int>(index) + offset + static_cast<int>(kGamutLutSize)) &
                (static_cast<int>(kGamutLutSize) - 1);
            sum += sampler[static_cast<std::size_t>(wrapped)];
        }
        lut[index] = sum / 5.0F;
        if (!std::isfinite(lut[index]))
        {
            return make_error(ErrorCode::kInternal,
                              "Color Balance RGB JzAzBz gamut LUT is non-finite",
                              {{"index", std::to_string(index)}});
        }
    }
    return lut;
}

struct DerivedParams
{
    Vec3 global{};
    Vec3 shadows{};
    Vec3 highlights{};
    Vec3 midtones{};
    Vec3 chroma{};
    Vec3 saturation{};
    Vec3 brilliance{};
    float midtones_y = 1.0F;
    float chroma_global = 0.0F;
    float vibrance = 0.0F;
    float contrast = 1.0F;
    float saturation_global = 0.0F;
    float brilliance_global = 0.0F;
    float hue_angle = 0.0F;
    float shadows_weight = 4.0F;
    float highlights_weight = 4.0F;
    float midtones_weight = 8.0F;
    float mask_grey_fulcrum = 0.5F;
    float white_fulcrum = 1.0F;
    float grey_fulcrum = 0.1845F;
    bool jzazbz = false;
};

[[nodiscard]] DerivedParams derive(const ColorBalanceRgbParams &params) noexcept
{
    DerivedParams result;
    result.vibrance = static_cast<float>(params.vibrance);
    result.contrast = 1.0F + static_cast<float>(params.contrast);
    result.grey_fulcrum = static_cast<float>(params.grey_fulcrum);
    result.chroma_global = static_cast<float>(params.chroma_global);
    result.chroma = {static_cast<float>(params.chroma_shadows),
                     static_cast<float>(params.chroma_midtones),
                     static_cast<float>(params.chroma_highlights)};
    result.saturation_global = static_cast<float>(params.saturation_global);
    result.saturation = {static_cast<float>(params.saturation_shadows),
                         static_cast<float>(params.saturation_midtones),
                         static_cast<float>(params.saturation_highlights)};
    result.brilliance_global = static_cast<float>(params.brilliance_global);
    result.brilliance = {static_cast<float>(params.brilliance_shadows),
                         static_cast<float>(params.brilliance_midtones),
                         static_cast<float>(params.brilliance_highlights)};
    result.hue_angle = static_cast<float>(params.hue_rotation) * kPi / 180.0F;

    const Vec3 normalization = ych_to_grading_rgb({1.0F, 0.0F, 1.0F, 0.0F});
    const auto make_grade = [&](const double chroma, const double hue)
    {
        return ych_to_grading_rgb(
            make_ych(1.0F, static_cast<float>(chroma), static_cast<float>(hue)));
    };
    result.global = make_grade(params.global_chroma, params.global_hue);
    result.shadows = make_grade(params.shadows_chroma, params.shadows_hue);
    result.highlights = make_grade(params.highlights_chroma, params.highlights_hue);
    result.midtones = make_grade(params.midtones_chroma, params.midtones_hue);
    for (std::size_t channel = 0; channel < 3; ++channel)
    {
        result.global[channel] = result.global[channel] - normalization[channel] +
                                 normalization[channel] * static_cast<float>(params.global_y);
        result.shadows[channel] = 1.0F + result.shadows[channel] - normalization[channel] +
                                  static_cast<float>(params.shadows_y);
        result.highlights[channel] = 1.0F + result.highlights[channel] - normalization[channel] +
                                     static_cast<float>(params.highlights_y);
        result.midtones[channel] =
            1.0F / (1.0F + result.midtones[channel] - normalization[channel]);
    }
    result.midtones_y = 1.0F / (1.0F + static_cast<float>(params.midtones_y));
    result.white_fulcrum = std::exp2(static_cast<float>(params.white_fulcrum_ev));
    result.shadows_weight = 2.0F + static_cast<float>(params.shadows_falloff) * 2.0F;
    result.highlights_weight = 2.0F + static_cast<float>(params.highlights_falloff) * 2.0F;
    const float shadows_squared = result.shadows_weight * result.shadows_weight;
    const float highlights_squared = result.highlights_weight * result.highlights_weight;
    result.midtones_weight =
        shadows_squared * highlights_squared / (shadows_squared + highlights_squared);
    result.mask_grey_fulcrum = std::pow(static_cast<float>(params.mask_grey_fulcrum), kMaskPower);
    result.jzazbz = params.saturation_formula == kColorBalanceRgbFormulaJzAzBz2021;
    return result;
}

[[nodiscard]] ColorBalanceRgbMasks opacity_masks(const float luminance,
                                                 const DerivedParams &params) noexcept
{
    const float transformed = std::pow(std::max(luminance, 0.0F), kMaskPower);
    const float offset = transformed - params.mask_grey_fulcrum;
    const float normalized = offset / params.mask_grey_fulcrum;
    const float shadows = 1.0F / (1.0F + std::exp(normalized * params.shadows_weight));
    const float highlights = 1.0F / (1.0F + std::exp(-normalized * params.highlights_weight));
    const float shadows_complement = 1.0F - shadows;
    const float highlights_complement = 1.0F - highlights;
    const float midtones = std::exp(-(offset * offset) * params.midtones_weight / 4.0F) *
                           shadows_complement * shadows_complement * highlights_complement *
                           highlights_complement * 8.0F;
    return {{shadows, midtones, highlights},
            {shadows_complement, 1.0F - midtones, highlights_complement}};
}

[[nodiscard]] float dot_masks(const std::array<float, 3> &masks, const Vec3 &values) noexcept
{
    return std::fma(masks[0], values[0], std::fma(masks[1], values[1], masks[2] * values[2]));
}

[[nodiscard]] Result<void> apply_jzazbz(Vec3 &xyz, const ColorBalanceRgbMasks &masks,
                                        const DerivedParams &params, const GamutLut &gamut)
{
    Vec3 jab = xyz_to_jzazbz(xyz);
    Vec3 jc{jab[0], std::hypot(jab[1], jab[2]), 0.0F};
    const float hue = std::atan2(jab[2], jab[1]);
    const float angle = std::atan2(jc[1], jc[0]);
    const float sine = std::sin(angle);
    const float cosine = std::cos(angle);
    const float brilliance =
        1.0F + params.brilliance_global + dot_masks(masks.opacity, params.brilliance);
    const float saturation = params.saturation_global + dot_masks(masks.opacity, params.saturation);
    float along = std::fma(jc[0], cosine, jc[1] * sine);
    float orthogonal = along * std::clamp(angle * saturation, -angle, kHalfPi - angle);
    along = std::max(along * brilliance, 0.0F);
    jc[0] = std::max(std::fma(along, cosine, -orthogonal * sine), 0.0F);
    jc[1] = std::max(std::fma(along, sine, orthogonal * cosine), 0.0F);

    const float maximum_saturation = lookup_gamut(gamut, hue);
    const float clipped_saturation =
        jc[0] > 0.0F ? soft_clip(jc[1] / jc[0], 0.8F * maximum_saturation, maximum_saturation) :
                       maximum_saturation;
    const float maximum_chroma = jc[0] * clipped_saturation;
    const float maximum_lightness = clipped_saturation > 0.0F ? jc[1] / clipped_saturation : jc[0];
    jc[0] = 0.5F * (jc[0] + maximum_lightness);
    jc[1] = 0.5F * (jc[1] + maximum_chroma);

    auto clipped = color_balance_rgb_jzazbz_negative_lms_clip(jc[0], jc[1], hue);
    if (!clipped)
    {
        return clipped.error();
    }
    const float cosine_hue = std::cos(hue);
    const float sine_hue = std::sin(hue);
    jab = {jc[0], clipped.value().chroma * cosine_hue, clipped.value().chroma * sine_hue};
    xyz = jzazbz_to_xyz(jab);
    return {};
}

[[nodiscard]] Result<void> apply_dt_ucs(Vec3 &xyz, const ColorBalanceRgbMasks &masks,
                                        const DerivedParams &params, const GamutLut &gamut,
                                        const float l_white)
{
    Vec3 xyy{};
    xyz_to_xyy(xyz, xyy);
    Vec3 jch = xyy_to_ucs_jch(xyy, l_white);
    Vec3 hcb = ucs_jch_to_hcb(jch);
    const float radius = std::hypot(hcb[1], hcb[2]);
    const float sine = radius > 0.0F ? hcb[1] / radius : 0.0F;
    const float cosine = radius > 0.0F ? hcb[2] / radius : 0.0F;
    const float positive_chroma = std::max(std::numeric_limits<float>::min(), hcb[1]);
    const float brightness = std::fma(sine, hcb[1], cosine * hcb[2]);
    float saturation = std::max(
        1.0F + params.saturation_global + dot_masks(masks.opacity, params.saturation), 0.0F);
    const float brilliance = std::max(
        1.0F + params.brilliance_global + dot_masks(masks.opacity, params.brilliance), 0.0F);
    const float maximum_saturation = std::hypot(positive_chroma, brightness) / positive_chroma;
    saturation = soft_clip(saturation, 0.5F * maximum_saturation, maximum_saturation);
    const float chroma_delta = (saturation - 1.0F) * positive_chroma;
    const float radicand = positive_chroma * positive_chroma * (1.0F - saturation * saturation) +
                           brightness * brightness;
    const float brightness_prime = std::sqrt(std::max(radicand, 0.0F)) * brilliance;
    hcb[1] = std::max(std::fma(cosine, chroma_delta, sine * brightness_prime), 0.0F);
    hcb[2] = std::max(std::fma(-sine, chroma_delta, cosine * brightness_prime), 0.0F);
    jch = ucs_hcb_to_jch(hcb);

    const float maximum_colorfulness = lookup_gamut(gamut, jch[2]);
    const float maximum_chroma = 15.932993652962535F *
                                 std::pow(jch[0] * l_white, 0.6523997524738018F) *
                                 std::pow(maximum_colorfulness, 0.6007557017508491F) / l_white;
    const Vec3 boundary = ucs_jch_to_hsb({jch[0], maximum_chroma, jch[2]});
    Vec3 hsb{hcb[0], hcb[2] > 0.0F ? hcb[1] / hcb[2] : 0.0F, hcb[2]};
    hsb[1] = soft_clip(hsb[1], 0.8F * boundary[1], boundary[1]);
    jch = ucs_hsb_to_jch(hsb);
    xyz = xyy_to_xyz(ucs_jch_to_xyy(jch, l_white));
    return {};
}

} // namespace

ColorBalanceRgbMasks color_balance_rgb_opacity_masks(const float luminance,
                                                     const ColorBalanceRgbParams &params) noexcept
{
    return opacity_masks(luminance, derive(params));
}

std::array<float, 4>
color_balance_rgb_working_to_ych(const std::array<float, 3> &working_rgb) noexcept
{
    Vec3 sanitized = working_rgb;
    for (float &sample : sanitized)
    {
        sample = std::max(sample, 0.0F);
    }
    return yrg_to_ych(lms_to_yrg(mat_apply(working_rgb_to_lms(), sanitized)));
}

std::array<float, 3> color_balance_rgb_ych_to_grading_rgb(const std::array<float, 4> &ych) noexcept
{
    return ych_to_grading_rgb(ych);
}

Result<ColorBalanceRgbJzClip> color_balance_rgb_jzazbz_negative_lms_clip(const float lightness,
                                                                         const float chroma,
                                                                         const float hue_radians)
{
    constexpr float d0 = 1.6295499532821566e-11F;
    constexpr float d = -0.56F;
    const float cosine = std::cos(hue_radians);
    const float sine = std::sin(hue_radians);
    const float iz = std::max((lightness + d0) / (1.0F + d - d * (lightness + d0)), 0.0F);
    const Vec3 lms = mat_apply(kJzAi, {iz, chroma * cosine, chroma * sine});
    ColorBalanceRgbJzClip result{chroma, false};
    for (std::size_t channel = 0; channel < lms.size(); ++channel)
    {
        if (lms[channel] >= 0.0F)
        {
            continue;
        }
        const float denominator = std::fma(kJzAi[channel][1], cosine, kJzAi[channel][2] * sine);
        if (denominator >= 0.0F)
        {
            return make_error(ErrorCode::kInternal,
                              "Color Balance RGB JzAzBz negative LMS clip is singular");
        }
        result.chroma = std::min(-iz / denominator, result.chroma);
        result.clipped = true;
    }
    if (!std::isfinite(result.chroma) || result.chroma < 0.0F)
    {
        return make_error(ErrorCode::kInternal,
                          "Color Balance RGB JzAzBz negative LMS clip is non-finite");
    }
    return result;
}

Result<void> apply_color_balance_rgb(WorkingImage &image, const OperationInstance &operation,
                                     const CancellationToken &cancellation)
{
    auto parsed = color_balance_rgb_from_parameters(operation.parameters);
    if (!parsed)
    {
        return parsed.error();
    }
    const ColorBalanceRgbParams &source = parsed.value();
    if (source.is_identity())
    {
        return {};
    }
    const std::size_t expected =
        static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 3U;
    if (image.width == 0 || image.height == 0 || image.rgb.size() != expected)
    {
        return make_error(ErrorCode::kValidation,
                          "Color Balance RGB input buffer is empty or undersized");
    }
    std::mutex error_mutex;
    std::size_t first_error_index = std::numeric_limits<std::size_t>::max();
    std::optional<TaskError> first_error;
    const auto remember_error = [&](const std::size_t index, TaskError error)
    {
        const std::lock_guard lock(error_mutex);
        if (index < first_error_index)
        {
            first_error_index = index;
            first_error = std::move(error);
        }
    };
    auto validated = detail::for_each_row(
        image.height, cancellation,
        [&](const std::uint32_t row)
        {
            for (std::uint32_t column = 0; column < image.width; ++column)
            {
                const std::size_t index =
                    (static_cast<std::size_t>(row) * image.width + column) * 3U;
                const Vec3 input{image.rgb[index], image.rgb[index + 1U], image.rgb[index + 2U]};
                if (!finite(input))
                {
                    remember_error(
                        index, make_error(ErrorCode::kValidation,
                                          "Color Balance RGB input contains a non-finite sample",
                                          {{"sample_index", std::to_string(index)}}));
                    return;
                }
            }
        });
    if (!validated)
    {
        return validated.error();
    }
    if (first_error)
    {
        return std::move(*first_error);
    }

    const DerivedParams params = derive(source);
    Mat3 xyz_to_working{};
    if (!output_matrix(xyz_to_working))
    {
        return make_error(ErrorCode::kInternal,
                          "Color Balance RGB working profile matrix is not invertible");
    }
    const Mat3 rgb_to_xyz_d65 = working_rgb_to_xyz_d65();
    const Mat3 rgb_to_lms = working_rgb_to_lms();
    Result<GamutLut> gamut = params.jzazbz ? build_jzazbz_gamut_lut(rgb_to_xyz_d65, cancellation) :
                                             build_dt_ucs_gamut_lut(rgb_to_xyz_d65, cancellation);
    if (!gamut)
    {
        return gamut.error();
    }
    const float l_white = y_to_ucs_l_star(params.white_fulcrum);
    const float hue_cosine = std::cos(params.hue_angle);
    const float hue_sine = std::sin(params.hue_angle);
    std::vector<float> output;
    try
    {
        output.resize(image.rgb.size());
    }
    catch (const std::bad_alloc &)
    {
        return make_error(ErrorCode::kInternal, "Color Balance RGB ran out of memory");
    }

    first_error_index = std::numeric_limits<std::size_t>::max();
    first_error.reset();
    auto processed = detail::for_each_row(
        image.height, cancellation,
        [&](const std::uint32_t row)
        {
            for (std::uint32_t column = 0; column < image.width; ++column)
            {
                const std::size_t index =
                    (static_cast<std::size_t>(row) * image.width + column) * 3U;
                Vec3 working{std::max(image.rgb[index], 0.0F),
                             std::max(image.rgb[index + 1U], 0.0F),
                             std::max(image.rgb[index + 2U], 0.0F)};
                Vec3 lms = mat_apply(rgb_to_lms, working);
                Vec3 yrg = lms_to_yrg(lms);
                Vec4 ych = yrg_to_ych(yrg);
                ych[0] = std::max(ych[0], 0.0F);
                const ColorBalanceRgbMasks masks = opacity_masks(ych[0], params);

                const float hue_x = ych[2];
                const float hue_y = ych[3];
                ych[2] = std::fma(hue_cosine, hue_x, -hue_sine * hue_y);
                ych[3] = std::fma(hue_sine, hue_x, hue_cosine * hue_y);
                const float chroma_boost =
                    params.chroma_global + dot_masks(masks.opacity, params.chroma);
                const float vibrance =
                    params.vibrance * (1.0F - std::pow(ych[1], std::abs(params.vibrance)));
                ych[1] *= std::max(1.0F + chroma_boost + vibrance, 0.0F);
                gamut_check_yrg(ych);

                yrg = ych_to_yrg(ych);
                lms = yrg_to_lms(yrg);
                Vec3 grading = mat_apply(kLmsToGradingRgb, lms);
                for (std::size_t channel = 0; channel < grading.size(); ++channel)
                {
                    grading[channel] += params.global[channel];
                    grading[channel] *=
                        masks.complement[2] *
                            (masks.complement[0] + masks.opacity[0] * params.shadows[channel]) +
                        masks.opacity[2] * params.highlights[channel];
                    const float sign = grading[channel] < 0.0F ? -1.0F : 1.0F;
                    grading[channel] = std::pow(std::abs(grading[channel]) / params.white_fulcrum,
                                                params.midtones[channel]) *
                                       sign * params.white_fulcrum;
                }
                lms = mat_apply(kGradingRgbToLms, grading);
                yrg = lms_to_yrg(lms);
                yrg[0] =
                    std::pow(std::max(yrg[0] / params.white_fulcrum, 0.0F), params.midtones_y) *
                    params.white_fulcrum;
                yrg[0] =
                    params.grey_fulcrum * std::pow(yrg[0] / params.grey_fulcrum, params.contrast);
                lms = yrg_to_lms(yrg);
                Vec3 xyz = mat_apply(kLms2006ToXyzD65, lms);
                Result<void> adjusted =
                    params.jzazbz ? apply_jzazbz(xyz, masks, params, gamut.value()) :
                                    apply_dt_ucs(xyz, masks, params, gamut.value(), l_white);
                if (!adjusted)
                {
                    remember_error(index, adjusted.error());
                    return;
                }
                working = mat_apply(xyz_to_working, xyz);
                for (float &sample : working)
                {
                    sample = std::max(sample, 0.0F);
                }
                if (!finite(working))
                {
                    remember_error(index,
                                   make_error(ErrorCode::kValidation,
                                              "Color Balance RGB produced a non-finite sample",
                                              {{"sample_index", std::to_string(index)}}));
                    return;
                }
                output[index] = working[0];
                output[index + 1U] = working[1];
                output[index + 2U] = working[2];
            }
        });
    if (!processed)
    {
        return processed.error();
    }
    if (first_error)
    {
        return std::move(*first_error);
    }
    image.rgb.swap(output);
    return {};
}

} // namespace ravo
