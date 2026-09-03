#include "image_ops.h"

#include "bayer_demosaic.h"
#include "dng_opcodes.h"
#include "xtrans_demosaic.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <numbers>
#include <string>
#include <vector>

#include <png.h>
#include <zlib.h>

#include "capability_ops.h"
#include "canvas_frame.h"
#include "color_contrast.h"
#include "color_correction.h"
#include "color_checker.h"
#include "color_harmonizer.h"
#include "color_reconstruction.h"
#include "color_zones.h"
#include "d50_lab.h"
#include "dehaze.h"
#include "hsl.h"
#include "lut3d.h"
#include "mask_evaluator.h"
#include "monochrome.h"
#include "output_color.h"
#include "parallel_rows.h"
#include "perspective_transform.h"
#include "primaries.h"
#include "raw_temperature.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/output_dither.h"
#include "ravo/recipe/watermark.h"
#include "retouch.h"
#include "sharpen.h"
#include "texture.h"
#include "split_toning.h"
#include "velvia.h"
#include "ravo/recipe/profile_gamma.h"

#include "image_ops_internal.h"

namespace ravo::image_ops_internal
{

using detail::for_each_row;

[[nodiscard]] bool absorbed_operation(const std::string_view id) noexcept
{
    return id == "ravo.core.identity" || id == "ravo.raw.prepare" || id == "ravo.raw.demosaic" ||
           id == "ravo.color.input" || id == kProfileGammaOperationId ||
           id == "ravo.color.output" || id == kOutputDitherOperationId || id == kFrameOperationId ||
           id == kWatermarkOperationId || id == "ravo.output.scale";
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

void linear_rgb_to_xyz_d50(const float r, const float g, const float b, float xyz[3]) noexcept
{
    const auto converted = d50_lab::linear_rec709_to_xyz({r, g, b});
    std::copy(converted.begin(), converted.end(), xyz);
}

void xyz_d50_to_linear_rgb(const float xyz[3], float &r, float &g, float &b) noexcept
{
    const auto converted = d50_lab::xyz_to_linear_rec709({xyz[0], xyz[1], xyz[2]});
    r = converted[0];
    g = converted[1];
    b = converted[2];
}

void xyz_d50_to_lab(const float xyz[3], float lab[3]) noexcept
{
    const auto converted = d50_lab::xyz_to_lab({xyz[0], xyz[1], xyz[2]});
    std::copy(converted.begin(), converted.end(), lab);
}

void lab_to_xyz_d50(const float lab[3], float xyz[3]) noexcept
{
    const auto converted = d50_lab::lab_to_xyz({lab[0], lab[1], lab[2]});
    std::copy(converted.begin(), converted.end(), xyz);
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

[[nodiscard]] float display_srgb_encode(const float linear) noexcept
{
    const float value = std::max(linear, 0.0F);
    return value <= 0.0031308F ? 12.92F * value : 1.055F * std::pow(value, 1.0F / 2.4F) - 0.055F;
}

[[nodiscard]] float display_srgb_decode(const float encoded) noexcept
{
    const float value = std::max(encoded, 0.0F);
    return value <= 0.04045F ? value / 12.92F : std::pow((value + 0.055F) / 1.055F, 2.4F);
}

[[nodiscard]] Result<void> build_unit_lut(const std::vector<ToneCurvePoint> &points,
                                          std::vector<float> &lut,
                                          const std::string_view interpolation)
{
    auto built = build_tone_curve_lut(points, interpolation, kToneCurveLut);
    if (!built)
    {
        return built.error();
    }
    lut = std::move(built).value();
    return {};
}

Result<void> apply_tone_curve(WorkingImage &image, const OperationInstance &operation,
                              const CancellationToken &cancellation)
{
    auto space = parse_tone_curve_working_space(
        parameter_string(operation, "working_space", std::string(kToneCurveWorkingSpaceRgb)));
    if (!space)
    {
        return space.error();
    }
    const auto interpolation = parameter_string(
        operation, "interpolation", std::string(kToneCurveInterpolationMonotoneHermite));
    if (!curve_interpolation_is_supported(interpolation))
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

    const bool rgb_linked = !independent && space.value() != ToneCurveWorkingSpace::kLab &&
                            space.value() != ToneCurveWorkingSpace::kXyz;
    const bool xyz_linked = space.value() == ToneCurveWorkingSpace::kXyz;
    const bool lab_linked = space.value() == ToneCurveWorkingSpace::kLab && !independent;
    const bool lab_independent = !rgb_linked && !xyz_linked && !lab_linked;

    std::vector<float> table_l;
    std::vector<float> table_a;
    std::vector<float> table_b;
    if (auto built = build_unit_lut(points, table_l, interpolation); !built)
    {
        return built.error();
    }
    if (lab_independent)
    {
        if (auto built = build_unit_lut(points_a, table_a, interpolation); !built)
        {
            return built.error();
        }
        if (auto built = build_unit_lut(points_b, table_b, interpolation); !built)
        {
            return built.error();
        }
    }
    for (int k = 0; k < kToneCurveLut; ++k)
    {
        table_l[static_cast<std::size_t>(k)] *= 100.0F;
        if (lab_independent)
        {
            table_a[static_cast<std::size_t>(k)] =
                table_a[static_cast<std::size_t>(k)] * 256.0F - 128.0F;
            table_b[static_cast<std::size_t>(k)] =
                table_b[static_cast<std::size_t>(k)] * 256.0F - 128.0F;
        }
    }

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

    const auto rows = for_each_row(
        image.height, cancellation,
        [&](const std::uint32_t row)
        {
            const std::size_t begin = static_cast<std::size_t>(row) * image.width * 3U;
            const std::size_t end = begin + static_cast<std::size_t>(image.width) * 3U;
            for (std::size_t index = begin; index < end; index += 3U)
            {
                float xyz[3]{};
                linear_rgb_to_xyz_d50(image.rgb[index], image.rgb[index + 1U],
                                      image.rgb[index + 2U], xyz);
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
                            const float curve_lum =
                                lookup_curve_lut(table_l, lum, xm_l, unbounded_l);
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
                    const int ia =
                        std::clamp(static_cast<int>(a_in * kToneCurveLut), 0, kToneCurveLut - 1);
                    const int ib =
                        std::clamp(static_cast<int>(b_in * kToneCurveLut), 0, kToneCurveLut - 1);
                    lab[1] = table_a[static_cast<std::size_t>(ia)];
                    lab[2] = table_b[static_cast<std::size_t>(ib)];
                }
                lab_to_xyz_d50(lab, xyz);
                xyz_d50_to_linear_rgb(xyz, image.rgb[index], image.rgb[index + 1U],
                                      image.rgb[index + 2U]);
            }
        });
    if (!rows)
    {
        return rows.error();
    }
    return {};
}

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

Result<SigmoidCurve> make_sigmoid_curve(const OperationInstance &operation)
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

[[nodiscard]] float smooth_tone_weight(const float start, const float end,
                                       const float value) noexcept
{
    const float t = std::clamp((value - start) / (end - start), 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
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

[[nodiscard]] Result<ExposureAffine> prepare_exposure_affine(const WorkingImage &input,
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
    return ExposureAffine{scale, params.black};
}

[[nodiscard]] Result<WorkingImage> apply_exposure_impl(const WorkingImage &input,
                                                       const ExposureParams &params,
                                                       const CancellationToken &cancellation)
{
    auto affine = prepare_exposure_affine(input, params, cancellation);
    if (!affine)
    {
        return affine.error();
    }
    const double scale = affine.value().scale;
    auto active = cancellation.check();
    if (!active)
    {
        return active.error();
    }

    WorkingImage output;
    output.width = input.width;
    output.height = input.height;
    output.color_profile = input.color_profile;
    output.exposure_analysis = input.exposure_analysis;
    output.canonical_roi_scale = input.canonical_roi_scale;
    output.mask_attached_frame = input.mask_attached_frame;
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

[[nodiscard]] int light_control_rank(const std::string_view id) noexcept
{
    if (id == "ravo.core.highlights")
    {
        return 0;
    }
    if (id == "ravo.core.shadows")
    {
        return 1;
    }
    if (id == "ravo.core.whites")
    {
        return 2;
    }
    if (id == "ravo.core.blacks")
    {
        return 3;
    }
    return -1;
}

Result<void> apply_light_controls(WorkingImage &image, const LightControlAmounts &amounts,
                                  const CancellationToken &cancellation)
{
    if (amounts.highlights == 0.0 && amounts.shadows == 0.0 && amounts.whites == 0.0 &&
        amounts.blacks == 0.0)
    {
        return {};
    }

    // Lightroom PV2012 endpoint references are substantially wider than the old Lab-L* masks.
    // Keep this a Ravo-owned scene-linear exposure envelope: the constants calibrate the
    // resulting display response without importing Adobe profiles or a PV2012 colour engine.
    constexpr float kMiddleGrey = 0.1842F;
    constexpr float kHighlightStartEv = -4.5F;
    constexpr float kHighlightEndEv = 2.75F;
    constexpr float kShadowStartEv = -6.0F;
    constexpr float kShadowEndEv = 0.75F;
    // Whites and Blacks are narrower companions to Highlights and Shadows. Work in scene EV,
    // anchor both sides at middle grey, and multiply all three channels by one positive factor.
    // The endpoint strengths and envelope widths keep d(EVout)/d(EVin) positive for every
    // slider value, so a light control cannot reverse tones or turn positive colour into a
    // clipped negative block.
    constexpr float kWhiteStartEv = 0.0F;
    constexpr float kWhiteEndEv = 4.0F;
    constexpr float kBlackStartEv = -8.0F;
    constexpr float kBlackEndEv = 0.0F;
    const float highlight_ev =
        static_cast<float>(amounts.highlights) * (amounts.highlights >= 0.0 ? 0.9F : 1.8F);
    const float shadow_ev =
        static_cast<float>(amounts.shadows) * (amounts.shadows >= 0.0 ? 2.0F : 2.9F);
    const float white_ev =
        static_cast<float>(amounts.whites) * (amounts.whites >= 0.0 ? 0.9F : 1.8F);
    const float black_ev =
        static_cast<float>(amounts.blacks) * (amounts.blacks >= 0.0 ? 2.0F : 2.9F);
    return for_each_row(
        image.height, cancellation,
        [&](const std::uint32_t row)
        {
            const std::size_t begin = static_cast<std::size_t>(row) * image.width * 3U;
            const std::size_t end = begin + static_cast<std::size_t>(image.width) * 3U;
            for (std::size_t index = begin; index < end; index += 3U)
            {
                const float luminance = working_luminance(image.rgb[index], image.rgb[index + 1U],
                                                          image.rgb[index + 2U]);
                if (luminance <= 0.0F)
                {
                    continue;
                }
                float tone_ev = std::log2(luminance / kMiddleGrey);
                float total_delta_ev = 0.0F;
                const auto apply_delta = [&](const float delta) noexcept
                {
                    tone_ev += delta;
                    total_delta_ev += delta;
                };
                if (highlight_ev != 0.0F)
                {
                    apply_delta(highlight_ev *
                                smooth_tone_weight(kHighlightStartEv, kHighlightEndEv, tone_ev));
                }
                if (shadow_ev != 0.0F)
                {
                    apply_delta(shadow_ev *
                                (1.0F - smooth_tone_weight(kShadowStartEv, kShadowEndEv, tone_ev)));
                }
                if (white_ev != 0.0F)
                {
                    apply_delta(white_ev * smooth_tone_weight(kWhiteStartEv, kWhiteEndEv, tone_ev));
                }
                if (black_ev != 0.0F)
                {
                    apply_delta(black_ev *
                                (1.0F - smooth_tone_weight(kBlackStartEv, kBlackEndEv, tone_ev)));
                }
                const float scale = std::exp2(total_delta_ev);
                image.rgb[index] *= scale;
                image.rgb[index + 1U] *= scale;
                image.rgb[index + 2U] *= scale;
            }
        });
}

Result<void> apply_vibrance_saturation(WorkingImage &image, const double vibrance,
                                       const double saturation,
                                       const CancellationToken &cancellation)
{
    if (vibrance == 0.0 && saturation == 0.0)
    {
        return {};
    }
    const float vibrance_amount = static_cast<float>(vibrance) / 1.4F;
    const float saturation_amount = static_cast<float>(saturation);
    return for_each_row(
        image.height, cancellation,
        [&](const std::uint32_t row)
        {
            const std::size_t begin = static_cast<std::size_t>(row) * image.width * 3U;
            const std::size_t end = begin + static_cast<std::size_t>(image.width) * 3U;
            for (std::size_t index = begin; index < end; index += 3U)
            {
                float &r = image.rgb[index];
                float &g = image.rgb[index + 1U];
                float &b = image.rgb[index + 2U];
                const float average = (r + g + b) / 3.0F;
                const float dr = average - r;
                const float dg = average - g;
                const float db = average - b;
                const float delta = std::sqrt(dr * dr + dg * dg + db * db);
                const float vibrance_gain =
                    vibrance_amount * (1.0F - std::pow(delta, std::abs(vibrance_amount)));
                const float gain = 1.0F + saturation_amount + vibrance_gain;
                r = average + gain * (r - average);
                g = average + gain * (g - average);
                b = average + gain * (b - average);
            }
        });
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
    output.canonical_roi_scale = image.canonical_roi_scale;
    output.mask_attached_frame.reset();
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
    output.canonical_roi_scale = image.canonical_roi_scale;
    output.mask_attached_frame.reset();
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
    output.canonical_roi_scale = image.canonical_roi_scale;
    output.mask_attached_frame.reset();
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

} // namespace ravo::image_ops_internal
