#include "image_ops.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <numbers>
#include <string>
#include <thread>
#include <vector>

#include <png.h>
#include <zlib.h>

#include "capability_ops.h"
#include "color_checker.h"
#include "output_color.h"
#include "primaries.h"
#include "raw_temperature.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/profile_gamma.h"

namespace ravo
{
namespace
{

[[nodiscard]] unsigned parallel_row_workers(const std::uint32_t rows) noexcept
{
    if (rows < 32U)
    {
        return 1U;
    }
    const unsigned hardware = std::max(1U, std::thread::hardware_concurrency());
    return std::min(std::min(hardware, 8U), rows);
}

template <typename Fn>
Result<void> for_each_row(const std::uint32_t height, const CancellationToken &cancellation,
                          Fn &&fn)
{
    auto cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    const unsigned workers = parallel_row_workers(height);
    if (workers <= 1U)
    {
        for (std::uint32_t row = 0; row < height; ++row)
        {
            cancelled = cancellation.check();
            if (!cancelled)
            {
                return cancelled.error();
            }
            fn(row);
        }
        return {};
    }

    std::atomic<std::uint32_t> next{0};
    std::atomic<bool> failed{false};
    TaskError error;
    std::mutex error_mutex;
    std::vector<std::thread> threads;
    threads.reserve(workers);
    for (unsigned worker = 0; worker < workers; ++worker)
    {
        threads.emplace_back(
            [&]()
            {
                for (;;)
                {
                    if (failed.load(std::memory_order_acquire))
                    {
                        return;
                    }
                    const std::uint32_t row = next.fetch_add(1, std::memory_order_relaxed);
                    if (row >= height)
                    {
                        return;
                    }
                    auto row_cancelled = cancellation.check();
                    if (!row_cancelled)
                    {
                        std::lock_guard lock(error_mutex);
                        if (!failed.exchange(true, std::memory_order_acq_rel))
                        {
                            error = row_cancelled.error();
                        }
                        return;
                    }
                    fn(row);
                }
            });
    }
    for (auto &thread : threads)
    {
        thread.join();
    }
    if (failed.load(std::memory_order_acquire))
    {
        return error;
    }
    return {};
}

[[nodiscard]] bool absorbed_operation(const std::string_view id) noexcept
{
    return id == "ravo.core.identity" || id == "ravo.raw.prepare" || id == "ravo.raw.demosaic" ||
           id == "ravo.color.input" || id == kProfileGammaOperationId ||
           id == "ravo.color.output" || id == "ravo.output.scale";
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

constexpr int kToneCurveLut = 0x10000;

void linear_rgb_to_xyz_d50(const float r, const float g, const float b, float xyz[3]) noexcept
{
    xyz[0] = 0.4360747F * r + 0.3850649F * g + 0.1430804F * b;
    xyz[1] = 0.2225045F * r + 0.7168786F * g + 0.0606169F * b;
    xyz[2] = 0.0139322F * r + 0.0971045F * g + 0.7141733F * b;
}

void xyz_d50_to_linear_rgb(const float xyz[3], float &r, float &g, float &b) noexcept
{
    r = 3.1338561F * xyz[0] - 1.6168667F * xyz[1] - 0.4906146F * xyz[2];
    g = -0.9787684F * xyz[0] + 1.9161415F * xyz[1] + 0.0334540F * xyz[2];
    b = 0.0719453F * xyz[0] - 0.2289914F * xyz[1] + 1.4052427F * xyz[2];
}

void xyz_d50_to_lab(const float xyz[3], float lab[3]) noexcept
{
    constexpr float kD50[3] = {0.9642F, 1.0F, 0.8249F};
    constexpr float kEpsilon = 216.0F / 24389.0F;
    constexpr float kKappa = 24389.0F / 27.0F;
    float f[3]{};
    for (int i = 0; i < 3; ++i)
    {
        const float x = xyz[i] / kD50[i];
        f[i] = x > kEpsilon ? std::cbrt(x) : (kKappa * x + 16.0F) / 116.0F;
    }
    lab[0] = 116.0F * f[1] - 16.0F;
    lab[1] = 500.0F * (f[0] - f[1]);
    lab[2] = 200.0F * (f[1] - f[2]);
}

void lab_to_xyz_d50(const float lab[3], float xyz[3]) noexcept
{
    constexpr float kD50[3] = {0.9642F, 1.0F, 0.8249F};
    constexpr float kEpsilon = 0.20689655172413796F;
    constexpr float kKappa = 24389.0F / 27.0F;
    const float fy = (lab[0] + 16.0F) / 116.0F;
    const float fx = fy + lab[1] / 500.0F;
    const float fz = fy - lab[2] / 200.0F;
    const auto inv = [](const float x)
    { return x > kEpsilon ? x * x * x : (116.0F * x - 16.0F) / kKappa; };
    xyz[0] = kD50[0] * inv(fx);
    xyz[1] = kD50[1] * inv(fy);
    xyz[2] = kD50[2] * inv(fz);
}

void xyz_to_prophoto(const float xyz[3], float rgb[3]) noexcept
{
    rgb[0] = 1.3459433F * xyz[0] - 0.2556075F * xyz[1] - 0.0511118F * xyz[2];
    rgb[1] = -0.5445989F * xyz[0] + 1.5081673F * xyz[1] + 0.0205351F * xyz[2];
    rgb[2] = 0.0000000F * xyz[0] + 0.0000000F * xyz[1] + 1.2118128F * xyz[2];
}

void prophoto_to_xyz(const float rgb[3], float xyz[3]) noexcept
{
    xyz[0] = 0.7976749F * rgb[0] + 0.1351917F * rgb[1] + 0.0313534F * rgb[2];
    xyz[1] = 0.2880402F * rgb[0] + 0.7118741F * rgb[1] + 0.0000857F * rgb[2];
    xyz[2] = 0.0000000F * rgb[0] + 0.0000000F * rgb[1] + 0.8252100F * rgb[2];
}

void lab_to_prophoto(const float lab[3], float rgb[3]) noexcept
{
    float xyz[3]{};
    lab_to_xyz_d50(lab, xyz);
    xyz_to_prophoto(xyz, rgb);
}

void prophoto_to_lab(const float rgb[3], float lab[3]) noexcept
{
    float xyz[3]{};
    prophoto_to_xyz(rgb, xyz);
    xyz_d50_to_lab(xyz, lab);
}

[[nodiscard]] float rgb_norm(const float rgb[3], const std::string_view preserve) noexcept
{
    if (preserve == kToneCurvePreserveColorsNone)
    {
        return (rgb[0] + rgb[1] + rgb[2]) / 3.0F;
    }
    if (preserve == kToneCurvePreserveColorsLuminance)
    {
        return 0.2880402F * rgb[0] + 0.7118741F * rgb[1] + 0.0000857F * rgb[2];
    }
    if (preserve == kToneCurvePreserveColorsMax)
    {
        return std::max(rgb[0], std::max(rgb[1], rgb[2]));
    }
    if (preserve == kToneCurvePreserveColorsSum)
    {
        return rgb[0] + rgb[1] + rgb[2];
    }
    if (preserve == kToneCurvePreserveColorsNorm)
    {
        return std::sqrt(rgb[0] * rgb[0] + rgb[1] * rgb[1] + rgb[2] * rgb[2]);
    }
    if (preserve == kToneCurvePreserveColorsPower)
    {
        const float r2 = rgb[0] * rgb[0];
        const float g2 = rgb[1] * rgb[1];
        const float b2 = rgb[2] * rgb[2];
        const float denom = r2 + g2 + b2;
        return denom > 0.0F ? (rgb[0] * r2 + rgb[1] * g2 + rgb[2] * b2) / denom : 0.0F;
    }
    return (rgb[0] + rgb[1] + rgb[2]) / 3.0F;
}

void estimate_exp(const float *x, const float *y, const int num, float coeff[3]) noexcept
{
    const float x0 = x[num - 1];
    const float y0 = y[num - 1];
    float g = 0.0F;
    int count = 0;
    for (int k = 0; k < num - 1; ++k)
    {
        if (y[k] / y0 > 0.0F && x[k] / x0 > 0.0F)
        {
            g += std::log(y[k] / y0) / std::log(x[k] / x0);
            ++count;
        }
    }
    coeff[0] = 1.0F / x0;
    coeff[1] = y0;
    coeff[2] = count > 0 ? g / static_cast<float>(count) : 1.0F;
}

[[nodiscard]] float eval_exp(const float coeff[3], const float x) noexcept
{
    return coeff[1] * std::pow(x * coeff[0], coeff[2]);
}

[[nodiscard]] float lookup_curve_lut(const std::vector<float> &lut, const float x, const float xm,
                                     const float coeff[3]) noexcept
{
    if (x < xm)
    {
        const int index = std::clamp(static_cast<int>(x * static_cast<float>(kToneCurveLut)), 0,
                                     kToneCurveLut - 1);
        return lut[static_cast<std::size_t>(index)];
    }
    return eval_exp(coeff, x);
}

void build_unit_lut(const std::vector<ToneCurvePoint> &points, std::vector<float> &lut)
{
    lut.assign(static_cast<std::size_t>(kToneCurveLut), 0.0F);
    for (int k = 0; k < kToneCurveLut; ++k)
    {
        lut[static_cast<std::size_t>(k)] = static_cast<float>(evaluate_tone_curve(
            points, static_cast<double>(k) / static_cast<double>(kToneCurveLut)));
    }
}

Result<void> apply_tone_curve(WorkingImage &image, const OperationInstance &operation)
{
    auto space = parse_tone_curve_working_space(
        parameter_string(operation, "working_space", std::string(kToneCurveWorkingSpaceRgb)));
    if (!space)
    {
        return space.error();
    }
    const auto interpolation = parameter_string(
        operation, "interpolation", std::string(kToneCurveInterpolationMonotoneHermite));
    if (interpolation != kToneCurveInterpolationMonotoneHermite)
    {
        return make_error(ErrorCode::kValidation, "Tone curve interpolation is unsupported",
                          {{"interpolation", interpolation}});
    }
    const auto channel_mode =
        parameter_string(operation, "channel_mode", std::string(kToneCurveChannelModeRgb));
    if (channel_mode != kToneCurveChannelModeRgb &&
        channel_mode != kToneCurveChannelModeIndependent)
    {
        return make_error(ErrorCode::kValidation, "Tone curve channel_mode is unsupported",
                          {{"channel_mode", channel_mode}});
    }
    const auto preserve = parameter_string(operation, "preserve_colors",
                                           std::string(kToneCurvePreserveColorsAverage));
    std::vector<ToneCurvePoint> points;
    if (const auto found = operation.parameters.find("points"); found != operation.parameters.end())
    {
        auto parsed = parse_tone_curve_points(found->second);
        if (!parsed)
        {
            return parsed.error();
        }
        points = std::move(parsed).value();
    }
    std::vector<ToneCurvePoint> points_a;
    std::vector<ToneCurvePoint> points_b;
    if (const auto found = operation.parameters.find("points_a");
        found != operation.parameters.end())
    {
        auto parsed = parse_tone_curve_points(found->second);
        if (!parsed)
        {
            return parsed.error();
        }
        points_a = std::move(parsed).value();
    }
    if (const auto found = operation.parameters.find("points_b");
        found != operation.parameters.end())
    {
        auto parsed = parse_tone_curve_points(found->second);
        if (!parsed)
        {
            return parsed.error();
        }
        points_b = std::move(parsed).value();
    }
    const bool independent = channel_mode == kToneCurveChannelModeIndependent ||
                             space.value() == ToneCurveWorkingSpace::kLabIndependent;
    if (tone_curve_is_identity(points) &&
        (!independent || (tone_curve_is_identity(points_a) && tone_curve_is_identity(points_b))))
    {
        return {};
    }

    std::vector<float> table_l;
    std::vector<float> table_a;
    std::vector<float> table_b;
    build_unit_lut(points, table_l);
    build_unit_lut(points_a, table_a);
    build_unit_lut(points_b, table_b);
    for (int k = 0; k < kToneCurveLut; ++k)
    {
        table_l[static_cast<std::size_t>(k)] *= 100.0F;
        table_a[static_cast<std::size_t>(k)] =
            table_a[static_cast<std::size_t>(k)] * 256.0F - 128.0F;
        table_b[static_cast<std::size_t>(k)] =
            table_b[static_cast<std::size_t>(k)] * 256.0F - 128.0F;
    }

    const bool rgb_linked = !independent && space.value() != ToneCurveWorkingSpace::kLab &&
                            space.value() != ToneCurveWorkingSpace::kXyz;
    const bool xyz_linked = space.value() == ToneCurveWorkingSpace::kXyz;
    const bool lab_linked = space.value() == ToneCurveWorkingSpace::kLab && !independent;

    if (xyz_linked)
    {
        for (int k = 0; k < kToneCurveLut; ++k)
        {
            const float t = static_cast<float>(k) / static_cast<float>(kToneCurveLut);
            float xyz[3]{t, t, t};
            float lab[3]{};
            xyz_d50_to_lab(xyz, lab);
            const int index =
                std::clamp(static_cast<int>(lab[0] / 100.0F * kToneCurveLut), 0, kToneCurveLut - 1);
            lab[0] = table_l[static_cast<std::size_t>(index)];
            lab_to_xyz_d50(lab, xyz);
            table_l[static_cast<std::size_t>(k)] = xyz[1];
        }
    }
    else if (rgb_linked)
    {
        for (int k = 0; k < kToneCurveLut; ++k)
        {
            const float t = static_cast<float>(k) / static_cast<float>(kToneCurveLut);
            float rgb[3]{t, t, t};
            float lab[3]{};
            prophoto_to_lab(rgb, lab);
            const int index =
                std::clamp(static_cast<int>(lab[0] / 100.0F * kToneCurveLut), 0, kToneCurveLut - 1);
            lab[0] = table_l[static_cast<std::size_t>(index)];
            lab_to_prophoto(lab, rgb);
            table_l[static_cast<std::size_t>(k)] = rgb[1];
        }
    }

    const float xm_l = points.empty() ? 1.0F : static_cast<float>(points.back().x);
    const float x_l[4] = {0.7F * xm_l, 0.8F * xm_l, 0.9F * xm_l, 1.0F * xm_l};
    float y_l[4]{};
    for (int i = 0; i < 4; ++i)
    {
        const int index =
            std::clamp(static_cast<int>(x_l[i] * kToneCurveLut), 0, kToneCurveLut - 1);
        y_l[i] = table_l[static_cast<std::size_t>(index)];
    }
    float unbounded_l[3]{};
    estimate_exp(x_l, y_l, 4, unbounded_l);

    const float low_approximation = table_l[static_cast<std::size_t>(0.01F * kToneCurveLut)];

    for (std::size_t index = 0; index + 2 < image.rgb.size(); index += 3)
    {
        float xyz[3]{};
        linear_rgb_to_xyz_d50(image.rgb[index], image.rgb[index + 1U], image.rgb[index + 2U], xyz);
        float lab[3]{};
        xyz_d50_to_lab(xyz, lab);
        const float l_in = lab[0] / 100.0F;
        if (rgb_linked)
        {
            float rgb[3]{};
            lab_to_prophoto(lab, rgb);
            if (preserve == kToneCurvePreserveColorsNone)
            {
                for (int c = 0; c < 3; ++c)
                {
                    rgb[c] = lookup_curve_lut(table_l, rgb[c], xm_l, unbounded_l);
                }
            }
            else
            {
                const float lum = rgb_norm(rgb, preserve);
                if (lum > 0.0F)
                {
                    const float curve_lum = lookup_curve_lut(table_l, lum, xm_l, unbounded_l);
                    const float ratio = curve_lum / lum;
                    rgb[0] *= ratio;
                    rgb[1] *= ratio;
                    rgb[2] *= ratio;
                }
            }
            prophoto_to_lab(rgb, lab);
        }
        else if (xyz_linked)
        {
            for (int c = 0; c < 3; ++c)
            {
                xyz[c] = lookup_curve_lut(table_l, xyz[c], xm_l, unbounded_l);
            }
            xyz_d50_to_lab(xyz, lab);
        }
        else if (lab_linked)
        {
            const float orig_l = lab[0];
            const float orig_a = lab[1];
            const float orig_b = lab[2];
            lab[0] = lookup_curve_lut(table_l, l_in, xm_l, unbounded_l);
            if (l_in > 0.01F)
            {
                lab[1] = orig_a * lab[0] / orig_l;
                lab[2] = orig_b * lab[0] / orig_l;
            }
            else
            {
                lab[1] = orig_a * low_approximation;
                lab[2] = orig_b * low_approximation;
            }
        }
        else
        {
            lab[0] = lookup_curve_lut(table_l, l_in, xm_l, unbounded_l);
            const float a_in = (lab[1] + 128.0F) / 256.0F;
            const float b_in = (lab[2] + 128.0F) / 256.0F;
            const int ia = std::clamp(static_cast<int>(a_in * kToneCurveLut), 0, kToneCurveLut - 1);
            const int ib = std::clamp(static_cast<int>(b_in * kToneCurveLut), 0, kToneCurveLut - 1);
            lab[1] = table_a[static_cast<std::size_t>(ia)];
            lab[2] = table_b[static_cast<std::size_t>(ib)];
        }
        lab_to_xyz_d50(lab, xyz);
        xyz_d50_to_linear_rgb(xyz, image.rgb[index], image.rgb[index + 1U], image.rgb[index + 2U]);
    }
    return {};
}

struct SigmoidCurve
{
    double white_target = 1.0;
    double black_target = 0.000152;
    double paper_exposure = 1.0;
    double film_fog = 0.0;
    double film_power = 1.0;
    double paper_power = 1.0;
    double hue_preservation = 1.0;
};

[[nodiscard]] double generalized_loglogistic(const double value, const double magnitude,
                                             const double paper_exposure, const double film_fog,
                                             const double film_power, const double paper_power)
{
    const double clamped = std::max(value, 0.0);
    const double film_response = std::pow(film_fog + clamped, film_power);
    const double paper_response =
        magnitude * std::pow(film_response / (paper_exposure + film_response), paper_power);
    return std::isfinite(paper_response) ? paper_response : magnitude;
}

[[nodiscard]] Result<SigmoidCurve> make_sigmoid_curve(const OperationInstance &operation)
{
    const double contrast = parameter(operation, "middle_grey_contrast", kSigmoidContrastDefault);
    const double skew = parameter(operation, "contrast_skewness", kSigmoidSkewDefault);
    SigmoidCurve curve;
    curve.white_target =
        0.01 * parameter(operation, "display_white_target", kSigmoidDisplayWhiteDefault);
    curve.black_target =
        0.01 * parameter(operation, "display_black_target", kSigmoidDisplayBlackDefault);
    curve.hue_preservation =
        parameter(operation, "hue_preservation", kSigmoidHuePreservationDefault);
    curve.paper_power = std::pow(5.0, -skew);

    constexpr double kDelta = 1e-6;
    const double reference_exposure =
        std::pow(kSigmoidMiddleGrey, contrast) * (1.0 / kSigmoidMiddleGrey - 1.0);
    const double reference_slope =
        (generalized_loglogistic(kSigmoidMiddleGrey + kDelta, 1.0, reference_exposure, 0.0,
                                 contrast, 1.0) -
         generalized_loglogistic(kSigmoidMiddleGrey - kDelta, 1.0, reference_exposure, 0.0,
                                 contrast, 1.0)) /
        (2.0 * kDelta);

    const double temporary_white_relation =
        std::pow(curve.white_target / kSigmoidMiddleGrey, 1.0 / curve.paper_power) - 1.0;
    const double temporary_exposure = kSigmoidMiddleGrey * temporary_white_relation;
    const double temporary_slope =
        (generalized_loglogistic(kSigmoidMiddleGrey + kDelta, curve.white_target,
                                 temporary_exposure, 0.0, 1.0, curve.paper_power) -
         generalized_loglogistic(kSigmoidMiddleGrey - kDelta, curve.white_target,
                                 temporary_exposure, 0.0, 1.0, curve.paper_power)) /
        (2.0 * kDelta);
    curve.film_power = reference_slope / temporary_slope;

    const double white_grey_relation =
        std::pow(curve.white_target / kSigmoidMiddleGrey, 1.0 / curve.paper_power) - 1.0;
    if (curve.black_target > 0.0)
    {
        const double white_black_relation =
            std::pow(curve.black_target / curve.white_target, -1.0 / curve.paper_power) - 1.0;
        const double grey_term = std::pow(white_grey_relation, 1.0 / curve.film_power);
        curve.film_fog = kSigmoidMiddleGrey * grey_term /
                         (std::pow(white_black_relation, 1.0 / curve.film_power) - grey_term);
    }
    curve.paper_exposure =
        std::pow(curve.film_fog + kSigmoidMiddleGrey, curve.film_power) * white_grey_relation;

    const std::array<double, 7> derived{
        curve.white_target, curve.black_target, curve.paper_exposure,   curve.film_fog,
        curve.film_power,   curve.paper_power,  curve.hue_preservation,
    };
    if (std::any_of(derived.begin(), derived.end(),
                    [](const double value) { return !std::isfinite(value); }) ||
        curve.white_target <= kSigmoidMiddleGrey || curve.black_target < 0.0 ||
        curve.black_target >= kSigmoidMiddleGrey || curve.paper_exposure <= 0.0 ||
        curve.film_power <= 0.0 || curve.paper_power <= 0.0 || curve.hue_preservation < 0.0 ||
        curve.hue_preservation > 1.0)
    {
        return make_error(ErrorCode::kValidation,
                          "Sigmoid parameters do not produce a finite monotonic curve");
    }
    return curve;
}

[[nodiscard]] std::array<std::size_t, 3> channel_order(const std::array<double, 3> &pixel) noexcept
{
    std::array<std::size_t, 3> order{0U, 1U, 2U};
    std::stable_sort(order.begin(), order.end(),
                     [&](const std::size_t left, const std::size_t right)
                     { return pixel[left] < pixel[right]; });
    return order;
}

[[nodiscard]] std::array<double, 3>
desaturate_negative_values(const std::array<double, 3> &pixel) noexcept
{
    const double average = std::max((pixel[0] + pixel[1] + pixel[2]) / 3.0, 0.0);
    const double minimum = std::min({pixel[0], pixel[1], pixel[2]});
    const double saturation = minimum < 0.0 ? -average / (minimum - average) : 1.0;
    return {
        average + saturation * (pixel[0] - average),
        average + saturation * (pixel[1] - average),
        average + saturation * (pixel[2] - average),
    };
}

[[nodiscard]] std::array<double, 3> preserve_sigmoid_hue(const std::array<double, 3> &input,
                                                         const std::array<double, 3> &per_channel,
                                                         const double preservation) noexcept
{
    const auto order = channel_order(input);
    const std::size_t minimum = order[0];
    const std::size_t middle = order[1];
    const std::size_t maximum = order[2];
    const double chroma = input[maximum] - input[minimum];
    const double middle_scale = chroma != 0.0 ? (input[middle] - input[minimum]) / chroma : 0.0;
    const double full_hue_middle =
        per_channel[minimum] + (per_channel[maximum] - per_channel[minimum]) * middle_scale;
    const double naive_hue_middle =
        (1.0 - preservation) * per_channel[middle] + preservation * full_hue_middle;
    const double per_channel_energy = per_channel[0] + per_channel[1] + per_channel[2];
    const double naive_hue_energy = per_channel[minimum] + naive_hue_middle + per_channel[maximum];
    const double minimum_plus_middle = input[minimum] + input[middle];
    const double blend =
        minimum_plus_middle != 0.0 ? 2.0 * input[minimum] / minimum_plus_middle : 0.0;
    const double target_energy = blend * per_channel_energy + (1.0 - blend) * naive_hue_energy;

    std::array<double, 3> output{};
    if (naive_hue_middle <= per_channel[middle])
    {
        const double corrected_middle =
            ((1.0 - preservation) * per_channel[middle] +
             preservation * (middle_scale * per_channel[maximum] +
                             (1.0 - middle_scale) * (target_energy - per_channel[maximum]))) /
            (1.0 + preservation * (1.0 - middle_scale));
        output[minimum] = target_energy - per_channel[maximum] - corrected_middle;
        output[middle] = corrected_middle;
        output[maximum] = per_channel[maximum];
    }
    else
    {
        const double corrected_middle =
            ((1.0 - preservation) * per_channel[middle] +
             preservation * (per_channel[minimum] * (1.0 - middle_scale) +
                             middle_scale * (target_energy - per_channel[minimum]))) /
            (1.0 + preservation * middle_scale);
        output[minimum] = per_channel[minimum];
        output[middle] = corrected_middle;
        output[maximum] = target_energy - per_channel[minimum] - corrected_middle;
    }
    return output;
}

Result<void> apply_sigmoid(WorkingImage &image, const OperationInstance &operation,
                           const CancellationToken &cancellation)
{
    const auto working_space =
        parameter_string(operation, "working_space", std::string(kSigmoidWorkingSpaceLinearSrgb));
    const auto color_processing = parameter_string(operation, "color_processing",
                                                   std::string(kSigmoidColorProcessingPerChannel));
    if (working_space != kSigmoidWorkingSpaceLinearSrgb ||
        (color_processing != kSigmoidColorProcessingPerChannel &&
         color_processing != kSigmoidColorProcessingRgbRatio))
    {
        return make_error(
            ErrorCode::kValidation, "Sigmoid color policy is unsupported",
            {{"working_space", working_space}, {"color_processing", color_processing}});
    }
    auto curve = make_sigmoid_curve(operation);
    if (!curve)
    {
        return curve.error();
    }

    std::atomic<bool> invalid{false};
    std::atomic<std::size_t> invalid_index{0};
    std::atomic<bool> invalid_output{false};
    const auto rows = for_each_row(
        image.height, cancellation,
        [&](const std::uint32_t y)
        {
            if (invalid.load(std::memory_order_acquire))
            {
                return;
            }
            for (std::uint32_t x = 0; x < image.width; ++x)
            {
                const std::size_t index = (static_cast<std::size_t>(y) * image.width + x) * 3U;
                const std::array<double, 3> input{
                    image.rgb[index],
                    image.rgb[index + 1U],
                    image.rgb[index + 2U],
                };
                if (std::any_of(input.begin(), input.end(),
                                [](const double value) { return !std::isfinite(value); }))
                {
                    invalid_index.store(index, std::memory_order_relaxed);
                    invalid_output.store(false, std::memory_order_relaxed);
                    invalid.store(true, std::memory_order_release);
                    return;
                }
                const auto positive = desaturate_negative_values(input);
                std::array<double, 3> output{};
                if (color_processing == kSigmoidColorProcessingRgbRatio)
                {
                    const double luma_value = (positive[0] + positive[1] + positive[2]) / 3.0;
                    const double mapped_luma = generalized_loglogistic(
                        luma_value, curve.value().white_target, curve.value().paper_exposure,
                        curve.value().film_fog, curve.value().film_power,
                        curve.value().paper_power);
                    std::array<double, 3> pre_out{};
                    if (luma_value > 1.0e-9)
                    {
                        const double scale = mapped_luma / luma_value;
                        pre_out = {scale * positive[0], scale * positive[1], scale * positive[2]};
                    }
                    else
                    {
                        pre_out = {mapped_luma, mapped_luma, mapped_luma};
                    }
                    const auto order = channel_order(pre_out);
                    const double pixel_min = pre_out[order[0]];
                    const double pixel_max = pre_out[order[2]];
                    constexpr double kEpsilon = 1.0e-6;
                    const double display_white = (curve.value().white_target - mapped_luma) /
                                                 (pixel_max - mapped_luma + kEpsilon);
                    const double display_black = (curve.value().black_target - mapped_luma) /
                                                 (pixel_min - mapped_luma - kEpsilon);
                    const double display_border = std::min(display_white, display_black);
                    const double chroma_vs_border =
                        (mapped_luma - pixel_min) / (mapped_luma + kEpsilon);
                    const double adjustment = 1.0 / (chroma_vs_border * display_border + kEpsilon);
                    const double hyperbolic =
                        2.0 * chroma_vs_border /
                        (1.0 - chroma_vs_border * chroma_vs_border + kEpsilon) * adjustment;
                    const double hyperbolic_z = std::sqrt(hyperbolic * hyperbolic + 1.0);
                    const double chroma_factor = hyperbolic / (1.0 + hyperbolic_z) * display_border;
                    output = {mapped_luma + chroma_factor * (pre_out[0] - mapped_luma),
                              mapped_luma + chroma_factor * (pre_out[1] - mapped_luma),
                              mapped_luma + chroma_factor * (pre_out[2] - mapped_luma)};
                }
                else
                {
                    std::array<double, 3> mapped{};
                    for (std::size_t channel = 0; channel < mapped.size(); ++channel)
                    {
                        mapped[channel] = generalized_loglogistic(
                            positive[channel], curve.value().white_target,
                            curve.value().paper_exposure, curve.value().film_fog,
                            curve.value().film_power, curve.value().paper_power);
                    }
                    output = preserve_sigmoid_hue(positive, mapped, curve.value().hue_preservation);
                }
                if (std::any_of(output.begin(), output.end(),
                                [](const double value) { return !std::isfinite(value); }))
                {
                    invalid_index.store(index, std::memory_order_relaxed);
                    invalid_output.store(true, std::memory_order_relaxed);
                    invalid.store(true, std::memory_order_release);
                    return;
                }
                image.rgb[index] = static_cast<float>(output[0]);
                image.rgb[index + 1U] = static_cast<float>(output[1]);
                image.rgb[index + 2U] = static_cast<float>(output[2]);
            }
        });
    if (!rows)
    {
        return rows.error();
    }
    if (invalid.load(std::memory_order_acquire))
    {
        return make_error(
            ErrorCode::kValidation,
            invalid_output.load(std::memory_order_relaxed) ?
                "Sigmoid produced a non-finite sample" :
                "Sigmoid input contains a non-finite sample",
            {{"sample_index", std::to_string(invalid_index.load(std::memory_order_relaxed))}});
    }
    return {};
}

[[nodiscard]] float working_luminance(const float r, const float g, const float b)
{
    return 0.2225045F * r + 0.7168786F * g + 0.0606169F * b;
}

[[nodiscard]] Result<void> validate_exposure_input(const WorkingImage &input,
                                                   const CancellationToken &cancellation)
{
    const std::uint64_t pixels = static_cast<std::uint64_t>(input.width) * input.height;
    if (input.width == 0 || input.height == 0 ||
        pixels > std::numeric_limits<std::size_t>::max() / 3U ||
        input.rgb.size() != static_cast<std::size_t>(pixels * 3U))
    {
        return make_error(ErrorCode::kValidation,
                          "Exposure input buffer does not match its dimensions",
                          {{"reason", "invalid_exposure_dimensions"}});
    }
    if (input.color_profile.model != ColorModel::kRgb)
    {
        return make_error(ErrorCode::kUnsupported, "Exposure requires an RGB working buffer",
                          {{"reason", "unsupported_exposure_color_model"}});
    }
    for (std::uint32_t row = 0; row < input.height; ++row)
    {
        auto active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
        const std::size_t begin = static_cast<std::size_t>(row) * input.width * 3U;
        const std::size_t end = begin + static_cast<std::size_t>(input.width) * 3U;
        for (std::size_t index = begin; index < end; ++index)
        {
            if (!std::isfinite(input.rgb[index]))
            {
                return make_error(ErrorCode::kValidation,
                                  "Exposure input contains a non-finite sample",
                                  {{"reason", "non_finite_exposure_input"},
                                   {"sample_index", std::to_string(index)}});
            }
        }
    }
    return {};
}

[[nodiscard]] Result<double> exposure_metadata_ev(const WorkingImage &input,
                                                  const ExposureParams &params)
{
    if (!params.compensate_exposure_bias && !params.compensate_highlight_preservation)
    {
        return params.exposure_ev;
    }
    if (!input.exposure_analysis)
    {
        const bool bias = params.compensate_exposure_bias;
        return make_error(ErrorCode::kUnsupported,
                          bias ?
                              "Exposure-bias compensation requires bounded RAW metadata" :
                              "Highlight-preservation compensation requires bounded RAW metadata",
                          {{"operation_id", std::string(kExposureOperationId)},
                           {"reason", bias ? "exposure_bias_compensation_requires_metadata" :
                                             "exposure_highlight_compensation_requires_metadata"}});
    }
    const auto &metadata = input.exposure_analysis->metadata;
    if (metadata.status == RawExposureMetadataStatus::kReadFailed)
    {
        std::map<std::string, std::string, std::less<>> context{
            {"operation_id", std::string(kExposureOperationId)},
            {"reason", "exposure_metadata_read_failed"}};
        if (!metadata.failure_detail.empty())
        {
            context.emplace("detail", metadata.failure_detail);
        }
        return make_error(ErrorCode::kIo, "RAW exposure metadata could not be read",
                          std::move(context));
    }
    if (metadata.status != RawExposureMetadataStatus::kReady)
    {
        return make_error(ErrorCode::kUnsupported,
                          "RAW exposure metadata was not captured for this working buffer",
                          {{"operation_id", std::string(kExposureOperationId)},
                           {"reason", "exposure_metadata_unavailable"}});
    }
    if (!std::isfinite(metadata.exposure_bias_ev) ||
        !std::isfinite(metadata.highlight_preservation_ev))
    {
        return make_error(ErrorCode::kValidation, "RAW exposure metadata is not finite",
                          {{"reason", "non_finite_exposure_metadata"}});
    }
    double effective = params.exposure_ev;
    if (params.compensate_exposure_bias)
    {
        effective -= std::clamp(metadata.exposure_bias_ev, -5.0, 5.0);
    }
    if (params.compensate_highlight_preservation)
    {
        effective += std::clamp(metadata.highlight_preservation_ev, -1.0, 4.0);
    }
    if (!std::isfinite(effective))
    {
        return make_error(ErrorCode::kValidation,
                          "Effective metadata-compensated exposure is not finite",
                          {{"reason", "non_finite_effective_exposure"}});
    }
    return effective;
}

[[nodiscard]] Result<double> exposure_deflicker_ev(const WorkingImage &input,
                                                   const ExposureParams &params,
                                                   const CancellationToken &cancellation)
{
    if (!input.exposure_analysis)
    {
        return make_error(
            ErrorCode::kUnsupported,
            "Exposure deflicker requires the original single-channel uint16 RAW histogram",
            {{"operation_id", std::string(kExposureOperationId)},
             {"reason", "exposure_deflicker_requires_raw_histogram"}});
    }
    const auto &analysis = *input.exposure_analysis;
    if (analysis.raw_histogram.size() != kExposureRawHistogramBins ||
        analysis.raw_pixel_count == 0U)
    {
        return make_error(ErrorCode::kValidation, "Exposure RAW histogram is malformed",
                          {{"reason", "invalid_exposure_raw_histogram"}});
    }
    if (analysis.raw_black_level >= analysis.raw_white_level ||
        analysis.raw_white_level >= kExposureRawHistogramBins)
    {
        return make_error(ErrorCode::kValidation, "Exposure RAW black/white levels are invalid",
                          {{"reason", "invalid_exposure_raw_levels"}});
    }

    std::uint64_t histogram_pixels = 0U;
    for (std::size_t bin = 0; bin < analysis.raw_histogram.size(); ++bin)
    {
        if (bin % 4096U == 0U)
        {
            auto active = cancellation.check();
            if (!active)
            {
                return active.error();
            }
        }
        histogram_pixels += analysis.raw_histogram[bin];
    }
    if (histogram_pixels != analysis.raw_pixel_count)
    {
        return make_error(ErrorCode::kValidation,
                          "Exposure RAW histogram pixel count is inconsistent",
                          {{"reason", "invalid_exposure_raw_histogram_count"}});
    }
    const double pixels = static_cast<double>(analysis.raw_pixel_count);
    const double threshold = std::clamp(pixels * params.deflicker_percentile / 100.0, 0.0, pixels);
    std::uint64_t cumulative = 0U;
    std::uint32_t raw_value = 0U;
    for (std::size_t bin = 0; bin < analysis.raw_histogram.size(); ++bin)
    {
        cumulative += analysis.raw_histogram[bin];
        if (static_cast<double>(cumulative) >= threshold)
        {
            raw_value = static_cast<std::uint32_t>(bin);
            break;
        }
    }

    const std::uint32_t raw_range = analysis.raw_white_level - analysis.raw_black_level;
    const std::int64_t black_relative =
        static_cast<std::int64_t>(raw_value) - analysis.raw_black_level;
    const std::int64_t bounded_raw = std::max<std::int64_t>(black_relative, 1);
    const double raw_ev =
        -std::log2(static_cast<double>(raw_range)) + std::log2(static_cast<double>(bounded_raw));
    const double effective = params.deflicker_target_ev - raw_ev;
    if (!std::isfinite(raw_ev) || !std::isfinite(effective))
    {
        return make_error(ErrorCode::kValidation, "Exposure deflicker correction is not finite",
                          {{"reason", "non_finite_exposure_deflicker"}});
    }
    return effective;
}

[[nodiscard]] Result<WorkingImage> apply_exposure_impl(const WorkingImage &input,
                                                       const ExposureParams &params,
                                                       const CancellationToken &cancellation)
{
    auto active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    auto valid_parameters = validate_exposure_parameters(exposure_to_parameters(params));
    if (!valid_parameters)
    {
        return valid_parameters.error();
    }
    auto valid_input = validate_exposure_input(input, cancellation);
    if (!valid_input)
    {
        return valid_input.error();
    }
    auto effective_exposure = params.mode == kExposureModeDeflicker ?
                                  exposure_deflicker_ev(input, params, cancellation) :
                                  exposure_metadata_ev(input, params);
    if (!effective_exposure)
    {
        return effective_exposure.error();
    }

    const double white = std::exp2(-effective_exposure.value());
    const double denominator = white - params.black;
    const double scale = 1.0 / denominator;
    if (!std::isfinite(white) || !std::isfinite(denominator) || denominator <= 0.0 ||
        !std::isfinite(scale))
    {
        return make_error(ErrorCode::kValidation, "Exposure scale is not representable",
                          {{"reason", "invalid_exposure_denominator"}});
    }

    WorkingImage output;
    output.width = input.width;
    output.height = input.height;
    output.color_profile = input.color_profile;
    output.exposure_analysis = input.exposure_analysis;
    output.rgb.resize(input.rgb.size());
    for (std::uint32_t row = 0; row < input.height; ++row)
    {
        active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
        const std::size_t begin = static_cast<std::size_t>(row) * input.width * 3U;
        const std::size_t end = begin + static_cast<std::size_t>(input.width) * 3U;
        for (std::size_t index = begin; index < end; ++index)
        {
            const double adjusted = (static_cast<double>(input.rgb[index]) - params.black) * scale;
            if (!std::isfinite(adjusted) ||
                std::abs(adjusted) > static_cast<double>(std::numeric_limits<float>::max()))
            {
                return make_error(ErrorCode::kValidation,
                                  "Exposure produced an unrepresentable sample",
                                  {{"reason", "unrepresentable_exposure_sample"},
                                   {"sample_index", std::to_string(index)}});
            }
            output.rgb[index] = static_cast<float>(adjusted);
        }
    }
    return output;
}

void apply_contrast(WorkingImage &image, const double amount)
{
    if (amount == 0.0)
    {
        return;
    }
    // darktable basic adjustments preserves the working-profile luminance by default and
    // applies contrast as an exponent around middle grey.
    constexpr float kMiddleGrey = 0.1842F;
    const float contrast = 1.0F + static_cast<float>(amount);
    for (std::size_t index = 0; index + 2U < image.rgb.size(); index += 3U)
    {
        const float luminance =
            working_luminance(image.rgb[index], image.rgb[index + 1U], image.rgb[index + 2U]);
        if (luminance <= 0.0F)
        {
            continue;
        }
        const float adjusted = std::pow(luminance / kMiddleGrey, contrast) * kMiddleGrey;
        const float scale = adjusted / luminance;
        image.rgb[index] *= scale;
        image.rgb[index + 1U] *= scale;
        image.rgb[index + 2U] *= scale;
    }
}

void rgb_to_lab(const float r, const float g, const float b, float &L, float &a, float &b_ch)
{
    float xyz[3]{};
    float lab[3]{};
    linear_rgb_to_xyz_d50(r, g, b, xyz);
    xyz_d50_to_lab(xyz, lab);
    L = lab[0];
    a = lab[1];
    b_ch = lab[2];
}

void lab_to_rgb(const float L, const float a, const float b_ch, float &r, float &g, float &b)
{
    const float lab[3]{L, a, b_ch};
    float xyz[3]{};
    lab_to_xyz_d50(lab, xyz);
    xyz_d50_to_linear_rgb(xyz, r, g, b);
}

void rgb_to_hsl(float r, float g, float b, float &h, float &s, float &l);
void hsl_to_rgb(float h, float s, float l, float &r, float &g, float &b);

void apply_highlights_shadows(WorkingImage &image, const double highlights, const double shadows)
{
    if (highlights == 0.0 && shadows == 0.0)
    {
        return;
    }
    const float highlight_ev = static_cast<float>(highlights);
    const float shadow_ev = static_cast<float>(shadows);
    const float compress = 0.5F;
    for (std::size_t index = 0; index + 2 < image.rgb.size(); index += 3)
    {
        float L = 0.0F;
        float a = 0.0F;
        float b = 0.0F;
        rgb_to_lab(image.rgb[index], image.rgb[index + 1U], image.rgb[index + 2U], L, a, b);
        const float ln = std::clamp(L / 100.0F, 0.0F, 1.0F);
        const float hmask =
            std::clamp((ln - (0.5F + compress * 0.25F)) / (0.5F - compress * 0.25F), 0.0F, 1.0F);
        const float smask =
            1.0F - std::clamp(ln / std::max(0.15F, 0.5F - compress * 0.25F), 0.0F, 1.0F);
        L *= std::exp2(highlight_ev * hmask) * std::exp2(shadow_ev * smask);
        lab_to_rgb(L, a, b, image.rgb[index], image.rgb[index + 1U], image.rgb[index + 2U]);
    }
}

void apply_whites_blacks(WorkingImage &image, const double whites, const double blacks)
{
    const float white = std::max(0.05F, 1.0F + static_cast<float>(whites) * 0.5F);
    const float black = static_cast<float>(blacks) * 0.25F;
    const float denom = std::max(1.0e-5F, white - black);
    for (float &sample : image.rgb)
    {
        sample = (sample - black) / denom;
    }
}

void apply_vibrance_saturation(WorkingImage &image, const double vibrance, const double saturation)
{
    if (vibrance == 0.0 && saturation == 0.0)
    {
        return;
    }
    for (std::size_t index = 0; index + 2 < image.rgb.size(); index += 3)
    {
        float &r = image.rgb[index];
        float &g = image.rgb[index + 1U];
        float &b = image.rgb[index + 2U];
        const float average = (r + g + b) / 3.0F;
        const float dr = average - r;
        const float dg = average - g;
        const float db = average - b;
        const float delta = std::sqrt(dr * dr + dg * dg + db * db);
        const float vibrance_amount = static_cast<float>(vibrance) / 1.4F;
        const float vibrance_gain =
            vibrance_amount * (1.0F - std::pow(delta, std::abs(vibrance_amount)));
        const float gain = 1.0F + static_cast<float>(saturation) + vibrance_gain;
        r = average + gain * (r - average);
        g = average + gain * (g - average);
        b = average + gain * (b - average);
    }
}

[[nodiscard]] Result<WorkingImage> rotate_working(WorkingImage image, const int quarters)
{
    const int turns = ((quarters % 4) + 4) % 4;
    if (turns == 0)
    {
        return image;
    }
    WorkingImage output;
    output.color_profile = image.color_profile;
    output.exposure_analysis = image.exposure_analysis;
    if (turns == 2)
    {
        output.width = image.width;
        output.height = image.height;
        output.rgb.resize(image.rgb.size());
        for (std::uint32_t y = 0; y < image.height; ++y)
        {
            for (std::uint32_t x = 0; x < image.width; ++x)
            {
                const std::size_t src = (static_cast<std::size_t>(y) * image.width + x) * 3U;
                const std::size_t dst =
                    (static_cast<std::size_t>(image.height - 1U - y) * image.width +
                     (image.width - 1U - x)) *
                    3U;
                output.rgb[dst] = image.rgb[src];
                output.rgb[dst + 1U] = image.rgb[src + 1U];
                output.rgb[dst + 2U] = image.rgb[src + 2U];
            }
        }
        return output;
    }

    output.width = image.height;
    output.height = image.width;
    output.rgb.resize(static_cast<std::size_t>(output.width) * output.height * 3U);
    for (std::uint32_t y = 0; y < image.height; ++y)
    {
        for (std::uint32_t x = 0; x < image.width; ++x)
        {
            const std::size_t src = (static_cast<std::size_t>(y) * image.width + x) * 3U;
            const std::uint32_t dx = turns == 1 ? image.height - 1U - y : y;
            const std::uint32_t dy = turns == 1 ? x : image.width - 1U - x;
            const std::size_t dst = (static_cast<std::size_t>(dy) * output.width + dx) * 3U;
            output.rgb[dst] = image.rgb[src];
            output.rgb[dst + 1U] = image.rgb[src + 1U];
            output.rgb[dst + 2U] = image.rgb[src + 2U];
        }
    }
    return output;
}

[[nodiscard]] Result<WorkingImage> crop_working(WorkingImage image, const double x, const double y,
                                                const double width, const double height)
{
    if (image.width == 0 || image.height == 0)
    {
        return make_error(ErrorCode::kValidation, "Cannot crop an empty image");
    }
    const auto left =
        static_cast<std::uint32_t>(std::clamp(std::llround(x * static_cast<double>(image.width)),
                                              0LL, static_cast<long long>(image.width - 1U)));
    const auto top =
        static_cast<std::uint32_t>(std::clamp(std::llround(y * static_cast<double>(image.height)),
                                              0LL, static_cast<long long>(image.height - 1U)));
    auto crop_w = static_cast<std::uint32_t>(
        std::clamp(std::llround(width * static_cast<double>(image.width)), 1LL,
                   static_cast<long long>(image.width)));
    auto crop_h = static_cast<std::uint32_t>(
        std::clamp(std::llround(height * static_cast<double>(image.height)), 1LL,
                   static_cast<long long>(image.height)));
    if (left + crop_w > image.width)
    {
        crop_w = image.width - left;
    }
    if (top + crop_h > image.height)
    {
        crop_h = image.height - top;
    }
    if (crop_w == 0 || crop_h == 0)
    {
        return make_error(ErrorCode::kValidation, "Crop rectangle is empty");
    }
    WorkingImage output;
    output.color_profile = image.color_profile;
    output.exposure_analysis = image.exposure_analysis;
    output.width = crop_w;
    output.height = crop_h;
    output.rgb.resize(static_cast<std::size_t>(crop_w) * crop_h * 3U);
    for (std::uint32_t row = 0; row < crop_h; ++row)
    {
        const float *src =
            image.rgb.data() + (static_cast<std::size_t>(top + row) * image.width + left) * 3U;
        float *dst = output.rgb.data() + static_cast<std::size_t>(row) * crop_w * 3U;
        std::copy_n(src, static_cast<std::size_t>(crop_w) * 3U, dst);
    }
    return output;
}

[[nodiscard]] Result<WorkingImage> flip_working(WorkingImage image, const bool horizontal,
                                                const bool vertical)
{
    if (!horizontal && !vertical)
    {
        return image;
    }
    WorkingImage output;
    output.color_profile = image.color_profile;
    output.exposure_analysis = image.exposure_analysis;
    output.width = image.width;
    output.height = image.height;
    output.rgb.resize(image.rgb.size());
    for (std::uint32_t y = 0; y < image.height; ++y)
    {
        for (std::uint32_t x = 0; x < image.width; ++x)
        {
            const std::uint32_t dx = horizontal ? image.width - 1U - x : x;
            const std::uint32_t dy = vertical ? image.height - 1U - y : y;
            const std::size_t src = (static_cast<std::size_t>(y) * image.width + x) * 3U;
            const std::size_t dst = (static_cast<std::size_t>(dy) * output.width + dx) * 3U;
            output.rgb[dst] = image.rgb[src];
            output.rgb[dst + 1U] = image.rgb[src + 1U];
            output.rgb[dst + 2U] = image.rgb[src + 2U];
        }
    }
    return output;
}

void sample_bilinear(const WorkingImage &image, double sx, double sy, float *rgb)
{
    if (image.width == 0 || image.height == 0)
    {
        rgb[0] = 0.0F;
        rgb[1] = 0.0F;
        rgb[2] = 0.0F;
        return;
    }
    sx = std::clamp(sx, 0.0, static_cast<double>(image.width - 1U));
    sy = std::clamp(sy, 0.0, static_cast<double>(image.height - 1U));
    const auto x0 = static_cast<std::uint32_t>(sx);
    const auto y0 = static_cast<std::uint32_t>(sy);
    const auto x1 = std::min(x0 + 1U, image.width - 1U);
    const auto y1 = std::min(y0 + 1U, image.height - 1U);
    const float tx = static_cast<float>(sx - static_cast<double>(x0));
    const float ty = static_cast<float>(sy - static_cast<double>(y0));
    const std::size_t i00 = (static_cast<std::size_t>(y0) * image.width + x0) * 3U;
    const std::size_t i10 = (static_cast<std::size_t>(y0) * image.width + x1) * 3U;
    const std::size_t i01 = (static_cast<std::size_t>(y1) * image.width + x0) * 3U;
    const std::size_t i11 = (static_cast<std::size_t>(y1) * image.width + x1) * 3U;
    for (std::size_t channel = 0; channel < 3; ++channel)
    {
        const float top = image.rgb[i00 + channel] * (1.0F - tx) + image.rgb[i10 + channel] * tx;
        const float bottom = image.rgb[i01 + channel] * (1.0F - tx) + image.rgb[i11 + channel] * tx;
        rgb[channel] = top * (1.0F - ty) + bottom * ty;
    }
}

[[nodiscard]] float coverage_smoothstep(const double inset, const double width)
{
    const double t = std::clamp((inset + width) / std::max(2.0 * width, 1.0e-6), 0.0, 1.0);
    return static_cast<float>(t * t * (3.0 - 2.0 * t));
}

[[nodiscard]] Result<WorkingImage> straighten_working(WorkingImage image, const double degrees)
{
    if (image.width == 0 || image.height == 0 || std::abs(degrees) < 1e-4)
    {
        return image;
    }
    const double rad = degrees * std::numbers::pi / 180.0;
    const double inv_c = std::cos(-rad);
    const double inv_s = std::sin(-rad);
    const double width = static_cast<double>(image.width);
    const double height = static_cast<double>(image.height);
    const double cx = (width - 1.0) * 0.5;
    const double cy = (height - 1.0) * 0.5;
    WorkingImage output;
    output.color_profile = image.color_profile;
    output.exposure_analysis = image.exposure_analysis;
    output.width = image.width;
    output.height = image.height;
    output.rgb.assign(image.rgb.size(), 0.0F);
    constexpr double kEdgeAa = 1.25;
    for (std::uint32_t y = 0; y < output.height; ++y)
    {
        for (std::uint32_t x = 0; x < output.width; ++x)
        {
            const double dx = static_cast<double>(x) - cx;
            const double dy = static_cast<double>(y) - cy;
            const double sx = inv_c * dx - inv_s * dy + cx;
            const double sy = inv_s * dx + inv_c * dy + cy;
            const double inset = std::min(std::min(sx + 0.5, width - 0.5 - sx),
                                          std::min(sy + 0.5, height - 0.5 - sy));
            const float cover = coverage_smoothstep(inset, kEdgeAa);
            float *dst = output.rgb.data() + (static_cast<std::size_t>(y) * output.width + x) * 3U;
            if (cover <= 0.0F)
            {
                continue;
            }
            sample_bilinear(image, sx, sy, dst);
            if (cover < 1.0F)
            {
                dst[0] *= cover;
                dst[1] *= cover;
                dst[2] *= cover;
            }
        }
    }
    return output;
}

void box_blur(WorkingImage &image, const int radius, const int passes)
{
    if (radius <= 0 || image.width == 0 || image.height == 0)
    {
        return;
    }
    const int rad = std::min(radius, 8);
    const int width = static_cast<int>(image.width);
    const int height = static_cast<int>(image.height);
    std::vector<float> source = image.rgb;
    std::vector<float> destination(image.rgb.size());
    const auto sample = [&](const std::vector<float> &buffer, const int x, const int y,
                            const int channel) -> float
    {
        const int sx = std::clamp(x, 0, width - 1);
        const int sy = std::clamp(y, 0, height - 1);
        return buffer[(static_cast<std::size_t>(sy) * static_cast<std::size_t>(width) +
                       static_cast<std::size_t>(sx)) *
                          3U +
                      static_cast<std::size_t>(channel)];
    };
    for (int pass = 0; pass < passes; ++pass)
    {
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                for (int channel = 0; channel < 3; ++channel)
                {
                    float sum = 0.0F;
                    int count = 0;
                    for (int offset = -rad; offset <= rad; ++offset)
                    {
                        sum += sample(source, x + offset, y, channel);
                        ++count;
                    }
                    destination[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                 static_cast<std::size_t>(x)) *
                                    3U +
                                static_cast<std::size_t>(channel)] =
                        sum / static_cast<float>(count);
                }
            }
        }
        source.swap(destination);
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                for (int channel = 0; channel < 3; ++channel)
                {
                    float sum = 0.0F;
                    int count = 0;
                    for (int offset = -rad; offset <= rad; ++offset)
                    {
                        sum += sample(source, x, y + offset, channel);
                        ++count;
                    }
                    destination[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                 static_cast<std::size_t>(x)) *
                                    3U +
                                static_cast<std::size_t>(channel)] =
                        sum / static_cast<float>(count);
                }
            }
        }
        source.swap(destination);
    }
    image.rgb = std::move(source);
}

void blur_luma_gaussian(std::vector<float> &luma, const std::uint32_t width,
                        const std::uint32_t height, const int radius)
{
    if (radius <= 0 || width == 0 || height == 0)
    {
        return;
    }
    const int rad = std::min(radius, 12);
    const float sigma2 = (1.0F / (2.5F * 2.5F)) * static_cast<float>(rad * rad);
    std::vector<float> kernel(static_cast<std::size_t>(2 * rad + 1));
    float weight = 0.0F;
    for (int offset = -rad; offset <= rad; ++offset)
    {
        kernel[static_cast<std::size_t>(offset + rad)] =
            std::exp(-static_cast<float>(offset * offset) / (2.0F * sigma2));
        weight += kernel[static_cast<std::size_t>(offset + rad)];
    }
    for (float &sample : kernel)
    {
        sample /= weight;
    }
    std::vector<float> temp(luma.size());
    const int w = static_cast<int>(width);
    const int h = static_cast<int>(height);
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            float sum = 0.0F;
            for (int offset = -rad; offset <= rad; ++offset)
            {
                const int xx = std::clamp(x + offset, 0, w - 1);
                sum += luma[static_cast<std::size_t>(y) * width + static_cast<std::uint32_t>(xx)] *
                       kernel[static_cast<std::size_t>(offset + rad)];
            }
            temp[static_cast<std::size_t>(y) * width + static_cast<std::uint32_t>(x)] = sum;
        }
    }
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            float sum = 0.0F;
            for (int offset = -rad; offset <= rad; ++offset)
            {
                const int yy = std::clamp(y + offset, 0, h - 1);
                sum += temp[static_cast<std::size_t>(yy) * width + static_cast<std::uint32_t>(x)] *
                       kernel[static_cast<std::size_t>(offset + rad)];
            }
            luma[static_cast<std::size_t>(y) * width + static_cast<std::uint32_t>(x)] = sum;
        }
    }
}

void apply_unsharp(WorkingImage &image, const double amount, const double radius,
                   const double threshold)
{
    if (amount == 0.0 || radius <= 0.0)
    {
        return;
    }
    const int rad = static_cast<int>(std::ceil(2.5 * std::clamp(radius, 0.0, 12.0)));
    if (rad <= 0 || image.width < static_cast<std::uint32_t>(2 * rad + 1) ||
        image.height < static_cast<std::uint32_t>(2 * rad + 1))
    {
        return;
    }
    const std::size_t pixels = static_cast<std::size_t>(image.width) * image.height;
    std::vector<float> L(pixels);
    std::vector<float> a(pixels);
    std::vector<float> b(pixels);
    for (std::size_t index = 0; index < pixels; ++index)
    {
        rgb_to_lab(image.rgb[index * 3U], image.rgb[index * 3U + 1U], image.rgb[index * 3U + 2U],
                   L[index], a[index], b[index]);
    }
    std::vector<float> blurred = L;
    blur_luma_gaussian(blurred, image.width, image.height, rad);
    const float gain = static_cast<float>(amount);
    const float limit = static_cast<float>(threshold);
    for (std::size_t index = 0; index < pixels; ++index)
    {
        const float diff = L[index] - blurred[index];
        const float absdiff = std::abs(diff);
        const float detail =
            absdiff > limit ? std::copysign(std::max(absdiff - limit, 0.0F), diff) : 0.0F;
        lab_to_rgb(L[index] + detail * gain, a[index], b[index], image.rgb[index * 3U],
                   image.rgb[index * 3U + 1U], image.rgb[index * 3U + 2U]);
    }
}

void apply_clarity(WorkingImage &image, const double amount)
{
    if (amount == 0.0)
    {
        return;
    }
    const std::size_t pixels = static_cast<std::size_t>(image.width) * image.height;
    std::vector<float> L(pixels);
    std::vector<float> a(pixels);
    std::vector<float> b(pixels);
    for (std::size_t index = 0; index < pixels; ++index)
    {
        rgb_to_lab(image.rgb[index * 3U], image.rgb[index * 3U + 1U], image.rgb[index * 3U + 2U],
                   L[index], a[index], b[index]);
    }
    std::vector<float> inverted(pixels);
    for (std::size_t index = 0; index < pixels; ++index)
    {
        inverted[index] = 100.0F - std::clamp(L[index], 0.0F, 100.0F);
    }
    WorkingImage luma_image;
    luma_image.width = image.width;
    luma_image.height = image.height;
    luma_image.rgb.resize(pixels * 3U);
    for (std::size_t index = 0; index < pixels; ++index)
    {
        luma_image.rgb[index * 3U] = inverted[index] / 100.0F;
        luma_image.rgb[index * 3U + 1U] = inverted[index] / 100.0F;
        luma_image.rgb[index * 3U + 2U] = inverted[index] / 100.0F;
    }
    box_blur(luma_image, 8, 2);
    const float contrast_scale = static_cast<float>(amount) * 3.75F;
    for (std::size_t index = 0; index < pixels; ++index)
    {
        const float mixed = (luma_image.rgb[index * 3U] * 100.0F + L[index]) - 100.0F;
        const float hipass = std::clamp(mixed * contrast_scale + 50.0F, 0.0F, 100.0F);
        const float out_L = L[index] + (hipass - 50.0F);
        lab_to_rgb(out_L, a[index], b[index], image.rgb[index * 3U], image.rgb[index * 3U + 1U],
                   image.rgb[index * 3U + 2U]);
    }
}

void apply_soften(WorkingImage &image, const double amount)
{
    if (amount <= 0.0)
    {
        return;
    }
    WorkingImage orton = image;
    const float brightness = 1.0F / std::exp2(-0.33F);
    constexpr float saturation = 1.0F;
    for (std::size_t index = 0; index + 2 < orton.rgb.size(); index += 3)
    {
        float h = 0.0F;
        float s = 0.0F;
        float l = 0.0F;
        rgb_to_hsl(orton.rgb[index], orton.rgb[index + 1U], orton.rgb[index + 2U], h, s, l);
        s *= saturation;
        l *= brightness;
        hsl_to_rgb(h, std::clamp(s, 0.0F, 1.0F), std::clamp(l, 0.0F, 1.0F), orton.rgb[index],
                   orton.rgb[index + 1U], orton.rgb[index + 2U]);
    }
    const float hypot =
        std::hypot(static_cast<float>(image.width), static_cast<float>(image.height));
    // darktable's hidden defaults are size=50%, saturation=100%, brightness=0.33 EV.
    const int radius = std::max(1, static_cast<int>(std::ceil(hypot * 0.0051F)));
    box_blur(orton, radius, 8);
    const float mix = static_cast<float>(std::clamp(amount, 0.0, 1.0));
    for (std::size_t index = 0; index < image.rgb.size(); ++index)
    {
        image.rgb[index] = image.rgb[index] * (1.0F - mix) + orton.rgb[index] * mix;
    }
}

void apply_bloom(WorkingImage &image, const double amount)
{
    if (amount <= 0.0)
    {
        return;
    }
    const std::size_t pixels = static_cast<std::size_t>(image.width) * image.height;
    std::vector<float> L(pixels);
    std::vector<float> a(pixels);
    std::vector<float> b(pixels);
    for (std::size_t index = 0; index < pixels; ++index)
    {
        rgb_to_lab(image.rgb[index * 3U], image.rgb[index * 3U + 1U], image.rgb[index * 3U + 2U],
                   L[index], a[index], b[index]);
    }
    constexpr float threshold = 90.0F;
    const float strength = static_cast<float>(std::clamp(amount, 0.0, 1.0));
    const float scale = std::exp2(std::min(1.0F, strength + 0.01F));
    WorkingImage lights;
    lights.width = image.width;
    lights.height = image.height;
    lights.rgb.resize(pixels * 3U, 0.0F);
    bool has_highlight = false;
    for (std::size_t index = 0; index < pixels; ++index)
    {
        const float sample = L[index] * scale;
        const float keep = sample > threshold ? sample : 0.0F;
        has_highlight = has_highlight || keep > 0.0F;
        lights.rgb[index * 3U] = keep / 100.0F;
        lights.rgb[index * 3U + 1U] = keep / 100.0F;
        lights.rgb[index * 3U + 2U] = keep / 100.0F;
    }
    if (!has_highlight)
    {
        return;
    }
    const float max_edge = static_cast<float>(std::max(image.width, image.height));
    const int radius = std::max(1, static_cast<int>(std::ceil(max_edge * 0.0135F)));
    box_blur(lights, radius, 8);
    for (std::size_t index = 0; index < pixels; ++index)
    {
        const float glow = lights.rgb[index * 3U] * 100.0F;
        const float screened = 100.0F - ((100.0F - L[index]) * (100.0F - glow) / 100.0F);
        lab_to_rgb(screened, a[index], b[index], image.rgb[index * 3U], image.rgb[index * 3U + 1U],
                   image.rgb[index * 3U + 2U]);
    }
}

void apply_vignette(WorkingImage &image, const double amount, const double midpoint,
                    const double falloff)
{
    if (amount <= 0.0 || image.width == 0 || image.height == 0)
    {
        return;
    }
    const float brightness = -0.5F * static_cast<float>(amount);
    const float saturation = -static_cast<float>(amount) * 0.5F;
    const float dscale = static_cast<float>(std::clamp(midpoint, 0.0, 1.0));
    const float fscale = std::max(0.05F, static_cast<float>(falloff));
    const float shape = 1.0F;
    const float exp1 = 2.0F / shape;
    const float exp2 = shape / 2.0F;
    const float xscale = 2.0F / static_cast<float>(image.width);
    const float yscale = 2.0F / static_cast<float>(image.height);
    const float cx = 1.0F;
    const float cy = 1.0F;
    for (std::uint32_t y = 0; y < image.height; ++y)
    {
        for (std::uint32_t x = 0; x < image.width; ++x)
        {
            const float pv_x = std::abs(static_cast<float>(x) * xscale - cx);
            const float pv_y = std::abs(static_cast<float>(y) * yscale - cy);
            const float cplen = std::pow(std::pow(pv_x, exp1) + std::pow(pv_y, exp1), exp2);
            float weight = 0.0F;
            if (cplen >= dscale)
            {
                weight = std::clamp((cplen - dscale) / fscale, 0.0F, 1.0F);
            }
            if (weight <= 0.0F)
            {
                continue;
            }
            const std::size_t index = (static_cast<std::size_t>(y) * image.width + x) * 3U;
            float r = image.rgb[index];
            float g = image.rgb[index + 1U];
            float b = image.rgb[index + 2U];
            if (brightness < 0.0F)
            {
                const float fall = 1.0F + weight * brightness;
                r *= fall;
                g *= fall;
                b *= fall;
            }
            else
            {
                r += weight * brightness;
                g += weight * brightness;
                b += weight * brightness;
            }
            const float mean = (r + g + b) / 3.0F;
            const float wss = weight * saturation;
            r -= (mean - r) * wss;
            g -= (mean - g) * wss;
            b -= (mean - b) * wss;
            image.rgb[index] = r;
            image.rgb[index + 1U] = g;
            image.rgb[index + 2U] = b;
        }
    }
}

constexpr std::array<int, 256> kSimplexPermutation = {
    151, 160, 137, 91,  90,  15,  131, 13,  201, 95,  96,  53,  194, 233, 7,   225, 140, 36,  103,
    30,  69,  142, 8,   99,  37,  240, 21,  10,  23,  190, 6,   148, 247, 120, 234, 75,  0,   26,
    197, 62,  94,  252, 219, 203, 117, 35,  11,  32,  57,  177, 33,  88,  237, 149, 56,  87,  174,
    20,  125, 136, 171, 168, 68,  175, 74,  165, 71,  134, 139, 48,  27,  166, 77,  146, 158, 231,
    83,  111, 229, 122, 60,  211, 133, 230, 220, 105, 92,  41,  55,  46,  245, 40,  244, 102, 143,
    54,  65,  25,  63,  161, 1,   216, 80,  73,  209, 76,  132, 187, 208, 89,  18,  169, 200, 196,
    135, 130, 116, 188, 159, 86,  164, 100, 109, 198, 173, 186, 3,   64,  52,  217, 226, 250, 124,
    123, 5,   202, 38,  147, 118, 126, 255, 82,  85,  212, 207, 206, 59,  227, 47,  16,  58,  17,
    182, 189, 28,  42,  223, 183, 170, 213, 119, 248, 152, 2,   44,  154, 163, 70,  221, 153, 101,
    155, 167, 43,  172, 9,   129, 22,  39,  253, 19,  98,  108, 110, 79,  113, 224, 232, 178, 185,
    112, 104, 218, 246, 97,  228, 251, 34,  242, 193, 238, 210, 144, 12,  191, 179, 162, 241, 81,
    51,  145, 235, 249, 14,  239, 107, 49,  192, 214, 31,  181, 199, 106, 157, 184, 84,  204, 176,
    115, 121, 50,  45,  127, 4,   150, 254, 138, 236, 205, 93,  222, 114, 67,  29,  24,  72,  243,
    141, 128, 195, 78,  66,  215, 61,  156, 180};

constexpr std::array<std::array<double, 3>, 12> kSimplexGrad = {{{1, 1, 0},
                                                                 {-1, 1, 0},
                                                                 {1, -1, 0},
                                                                 {-1, -1, 0},
                                                                 {1, 0, 1},
                                                                 {-1, 0, 1},
                                                                 {1, 0, -1},
                                                                 {-1, 0, -1},
                                                                 {0, 1, 1},
                                                                 {0, -1, 1},
                                                                 {0, 1, -1},
                                                                 {0, -1, -1}}};

std::array<std::size_t, 512> g_simplex_perm{};
std::array<std::size_t, 512> g_simplex_perm_mod{};
std::once_flag g_simplex_once;

void init_simplex()
{
    std::call_once(g_simplex_once,
                   []
                   {
                       for (int index = 0; index < 512; ++index)
                       {
                           g_simplex_perm[static_cast<std::size_t>(index)] =
                               static_cast<std::size_t>(kSimplexPermutation[index & 255]);
                           g_simplex_perm_mod[static_cast<std::size_t>(index)] =
                               g_simplex_perm[static_cast<std::size_t>(index)] % 12U;
                       }
                   });
}

[[nodiscard]] double simplex_noise(double xin, double yin, double zin)
{
    init_simplex();
    constexpr double f3 = 1.0 / 3.0;
    constexpr double g3 = 1.0 / 6.0;
    const double s = (xin + yin + zin) * f3;
    const int i = static_cast<int>(std::floor(xin + s));
    const int j = static_cast<int>(std::floor(yin + s));
    const int k = static_cast<int>(std::floor(zin + s));
    const double t = (i + j + k) * g3;
    const double x0 = xin - (i - t);
    const double y0 = yin - (j - t);
    const double z0 = zin - (k - t);
    int i1 = 0;
    int j1 = 0;
    int k1 = 0;
    int i2 = 0;
    int j2 = 0;
    int k2 = 0;
    if (x0 >= y0)
    {
        if (y0 >= z0)
        {
            i1 = 1;
            i2 = 1;
            j2 = 1;
        }
        else if (x0 >= z0)
        {
            i1 = 1;
            i2 = 1;
            k2 = 1;
        }
        else
        {
            k1 = 1;
            i2 = 1;
            k2 = 1;
        }
    }
    else if (y0 < z0)
    {
        k1 = 1;
        j2 = 1;
        k2 = 1;
    }
    else if (x0 < z0)
    {
        j1 = 1;
        j2 = 1;
        k2 = 1;
    }
    else
    {
        j1 = 1;
        i2 = 1;
        j2 = 1;
    }
    const double x1 = x0 - i1 + g3;
    const double y1 = y0 - j1 + g3;
    const double z1 = z0 - k1 + g3;
    const double x2 = x0 - i2 + 2.0 * g3;
    const double y2 = y0 - j2 + 2.0 * g3;
    const double z2 = z0 - k2 + 2.0 * g3;
    const double x3 = x0 - 1.0 + 3.0 * g3;
    const double y3 = y0 - 1.0 + 3.0 * g3;
    const double z3 = z0 - 1.0 + 3.0 * g3;
    const std::size_t ii = static_cast<std::size_t>(i) & 255U;
    const std::size_t jj = static_cast<std::size_t>(j) & 255U;
    const std::size_t kk = static_cast<std::size_t>(k) & 255U;
    const auto contrib =
        [](const std::array<double, 3> &grad, const double x, const double y, const double z)
    {
        double t0 = 0.6 - x * x - y * y - z * z;
        if (t0 < 0.0)
        {
            return 0.0;
        }
        t0 *= t0;
        return t0 * t0 * (grad[0] * x + grad[1] * y + grad[2] * z);
    };
    const auto gi0 = g_simplex_perm_mod[ii + g_simplex_perm[jj + g_simplex_perm[kk]]];
    const auto gi1 =
        g_simplex_perm_mod[ii + static_cast<std::size_t>(i1) +
                           g_simplex_perm[jj + static_cast<std::size_t>(j1) +
                                          g_simplex_perm[kk + static_cast<std::size_t>(k1)]]];
    const auto gi2 =
        g_simplex_perm_mod[ii + static_cast<std::size_t>(i2) +
                           g_simplex_perm[jj + static_cast<std::size_t>(j2) +
                                          g_simplex_perm[kk + static_cast<std::size_t>(k2)]]];
    const auto gi3 =
        g_simplex_perm_mod[ii + 1U + g_simplex_perm[jj + 1U + g_simplex_perm[kk + 1U]]];
    return 32.0 * (contrib(kSimplexGrad[gi0], x0, y0, z0) + contrib(kSimplexGrad[gi1], x1, y1, z1) +
                   contrib(kSimplexGrad[gi2], x2, y2, z2) + contrib(kSimplexGrad[gi3], x3, y3, z3));
}

[[nodiscard]] double simplex_2d_noise(const double x, const double y, const double z)
{
    static constexpr double f[] = {0.4910, 0.9441, 1.7280};
    static constexpr double a[] = {0.2340, 0.7850, 1.2150};
    double total = 0.0;
    for (int octave = 0; octave < 3; ++octave)
    {
        total += simplex_noise(x * f[octave] / z, y * f[octave] / z, octave) * a[octave];
    }
    return total;
}

void apply_grain(WorkingImage &image, const double amount)
{
    if (amount <= 0.0)
    {
        return;
    }
    const float strength = static_cast<float>(amount);
    const double wd = std::min(image.width, image.height);
    constexpr double kDefaultScale = 1600.0 / 213.2;
    const double zoom = (1.0 + 8.0 * kDefaultScale) / 800.0;
    constexpr float kLightnessScale = 0.15F;
    for (std::uint32_t y = 0; y < image.height; ++y)
    {
        for (std::uint32_t x = 0; x < image.width; ++x)
        {
            const std::size_t index = (static_cast<std::size_t>(y) * image.width + x) * 3U;
            float L = 0.0F;
            float a = 0.0F;
            float b = 0.0F;
            rgb_to_lab(image.rgb[index], image.rgb[index + 1U], image.rgb[index + 2U], L, a, b);
            const double nx = static_cast<double>(x) / wd;
            const double ny = static_cast<double>(y) / wd;
            const float noise = static_cast<float>(simplex_2d_noise(nx, ny, zoom));
            const float mid = 1.0F - std::abs(L / 100.0F - 0.5F) * 2.0F;
            L += noise * strength * kLightnessScale * 100.0F * (0.25F + 0.75F * mid);
            lab_to_rgb(L, a, b, image.rgb[index], image.rgb[index + 1U], image.rgb[index + 2U]);
        }
    }
}

void apply_gamma(WorkingImage &image, const double gamma)
{
    if (std::abs(gamma - 1.0) <= 1.0e-6)
    {
        return;
    }
    const float exponent = static_cast<float>(1.0 / std::max(0.2, gamma));
    for (float &sample : image.rgb)
    {
        sample = sample <= 0.0F ? sample : std::pow(sample, exponent);
    }
}

void apply_color_contrast(WorkingImage &image, const double amount)
{
    if (amount == 0.0)
    {
        return;
    }
    const float slope = 1.0F + static_cast<float>(amount);
    for (std::size_t index = 0; index + 2 < image.rgb.size(); index += 3)
    {
        float L = 0.0F;
        float a = 0.0F;
        float b = 0.0F;
        rgb_to_lab(image.rgb[index], image.rgb[index + 1U], image.rgb[index + 2U], L, a, b);
        a *= slope;
        b *= slope;
        lab_to_rgb(L, a, b, image.rgb[index], image.rgb[index + 1U], image.rgb[index + 2U]);
    }
}

void apply_velvia(WorkingImage &image, const double amount, const double bias)
{
    if (amount <= 0.0)
    {
        return;
    }
    const float strength = static_cast<float>(amount);
    const float velvia_bias = static_cast<float>(std::clamp(bias, 0.0, 1.0));
    for (std::size_t index = 0; index + 2 < image.rgb.size(); index += 3)
    {
        float &r = image.rgb[index];
        float &g = image.rgb[index + 1U];
        float &b = image.rgb[index + 2U];
        const float pmax = std::max(r, std::max(g, b));
        const float pmin = std::min(r, std::min(g, b));
        const float plum = (pmax + pmin) * 0.5F;
        const float psat = plum <= 0.5F ?
                               (pmax - pmin) / (1.0e-5F + pmax + pmin) :
                               (pmax - pmin) / (1.0e-5F + std::max(0.0F, 2.0F - pmax - pmin));
        const float pweight =
            std::clamp(((1.0F - (1.5F * psat)) +
                        ((1.0F + (std::abs(plum - 0.5F) * 2.0F)) * (1.0F - velvia_bias))) /
                           (1.0F + (1.0F - velvia_bias)),
                       0.0F, 1.0F);
        const float saturation = strength * pweight;
        const float others_r = g + b;
        const float others_g = r + b;
        const float others_b = r + g;
        r = std::clamp(r + saturation * (r - 0.5F * others_r), 0.0F, 1.0F);
        g = std::clamp(g + saturation * (g - 0.5F * others_g), 0.0F, 1.0F);
        b = std::clamp(b + saturation * (b - 0.5F * others_b), 0.0F, 1.0F);
    }
}

void apply_monochrome(WorkingImage &image, const double amount)
{
    if (amount <= 0.0)
    {
        return;
    }
    const float mix = static_cast<float>(std::clamp(amount, 0.0, 1.0));
    for (std::size_t index = 0; index + 2 < image.rgb.size(); index += 3)
    {
        float L = 0.0F;
        float a = 0.0F;
        float b = 0.0F;
        rgb_to_lab(image.rgb[index], image.rgb[index + 1U], image.rgb[index + 2U], L, a, b);
        a *= 1.0F - mix;
        b *= 1.0F - mix;
        lab_to_rgb(L, a, b, image.rgb[index], image.rgb[index + 1U], image.rgb[index + 2U]);
    }
}

void rgb_to_hsl(const float r, const float g, const float b, float &h, float &s, float &l)
{
    const float maxc = std::max(r, std::max(g, b));
    const float minc = std::min(r, std::min(g, b));
    l = (maxc + minc) * 0.5F;
    if (maxc <= minc + 1.0e-6F)
    {
        h = 0.0F;
        s = 0.0F;
        return;
    }
    const float delta = maxc - minc;
    s = l > 0.5F ? delta / (2.0F - maxc - minc) : delta / (maxc + minc);
    if (maxc == r)
    {
        h = (g - b) / delta + (g < b ? 6.0F : 0.0F);
    }
    else if (maxc == g)
    {
        h = (b - r) / delta + 2.0F;
    }
    else
    {
        h = (r - g) / delta + 4.0F;
    }
    h /= 6.0F;
}

[[nodiscard]] float hue_to_rgb(const float p, const float q, float t)
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
}

void hsl_to_rgb(const float h, const float s, const float l, float &r, float &g, float &b)
{
    if (s <= 1.0e-6F)
    {
        r = g = b = l;
        return;
    }
    const float q = l < 0.5F ? l * (1.0F + s) : l + s - l * s;
    const float p = 2.0F * l - q;
    r = hue_to_rgb(p, q, h + 1.0F / 3.0F);
    g = hue_to_rgb(p, q, h);
    b = hue_to_rgb(p, q, h - 1.0F / 3.0F);
}

void apply_split_toning(WorkingImage &image, const double shadows_hue, const double highlights_hue,
                        const double balance, const double amount)
{
    if (amount <= 0.0)
    {
        return;
    }
    const float mix_amount = static_cast<float>(std::clamp(amount, 0.0, 1.0));
    const float shadow_h = static_cast<float>(shadows_hue);
    const float highlight_h = static_cast<float>(highlights_hue);
    const float center = static_cast<float>(balance);
    const float compress = 0.15F;
    for (std::size_t index = 0; index + 2 < image.rgb.size(); index += 3)
    {
        float r = image.rgb[index];
        float g = image.rgb[index + 1U];
        float b = image.rgb[index + 2U];
        float h = 0.0F;
        float s = 0.0F;
        float l = 0.0F;
        rgb_to_hsl(r, g, b, h, s, l);
        float mix_r = r;
        float mix_g = g;
        float mix_b = b;
        float weight = 0.0F;
        if (l < center - compress)
        {
            hsl_to_rgb(shadow_h, 0.5F, l, mix_r, mix_g, mix_b);
            weight = std::clamp((center - compress - l) * 2.0F, 0.0F, 1.0F) * mix_amount;
        }
        else if (l > center + compress)
        {
            hsl_to_rgb(highlight_h, 0.5F, l, mix_r, mix_g, mix_b);
            weight = std::clamp((l - (center + compress)) * 2.0F, 0.0F, 1.0F) * mix_amount;
        }
        image.rgb[index] = std::clamp(r * (1.0F - weight) + mix_r * weight, 0.0F, 1.0F);
        image.rgb[index + 1U] = std::clamp(g * (1.0F - weight) + mix_g * weight, 0.0F, 1.0F);
        image.rgb[index + 2U] = std::clamp(b * (1.0F - weight) + mix_b * weight, 0.0F, 1.0F);
    }
}

void apply_dehaze(WorkingImage &image, const double amount)
{
    if (amount == 0.0)
    {
        return;
    }
    const float strength = static_cast<float>(amount);
    constexpr float kAirlight = 0.92F;
    for (std::size_t index = 0; index + 2 < image.rgb.size(); index += 3)
    {
        float &r = image.rgb[index];
        float &g = image.rgb[index + 1U];
        float &b = image.rgb[index + 2U];
        const float dark = std::min(r, std::min(g, b));
        if (strength > 0.0F)
        {
            const float transmission = std::max(0.1F, 1.0F - strength * dark / kAirlight);
            r = (r - kAirlight) / transmission + kAirlight;
            g = (g - kAirlight) / transmission + kAirlight;
            b = (b - kAirlight) / transmission + kAirlight;
        }
        else
        {
            const float haze = -strength;
            const float transmission = std::max(0.1F, 1.0F - haze * dark / kAirlight);
            r = r * transmission + kAirlight * (1.0F - transmission);
            g = g * transmission + kAirlight * (1.0F - transmission);
            b = b * transmission + kAirlight * (1.0F - transmission);
        }
    }
}

} // namespace

Result<WorkingImage> apply_exposure(const WorkingImage &input, const ExposureParams &params,
                                    const CancellationToken &cancellation)
try
{
    return apply_exposure_impl(input, params, cancellation);
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Exposure output allocation failed",
                      {{"reason", "allocation_failed"}});
}

Result<WorkingImage> apply_exposure(const WorkingImage &input, const OperationInstance &operation,
                                    const CancellationToken &cancellation)
try
{
    auto active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    if (operation.id != kExposureOperationId)
    {
        return make_error(ErrorCode::kValidation, "Operation is not exposure",
                          {{"operation_id", operation.id}});
    }
    if (operation.mask_id.has_value())
    {
        return make_error(
            ErrorCode::kUnsupported, "Exposure mask evaluation is unavailable",
            {{"operation_id", operation.id}, {"reason", "exposure_mask_graph_unavailable"}});
    }
    if (!operation.enabled)
    {
        return input;
    }
    OperationInstance canonical = operation;
    auto upgraded = upgrade_exposure_operation(canonical);
    if (!upgraded)
    {
        return upgraded.error();
    }
    auto params = exposure_from_parameters(canonical.parameters);
    if (!params)
    {
        return params.error();
    }
    return apply_exposure_impl(input, params.value(), cancellation);
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Exposure operation allocation failed",
                      {{"reason", "allocation_failed"}});
}

Result<WorkingImage> apply_color_balance(const WorkingImage &input,
                                         const ColorBalanceParams &params,
                                         const CancellationToken &cancellation)
try
{
    auto active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    auto canonical = color_balance_from_parameters(color_balance_to_parameters(params));
    if (!canonical)
    {
        return canonical.error();
    }
    if (input.width == 0U || input.height == 0U)
    {
        return make_error(ErrorCode::kValidation, "Color Balance input dimensions must be non-zero",
                          {{"reason", "invalid_colorbalance_dimensions"}});
    }
    const std::uint64_t pixels = static_cast<std::uint64_t>(input.width) * input.height;
    if (pixels > std::numeric_limits<std::size_t>::max() / 3U ||
        input.rgb.size() != static_cast<std::size_t>(pixels * 3U))
    {
        return make_error(ErrorCode::kValidation,
                          "Color Balance input buffer does not match its dimensions",
                          {{"reason", "invalid_colorbalance_buffer"}});
    }
    if (input.color_profile.model != ColorModel::kRgb ||
        input.color_profile.identifier != kInputProfileLinearRec709)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Color Balance requires linear sRGB D50 working pixels",
                          {{"profile", input.color_profile.identifier},
                           {"reason", "unsupported_colorbalance_working_space"}});
    }
    for (std::uint32_t row = 0U; row < input.height; ++row)
    {
        active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
        const std::size_t begin = static_cast<std::size_t>(row) * input.width * 3U;
        const std::size_t end = begin + static_cast<std::size_t>(input.width) * 3U;
        for (std::size_t index = begin; index < end; ++index)
        {
            if (!std::isfinite(input.rgb[index]))
            {
                return make_error(ErrorCode::kValidation,
                                  "Color Balance input contains a non-finite sample",
                                  {{"sample_index", std::to_string(index)},
                                   {"reason", "nonfinite_colorbalance_input"}});
            }
        }
    }

    constexpr std::array<float, 3> kProPhotoLuma{0.2880402F, 0.7118741F, 0.0000857F};
    const auto corrected = [&](const std::array<double, kColorBalanceChannelCount> &values)
    {
        const float luma = std::fma(kProPhotoLuma[0], static_cast<float>(values[1]),
                                    std::fma(kProPhotoLuma[1], static_cast<float>(values[2]),
                                             kProPhotoLuma[2] * static_cast<float>(values[3])));
        return std::array<float, 4>{static_cast<float>(values[0]),
                                    static_cast<float>(values[1]) - luma + 1.0F,
                                    static_cast<float>(values[2]) - luma + 1.0F,
                                    static_cast<float>(values[3]) - luma + 1.0F};
    };
    const auto lift = corrected(canonical.value().lift);
    const auto gamma = corrected(canonical.value().gamma);
    const auto gain = corrected(canonical.value().gain);
    std::array<float, 3> effective_lift{};
    std::array<float, 3> effective_gain{};
    std::array<float, 3> effective_power{};
    const bool lgg = canonical.value().mode == kColorBalanceModeLiftGammaGain;
    for (std::size_t channel = 0U; channel < 3U; ++channel)
    {
        effective_gain[channel] = gain[channel + 1U] * gain[0];
        if (lgg)
        {
            effective_lift[channel] = 2.0F - lift[channel + 1U] * lift[0];
            const float denominator = gamma[channel + 1U] * gamma[0];
            effective_power[channel] =
                2.2F * (denominator != 0.0F ? 1.0F / denominator : 1000000.0F);
        }
        else
        {
            effective_lift[channel] = lift[channel + 1U] + lift[0] - 2.0F;
            effective_power[channel] = (2.0F - gamma[channel + 1U]) * (2.0F - gamma[0]);
        }
        if (!std::isfinite(effective_lift[channel]) || !std::isfinite(effective_gain[channel]) ||
            !std::isfinite(effective_power[channel]))
        {
            return make_error(
                ErrorCode::kValidation, "Color Balance derived curve is not finite",
                {{"channel", std::to_string(channel)}, {"reason", "invalid_colorbalance_curve"}});
        }
    }
    const float input_saturation = static_cast<float>(canonical.value().input_saturation);
    const float output_saturation = static_cast<float>(canonical.value().output_saturation);
    const float contrast = static_cast<float>(canonical.value().contrast);
    const float contrast_power = 1.0F / contrast;
    const float grey = static_cast<float>(canonical.value().grey_fulcrum_percent / 100.0);
    if (!std::isfinite(contrast_power) || !std::isfinite(grey) || grey <= 0.0F)
    {
        return make_error(ErrorCode::kValidation, "Color Balance contrast denominator is invalid",
                          {{"reason", "invalid_colorbalance_denominator"}});
    }

    WorkingImage output;
    output.width = input.width;
    output.height = input.height;
    output.color_profile = input.color_profile;
    output.exposure_analysis = input.exposure_analysis;
    output.rgb.resize(input.rgb.size());
    const bool run_input_saturation = std::abs(input_saturation - 1.0F) > 1.0e-6F;
    const bool run_output_saturation = std::abs(output_saturation - 1.0F) > 1.0e-6F;
    const bool run_contrast = std::abs((lgg ? contrast_power : contrast) - 1.0F) > 1.0e-6F;

    for (std::uint32_t row = 0U; row < input.height; ++row)
    {
        active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
        for (std::uint32_t column = 0U; column < input.width; ++column)
        {
            const std::size_t index = (static_cast<std::size_t>(row) * input.width + column) * 3U;
            float xyz[3]{};
            linear_rgb_to_xyz_d50(input.rgb[index], input.rgb[index + 1U], input.rgb[index + 2U],
                                  xyz);
            float lab[3]{};
            xyz_d50_to_lab(xyz, lab);
            // Preserve the frozen module boundary: it receives Lab D50, then converts
            // Lab -> XYZ -> ProPhoto even though Ravo stores the surrounding pixels as RGB.
            lab_to_xyz_d50(lab, xyz);
            float rgb[3]{};
            xyz_to_prophoto(xyz, rgb);
            if (run_input_saturation)
            {
                for (std::size_t channel = 0U; channel < 3U; ++channel)
                {
                    rgb[channel] = xyz[1] + input_saturation * (rgb[channel] - xyz[1]);
                }
            }
            for (std::size_t channel = 0U; channel < 3U; ++channel)
            {
                if (lgg)
                {
                    float value = std::pow(std::max(rgb[channel], 0.0F), 1.0F / 2.2F);
                    value =
                        ((value - 1.0F) * effective_lift[channel] + 1.0F) * effective_gain[channel];
                    rgb[channel] = std::pow(std::max(value, 0.0F), effective_power[channel]);
                }
                else
                {
                    const float value = std::max(
                        effective_gain[channel] * rgb[channel] + effective_lift[channel], 0.0F);
                    rgb[channel] = std::pow(value, effective_power[channel]);
                }
                if (!std::isfinite(rgb[channel]))
                {
                    return make_error(ErrorCode::kValidation,
                                      "Color Balance curve produced a non-finite sample",
                                      {{"sample_index", std::to_string(index + channel)},
                                       {"reason", "nonfinite_colorbalance_curve"}});
                }
            }
            if (run_output_saturation)
            {
                float balanced_xyz[3]{};
                prophoto_to_xyz(rgb, balanced_xyz);
                for (std::size_t channel = 0U; channel < 3U; ++channel)
                {
                    rgb[channel] =
                        balanced_xyz[1] + output_saturation * (rgb[channel] - balanced_xyz[1]);
                }
            }
            if (run_contrast)
            {
                for (float &sample : rgb)
                {
                    sample = std::pow(std::max(sample, 0.0F) / grey, contrast_power) * grey;
                }
            }
            prophoto_to_lab(rgb, lab);
            lab_to_xyz_d50(lab, xyz);
            xyz_d50_to_linear_rgb(xyz, output.rgb[index], output.rgb[index + 1U],
                                  output.rgb[index + 2U]);
            for (std::size_t channel = 0U; channel < 3U; ++channel)
            {
                if (!std::isfinite(output.rgb[index + channel]))
                {
                    return make_error(ErrorCode::kValidation,
                                      "Color Balance produced a non-finite output sample",
                                      {{"sample_index", std::to_string(index + channel)},
                                       {"reason", "nonfinite_colorbalance_output"}});
                }
            }
        }
    }
    active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    return output;
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Color Balance output allocation failed",
                      {{"reason", "allocation_failed"}});
}

Result<WorkingImage> apply_color_balance(const WorkingImage &input,
                                         const OperationInstance &operation,
                                         const CancellationToken &cancellation)
try
{
    auto active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    if (operation.id != kColorBalanceOperationId)
    {
        return make_error(ErrorCode::kValidation, "Operation is not Color Balance",
                          {{"operation_id", operation.id}});
    }
    if (operation.schema_version != kColorBalanceOperationSchemaVersion)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Color Balance operation schema version is unsupported",
                          {{"operation_id", operation.id},
                           {"schema_version", std::to_string(operation.schema_version)}});
    }
    if (operation.mask_id.has_value())
    {
        return make_error(
            ErrorCode::kUnsupported, "Color Balance mask evaluation is unavailable",
            {{"operation_id", operation.id}, {"reason", "colorbalance_mask_graph_unavailable"}});
    }
    if (!operation.enabled)
    {
        return input;
    }
    auto params = color_balance_from_parameters(operation.parameters);
    if (!params)
    {
        return params.error();
    }
    return apply_color_balance(input, params.value(), cancellation);
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Color Balance operation allocation failed",
                      {{"reason", "allocation_failed"}});
}

Result<WorkingImage> working_from_raw(const DecodedRaw &raw, const std::uint32_t width,
                                      const std::uint32_t height,
                                      const std::array<float, 4> &white_balance,
                                      const CancellationToken &cancellation)
{
    if (width == 0 || height == 0)
    {
        return make_error(ErrorCode::kInvalidArgument, "Render output dimensions must be non-zero");
    }
    for (std::size_t channel = 0; channel < white_balance.size(); ++channel)
    {
        if (!std::isfinite(white_balance[channel]) || white_balance[channel] <= 0.0F ||
            white_balance[channel] > 8.0F)
        {
            return make_error(ErrorCode::kValidation,
                              "RAW temperature coefficient is outside (0, 8]",
                              {{"channel", std::to_string(channel)}});
        }
    }
    if (raw.cfa_width == 0 || raw.cfa_height == 0 ||
        raw.cfa_channels.size() != static_cast<std::size_t>(raw.cfa_width) * raw.cfa_height ||
        std::any_of(raw.cfa_channels.begin(), raw.cfa_channels.end(),
                    [](const std::uint8_t channel) { return channel >= 4U; }))
    {
        return make_error(ErrorCode::kUnsupported,
                          "RAW temperature requires a one-to-four-channel CFA pattern");
    }
    const int turns = normalized_rotate_quarters(raw.rotate_quarters);
    std::uint32_t demosaic_width = width;
    std::uint32_t demosaic_height = height;
    apply_display_rotation_to_size(demosaic_width, demosaic_height, turns);
    WorkingImage image;
    image.width = demosaic_width;
    image.height = demosaic_height;
    image.rgb.resize(static_cast<std::size_t>(demosaic_width) * demosaic_height * 3U);
    image.color_profile = raw.color_profile;
    const float denominator = static_cast<float>(
        std::max<std::int64_t>(1, static_cast<std::int64_t>(raw.white_level) - raw.black_level));

    auto rows = for_each_row(
        demosaic_height, cancellation,
        [&](const std::uint32_t output_y)
        {
            const std::uint32_t source_y = std::min(
                raw.height - 1, static_cast<std::uint32_t>(static_cast<std::uint64_t>(output_y) *
                                                           raw.height / demosaic_height));
            for (std::uint32_t output_x = 0; output_x < demosaic_width; ++output_x)
            {
                const std::uint32_t source_x = std::min(
                    raw.width - 1, static_cast<std::uint32_t>(static_cast<std::uint64_t>(output_x) *
                                                              raw.width / demosaic_width));
                std::array<float, 3> sum{};
                std::array<std::uint32_t, 3> count{};
                for (int offset_y = -1; offset_y <= 1; ++offset_y)
                {
                    const std::uint32_t y =
                        static_cast<std::uint32_t>(std::clamp(static_cast<int>(source_y) + offset_y,
                                                              0, static_cast<int>(raw.height) - 1));
                    for (int offset_x = -1; offset_x <= 1; ++offset_x)
                    {
                        const std::uint32_t x = static_cast<std::uint32_t>(
                            std::clamp(static_cast<int>(source_x) + offset_x, 0,
                                       static_cast<int>(raw.width) - 1));
                        const std::uint8_t cfa_channel =
                            raw.cfa_channels[(y % raw.cfa_height) * raw.cfa_width +
                                             (x % raw.cfa_width)];
                        if (cfa_channel >= white_balance.size())
                        {
                            continue;
                        }
                        const std::size_t channel = cfa_channel == 3U ? 1U : cfa_channel;
                        const float sample = std::max(
                            0.0F, (static_cast<float>(
                                       raw.pixels[static_cast<std::size_t>(y) * raw.width + x]) -
                                   static_cast<float>(raw.black_level)) /
                                      denominator);
                        sum[channel] += sample * white_balance[cfa_channel];
                        ++count[channel];
                    }
                }

                std::array<float, 3> camera_rgb{};
                for (std::size_t channel = 0; channel < camera_rgb.size(); ++channel)
                {
                    const float sample = count[channel] == 0 ?
                                             0.0F :
                                             sum[channel] / static_cast<float>(count[channel]);
                    camera_rgb[channel] = sample;
                }
                const std::size_t output_index =
                    (static_cast<std::size_t>(output_y) * demosaic_width + output_x) * 3U;
                std::copy(camera_rgb.begin(), camera_rgb.end(),
                          image.rgb.begin() + static_cast<std::ptrdiff_t>(output_index));
            }
        });
    if (!rows)
    {
        return rows.error();
    }
    if (turns == 0)
    {
        return image;
    }
    return rotate_working(std::move(image), turns);
}

Result<WorkingImage> working_from_encoded_rgb8(const RasterBuffer &raster)
{
    if (raster.width == 0 || raster.height == 0 ||
        raster.srgb.size() != static_cast<std::size_t>(raster.width) * raster.height * 3U)
    {
        return make_error(ErrorCode::kValidation, "Raster buffer is empty or undersized");
    }
    WorkingImage image;
    image.width = raster.width;
    image.height = raster.height;
    image.rgb.resize(static_cast<std::size_t>(raster.width) * raster.height * 3U);
    image.color_profile = raster.color_profile;
    for (std::size_t index = 0; index < image.rgb.size(); ++index)
    {
        image.rgb[index] = static_cast<float>(raster.srgb[index]) / 255.0F;
    }
    return image;
}

Result<WorkingImage> apply_recipe_ops(WorkingImage image, const Recipe &recipe,
                                      const CancellationToken &cancellation)
{
    for (auto iterator = recipe.operations.cbegin(); iterator != recipe.operations.cend();
         ++iterator)
    {
        const auto &operation = *iterator;
        auto cancelled = cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
        if (!operation.enabled || absorbed_operation(operation.id))
        {
            continue;
        }
        if (operation.id == kPrimariesOperationId)
        {
            auto transformed = apply_primaries(image, operation, cancellation);
            if (!transformed)
            {
                return transformed.error();
            }
            image = std::move(transformed).value();
            continue;
        }
        if (operation.id == "ravo.color.temperature")
        {
            auto balanced = apply_temperature_rgb(image, operation, cancellation);
            if (!balanced)
            {
                return balanced.error();
            }
            continue;
        }
        if (operation.id == "ravo.color.channelmixerrgb")
        {
            auto mixed = apply_channel_mixer_rgb(image, operation, cancellation);
            if (!mixed)
            {
                return mixed.error();
            }
            continue;
        }
        if (operation.id == kExposureOperationId)
        {
            auto exposed = apply_exposure(image, operation, cancellation);
            if (!exposed)
            {
                return exposed.error();
            }
            image = std::move(exposed).value();
            continue;
        }
        if (operation.id == "ravo.core.contrast")
        {
            apply_contrast(image, parameter(operation, "amount", 0.0));
            continue;
        }
        if (operation.id == "ravo.core.highlights")
        {
            apply_highlights_shadows(image, parameter(operation, "amount", 0.0), 0.0);
            continue;
        }
        if (operation.id == "ravo.core.shadows")
        {
            apply_highlights_shadows(image, 0.0, parameter(operation, "amount", 0.0));
            continue;
        }
        if (operation.id == "ravo.core.whites")
        {
            apply_whites_blacks(image, parameter(operation, "amount", 0.0), 0.0);
            continue;
        }
        if (operation.id == "ravo.core.blacks")
        {
            apply_whites_blacks(image, 0.0, parameter(operation, "amount", 0.0));
            continue;
        }
        if (operation.id == "ravo.color.vibrance")
        {
            double saturation = 0.0;
            const auto next = std::next(iterator);
            if (next != recipe.operations.cend() && next->enabled &&
                next->id == "ravo.color.saturation")
            {
                saturation = parameter(*next, "amount", 0.0);
                iterator = next;
            }
            apply_vibrance_saturation(image, parameter(operation, "amount", 0.0), saturation);
            continue;
        }
        if (operation.id == "ravo.color.saturation")
        {
            double vibrance = 0.0;
            const auto next = std::next(iterator);
            if (next != recipe.operations.cend() && next->enabled &&
                next->id == "ravo.color.vibrance")
            {
                vibrance = parameter(*next, "amount", 0.0);
                iterator = next;
            }
            apply_vibrance_saturation(image, vibrance, parameter(operation, "amount", 0.0));
            continue;
        }
        if (operation.id == "ravo.geometry.rotate")
        {
            auto rotated = rotate_working(std::move(image),
                                          static_cast<int>(parameter(operation, "quarters", 0.0)));
            if (!rotated)
            {
                return rotated.error();
            }
            image = std::move(rotated).value();
            continue;
        }
        if (operation.id == "ravo.geometry.crop")
        {
            auto cropped = crop_working(
                std::move(image), parameter(operation, "x", 0.0), parameter(operation, "y", 0.0),
                parameter(operation, "width", 1.0), parameter(operation, "height", 1.0));
            if (!cropped)
            {
                return cropped.error();
            }
            image = std::move(cropped).value();
            continue;
        }
        if (operation.id == "ravo.geometry.flip")
        {
            auto flipped =
                flip_working(std::move(image), parameter(operation, "horizontal", 0.0) != 0.0,
                             parameter(operation, "vertical", 0.0) != 0.0);
            if (!flipped)
            {
                return flipped.error();
            }
            image = std::move(flipped).value();
            continue;
        }
        if (operation.id == "ravo.geometry.straighten")
        {
            auto straightened =
                straighten_working(std::move(image), parameter(operation, "degrees", 0.0));
            if (!straightened)
            {
                return straightened.error();
            }
            image = std::move(straightened).value();
            continue;
        }
        if (operation.id == "ravo.core.gamma")
        {
            apply_gamma(image, parameter(operation, "gamma", 1.0));
            continue;
        }
        if (operation.id == "ravo.core.tonecurve")
        {
            auto curved = apply_tone_curve(image, operation);
            if (!curved)
            {
                return curved.error();
            }
            continue;
        }
        if (operation.id == kColorBalanceOperationId)
        {
            auto balanced = apply_color_balance(image, operation, cancellation);
            if (!balanced)
            {
                return balanced.error();
            }
            image = std::move(balanced).value();
            continue;
        }
        if (operation.id == kColorCheckerOperationId)
        {
            auto corrected = apply_color_checker(image, operation, cancellation);
            if (!corrected)
            {
                return corrected.error();
            }
            image = std::move(corrected).value();
            continue;
        }
        if (operation.id == "ravo.color.colorbalancergb")
        {
            auto balanced = apply_color_balance_rgb(image, operation, cancellation);
            if (!balanced)
            {
                return balanced.error();
            }
            continue;
        }
        if (operation.id == "ravo.color.colorcontrast")
        {
            apply_color_contrast(image, parameter(operation, "amount", 0.0));
            continue;
        }
        if (operation.id == "ravo.color.velvia")
        {
            apply_velvia(image, parameter(operation, "amount", 0.0),
                         parameter(operation, "bias", 1.0));
            continue;
        }
        if (operation.id == "ravo.color.monochrome")
        {
            apply_monochrome(image, parameter(operation, "amount", 0.0));
            continue;
        }
        if (operation.id == "ravo.color.splittoning")
        {
            apply_split_toning(image, parameter(operation, "shadows_hue", 0.0),
                               parameter(operation, "highlights_hue", 0.2),
                               parameter(operation, "balance", 0.5),
                               parameter(operation, "amount", 0.0));
            continue;
        }
        if (operation.id == "ravo.detail.sharpen")
        {
            apply_unsharp(image, parameter(operation, "amount", 0.0),
                          parameter(operation, "radius", 2.0),
                          parameter(operation, "threshold", 0.5));
            continue;
        }
        if (operation.id == "ravo.detail.clarity")
        {
            apply_clarity(image, parameter(operation, "amount", 0.0));
            continue;
        }
        if (operation.id == "ravo.effect.vignette")
        {
            apply_vignette(image, parameter(operation, "amount", 0.0),
                           parameter(operation, "midpoint", 0.8),
                           parameter(operation, "falloff", 0.5));
            continue;
        }
        if (operation.id == "ravo.effect.grain")
        {
            apply_grain(image, parameter(operation, "amount", 0.0));
            continue;
        }
        if (operation.id == "ravo.effect.bloom")
        {
            apply_bloom(image, parameter(operation, "amount", 0.0));
            continue;
        }
        if (operation.id == "ravo.effect.soften")
        {
            apply_soften(image, parameter(operation, "amount", 0.0));
            continue;
        }
        if (operation.id == "ravo.effect.dehaze")
        {
            apply_dehaze(image, parameter(operation, "amount", 0.0));
            continue;
        }
        if (operation.id == "ravo.display.sigmoid")
        {
            auto transformed = apply_sigmoid(image, operation, cancellation);
            if (!transformed)
            {
                return transformed.error();
            }
            continue;
        }
        if (operation.id == "ravo.raw.hotpixels")
        {
            return make_error(ErrorCode::kUnsupported,
                              "Hot pixel correction requires a Bayer CFA working buffer",
                              {{"operation_id", operation.id}});
        }
        if (operation.id == "ravo.raw.cacorrect")
        {
            return make_error(
                ErrorCode::kUnsupported,
                "RAW chromatic aberration correction requires a Bayer CFA working buffer",
                {{"operation_id", operation.id}});
        }
        if (operation.id == "ravo.raw.highlights")
        {
            return make_error(ErrorCode::kUnsupported,
                              "RAW highlight reconstruction requires a Bayer CFA working buffer",
                              {{"operation_id", operation.id}});
        }
        if (operation.id == "ravo.detail.denoiseprofile")
        {
            auto denoised = apply_denoise_profile(image, operation, cancellation);
            if (!denoised)
            {
                return denoised.error();
            }
            continue;
        }
        if (operation.id == "ravo.geometry.lens")
        {
            auto corrected = apply_lens_correction(image, operation, cancellation);
            if (!corrected)
            {
                return corrected.error();
            }
            continue;
        }
        if (operation.id == "ravo.color.colorequal")
        {
            auto equalized = apply_color_equalizer(image, operation, cancellation);
            if (!equalized)
            {
                return equalized.error();
            }
            continue;
        }
        if (operation.id == "ravo.effect.graduatednd")
        {
            auto graduated = apply_graduated_nd(image, operation, cancellation);
            if (!graduated)
            {
                return graduated.error();
            }
            continue;
        }
        if (operation.id == "ravo.core.toneequal")
        {
            auto equalized = apply_tone_equalizer(image, operation, cancellation);
            if (!equalized)
            {
                return equalized.error();
            }
            continue;
        }
        return make_error(ErrorCode::kUnsupported, "Operation has no CPU implementation",
                          {{"operation_id", operation.id}});
    }
    return image;
}

Result<std::vector<std::uint8_t>> encode_png_bytes(const RenderedImage &image)
{
    const std::uint64_t pixels =
        static_cast<std::uint64_t>(image.width) * static_cast<std::uint64_t>(image.height);
    if (image.width == 0 || image.height == 0 ||
        pixels > std::numeric_limits<std::size_t>::max() / 3U ||
        image.rgb.size() != static_cast<std::size_t>(pixels * 3U))
    {
        return make_error(ErrorCode::kValidation, "PNG image buffer does not match its dimensions");
    }
    auto valid_profile = validate_output_profile_state(image.color_profile);
    if (!valid_profile)
    {
        return valid_profile.error();
    }
    const bool standard_srgb = image.color_profile.kind == ColorProfileKind::kBuiltin &&
                               image.color_profile.identifier == kInputProfileSrgb;
    if (image.color_profile.icc_bytes.size() >
        static_cast<std::size_t>(std::numeric_limits<uLong>::max()))
    {
        return make_error(ErrorCode::kValidation, "PNG output ICC profile is too large");
    }
    png_image png{};
    png.version = PNG_IMAGE_VERSION;
    png.width = image.width;
    png.height = image.height;
    png.format = PNG_FORMAT_RGB;
    png.flags = standard_srgb ? 0U : PNG_IMAGE_FLAG_COLORSPACE_NOT_sRGB;
    png_alloc_size_t encoded_size = 0;
    if (png_image_write_to_memory(&png, nullptr, &encoded_size, 0, image.rgb.data(), 0, nullptr) ==
        0)
    {
        return make_error(ErrorCode::kIo, "Unable to size PNG output",
                          {{"png_error", png.message}});
    }
    std::vector<std::uint8_t> encoded(encoded_size);
    if (png_image_write_to_memory(&png, encoded.data(), &encoded_size, 0, image.rgb.data(), 0,
                                  nullptr) == 0)
    {
        return make_error(ErrorCode::kIo, "Unable to encode PNG output",
                          {{"png_error", png.message}});
    }
    encoded.resize(encoded_size);
    if (standard_srgb)
    {
        return encoded;
    }
    constexpr std::size_t kAfterIhdr = 8U + 4U + 4U + 13U + 4U;
    if (encoded.size() < kAfterIhdr || encoded[12] != 'I' || encoded[13] != 'H' ||
        encoded[14] != 'D' || encoded[15] != 'R')
    {
        return make_error(ErrorCode::kValidation, "PNG encoder produced an invalid IHDR layout");
    }

    const auto &icc = image.color_profile.icc_bytes;
    uLongf compressed_size = compressBound(static_cast<uLong>(icc.size()));
    std::vector<std::uint8_t> compressed(compressed_size);
    if (compress2(compressed.data(), &compressed_size, icc.data(), static_cast<uLong>(icc.size()),
                  Z_BEST_COMPRESSION) != Z_OK)
    {
        return make_error(ErrorCode::kIo, "Unable to compress PNG output ICC profile");
    }
    compressed.resize(compressed_size);
    constexpr std::string_view kProfileName = "Ravo output";
    const std::size_t data_size = kProfileName.size() + 2U + compressed.size();
    if (data_size > std::numeric_limits<std::uint32_t>::max())
    {
        return make_error(ErrorCode::kValidation, "Compressed PNG output ICC profile is too large");
    }
    std::vector<std::uint8_t> chunk(4U + 4U + data_size + 4U);
    const auto write_u32 = [&chunk](const std::size_t offset, const std::uint32_t value)
    {
        chunk[offset] = static_cast<std::uint8_t>((value >> 24U) & 0xffU);
        chunk[offset + 1U] = static_cast<std::uint8_t>((value >> 16U) & 0xffU);
        chunk[offset + 2U] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
        chunk[offset + 3U] = static_cast<std::uint8_t>(value & 0xffU);
    };
    write_u32(0U, static_cast<std::uint32_t>(data_size));
    chunk[4] = 'i';
    chunk[5] = 'C';
    chunk[6] = 'C';
    chunk[7] = 'P';
    std::copy(kProfileName.begin(), kProfileName.end(), chunk.begin() + 8);
    const std::size_t compression_method = 8U + kProfileName.size() + 1U;
    chunk[compression_method] = 0U;
    std::copy(compressed.begin(), compressed.end(),
              chunk.begin() + static_cast<std::ptrdiff_t>(compression_method + 1U));
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, chunk.data() + 4U, static_cast<uInt>(4U + data_size));
    write_u32(chunk.size() - 4U, static_cast<std::uint32_t>(crc));
    encoded.insert(encoded.begin() + static_cast<std::ptrdiff_t>(kAfterIhdr), chunk.begin(),
                   chunk.end());
    return encoded;
}

} // namespace ravo
