#include "capability_ops.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

#include "ravo/recipe/develop.h"

namespace ravo
{
namespace
{

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kTwoPi = 2.0F * kPi;
constexpr int kColorNodes = 8;
constexpr int kUcsLutSize = 512;
constexpr int kToneChannels = 9;
constexpr int kTonePixelChan = 8;
constexpr int kToneLutResolution = 10000;
constexpr int kVignetteSplines = 512;
constexpr int kDenoiseBands = 7;
constexpr float kDenoisePFulcrum = 0.05F;
constexpr float kSatEffect = 2.0F;
constexpr float kBrightEffect = 8.0F;
constexpr float kUcsLStarRange = 2.098883786377F;
constexpr float kUcsLStarUpper = 2.09885F;
constexpr float kAngleShiftDeg = 20.0F;
constexpr float kGenericNoiseA = 5.0e-5F;
constexpr float kGenericNoiseB = 1.0e-6F;

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

[[nodiscard]] double parameter(const OperationInstance &operation, const std::string_view name,
                               const double fallback)
{
    const auto found = operation.parameters.find(std::string(name));
    if (found == operation.parameters.end())
    {
        return fallback;
    }
    return as_number(found->second, fallback);
}

[[nodiscard]] std::string parameter_string(const OperationInstance &operation,
                                           const std::string_view name, const std::string &fallback)
{
    const auto found = operation.parameters.find(std::string(name));
    if (found == operation.parameters.end())
    {
        return fallback;
    }
    if (const auto *text = std::get_if<std::string>(&found->second.value); text != nullptr)
    {
        return *text;
    }
    return fallback;
}

[[nodiscard]] Result<std::array<double, kColorEqualizerBandCount>>
parameter_band_array(const OperationInstance &operation, const std::string_view name)
{
    std::array<double, kColorEqualizerBandCount> values{};
    const auto found = operation.parameters.find(std::string(name));
    if (found == operation.parameters.end())
    {
        return values;
    }
    const auto *array = std::get_if<ParameterValue::Array>(&found->second.value);
    if (array == nullptr)
    {
        return make_error(ErrorCode::kValidation, "Color equalizer band parameter must be an array",
                          {{"parameter", std::string(name)}});
    }
    if (array->size() != kColorEqualizerBandCount)
    {
        return make_error(ErrorCode::kValidation, "Color equalizer band array must have 8 values",
                          {{"parameter", std::string(name)},
                           {"count", std::to_string(array->size())}});
    }
    for (std::size_t index = 0; index < values.size(); ++index)
    {
        const double value = as_number((*array)[index], std::numeric_limits<double>::quiet_NaN());
        if (!std::isfinite(value))
        {
            return make_error(ErrorCode::kValidation, "Color equalizer band value must be finite",
                              {{"parameter", std::string(name)}, {"index", std::to_string(index)}});
        }
        values[index] = value;
    }
    return values;
}

[[nodiscard]] bool ascii_iequals(const std::string_view left, const std::string_view right) noexcept
{
    if (left.size() != right.size())
    {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index)
    {
        const auto a = static_cast<unsigned char>(left[index]);
        const auto b = static_cast<unsigned char>(right[index]);
        if (std::tolower(a) != std::tolower(b))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] float clip01(const float value) noexcept
{
    return std::clamp(value, 0.0F, 1.0F);
}

void hsl_to_rgb(const float h, const float s, const float l, float &r, float &g, float &b) noexcept
{
    if (s <= 1.0e-6F)
    {
        r = g = b = l;
        return;
    }
    const auto hue_to_rgb = [](const float p, const float q, float t)
    {
        if (t < 0.0F)
        {
            t += 1.0F;
        }
        if (t > 1.0F)
        {
            t -= 1.0F;
        }
        if (t < 1.0F / 6.0F)
        {
            return p + (q - p) * 6.0F * t;
        }
        if (t < 0.5F)
        {
            return q;
        }
        if (t < 2.0F / 3.0F)
        {
            return p + (q - p) * (2.0F / 3.0F - t) * 6.0F;
        }
        return p;
    };
    const float q = l < 0.5F ? l * (1.0F + s) : l + s - l * s;
    const float p = 2.0F * l - q;
    r = hue_to_rgb(p, q, h + 1.0F / 3.0F);
    g = hue_to_rgb(p, q, h);
    b = hue_to_rgb(p, q, h - 1.0F / 3.0F);
}

[[nodiscard]] float sample_channel(const WorkingImage &image, const float x, const float y,
                                   const std::size_t channel) noexcept
{
    const float max_x = static_cast<float>(image.width - 1U);
    const float max_y = static_cast<float>(image.height - 1U);
    const float sx = std::clamp(x, 0.0F, max_x);
    const float sy = std::clamp(y, 0.0F, max_y);
    const std::uint32_t x0 = static_cast<std::uint32_t>(sx);
    const std::uint32_t y0 = static_cast<std::uint32_t>(sy);
    const std::uint32_t x1 = std::min(x0 + 1U, image.width - 1U);
    const std::uint32_t y1 = std::min(y0 + 1U, image.height - 1U);
    const float tx = sx - static_cast<float>(x0);
    const float ty = sy - static_cast<float>(y0);
    const auto at = [&](const std::uint32_t px, const std::uint32_t py)
    {
        return image.rgb[(static_cast<std::size_t>(py) * image.width + px) * 3U + channel];
    };
    const float top = at(x0, y0) * (1.0F - tx) + at(x1, y0) * tx;
    const float bottom = at(x0, y1) * (1.0F - tx) + at(x1, y1) * tx;
    return top * (1.0F - ty) + bottom * ty;
}

[[nodiscard]] std::uint8_t cfa_channel(const DecodedRaw &raw, const std::uint32_t x,
                                       const std::uint32_t y) noexcept
{
    return raw.cfa_channels[(y % raw.cfa_height) * raw.cfa_width + (x % raw.cfa_width)];
}

struct LensCalibration
{
    std::string_view camera_make;
    std::string_view camera_model;
    std::string_view lens;
    double focal_mm = 50.0;
    double k1 = 0.0;
    double k2 = 0.0;
    double tca_r = 1.0;
    double tca_b = 1.0;
    double vignetting = 0.0;
};

constexpr LensCalibration kLensCalibrations[] = {
    {"RavoTest", "RavoSensor", "FixtureLens", 50.0, -0.15, 0.02, 1.002, 0.998, 0.35},
};

[[nodiscard]] const LensCalibration *find_lens_calibration(const std::string_view make,
                                                           const std::string_view model,
                                                           const std::string_view lens,
                                                           const double focal_mm)
{
    const LensCalibration *best = nullptr;
    double best_delta = 1.0e9;
    for (const auto &entry : kLensCalibrations)
    {
        if (!ascii_iequals(entry.camera_make, make) || !ascii_iequals(entry.camera_model, model) ||
            !ascii_iequals(entry.lens, lens))
        {
            continue;
        }
        const double delta = std::abs(entry.focal_mm - focal_mm);
        if (delta <= 2.0 && delta < best_delta)
        {
            best = &entry;
            best_delta = delta;
        }
    }
    return best;
}

void blur_plane(std::vector<float> &plane, const std::uint32_t width, const std::uint32_t height,
                const float sigma)
{
    if (plane.empty() || width == 0 || height == 0 || sigma <= 0.01F)
    {
        return;
    }
    const int radius = std::max(1, static_cast<int>(std::ceil(static_cast<double>(sigma) * 3.0)));
    std::vector<float> kernel(static_cast<std::size_t>(radius) * 2U + 1U);
    float kernel_sum = 0.0F;
    const float denom = 2.0F * sigma * sigma;
    for (int offset = -radius; offset <= radius; ++offset)
    {
        const float weight = std::exp(-static_cast<float>(offset * offset) / denom);
        kernel[static_cast<std::size_t>(offset + radius)] = weight;
        kernel_sum += weight;
    }
    for (float &weight : kernel)
    {
        weight /= kernel_sum;
    }

    std::vector<float> temp(plane.size());
    for (std::uint32_t y = 0; y < height; ++y)
    {
        for (std::uint32_t x = 0; x < width; ++x)
        {
            float acc = 0.0F;
            for (int offset = -radius; offset <= radius; ++offset)
            {
                const int sample_x = std::clamp(static_cast<int>(x) + offset, 0,
                                                static_cast<int>(width) - 1);
                acc += plane[static_cast<std::size_t>(y) * width +
                             static_cast<std::uint32_t>(sample_x)] *
                       kernel[static_cast<std::size_t>(offset + radius)];
            }
            temp[static_cast<std::size_t>(y) * width + x] = acc;
        }
    }
    for (std::uint32_t x = 0; x < width; ++x)
    {
        for (std::uint32_t y = 0; y < height; ++y)
        {
            float acc = 0.0F;
            for (int offset = -radius; offset <= radius; ++offset)
            {
                const int sample_y = std::clamp(static_cast<int>(y) + offset, 0,
                                                static_cast<int>(height) - 1);
                acc += temp[static_cast<std::size_t>(static_cast<std::uint32_t>(sample_y)) * width +
                            x] *
                       kernel[static_cast<std::size_t>(offset + radius)];
            }
            plane[static_cast<std::size_t>(y) * width + x] = acc;
        }
    }
}

void box_blur_plane(const std::vector<float> &input, std::vector<float> &output,
                    const std::uint32_t width, const std::uint32_t height, const int radius)
{
    output.assign(input.size(), 0.0F);
    if (input.empty() || width == 0 || height == 0)
    {
        return;
    }
    if (radius <= 0)
    {
        output = input;
        return;
    }
    const int window = radius * 2 + 1;
    std::vector<float> temp(input.size());
    for (std::uint32_t y = 0; y < height; ++y)
    {
        double acc = 0.0;
        for (int x = -radius; x <= radius; ++x)
        {
            const int sx = std::clamp(x, 0, static_cast<int>(width) - 1);
            acc += input[static_cast<std::size_t>(y) * width + static_cast<std::uint32_t>(sx)];
        }
        for (std::uint32_t x = 0; x < width; ++x)
        {
            temp[static_cast<std::size_t>(y) * width + x] = static_cast<float>(acc / window);
            const int drop = std::clamp(static_cast<int>(x) - radius, 0, static_cast<int>(width) - 1);
            const int add =
                std::clamp(static_cast<int>(x) + radius + 1, 0, static_cast<int>(width) - 1);
            acc += input[static_cast<std::size_t>(y) * width + static_cast<std::uint32_t>(add)] -
                   input[static_cast<std::size_t>(y) * width + static_cast<std::uint32_t>(drop)];
        }
    }
    for (std::uint32_t x = 0; x < width; ++x)
    {
        double acc = 0.0;
        for (int y = -radius; y <= radius; ++y)
        {
            const int sy = std::clamp(y, 0, static_cast<int>(height) - 1);
            acc += temp[static_cast<std::size_t>(sy) * width + x];
        }
        for (std::uint32_t y = 0; y < height; ++y)
        {
            output[static_cast<std::size_t>(y) * width + x] = static_cast<float>(acc / window);
            const int drop = std::clamp(static_cast<int>(y) - radius, 0, static_cast<int>(height) - 1);
            const int add =
                std::clamp(static_cast<int>(y) + radius + 1, 0, static_cast<int>(height) - 1);
            acc += temp[static_cast<std::size_t>(add) * width + x] -
                   temp[static_cast<std::size_t>(drop) * width + x];
        }
    }
}

void guided_filter_plane(std::vector<float> &plane, const std::vector<float> &guide,
                         const std::uint32_t width, const std::uint32_t height, const int radius,
                         const float eps)
{
    if (plane.size() != guide.size() || radius <= 0)
    {
        return;
    }
    const std::size_t count = plane.size();
    std::vector<float> mean_i;
    std::vector<float> mean_p;
    std::vector<float> corr_i;
    std::vector<float> corr_ip;
    std::vector<float> prod_i(count);
    std::vector<float> prod_ip(count);
    for (std::size_t index = 0; index < count; ++index)
    {
        prod_i[index] = guide[index] * guide[index];
        prod_ip[index] = guide[index] * plane[index];
    }
    box_blur_plane(guide, mean_i, width, height, radius);
    box_blur_plane(plane, mean_p, width, height, radius);
    box_blur_plane(prod_i, corr_i, width, height, radius);
    box_blur_plane(prod_ip, corr_ip, width, height, radius);
    std::vector<float> a(count);
    std::vector<float> b(count);
    for (std::size_t index = 0; index < count; ++index)
    {
        const float var = std::max(0.0F, corr_i[index] - mean_i[index] * mean_i[index]);
        const float cov = corr_ip[index] - mean_i[index] * mean_p[index];
        a[index] = cov / (var + eps);
        b[index] = mean_p[index] - a[index] * mean_i[index];
    }
    std::vector<float> mean_a;
    std::vector<float> mean_b;
    box_blur_plane(a, mean_a, width, height, radius);
    box_blur_plane(b, mean_b, width, height, radius);
    for (std::size_t index = 0; index < count; ++index)
    {
        plane[index] = mean_a[index] * guide[index] + mean_b[index];
    }
}

bool cholesky_solve(std::vector<float> &a_square, std::vector<float> &y, const std::size_t n)
{
    std::vector<float> l(n * n, 0.0F);
    std::vector<float> b(n, 0.0F);
    std::vector<float> x(n, 0.0F);
    if (a_square[0] <= 0.0F)
    {
        return false;
    }
    for (std::size_t i = 0; i < n; ++i)
    {
        for (std::size_t j = 0; j < i + 1U; ++j)
        {
            float sum = 0.0F;
            for (std::size_t k = 0; k < j; ++k)
            {
                sum += l[i * n + k] * l[j * n + k];
            }
            if (i == j)
            {
                const float temp = a_square[i * n + i] - sum;
                if (temp <= 0.0F)
                {
                    return false;
                }
                l[i * n + j] = std::sqrt(temp);
            }
            else
            {
                if (l[j * n + j] == 0.0F)
                {
                    return false;
                }
                l[i * n + j] = (a_square[i * n + j] - sum) / l[j * n + j];
            }
        }
    }
    for (std::size_t i = 0; i < n; ++i)
    {
        float sum = y[i];
        for (std::size_t j = 0; j < i; ++j)
        {
            sum -= l[i * n + j] * b[j];
        }
        if (l[i * n + i] == 0.0F)
        {
            return false;
        }
        b[i] = sum / l[i * n + i];
    }
    for (int i = static_cast<int>(n) - 1; i >= 0; --i)
    {
        float sum = b[static_cast<std::size_t>(i)];
        for (int j = static_cast<int>(n) - 1; j > i; --j)
        {
            sum -= l[static_cast<std::size_t>(j) * n + static_cast<std::size_t>(i)] *
                   x[static_cast<std::size_t>(j)];
        }
        if (l[static_cast<std::size_t>(i) * n + static_cast<std::size_t>(i)] == 0.0F)
        {
            return false;
        }
        x[static_cast<std::size_t>(i)] =
            sum / l[static_cast<std::size_t>(i) * n + static_cast<std::size_t>(i)];
    }
    y = x;
    return true;
}

bool pseudo_solve(std::vector<float> &a, std::vector<float> &y, const std::size_t m,
                  const std::size_t n)
{
    if (m < n || n < 2)
    {
        return false;
    }
    std::vector<float> a_square(n * n, 0.0F);
    std::vector<float> y_square(n, 0.0F);
    for (std::size_t i = 0; i < n; ++i)
    {
        for (std::size_t j = 0; j < n; ++j)
        {
            float sum = 0.0F;
            for (std::size_t k = 0; k < m; ++k)
            {
                sum += a[k * n + i] * a[k * n + j];
            }
            a_square[i * n + j] = sum;
        }
        float sum = 0.0F;
        for (std::size_t k = 0; k < m; ++k)
        {
            sum += a[k * n + i] * y[k];
        }
        y_square[i] = sum;
    }
    if (!cholesky_solve(a_square, y_square, n))
    {
        return false;
    }
    for (std::size_t i = 0; i < n; ++i)
    {
        y[i] = y_square[i];
    }
    return true;
}

void rgb_to_xyz_d65(const float r, const float g, const float b, float &x, float &y,
                    float &z) noexcept
{
    x = 0.4124564F * r + 0.3575761F * g + 0.1804375F * b;
    y = 0.2126729F * r + 0.7151522F * g + 0.0721750F * b;
    z = 0.0193339F * r + 0.1191920F * g + 0.9503041F * b;
}

void xyz_d65_to_rgb(const float x, const float y, const float z, float &r, float &g,
                    float &b) noexcept
{
    r = 3.2404542F * x - 1.5371385F * y - 0.4985314F * z;
    g = -0.9692660F * x + 1.8760108F * y + 0.0415560F * z;
    b = 0.0556434F * x - 0.2040259F * y + 1.0572252F * z;
}

void xyz_to_xyy(const float x, const float y, const float z, float &xyx, float &xyy,
                float &xy_y) noexcept
{
    const float sum = x + y + z;
    if (sum <= 1.0e-12F)
    {
        xyx = 0.3127F;
        xyy = 0.3290F;
        xy_y = y;
        return;
    }
    xyx = x / sum;
    xyy = y / sum;
    xy_y = y;
}

void xyy_to_xyz(const float xyx, const float xyy, const float xy_y, float &x, float &y,
                float &z) noexcept
{
    if (xyy <= 1.0e-12F)
    {
        x = y = z = 0.0F;
        return;
    }
    x = xyx * xy_y / xyy;
    y = xy_y;
    z = (1.0F - xyx - xyy) * xy_y / xyy;
}

[[nodiscard]] float y_to_ucs_l_star(const float y) noexcept
{
    const float y_hat = std::pow(std::max(y, 0.0F), 0.631651345306265F);
    return kUcsLStarRange * y_hat / (y_hat + 1.12426773749357F);
}

[[nodiscard]] float ucs_l_star_to_y(const float l_star) noexcept
{
    const float clamped = std::clamp(l_star, 0.0F, kUcsLStarUpper);
    return std::pow((1.12426773749357F * clamped) / (kUcsLStarRange - clamped), 1.5831518565279648F);
}

void xyy_to_ucs_uv(const float xyx, const float xyy, float uv[2]) noexcept
{
    const float uvd0 = -0.783941002840055F * xyx + 0.277512987809202F * xyy + 0.153836578598858F;
    const float uvd1 = 0.745273540913283F * xyx - 0.205375866083878F * xyy - 0.165478376301988F;
    const float uvd2 = 0.318707282433486F * xyx + 2.16743692732158F * xyy + 0.291320554395942F;
    const float div = uvd2 >= 0.0F ? std::max(uvd2, std::numeric_limits<float>::min()) :
                                     std::min(uvd2, -std::numeric_limits<float>::min());
    const float uv0 = uvd0 / div;
    const float uv1 = uvd1 / div;
    const float uv_star0 = 1.39656225667F * uv0 / (std::abs(uv0) + 1.49217352929F);
    const float uv_star1 = 1.4513954287F * uv1 / (std::abs(uv1) + 1.52488637914F);
    uv[0] = -1.124983854323892F * uv_star0 - 0.980483721769325F * uv_star1;
    uv[1] = 1.86323315098672F * uv_star0 + 1.971853092390862F * uv_star1;
}

void ucs_luv_to_jch(const float l_star, const float l_white, const float uv[2], float jch[3]) noexcept
{
    const float m2 = uv[0] * uv[0] + uv[1] * uv[1];
    jch[0] = l_star / l_white;
    jch[1] = 15.932993652962535F * std::pow(std::max(l_star, 0.0F), 0.6523997524738018F) *
             std::pow(m2, 0.6007557017508491F) / l_white;
    jch[2] = std::atan2(uv[1], uv[0]);
}

void ucs_jch_to_hsb(const float jch[3], float hsb[3]) noexcept
{
    hsb[2] = jch[0] * (std::pow(std::max(jch[1], 0.0F), 1.33654221029386F) + 1.0F);
    hsb[1] = hsb[2] > 0.0F ? jch[1] / hsb[2] : 0.0F;
    hsb[0] = jch[2];
}

void ucs_hsb_to_jch(const float hsb[3], float jch[3]) noexcept
{
    jch[2] = hsb[0];
    jch[1] = hsb[1] * hsb[2];
    jch[0] = hsb[2] / (std::pow(std::max(jch[1], 0.0F), 1.33654221029386F) + 1.0F);
}

void ucs_jch_to_xyy(const float jch[3], const float l_white, float &xyx, float &xyy, float &xy_y)
{
    const float l_star = std::clamp(jch[0] * l_white, 0.0F, kUcsLStarUpper);
    const float m = l_star != 0.0F ?
                        std::pow(jch[1] * l_white /
                                     (15.932993652962535F * std::pow(l_star, 0.6523997524738018F)),
                                 0.8322850678616855F) :
                        0.0F;
    const float u_star_prime = m * std::cos(jch[2]);
    const float v_star_prime = m * std::sin(jch[2]);
    const float uv_star0 = -5.037522385190711F * u_star_prime - 2.504856328185843F * v_star_prime;
    const float uv_star1 = 4.760029407436461F * u_star_prime + 2.874012963239247F * v_star_prime;
    const float uv0 = -1.49217352929F * uv_star0 / (std::abs(uv_star0) - 1.39656225667F);
    const float uv1 = -1.52488637914F * uv_star1 / (std::abs(uv_star1) - 1.4513954287F);
    const float xyd0 = 0.167171472114775F * uv0 + 0.141299802443708F * uv1 - 0.00801531300850582F;
    const float xyd1 = -0.150959086409163F * uv0 - 0.155185060382272F * uv1 - 0.00843312433578007F;
    const float xyd2 = 0.940254742367256F * uv0 + 1.000000000000000F * uv1 - 0.0256325967652889F;
    const float div = xyd2 >= 0.0F ? std::max(xyd2, std::numeric_limits<float>::min()) :
                                     std::min(xyd2, -std::numeric_limits<float>::min());
    xyx = xyd0 / div;
    xyy = xyd1 / div;
    xy_y = ucs_l_star_to_y(l_star);
}

void ucs_hsb_to_rgb(const float hsb[3], const float l_white, float &r, float &g, float &b)
{
    float jch[3]{};
    ucs_hsb_to_jch(hsb, jch);
    float xyx = 0.0F;
    float xyy = 0.0F;
    float xy_y = 0.0F;
    ucs_jch_to_xyy(jch, l_white, xyx, xyy, xy_y);
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    xyy_to_xyz(xyx, xyy, xy_y, x, y, z);
    xyz_d65_to_rgb(x, y, z, r, g, b);
}

[[nodiscard]] float lookup_lut_periodic(const std::array<float, kUcsLutSize> &lut,
                                        const float hue) noexcept
{
    const float x_test = static_cast<float>(kUcsLutSize) * (hue + kPi) / kTwoPi;
    const float x_prev = std::floor(x_test);
    const float x_next = std::ceil(x_test);
    const int xi = static_cast<int>(x_prev) & (kUcsLutSize - 1);
    const int xii = static_cast<int>(x_next) & (kUcsLutSize - 1);
    const float y_prev = lut[static_cast<std::size_t>(xi)];
    return y_prev + ((xi != xii) ? (x_test - x_prev) * (lut[static_cast<std::size_t>(xii)] - y_prev) :
                                   0.0F);
}

[[nodiscard]] float conventional_hue_deg_to_ucs_rad(const float angle) noexcept
{
    return (angle + kAngleShiftDeg) * kPi / 180.0F;
}

void periodic_rbf_interpolate(std::array<float, kColorNodes> nodes, const float smoothing,
                              std::array<float, kUcsLutSize> &lut, const float hue_shift,
                              const bool clip_positive)
{
    const int terms = std::max(1, static_cast<int>(std::ceil(3.0F * std::sqrt(smoothing))));
    std::vector<float> matrix(static_cast<std::size_t>(kColorNodes * kColorNodes), 0.0F);
    const auto node_hue = [hue_shift](const int k)
    {
        return conventional_hue_deg_to_ucs_rad(static_cast<float>(k) * 360.0F /
                                                   static_cast<float>(kColorNodes) +
                                               hue_shift);
    };
    for (int i = 0; i < kColorNodes; ++i)
    {
        for (int j = 0; j < kColorNodes; ++j)
        {
            float value = 0.0F;
            for (int l = 0; l < terms; ++l)
            {
                value += std::exp(-static_cast<float>(l * l) / smoothing) *
                         std::cos(static_cast<float>(l) * std::abs(node_hue(i) - node_hue(j)));
            }
            matrix[static_cast<std::size_t>(i * kColorNodes + j)] = std::exp(value);
        }
    }
    std::vector<float> rhs(nodes.begin(), nodes.end());
    if (!pseudo_solve(matrix, rhs, kColorNodes, kColorNodes))
    {
        rhs.assign(nodes.begin(), nodes.end());
    }
    for (int i = 0; i < kUcsLutSize; ++i)
    {
        const float hue = static_cast<float>(i) * 360.0F / static_cast<float>(kUcsLutSize) * kPi /
                              180.0F -
                          kPi;
        float acc = 0.0F;
        for (int k = 0; k < kColorNodes; ++k)
        {
            float result = 0.0F;
            for (int l = 0; l < terms; ++l)
            {
                result += std::exp(-static_cast<float>(l * l) / smoothing) *
                          std::cos(static_cast<float>(l) * std::abs(hue - node_hue(k)));
            }
            acc += rhs[static_cast<std::size_t>(k)] * std::exp(result);
        }
        lut[static_cast<std::size_t>(i)] = clip_positive ? std::max(0.0F, acc) : acc;
    }
}

[[nodiscard]] float compute_density(const float dens, const float length) noexcept
{
    return std::exp2(dens * clip01(0.5F + length));
}

[[nodiscard]] float calc_refavg(const std::vector<float> &input, const DecodedRaw &raw,
                                const std::uint32_t width, const std::uint32_t height,
                                const std::uint32_t row, const std::uint32_t col) noexcept
{
    const auto color = cfa_channel(raw, col, row);
    std::array<float, 3> mean{};
    std::array<float, 3> count{};
    const int dymin = std::max(0, static_cast<int>(row) - 1);
    const int dxmin = std::max(0, static_cast<int>(col) - 1);
    const int dymax = std::min(static_cast<int>(height) - 1, static_cast<int>(row) + 2);
    const int dxmax = std::min(static_cast<int>(width) - 1, static_cast<int>(col) + 2);
    for (int dy = dymin; dy < dymax; ++dy)
    {
        for (int dx = dxmin; dx < dxmax; ++dx)
        {
            const auto c = cfa_channel(raw, static_cast<std::uint32_t>(dx),
                                       static_cast<std::uint32_t>(dy));
            mean[c] += std::max(0.0F, input[static_cast<std::size_t>(dy) * width +
                                            static_cast<std::uint32_t>(dx)]);
            count[c] += 1.0F;
        }
    }
    for (int c = 0; c < 3; ++c)
    {
        mean[c] = count[c] > 0.0F ? std::cbrt(mean[c] / count[c]) : 0.0F;
    }
    const std::array<float, 3> opp{0.5F * (mean[1] + mean[2]), 0.5F * (mean[0] + mean[2]),
                                   0.5F * (mean[0] + mean[1])};
    return opp[color] * opp[color] * opp[color];
}

void process_highlights_clip(std::vector<float> &buffer, const DecodedRaw &raw,
                             const std::array<float, 3> &clips)
{
    for (std::uint32_t y = 0; y < raw.height; ++y)
    {
        for (std::uint32_t x = 0; x < raw.width; ++x)
        {
            const std::size_t index = static_cast<std::size_t>(y) * raw.width + x;
            const auto channel = cfa_channel(raw, x, y);
            buffer[index] = std::min(buffer[index], clips[channel]);
        }
    }
}

void process_highlights_lch(std::vector<float> &buffer, const DecodedRaw &raw, const float clip)
{
    constexpr float kSqrt3 = 1.7320508075688772F;
    constexpr float kSqrt12 = 3.4641016151377546F;
    std::vector<float> out = buffer;
    for (std::uint32_t j = 0; j < raw.height; ++j)
    {
        for (std::uint32_t i = 0; i < raw.width; ++i)
        {
            const std::size_t index = static_cast<std::size_t>(j) * raw.width + i;
            if (i == raw.width - 1U || j == raw.height - 1U)
            {
                out[index] = std::min(clip, buffer[index]);
                continue;
            }
            float r = 0.0F;
            float gmin = std::numeric_limits<float>::max();
            float gmax = -std::numeric_limits<float>::max();
            float b = 0.0F;
            bool clipped = false;
            for (int jj = 0; jj <= 1; ++jj)
            {
                for (int ii = 0; ii <= 1; ++ii)
                {
                    const float val =
                        buffer[static_cast<std::size_t>(j + static_cast<std::uint32_t>(jj)) *
                                   raw.width +
                               (i + static_cast<std::uint32_t>(ii))];
                    clipped = clipped || val > clip;
                    const auto c = cfa_channel(raw, i + static_cast<std::uint32_t>(ii),
                                               j + static_cast<std::uint32_t>(jj));
                    if (c == 0)
                    {
                        r = val;
                    }
                    else if (c == 1)
                    {
                        gmin = std::min(gmin, val);
                        gmax = std::max(gmax, val);
                    }
                    else
                    {
                        b = val;
                    }
                }
            }
            if (!clipped)
            {
                out[index] = buffer[index];
                continue;
            }
            const float ro = std::min(r, clip);
            const float go = std::min(gmin, clip);
            const float bo = std::min(b, clip);
            const float l = (r + gmax + b) / 3.0F;
            float c = kSqrt3 * (r - gmax);
            float h = 2.0F * b - gmax - r;
            const float co = kSqrt3 * (ro - go);
            const float ho = 2.0F * bo - go - ro;
            if (r != gmax && gmax != b)
            {
                const float ratio = std::sqrt((co * co + ho * ho) / (c * c + h * h));
                c *= ratio;
                h *= ratio;
            }
            std::array<float, 3> rgb{l - h / 6.0F + c / kSqrt12, l - h / 6.0F - c / kSqrt12,
                                     l + h / 3.0F};
            out[index] = rgb[cfa_channel(raw, i, j)];
        }
    }
    buffer.swap(out);
}

void interpolate_color_bayer(const std::vector<float> &in, std::vector<float> &out,
                             const DecodedRaw &raw, const int dim, const int dir, const int other,
                             const std::array<float, 3> &clip, const int pass)
{
    const int width = static_cast<int>(raw.width);
    const int height = static_cast<int>(raw.height);
    int i = dim == 0 ? 0 : other;
    int j = dim == 0 ? other : 0;
    const int offs = (dim ? width : 1) * (dir < 0 ? -1 : 1);
    int beg = 0;
    int end = 0;
    if (dim == 0 && dir == 1)
    {
        beg = 0;
        end = width;
    }
    else if (dim == 0 && dir == -1)
    {
        beg = width - 1;
        end = -1;
    }
    else if (dim == 1 && dir == 1)
    {
        beg = 0;
        end = height;
    }
    else
    {
        beg = height - 1;
        end = -1;
    }
    std::ptrdiff_t pos = dim == 1 ? i + static_cast<std::ptrdiff_t>(beg) * width :
                                    beg + static_cast<std::ptrdiff_t>(j) * width;
    float ratio = 1.0F;
    for (int k = beg; k != end; k += dir)
    {
        if (dim == 1)
        {
            j = k;
        }
        else
        {
            i = k;
        }
        const float clip0 = clip[cfa_channel(raw, static_cast<std::uint32_t>(i),
                                             static_cast<std::uint32_t>(j))];
        const float clip1 = clip[cfa_channel(
            raw, static_cast<std::uint32_t>(dim ? i : i + 1),
            static_cast<std::uint32_t>(dim ? j + 1 : j))];
        if (i == 0 || i == width - 1 || j == 0 || j == height - 1)
        {
            if (pass == 3)
            {
                out[static_cast<std::size_t>(pos)] = in[static_cast<std::size_t>(pos)];
            }
        }
        else
        {
            const float current = in[static_cast<std::size_t>(pos)];
            const float neighbor = in[static_cast<std::size_t>(pos + offs)];
            if (current < clip0 && current > 1.0e-5F && neighbor < clip1 && neighbor > 1.0e-5F)
            {
                if ((k & 1) != 0)
                {
                    ratio = (3.0F * ratio + current / neighbor) / 4.0F;
                }
                else
                {
                    ratio = (3.0F * ratio + neighbor / current) / 4.0F;
                }
            }
            if (current >= clip0 - 1.0e-5F)
            {
                float add = 0.0F;
                if (neighbor >= clip1 - 1.0e-5F)
                {
                    add = std::max(clip0, clip1);
                }
                else if ((k & 1) != 0)
                {
                    add = neighbor * ratio;
                }
                else
                {
                    add = neighbor / ratio;
                }
                if (pass == 0)
                {
                    out[static_cast<std::size_t>(pos)] = add;
                }
                else if (pass == 3)
                {
                    out[static_cast<std::size_t>(pos)] =
                        (out[static_cast<std::size_t>(pos)] + add) / 4.0F;
                }
                else
                {
                    out[static_cast<std::size_t>(pos)] += add;
                }
            }
            else if (pass == 3)
            {
                out[static_cast<std::size_t>(pos)] = current;
            }
        }
        pos += offs;
    }
}

void process_highlights_inpaint(std::vector<float> &buffer, const DecodedRaw &raw,
                                const std::array<float, 3> &clips)
{
    std::vector<float> out = buffer;
    for (std::uint32_t j = 0; j < raw.height; ++j)
    {
        interpolate_color_bayer(buffer, out, raw, 0, 1, static_cast<int>(j), clips, 0);
        interpolate_color_bayer(buffer, out, raw, 0, -1, static_cast<int>(j), clips, 1);
    }
    for (std::uint32_t i = 0; i < raw.width; ++i)
    {
        interpolate_color_bayer(buffer, out, raw, 1, 1, static_cast<int>(i), clips, 2);
        interpolate_color_bayer(buffer, out, raw, 1, -1, static_cast<int>(i), clips, 3);
    }
    buffer.swap(out);
}

void process_highlights_opposed(std::vector<float> &buffer, const DecodedRaw &raw,
                                const std::array<float, 3> &clips)
{
    const std::size_t width = raw.width;
    const std::size_t height = raw.height;
    const std::size_t mwidth = width / 3U;
    const std::size_t mheight = height / 3U;
    std::array<float, 3> chrominance{};
    if (mwidth > 7 && mheight > 7)
    {
        const std::size_t msize = mwidth * mheight;
        std::vector<char> mask(msize * 3U, 0);
        bool any_clipped = false;
        for (std::size_t mrow = 0; mrow < mheight; ++mrow)
        {
            for (std::size_t mcol = 0; mcol < mwidth; ++mcol)
            {
                std::array<char, 3> mbuff{};
                for (int y = 0; y < 3; ++y)
                {
                    for (int x = 0; x < 3; ++x)
                    {
                        const std::size_t row = 3U * mrow + static_cast<std::size_t>(y);
                        const std::size_t col = 3U * mcol + static_cast<std::size_t>(x);
                        if (row >= height || col >= width)
                        {
                            continue;
                        }
                        const auto color = cfa_channel(raw, static_cast<std::uint32_t>(col),
                                                       static_cast<std::uint32_t>(row));
                        if (buffer[row * width + col] >= clips[color])
                        {
                            mbuff[color] = 1;
                            any_clipped = true;
                        }
                    }
                }
                const std::size_t mx = mrow * mwidth + mcol;
                mask[mx] = mbuff[0];
                mask[msize + mx] = mbuff[1];
                mask[2U * msize + mx] = mbuff[2];
            }
        }
        if (any_clipped)
        {
            std::vector<char> dilated(msize * 3U, 0);
            for (int c = 0; c < 3; ++c)
            {
                for (std::size_t row = 0; row < mheight; ++row)
                {
                    for (std::size_t col = 0; col < mwidth; ++col)
                    {
                        const std::size_t mx = row * mwidth + col;
                        const bool safe =
                            col >= 3 && row >= 3 && col + 4 < mwidth && row + 4 < mheight;
                        if (!safe)
                        {
                            dilated[static_cast<std::size_t>(c) * msize + mx] =
                                mask[static_cast<std::size_t>(c) * msize + mx];
                            continue;
                        }
                        char value = mask[static_cast<std::size_t>(c) * msize + mx];
                        if (value == 0)
                        {
                            const auto stride = static_cast<std::ptrdiff_t>(mwidth);
                            const auto base = static_cast<std::ptrdiff_t>(
                                static_cast<std::size_t>(c) * msize + mx);
                            const auto at = [&](const std::ptrdiff_t offset) -> char
                            {
                                const auto index = base + offset;
                                if (index < 0 ||
                                    index >= static_cast<std::ptrdiff_t>(mask.size()))
                                {
                                    return 0;
                                }
                                return mask[static_cast<std::size_t>(index)];
                            };
                            value = static_cast<char>(at(-stride - 1) | at(-stride) |
                                                      at(-stride + 1) | at(-1) | at(1) |
                                                      at(stride - 1) | at(stride) | at(stride + 1));
                        }
                        dilated[static_cast<std::size_t>(c) * msize + mx] = value;
                    }
                }
            }
            std::array<float, 3> sums{};
            std::array<float, 3> counts{};
            for (std::size_t row = 0; row < height; ++row)
            {
                for (std::size_t col = 0; col < width; ++col)
                {
                    const auto color = cfa_channel(raw, static_cast<std::uint32_t>(col),
                                                   static_cast<std::uint32_t>(row));
                    const float inval = buffer[row * width + col];
                    const std::size_t mx =
                        std::min(row / 3U, mheight - 1U) * mwidth + std::min(col / 3U, mwidth - 1U);
                    if (inval < clips[color] && inval > 0.2F * clips[color] &&
                        dilated[static_cast<std::size_t>(color) * msize + mx] != 0)
                    {
                        sums[color] +=
                            inval - calc_refavg(buffer, raw, raw.width, raw.height,
                                                static_cast<std::uint32_t>(row),
                                                static_cast<std::uint32_t>(col));
                        counts[color] += 1.0F;
                    }
                }
            }
            for (int c = 0; c < 3; ++c)
            {
                chrominance[static_cast<std::size_t>(c)] =
                    counts[static_cast<std::size_t>(c)] > 100.0F ?
                        sums[static_cast<std::size_t>(c)] / counts[static_cast<std::size_t>(c)] :
                        0.0F;
            }
        }
    }

    for (std::uint32_t row = 0; row < raw.height; ++row)
    {
        for (std::uint32_t col = 0; col < raw.width; ++col)
        {
            const std::size_t index = static_cast<std::size_t>(row) * raw.width + col;
            const auto color = cfa_channel(raw, col, row);
            const float inval = buffer[index];
            if (inval >= clips[color])
            {
                const float ref = calc_refavg(buffer, raw, raw.width, raw.height, row, col);
                buffer[index] = std::max(inval, ref + chrominance[color]);
            }
        }
    }
}

[[nodiscard]] Result<std::vector<float>> raw_to_float(const DecodedRaw &raw)
{
    const std::size_t count = static_cast<std::size_t>(raw.width) * raw.height;
    if (raw.pixels.size() < count)
    {
        return make_error(ErrorCode::kValidation, "RAW buffer is undersized for highlight reconstruction");
    }
    const float black = static_cast<float>(std::max(0, raw.black_level));
    const float scale = 1.0F / std::max(1.0F, static_cast<float>(raw.white_level) - black);
    std::vector<float> buffer(count);
    for (std::size_t index = 0; index < count; ++index)
    {
        buffer[index] = (static_cast<float>(raw.pixels[index]) - black) * scale;
    }
    return buffer;
}

void float_to_raw(DecodedRaw &raw, const std::vector<float> &buffer, const double amount)
{
    const float black = static_cast<float>(std::max(0, raw.black_level));
    const float range = std::max(1.0F, static_cast<float>(raw.white_level) - black);
    const std::size_t count = buffer.size();
    for (std::size_t index = 0; index < count; ++index)
    {
        const float restored = buffer[index] * range + black;
        const double mixed = static_cast<double>(raw.pixels[index]) * (1.0 - amount) +
                             static_cast<double>(restored) * amount;
        raw.pixels[index] =
            static_cast<std::uint16_t>(std::clamp(std::lround(mixed), 0L, 65535L));
    }
}

void pack_rgb(const WorkingImage &image, std::vector<float> &packed)
{
    const std::size_t count = static_cast<std::size_t>(image.width) * image.height;
    packed.assign(count * 4U, 0.0F);
    for (std::size_t pixel = 0; pixel < count; ++pixel)
    {
        packed[pixel * 4U] = image.rgb[pixel * 3U];
        packed[pixel * 4U + 1U] = image.rgb[pixel * 3U + 1U];
        packed[pixel * 4U + 2U] = image.rgb[pixel * 3U + 2U];
    }
}

void unpack_rgb(WorkingImage &image, const std::vector<float> &packed)
{
    const std::size_t count = static_cast<std::size_t>(image.width) * image.height;
    for (std::size_t pixel = 0; pixel < count; ++pixel)
    {
        image.rgb[pixel * 3U] = packed[pixel * 4U];
        image.rgb[pixel * 3U + 1U] = packed[pixel * 4U + 1U];
        image.rgb[pixel * 3U + 2U] = packed[pixel * 4U + 2U];
    }
}

float dn_weight(const float *left, const float *right, const float inv_sigma2) noexcept
{
    float dot = 0.0F;
    for (int c = 0; c < 3; ++c)
    {
        const float diff = left[c] - right[c];
        dot += diff * diff;
    }
    dot *= inv_sigma2;
    return std::exp2(-std::max(0.0F, dot * 0.02F - 9.0F));
}

void eaw_dn_decompose(std::vector<float> &coarse, const std::vector<float> &input,
                      std::vector<float> &detail, std::array<float, 3> &sum_squared, const int scale,
                      const float inv_sigma2, const int width, const int height)
{
    static constexpr float kFilter[25] = {
        1.0F / 256.0F, 4.0F / 256.0F,  6.0F / 256.0F,  4.0F / 256.0F,  1.0F / 256.0F,
        4.0F / 256.0F, 16.0F / 256.0F, 24.0F / 256.0F, 16.0F / 256.0F, 4.0F / 256.0F,
        6.0F / 256.0F, 24.0F / 256.0F, 36.0F / 256.0F, 24.0F / 256.0F, 6.0F / 256.0F,
        4.0F / 256.0F, 16.0F / 256.0F, 24.0F / 256.0F, 16.0F / 256.0F, 4.0F / 256.0F,
        1.0F / 256.0F, 4.0F / 256.0F,  6.0F / 256.0F,  4.0F / 256.0F,  1.0F / 256.0F};
    const int mult = 1 << scale;
    sum_squared = {};
    coarse.assign(input.size(), 0.0F);
    detail.assign(input.size(), 0.0F);
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            const float *px = input.data() + (static_cast<std::size_t>(y) * width + x) * 4U;
            std::array<float, 3> sum{};
            std::array<float, 3> wgt{};
            int filter_idx = 0;
            for (int jj = 0; jj < 5; ++jj)
            {
                const int sy = std::clamp(y + mult * (jj - 2), 0, height - 1);
                for (int ii = 0; ii < 5; ++ii)
                {
                    const int sx = std::clamp(x + mult * (ii - 2), 0, width - 1);
                    const float *px2 =
                        input.data() + (static_cast<std::size_t>(sy) * width + sx) * 4U;
                    const float weight = kFilter[filter_idx++] * dn_weight(px, px2, inv_sigma2);
                    for (int c = 0; c < 3; ++c)
                    {
                        wgt[static_cast<std::size_t>(c)] += weight;
                        sum[static_cast<std::size_t>(c)] += weight * px2[c];
                    }
                }
            }
            const std::size_t index = (static_cast<std::size_t>(y) * width + x) * 4U;
            for (int c = 0; c < 3; ++c)
            {
                const float mean = sum[static_cast<std::size_t>(c)] /
                                   std::max(wgt[static_cast<std::size_t>(c)], 1.0e-12F);
                coarse[index + static_cast<std::size_t>(c)] = mean;
                const float det = px[c] - mean;
                detail[index + static_cast<std::size_t>(c)] = det;
                sum_squared[static_cast<std::size_t>(c)] += det * det;
            }
        }
    }
}

void eaw_synthesize(std::vector<float> &accum, const std::vector<float> &detail,
                    const std::array<float, 3> &threshold, const int width, const int height)
{
    const std::size_t count = static_cast<std::size_t>(width) * height;
    for (std::size_t pixel = 0; pixel < count; ++pixel)
    {
        for (int c = 0; c < 3; ++c)
        {
            const float value = detail[pixel * 4U + static_cast<std::size_t>(c)];
            const float thresh = threshold[static_cast<std::size_t>(c)];
            const float amount = std::max(value - thresh, 0.0F) + std::min(value + thresh, 0.0F);
            accum[pixel * 4U + static_cast<std::size_t>(c)] += amount;
        }
    }
}

bool invert_matrix3(const float in[3][3], float out[3][3]) noexcept
{
    const float biga = in[1][1] * in[2][2] - in[1][2] * in[2][1];
    const float bigb = -in[1][0] * in[2][2] + in[1][2] * in[2][0];
    const float bigc = in[1][0] * in[2][1] - in[1][1] * in[2][0];
    const float det = in[0][0] * biga + in[0][1] * bigb + in[0][2] * bigc;
    if (det == 0.0F)
    {
        return false;
    }
    out[0][0] = biga / det;
    out[0][1] = (-in[0][1] * in[2][2] + in[0][2] * in[2][1]) / det;
    out[0][2] = (in[0][1] * in[1][2] - in[0][2] * in[1][1]) / det;
    out[1][0] = bigb / det;
    out[1][1] = (in[0][0] * in[2][2] - in[0][2] * in[2][0]) / det;
    out[1][2] = (-in[0][0] * in[1][2] + in[0][2] * in[1][0]) / det;
    out[2][0] = bigc / det;
    out[2][1] = (-in[0][0] * in[2][1] + in[0][1] * in[2][0]) / det;
    out[2][2] = (in[0][0] * in[1][1] - in[0][1] * in[1][0]) / det;
    return true;
}

void apply_matrix(const float matrix[3][3], const float in[3], float out[3]) noexcept
{
    out[0] = matrix[0][0] * in[0] + matrix[0][1] * in[1] + matrix[0][2] * in[2];
    out[1] = matrix[1][0] * in[0] + matrix[1][1] * in[1] + matrix[1][2] * in[2];
    out[2] = matrix[2][0] * in[0] + matrix[2][1] * in[1] + matrix[2][2] * in[2];
}

[[nodiscard]] float gaussian_denom(const float sigma) noexcept
{
    return 2.0F * sigma * sigma;
}

[[nodiscard]] float gaussian_func(const float radius, const float denominator) noexcept
{
    return std::exp(-radius * radius / denominator);
}

} // namespace

Result<void> apply_raw_highlights(DecodedRaw &raw, const OperationInstance &operation,
                                  const CancellationToken &cancellation)
{
    if (raw.cfa_width != 2 || raw.cfa_height != 2 || raw.cfa_channels.size() != 4U)
    {
        return make_error(ErrorCode::kUnsupported,
                          "RAW highlight reconstruction supports Bayer 2x2 CFA only",
                          {{"cfa_width", std::to_string(raw.cfa_width)},
                           {"cfa_height", std::to_string(raw.cfa_height)}});
    }
    const std::string mode = parameter_string(operation, "mode", std::string(kRawHighlightsModeOpposed));
    if (mode != kRawHighlightsModeClip && mode != kRawHighlightsModeInpaint &&
        mode != kRawHighlightsModeOpposed && mode != kRawHighlightsModeLch)
    {
        return make_error(ErrorCode::kUnsupported, "RAW highlight reconstruction mode is unsupported",
                          {{"mode", mode}});
    }
    const double amount = std::clamp(parameter(operation, "amount", 1.0), 0.0, 1.0);
    const double clip = std::clamp(parameter(operation, "clip", 1.0), 0.0, 2.0);
    if (amount <= 0.0 || raw.width < 3 || raw.height < 3)
    {
        return {};
    }
    auto cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    auto converted = raw_to_float(raw);
    if (!converted)
    {
        return converted.error();
    }
    auto &buffer = converted.value();
    float magic = 1.0F;
    if (mode == kRawHighlightsModeInpaint || mode == kRawHighlightsModeOpposed)
    {
        magic = 0.987F;
    }
    const float clipper = static_cast<float>(clip) * magic;
    const std::array<float, 3> clips{clipper, clipper, clipper};
    if (mode == kRawHighlightsModeClip)
    {
        process_highlights_clip(buffer, raw, clips);
    }
    else if (mode == kRawHighlightsModeLch)
    {
        process_highlights_lch(buffer, raw, clipper);
    }
    else if (mode == kRawHighlightsModeInpaint)
    {
        process_highlights_inpaint(buffer, raw, clips);
    }
    else
    {
        process_highlights_opposed(buffer, raw, clips);
    }
    float_to_raw(raw, buffer, amount);
    return {};
}

Result<void> apply_denoise_profile(WorkingImage &image, const OperationInstance &operation,
                                   const CancellationToken &cancellation)
{
    const double strength = std::clamp(parameter(operation, "strength", 0.0), 0.0, 4.0);
    if (strength <= 0.0 || image.width < 8 || image.height < 8)
    {
        return {};
    }
    const double chroma = std::clamp(parameter(operation, "chroma", 1.0), 0.0, 2.0);
    const float shadows = static_cast<float>(std::clamp(parameter(operation, "shadows", 1.0), 0.0, 1.8));
    const float bias = static_cast<float>(parameter(operation, "bias", 0.0));
    const float noise_a = static_cast<float>(
        std::max(1.0e-8, parameter(operation, "noise_a", static_cast<double>(kGenericNoiseA))));
    const float noise_b = static_cast<float>(
        std::max(0.0, parameter(operation, "noise_b", static_cast<double>(kGenericNoiseB))));
    const int width = static_cast<int>(image.width);
    const int height = static_cast<int>(image.height);
    const std::size_t npixels = static_cast<std::size_t>(width) * height;

    int max_scale = 0;
    const float supp0 = std::min(2.0F * (2U << (kDenoiseBands - 1)) + 1.0F,
                                 static_cast<float>(std::max(width, height)) * 0.2F);
    const float i0 = std::log2(std::max(supp0 - 1.0F, 1.0F) * 0.5F);
    for (; max_scale < kDenoiseBands; ++max_scale)
    {
        const float supp = 2.0F * static_cast<float>(2U << max_scale) + 1.0F;
        const float i_in = std::log2(std::max(supp - 1.0F, 1.0F) * 0.5F) - 1.0F;
        const float t = 1.0F - (i_in + 0.5F) / i0;
        if (t < 0.0F)
        {
            break;
        }
    }
    max_scale = std::max(1, max_scale);
    const int max_mult = 1 << (max_scale - 1);
    if (width < 2 * max_mult || height < 2 * max_mult)
    {
        return {};
    }

    auto cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }

    std::array<float, 3> wb{1.0F, 1.0F, 1.0F};
    const float p_base = std::max(shadows, 0.0F);
    const std::array<float, 3> p{p_base, p_base, p_base};
    const float compensate_p = kDenoisePFulcrum / std::pow(kDenoisePFulcrum, shadows);
    const float compensate_strength = 2.5F;
    const float strength_scale = static_cast<float>(strength) * compensate_strength;

    float to_yuv[3][3] = {{1.0F / 3.0F, 1.0F / 3.0F, 1.0F / 3.0F},
                          {0.5F, 0.0F, -0.5F},
                          {0.25F, -0.5F, 0.25F}};
    float sum_invwb = (1.0F / wb[0] + 1.0F / wb[1] + 1.0F / wb[2]) * std::sqrt(3.0F);
    to_yuv[0][0] = sum_invwb / wb[0];
    to_yuv[0][1] = sum_invwb / wb[1];
    to_yuv[0][2] = sum_invwb / wb[2];
    const float stddev_u = std::sqrt(0.25F * wb[0] * wb[0] + 0.25F * wb[2] * wb[2]);
    const float stddev_v =
        std::sqrt(0.0625F * wb[0] * wb[0] + 0.25F * wb[1] * wb[1] + 0.0625F * wb[2] * wb[2]);
    to_yuv[1][0] /= stddev_u;
    to_yuv[1][1] /= stddev_u;
    to_yuv[1][2] /= stddev_u;
    to_yuv[2][0] /= stddev_v;
    to_yuv[2][1] /= stddev_v;
    to_yuv[2][2] /= stddev_v;
    float to_rgb[3][3]{};
    if (!invert_matrix3(to_yuv, to_rgb))
    {
        const float stddev_y =
            std::sqrt((wb[0] * wb[0] + wb[1] * wb[1] + wb[2] * wb[2]) / 9.0F);
        to_yuv[0][0] = to_yuv[0][1] = to_yuv[0][2] = 1.0F / (3.0F * stddev_y);
        invert_matrix3(to_yuv, to_rgb);
    }
    for (int k = 0; k < 3; ++k)
    {
        for (int c = 0; c < 3; ++c)
        {
            to_yuv[k][c] /= strength_scale;
            to_rgb[k][c] *= strength_scale;
        }
    }

    const float a = noise_a * compensate_p;
    const std::array<float, 3> expon{-p[0] / 2.0F + 1.0F, -p[1] / 2.0F + 1.0F, -p[2] / 2.0F + 1.0F};
    const std::array<float, 3> scale{2.0F / ((-p[0] + 2.0F) * std::sqrt(a)),
                                     2.0F / ((-p[1] + 2.0F) * std::sqrt(a)),
                                     2.0F / ((-p[2] + 2.0F) * std::sqrt(a))};
    std::vector<float> packed;
    pack_rgb(image, packed);
    std::vector<float> precond(packed.size(), 0.0F);
    for (std::size_t pixel = 0; pixel < npixels; ++pixel)
    {
        float tmp[3]{};
        for (int c = 0; c < 3; ++c)
        {
            tmp[c] = std::pow(std::max(packed[pixel * 4U + static_cast<std::size_t>(c)] + noise_b, 0.0F),
                              expon[static_cast<std::size_t>(c)]) *
                     scale[static_cast<std::size_t>(c)];
        }
        float yuv[3]{};
        apply_matrix(to_yuv, tmp, yuv);
        precond[pixel * 4U] = yuv[0];
        precond[pixel * 4U + 1U] = yuv[1];
        precond[pixel * 4U + 2U] = yuv[2];
    }

    std::vector<float> out(packed.size(), 0.0F);
    std::vector<float> current = precond;
    const float varf = std::sqrt(2.0F + 2.0F * 16.0F + 36.0F) / 16.0F;
    for (int scale_index = 0; scale_index < max_scale; ++scale_index)
    {
        cancelled = cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
        const float sigma_band = std::pow(varf, static_cast<float>(scale_index));
        std::vector<float> coarse;
        std::vector<float> detail;
        std::array<float, 3> sum_y2{};
        eaw_dn_decompose(coarse, current, detail, sum_y2, scale_index, 1.0F / (sigma_band * sigma_band),
                         width, height);
        const float sb2 = sigma_band * sigma_band;
        const int offset_scale = kDenoiseBands - max_scale;
        const int band_index = kDenoiseBands - (scale_index + offset_scale + 1);
        const float y_force = 0.5F;
        const float uv_force = 0.5F * static_cast<float>(chroma);
        auto band_adj = [](const float force)
        {
            return 8.0F * force * force * 4.0F;
        };
        std::array<float, 3> std_x{};
        for (int c = 0; c < 3; ++c)
        {
            std_x[static_cast<std::size_t>(c)] =
                std::sqrt(std::max(1.0e-6F, sum_y2[static_cast<std::size_t>(c)] / (static_cast<float>(npixels) - 1.0F) - sb2));
        }
        const std::array<float, 3> thrs{band_adj(y_force) * sb2 / std_x[0],
                                        band_adj(uv_force) * sb2 / std_x[1],
                                        band_adj(uv_force) * sb2 / std_x[2]};
        (void)band_index;
        eaw_synthesize(out, detail, thrs, width, height);
        current.swap(coarse);
    }
    for (std::size_t index = 0; index < out.size(); ++index)
    {
        out[index] += current[index];
    }

    const std::array<float, 3> back_expon{1.0F / (1.0F - p[0] / 2.0F), 1.0F / (1.0F - p[1] / 2.0F),
                                          1.0F / (1.0F - p[2] / 2.0F)};
    const std::array<float, 3> back_scale{(std::sqrt(a) * (2.0F - p[0])) / 4.0F,
                                          (std::sqrt(a) * (2.0F - p[1])) / 4.0F,
                                          (std::sqrt(a) * (2.0F - p[2])) / 4.0F};
    const float applied_bias = bias - 0.5F * std::log(1.0F);
    for (std::size_t pixel = 0; pixel < npixels; ++pixel)
    {
        float yuv[3]{out[pixel * 4U], out[pixel * 4U + 1U], out[pixel * 4U + 2U]};
        float rgb[3]{};
        apply_matrix(to_rgb, yuv, rgb);
        for (int c = 0; c < 3; ++c)
        {
            const float x = std::max(rgb[c], 0.0F);
            const float delta = x * x + applied_bias * wb[static_cast<std::size_t>(c)];
            const float z1 = (x + std::sqrt(std::max(delta, 0.0F))) * back_scale[static_cast<std::size_t>(c)];
            rgb[c] = std::pow(std::max(z1, 0.0F), back_expon[static_cast<std::size_t>(c)]) - noise_b;
        }
        packed[pixel * 4U] = rgb[0];
        packed[pixel * 4U + 1U] = rgb[1];
        packed[pixel * 4U + 2U] = rgb[2];
    }
    unpack_rgb(image, packed);
    return {};
}

Result<void> apply_lens_correction(WorkingImage &image, const OperationInstance &operation,
                                   const CancellationToken &cancellation)
{
    const std::string mode = parameter_string(operation, "mode", "manual");
    double k1 = parameter(operation, "k1", 0.0);
    double k2 = parameter(operation, "k2", 0.0);
    double tca_r = parameter(operation, "tca_r", 1.0);
    double tca_b = parameter(operation, "tca_b", 1.0);
    double vignetting = parameter(operation, "vignetting", 0.0);
    double v_radius = std::clamp(parameter(operation, "v_radius", 0.5), 0.0, 1.0);
    double v_steepness = std::clamp(parameter(operation, "v_steepness", 0.5), 0.0, 1.0);
    if (mode == kLensModeLookup)
    {
        const auto *calibration = find_lens_calibration(
            parameter_string(operation, "camera_make", ""),
            parameter_string(operation, "camera_model", ""),
            parameter_string(operation, "lens", ""), parameter(operation, "focal_mm", 50.0));
        if (calibration == nullptr)
        {
            return make_error(ErrorCode::kNotFound, "No lens calibration matches the lookup request",
                              {{"camera_make", parameter_string(operation, "camera_make", "")},
                               {"camera_model", parameter_string(operation, "camera_model", "")},
                               {"lens", parameter_string(operation, "lens", "")}});
        }
        k1 = calibration->k1;
        k2 = calibration->k2;
        tca_r = calibration->tca_r;
        tca_b = calibration->tca_b;
        vignetting = calibration->vignetting;
    }
    else if (mode != kLensModeManual)
    {
        return make_error(ErrorCode::kUnsupported, "Lens correction mode is unsupported",
                          {{"mode", mode}});
    }
    if (!std::isfinite(k1) || !std::isfinite(k2) || !std::isfinite(tca_r) || !std::isfinite(tca_b) ||
        !std::isfinite(vignetting))
    {
        return make_error(ErrorCode::kValidation, "Lens correction coefficients must be finite");
    }
    if (std::abs(k1) <= 1.0e-8 && std::abs(k2) <= 1.0e-8 && std::abs(tca_r - 1.0) <= 1.0e-8 &&
        std::abs(tca_b - 1.0) <= 1.0e-8 && std::abs(vignetting) <= 1.0e-8)
    {
        return {};
    }
    if (image.width < 2 || image.height < 2)
    {
        return make_error(ErrorCode::kValidation, "Lens correction requires a non-empty image");
    }

    std::array<float, kVignetteSplines> spline{};
    const double v = v_steepness;
    const double b = 1.0 + v_radius * 10.0;
    const double mul = -v / std::tanh(b);
    for (int i = 0; i < kVignetteSplines; ++i)
    {
        const double radius = static_cast<double>(i) / static_cast<double>(kVignetteSplines - 1);
        spline[static_cast<std::size_t>(i)] = static_cast<float>(v + mul * std::tanh(b * (1.0 - radius)));
    }
    const auto vignette_at = [&](const float radius)
    {
        if (radius >= 1.0F)
        {
            return spline[kVignetteSplines - 1];
        }
        const float r = radius * static_cast<float>(kVignetteSplines - 1);
        const float frac = r - std::trunc(r);
        const int i = static_cast<int>(r);
        return spline[static_cast<std::size_t>(i)] +
               (spline[static_cast<std::size_t>(i + 1)] - spline[static_cast<std::size_t>(i)]) * frac;
    };

    WorkingImage source = image;
    const float cx = static_cast<float>(image.width) * 0.5F;
    const float cy = static_cast<float>(image.height) * 0.5F;
    const float inv_maxr = 1.0F / std::hypot(cx, cy);
    const float vig_strength = 2.0F * static_cast<float>(vignetting);
    const auto scales = std::array<float, 3>{static_cast<float>(tca_r), 1.0F, static_cast<float>(tca_b)};
    for (std::uint32_t y = 0; y < image.height; ++y)
    {
        auto cancelled = cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
        for (std::uint32_t x = 0; x < image.width; ++x)
        {
            const float dx = static_cast<float>(x) - cx;
            const float dy = static_cast<float>(y) - cy;
            const float ru = std::hypot(dx, dy) * inv_maxr;
            const float ru2 = ru * ru;
            // lensfun poly5 dest-to-source: Rd = Ru * (1 + k1 Ru^2 + k2 Ru^4)
            const float geometry = 1.0F + static_cast<float>(k1) * ru2 +
                                   static_cast<float>(k2) * ru2 * ru2;
            const float vig = std::max(0.0F, vig_strength * vignette_at(ru));
            const std::size_t index = (static_cast<std::size_t>(y) * image.width + x) * 3U;
            for (std::size_t channel = 0; channel < 3; ++channel)
            {
                const float scale = geometry * scales[channel];
                const float sx = cx + dx * scale;
                const float sy = cy + dy * scale;
                image.rgb[index + channel] = sample_channel(source, sx, sy, channel) * (1.0F + vig);
            }
        }
    }
    return {};
}

Result<void> apply_color_equalizer(WorkingImage &image, const OperationInstance &operation,
                                   const CancellationToken &cancellation)
{
    auto hue_shifts = parameter_band_array(operation, "hue_shift");
    if (!hue_shifts)
    {
        return hue_shifts.error();
    }
    auto sat_shifts = parameter_band_array(operation, "saturation");
    if (!sat_shifts)
    {
        return sat_shifts.error();
    }
    auto light_shifts = parameter_band_array(operation, "lightness");
    if (!light_shifts)
    {
        return light_shifts.error();
    }
    bool identity = true;
    std::array<float, kColorNodes> sat_nodes{};
    std::array<float, kColorNodes> hue_nodes{};
    std::array<float, kColorNodes> bright_nodes{};
    for (std::size_t index = 0; index < kColorEqualizerBandCount; ++index)
    {
        if (std::abs(hue_shifts.value()[index]) > 1.0e-8 ||
            std::abs(sat_shifts.value()[index]) > 1.0e-8 ||
            std::abs(light_shifts.value()[index]) > 1.0e-8)
        {
            identity = false;
        }
        hue_nodes[index] = static_cast<float>(hue_shifts.value()[index]) * kTwoPi;
        sat_nodes[index] = 1.0F + static_cast<float>(sat_shifts.value()[index]);
        bright_nodes[index] = 1.0F + static_cast<float>(light_shifts.value()[index]);
    }
    if (identity)
    {
        return {};
    }

    const float hue_shift = static_cast<float>(parameter(operation, "node_placement", 0.0));
    const float smoothing_hue = static_cast<float>(std::clamp(parameter(operation, "smoothing_hue", 1.0), 0.05, 2.0));
    std::array<float, kUcsLutSize> lut_sat{};
    std::array<float, kUcsLutSize> lut_hue{};
    std::array<float, kUcsLutSize> lut_bright{};
    periodic_rbf_interpolate(sat_nodes, kPi, lut_sat, hue_shift, true);
    periodic_rbf_interpolate(hue_nodes, kPi / smoothing_hue, lut_hue, hue_shift, false);
    periodic_rbf_interpolate(bright_nodes, kPi, lut_bright, hue_shift, true);
    const float white = y_to_ucs_l_star(1.0F);
    const std::size_t count = static_cast<std::size_t>(image.width) * image.height;
    std::vector<float> uv(count * 2U);
    std::vector<float> lstar(count);
    std::vector<float> saturation(count);
    for (std::size_t pixel = 0; pixel < count; ++pixel)
    {
        const float r = image.rgb[pixel * 3U];
        const float g = image.rgb[pixel * 3U + 1U];
        const float b = image.rgb[pixel * 3U + 2U];
        const float dmin = std::min(r, std::min(g, b));
        const float dmax = std::max(r, std::max(g, b));
        const float delta = dmax - dmin;
        saturation[pixel] = (dmax > 1.0e-6F && delta > 1.0e-6F) ? delta / dmax : 0.0F;
        float x = 0.0F;
        float y = 0.0F;
        float z = 0.0F;
        rgb_to_xyz_d65(r, g, b, x, y, z);
        float xyx = 0.0F;
        float xyy = 0.0F;
        float xy_y = 0.0F;
        xyz_to_xyy(x, y, z, xyx, xyy, xy_y);
        xyy_to_ucs_uv(xyx, xyy, uv.data() + pixel * 2U);
        lstar[pixel] = y_to_ucs_l_star(xy_y);
    }
    const float hue_sigma = 0.5F * static_cast<float>(std::clamp(parameter(operation, "chroma_size", 1.5), 1.0, 10.0));
    std::vector<float> u_plane(count);
    std::vector<float> v_plane(count);
    for (std::size_t pixel = 0; pixel < count; ++pixel)
    {
        u_plane[pixel] = uv[pixel * 2U];
        v_plane[pixel] = uv[pixel * 2U + 1U];
    }
    blur_plane(u_plane, image.width, image.height, hue_sigma);
    blur_plane(v_plane, image.width, image.height, hue_sigma);
    const int guide_radius = std::max(1, static_cast<int>(std::lround(hue_sigma)));
    guided_filter_plane(u_plane, u_plane, image.width, image.height, guide_radius, 1.0e-5F);
    guided_filter_plane(v_plane, v_plane, image.width, image.height, guide_radius, 1.0e-5F);
    for (std::size_t pixel = 0; pixel < count; ++pixel)
    {
        uv[pixel * 2U] = u_plane[pixel];
        uv[pixel * 2U + 1U] = v_plane[pixel];
    }

    for (std::uint32_t y = 0; y < image.height; ++y)
    {
        auto cancelled = cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
        for (std::uint32_t x = 0; x < image.width; ++x)
        {
            const std::size_t pixel = static_cast<std::size_t>(y) * image.width + x;
            float jch[3]{};
            ucs_luv_to_jch(lstar[pixel], white, uv.data() + pixel * 2U, jch);
            float hsb[3]{};
            ucs_jch_to_hsb(jch, hsb);
            if (jch[1] > 1.0e-6F)
            {
                const float hue = hsb[0];
                const float sat = hsb[1];
                hsb[0] += lookup_lut_periodic(lut_hue, hue);
                hsb[1] = std::max(0.0F, sat * (1.0F + kSatEffect * (lookup_lut_periodic(lut_sat, hue) - 1.0F)));
                const float bright_corr = sat * (lookup_lut_periodic(lut_bright, hue) - 1.0F);
                hsb[2] = std::max(0.0F, hsb[2] * (1.0F + kBrightEffect * bright_corr));
            }
            float r = 0.0F;
            float g = 0.0F;
            float b = 0.0F;
            ucs_hsb_to_rgb(hsb, white, r, g, b);
            image.rgb[pixel * 3U] = r;
            image.rgb[pixel * 3U + 1U] = g;
            image.rgb[pixel * 3U + 2U] = b;
        }
    }
    return {};
}

Result<void> apply_graduated_nd(WorkingImage &image, const OperationInstance &operation,
                                const CancellationToken &cancellation)
{
    const double density = std::clamp(parameter(operation, "density_ev", 0.0), -8.0, 8.0);
    if (std::abs(density) <= 1.0e-8)
    {
        return {};
    }
    const double hardness = std::clamp(parameter(operation, "hardness", 0.5), 0.0, 1.0) * 100.0;
    const double rotation_deg = std::clamp(parameter(operation, "rotation_deg", 0.0), -180.0, 180.0);
    const double offset_norm = std::clamp(parameter(operation, "offset", 0.0), -1.0, 1.0);
    const double offset = (offset_norm + 1.0) * 50.0;
    const float hue = static_cast<float>(std::clamp(parameter(operation, "hue", 0.0), 0.0, 1.0));
    const float saturation =
        static_cast<float>(std::clamp(parameter(operation, "saturation", 0.0), 0.0, 1.0));

    std::array<float, 4> color{};
    hsl_to_rgb(hue, saturation, 0.5F, color[0], color[1], color[2]);
    if (density < 0.0)
    {
        for (float &channel : color)
        {
            channel = 1.0F - channel;
        }
    }
    std::array<float, 4> color1{};
    for (int c = 0; c < 4; ++c)
    {
        color1[static_cast<std::size_t>(c)] = 1.0F - color[static_cast<std::size_t>(c)];
    }

    const float iw = static_cast<float>(image.width);
    const float ih = static_cast<float>(image.height);
    const float hw = iw / 2.0F;
    const float hh = ih / 2.0F;
    const float hw_inv = 1.0F / hw;
    const float hh_inv = 1.0F / hh;
    const float v = static_cast<float>(-rotation_deg) * kPi / 180.0F;
    const float sinv = std::sin(v);
    const float cosv = std::cos(v);
    const float cosv_hh_inv = cosv * hh_inv;
    const float filter_radie = std::hypot(hh, hw) / hh;
    const float offset_c = static_cast<float>(offset / 100.0 * 2.0);
    const float filter_hardness =
        (1.0F / filter_radie) /
        (1.0F - (0.5F + (static_cast<float>(hardness) / 100.0F) * 0.9F / 2.0F)) * 0.5F;
    const float length_base = sinv * (-1.0F) + cosv - 1.0F + offset_c;
    const float length_inc = sinv * hw_inv * filter_hardness;
    const float dens = static_cast<float>(density);

    for (std::uint32_t y = 0; y < image.height; ++y)
    {
        auto cancelled = cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
        float length = (length_base - static_cast<float>(y) * cosv_hh_inv) * filter_hardness;
        for (std::uint32_t x = 0; x < image.width; ++x)
        {
            const std::size_t index = (static_cast<std::size_t>(y) * image.width + x) * 3U;
            if (dens > 0.0F)
            {
                const float curr = compute_density(dens, length);
                for (int c = 0; c < 3; ++c)
                {
                    image.rgb[index + static_cast<std::size_t>(c)] /=
                        color[static_cast<std::size_t>(c)] +
                        color1[static_cast<std::size_t>(c)] * curr;
                }
            }
            else
            {
                const float curr = compute_density(-dens, -length);
                for (int c = 0; c < 3; ++c)
                {
                    image.rgb[index + static_cast<std::size_t>(c)] *=
                        color[static_cast<std::size_t>(c)] +
                        color1[static_cast<std::size_t>(c)] * curr;
                }
            }
            length += length_inc;
        }
    }
    return {};
}

Result<void> apply_tone_equalizer(WorkingImage &image, const OperationInstance &operation,
                                  const CancellationToken &cancellation)
{
    std::array<float, kToneChannels> gains{
        static_cast<float>(parameter(operation, "noise", parameter(operation, "blacks", 0.0))),
        static_cast<float>(parameter(operation, "ultra_deep_blacks", 0.0)),
        static_cast<float>(parameter(operation, "deep_blacks", 0.0)),
        static_cast<float>(parameter(operation, "blacks", 0.0)),
        static_cast<float>(parameter(operation, "shadows", 0.0)),
        static_cast<float>(parameter(operation, "midtones", 0.0)),
        static_cast<float>(parameter(operation, "highlights", 0.0)),
        static_cast<float>(parameter(operation, "whites", 0.0)),
        static_cast<float>(parameter(operation, "speculars", parameter(operation, "whites", 0.0) * 0.0)),
    };
    // Map the 5-slider Develop schema onto the frozen 9-band C params:
    // blacks(-8 via noise), shadows(-4), midtones(-3), highlights(-2), whites(-1).
    if (operation.parameters.find("noise") == operation.parameters.end())
    {
        gains[0] = static_cast<float>(parameter(operation, "blacks", 0.0));
    }
    if (operation.parameters.find("speculars") == operation.parameters.end())
    {
        gains[8] = 0.0F;
    }
    bool identity = true;
    for (float &gain : gains)
    {
        if (!std::isfinite(gain) || std::abs(gain) > 4.0F)
        {
            return make_error(ErrorCode::kValidation,
                              "Tone equalizer band must be a finite EV in [-4, 4]");
        }
        if (std::abs(gain) > 1.0e-8F)
        {
            identity = false;
        }
        gain = std::exp2(gain);
    }
    if (identity || image.width == 0 || image.height == 0)
    {
        return {};
    }

    constexpr std::array<float, kTonePixelChan> kCentersOps{
        -56.0F / 7.0F, -48.0F / 7.0F, -40.0F / 7.0F, -32.0F / 7.0F,
        -24.0F / 7.0F, -16.0F / 7.0F, -8.0F / 7.0F,  0.0F};
    constexpr std::array<float, kToneChannels> kCentersParams{-8.0F, -7.0F, -6.0F, -5.0F, -4.0F,
                                                              -3.0F, -2.0F, -1.0F, 0.0F};
    const float sigma = static_cast<float>(parameter(operation, "smoothing", std::sqrt(2.0)));
    const float denom = gaussian_denom(sigma);
    std::vector<float> matrix(static_cast<std::size_t>(kToneChannels * kTonePixelChan));
    for (int i = 0; i < kToneChannels; ++i)
    {
        for (int j = 0; j < kTonePixelChan; ++j)
        {
            matrix[static_cast<std::size_t>(i * kTonePixelChan + j)] =
                gaussian_func(kCentersParams[static_cast<std::size_t>(i)] -
                                  kCentersOps[static_cast<std::size_t>(j)],
                              denom);
        }
    }
    std::vector<float> factors(gains.begin(), gains.end());
    if (!pseudo_solve(matrix, factors, kToneChannels, kTonePixelChan))
    {
        factors.assign(kTonePixelChan, 1.0F);
        for (int i = 0; i < kTonePixelChan && i < kToneChannels; ++i)
        {
            factors[static_cast<std::size_t>(i)] = gains[static_cast<std::size_t>(i)];
        }
    }

    std::vector<float> lut(static_cast<std::size_t>(kToneLutResolution * kTonePixelChan + 1));
    for (int j = 0; j <= kToneLutResolution * kTonePixelChan; ++j)
    {
        const float exposure = static_cast<float>(j) / static_cast<float>(kToneLutResolution) - 8.0F;
        float result = 0.0F;
        for (int i = 0; i < kTonePixelChan; ++i)
        {
            result += gaussian_func(exposure - kCentersOps[static_cast<std::size_t>(i)], denom) *
                      factors[static_cast<std::size_t>(i)];
        }
        lut[static_cast<std::size_t>(j)] = std::clamp(result, 0.25F, 4.0F);
    }

    const std::size_t count = static_cast<std::size_t>(image.width) * image.height;
    std::vector<float> luminance(count);
    for (std::size_t pixel = 0; pixel < count; ++pixel)
    {
        const float r = image.rgb[pixel * 3U];
        const float g = image.rgb[pixel * 3U + 1U];
        const float b = image.rgb[pixel * 3U + 2U];
        luminance[pixel] = std::max(std::exp2(-16.0F), std::sqrt(r * r + g * g + b * b));
    }
    const float blending = static_cast<float>(parameter(operation, "blending", 5.0)) / 100.0F;
    const int max_size = static_cast<int>(std::max(image.width, image.height));
    const int radius = static_cast<int>((blending * static_cast<float>(max_size) - 1.0F) / 2.0F);
    if (radius >= 1)
    {
        const float feathering =
            1.0F / static_cast<float>(std::max(0.01, parameter(operation, "feathering", 1.0)));
        guided_filter_plane(luminance, luminance, image.width, image.height, radius, feathering);
    }
    auto cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }

    for (std::uint32_t y = 0; y < image.height; ++y)
    {
        cancelled = cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
        for (std::uint32_t x = 0; x < image.width; ++x)
        {
            const std::size_t pixel = static_cast<std::size_t>(y) * image.width + x;
            const float exposure = std::clamp(std::log2(luminance[pixel]), -8.0F, 0.0F);
            const auto lut_index =
                static_cast<std::size_t>(std::lround((exposure + 8.0F) * kToneLutResolution));
            const float correction = lut[std::min(lut_index, lut.size() - 1U)];
            image.rgb[pixel * 3U] *= correction;
            image.rgb[pixel * 3U + 1U] *= correction;
            image.rgb[pixel * 3U + 2U] *= correction;
        }
    }
    return {};
}

} // namespace ravo
