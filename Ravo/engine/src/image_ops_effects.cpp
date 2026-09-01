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
    output.canonical_roi_scale = image.canonical_roi_scale;
    output.mask_attached_frame.reset();
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

[[nodiscard]] Result<void> apply_rgb_levels(WorkingImage &image, const OperationInstance &operation)
{
    const auto mode = parameter_string(operation, "mode", std::string(kRgbLevelsModeLinked));
    if (mode != kRgbLevelsModeLinked && mode != kRgbLevelsModeIndependent)
    {
        return make_error(ErrorCode::kValidation, "RGB levels mode is unsupported",
                          {{"mode", mode}});
    }
    const auto preserve = parameter_string(operation, "preserve_colors",
                                           std::string(kToneCurvePreserveColorsLuminance));
    if (preserve != kToneCurvePreserveColorsNone && preserve != kToneCurvePreserveColorsLuminance &&
        preserve != kToneCurvePreserveColorsMax && preserve != kToneCurvePreserveColorsAverage &&
        preserve != kToneCurvePreserveColorsSum && preserve != kToneCurvePreserveColorsNorm &&
        preserve != kToneCurvePreserveColorsPower)
    {
        return make_error(ErrorCode::kValidation, "RGB levels preserve-colors is unsupported",
                          {{"preserve_colors", preserve}});
    }
    const std::array<std::array<float, 3>, 3> levels{
        {{static_cast<float>(parameter(operation, "black", 0.0)),
          static_cast<float>(parameter(operation, "grey", 0.5)),
          static_cast<float>(parameter(operation, "white", 1.0))},
         {static_cast<float>(parameter(operation, "black_g", 0.0)),
          static_cast<float>(parameter(operation, "grey_g", 0.5)),
          static_cast<float>(parameter(operation, "white_g", 1.0))},
         {static_cast<float>(parameter(operation, "black_b", 0.0)),
          static_cast<float>(parameter(operation, "grey_b", 0.5)),
          static_cast<float>(parameter(operation, "white_b", 1.0))}}};
    std::array<float, 3> inv_gamma{};
    std::array<float, 3> mult{};
    std::vector<float> lut(3U * 65536U);
    const int channels = mode == kRgbLevelsModeIndependent ? 3 : 1;
    for (int c = 0; c < channels; ++c)
    {
        const float black = levels[static_cast<std::size_t>(c)][0];
        const float grey = levels[static_cast<std::size_t>(c)][1];
        const float white = levels[static_cast<std::size_t>(c)][2];
        if (!(white > black))
        {
            return make_error(ErrorCode::kValidation, "RGB levels white must be greater than black",
                              {{"channel", std::to_string(c)}});
        }
        const float delta = (white - black) * 0.5F;
        const float mid = black + delta;
        inv_gamma[static_cast<std::size_t>(c)] = std::pow(10.0F, (grey - mid) / delta);
        mult[static_cast<std::size_t>(c)] = 1.0F / (white - black);
        float *row = lut.data() + static_cast<std::size_t>(c) * 65536U;
        for (unsigned i = 0; i < 65536U; ++i)
        {
            const float percentage = static_cast<float>(i) / 65536.0F;
            row[i] = std::pow(percentage, inv_gamma[static_cast<std::size_t>(c)]);
        }
    }
    if (mode == kRgbLevelsModeLinked)
    {
        inv_gamma[1] = inv_gamma[2] = inv_gamma[0];
        mult[1] = mult[2] = mult[0];
        const float *src = lut.data();
        std::copy(src, src + 65536U, lut.data() + 65536U);
        std::copy(src, src + 65536U, lut.data() + 2U * 65536U);
    }
    const auto apply_channel = [&](const float sample, const int channel) -> float
    {
        const float black = levels[static_cast<std::size_t>(channel)][0];
        const float white = levels[static_cast<std::size_t>(channel)][2];
        if (sample <= black)
        {
            return 0.0F;
        }
        const float percentage = (sample - black) * mult[static_cast<std::size_t>(channel)];
        if (sample >= white)
        {
            return std::pow(percentage, inv_gamma[static_cast<std::size_t>(channel)]);
        }
        const int index = std::clamp(static_cast<int>(percentage * 65536.0F), 0, 65535);
        return lut[static_cast<std::size_t>(channel) * 65536U + static_cast<std::size_t>(index)];
    };
    const bool independent_or_none =
        mode == kRgbLevelsModeIndependent || preserve == kToneCurvePreserveColorsNone;
    for (std::size_t index = 0; index + 2 < image.rgb.size(); index += 3)
    {
        if (independent_or_none)
        {
            image.rgb[index] = apply_channel(image.rgb[index], 0);
            image.rgb[index + 1U] =
                apply_channel(image.rgb[index + 1U], mode == kRgbLevelsModeIndependent ? 1 : 0);
            image.rgb[index + 2U] =
                apply_channel(image.rgb[index + 2U], mode == kRgbLevelsModeIndependent ? 2 : 0);
            continue;
        }
        const float rgb[3]{image.rgb[index], image.rgb[index + 1U], image.rgb[index + 2U]};
        const float lum = rgb_norm(rgb, preserve);
        if (lum <= levels[0][0])
        {
            image.rgb[index] = 0.0F;
            image.rgb[index + 1U] = 0.0F;
            image.rgb[index + 2U] = 0.0F;
            continue;
        }
        const float curve_lum = apply_channel(lum, 0);
        const float ratio = curve_lum / lum;
        image.rgb[index] *= ratio;
        image.rgb[index + 1U] *= ratio;
        image.rgb[index + 2U] *= ratio;
    }
    return {};
}

[[nodiscard]] Result<std::array<float, 9>> invert_working_matrix(const std::array<float, 9> &matrix)
{
    const double a = matrix[0];
    const double b = matrix[1];
    const double c = matrix[2];
    const double d = matrix[3];
    const double e = matrix[4];
    const double f = matrix[5];
    const double g = matrix[6];
    const double h = matrix[7];
    const double i = matrix[8];
    const double determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!std::isfinite(determinant) || std::abs(determinant) < 1.0e-12)
    {
        return make_error(ErrorCode::kValidation, "Working colour matrix is singular",
                          {{"reason", "unsupported_rgbcurve_middle_grey_profile"}});
    }
    const double inverse = 1.0 / determinant;
    std::array<float, 9> result{
        static_cast<float>((e * i - f * h) * inverse),
        static_cast<float>((c * h - b * i) * inverse),
        static_cast<float>((b * f - c * e) * inverse),
        static_cast<float>((f * g - d * i) * inverse),
        static_cast<float>((a * i - c * g) * inverse),
        static_cast<float>((c * d - a * f) * inverse),
        static_cast<float>((d * h - e * g) * inverse),
        static_cast<float>((b * g - a * h) * inverse),
        static_cast<float>((a * e - b * d) * inverse),
    };
    return result;
}

[[nodiscard]] Result<float> uncompensate_middle_grey(const float sample,
                                                     const std::array<float, 9> &xyz_to_rgb)
{
    if (!std::isfinite(sample))
    {
        return make_error(ErrorCode::kValidation, "RGB curve node is not finite");
    }
    const float lab[3]{sample * 100.0F, 0.0F, 0.0F};
    float xyz[3]{};
    lab_to_xyz_d50(lab, xyz);
    const float rgb0 = xyz_to_rgb[0] * xyz[0] + xyz_to_rgb[1] * xyz[1] + xyz_to_rgb[2] * xyz[2];
    if (!std::isfinite(rgb0))
    {
        return make_error(ErrorCode::kValidation,
                          "RGB curve middle-grey uncompensate is non-finite");
    }
    return rgb0;
}

[[nodiscard]] Result<void> apply_rgb_curve(WorkingImage &image, const OperationInstance &operation,
                                           const CancellationToken &cancellation)
{
    const auto application_space = parameter_string(
        operation, "application_space", std::string(kRgbCurveApplicationSpaceSceneLinear));
    if (application_space != kRgbCurveApplicationSpaceSceneLinear &&
        application_space != kRgbCurveApplicationSpaceDisplaySrgb)
    {
        return make_error(ErrorCode::kValidation, "RGB curve application space is unsupported",
                          {{"application_space", application_space}});
    }
    const bool display_srgb = application_space == kRgbCurveApplicationSpaceDisplaySrgb;
    const auto mode = parameter_string(operation, "mode", std::string(kRgbLevelsModeLinked));
    if (mode != kRgbLevelsModeLinked && mode != kRgbLevelsModeIndependent)
    {
        return make_error(ErrorCode::kValidation, "RGB curve mode is unsupported",
                          {{"mode", mode}});
    }
    const auto interpolation = parameter_string(
        operation, "interpolation", std::string(kToneCurveInterpolationMonotoneHermite));
    if (!curve_interpolation_is_supported(interpolation))
    {
        return make_error(ErrorCode::kValidation, "RGB curve interpolation is unsupported",
                          {{"interpolation", interpolation}});
    }
    const auto preserve = parameter_string(operation, "preserve_colors",
                                           std::string(kToneCurvePreserveColorsLuminance));
    if (preserve != kToneCurvePreserveColorsNone && preserve != kToneCurvePreserveColorsLuminance &&
        preserve != kToneCurvePreserveColorsMax && preserve != kToneCurvePreserveColorsAverage &&
        preserve != kToneCurvePreserveColorsSum && preserve != kToneCurvePreserveColorsNorm &&
        preserve != kToneCurvePreserveColorsPower)
    {
        return make_error(ErrorCode::kValidation, "RGB curve preserve-colors is unsupported",
                          {{"preserve_colors", preserve}});
    }
    bool compensate = false;
    if (const auto found = operation.parameters.find("compensate_middle_grey");
        found != operation.parameters.end())
    {
        if (const auto *flag = std::get_if<bool>(&found->second.value); flag != nullptr)
        {
            compensate = *flag;
        }
        else
        {
            compensate = parameter(operation, "compensate_middle_grey", 0.0) != 0.0;
        }
    }
    const auto parse_channel = [&](const char *name) -> Result<std::vector<ToneCurvePoint>>
    {
        if (const auto found = operation.parameters.find(name); found != operation.parameters.end())
        {
            return parse_rgb_curve_points(found->second);
        }
        return std::vector<ToneCurvePoint>{{0.0, 0.0}, {1.0, 1.0}};
    };
    auto red_points = parse_channel("points");
    if (!red_points)
    {
        return red_points.error();
    }
    auto green_points = parse_channel("points_g");
    if (!green_points)
    {
        return green_points.error();
    }
    auto blue_points = parse_channel("points_b");
    if (!blue_points)
    {
        return blue_points.error();
    }
    if (compensate)
    {
        if (image.color_profile.model != ColorModel::kRgb || !image.color_profile.has_matrix)
        {
            return make_error(
                ErrorCode::kUnsupported,
                "RGB curve middle-grey compensation requires a linear working RGB matrix",
                {{"reason", "unsupported_rgbcurve_middle_grey_profile"}});
        }
        auto xyz_to_rgb = invert_working_matrix(image.color_profile.matrix_to_xyz_d50);
        if (!xyz_to_rgb)
        {
            return xyz_to_rgb.error();
        }
        const auto remap = [&](std::vector<ToneCurvePoint> &points) -> Result<void>
        {
            for (auto &point : points)
            {
                auto x = uncompensate_middle_grey(static_cast<float>(point.x), xyz_to_rgb.value());
                if (!x)
                {
                    return x.error();
                }
                auto y = uncompensate_middle_grey(static_cast<float>(point.y), xyz_to_rgb.value());
                if (!y)
                {
                    return y.error();
                }
                point.x = x.value();
                point.y = y.value();
            }
            for (std::size_t index = 1; index < points.size(); ++index)
            {
                if (!(points[index].x > points[index - 1U].x))
                {
                    return make_error(
                        ErrorCode::kValidation,
                        "RGB curve nodes are not increasing after middle-grey uncompensate");
                }
            }
            return {};
        };
        if (auto mapped = remap(red_points.value()); !mapped)
        {
            return mapped.error();
        }
        if (auto mapped = remap(green_points.value()); !mapped)
        {
            return mapped.error();
        }
        if (auto mapped = remap(blue_points.value()); !mapped)
        {
            return mapped.error();
        }
    }
    std::array<std::vector<float>, 3> tables{};
    std::array<std::array<float, 3>, 3> unbounded{};
    std::array<float, 3> xm{};
    const std::array<const std::vector<ToneCurvePoint> *, 3> channels{
        &red_points.value(), &green_points.value(), &blue_points.value()};
    RgbCurveParams parametric;
    parametric.parametric_shadows = parameter(operation, "parametric_shadows", 0.0);
    parametric.parametric_darks = parameter(operation, "parametric_darks", 0.0);
    parametric.parametric_lights = parameter(operation, "parametric_lights", 0.0);
    parametric.parametric_highlights = parameter(operation, "parametric_highlights", 0.0);
    parametric.parametric_split_shadows = parameter(operation, "parametric_split_shadows", 0.25);
    parametric.parametric_split_mid = parameter(operation, "parametric_split_mid", 0.50);
    parametric.parametric_split_highlights =
        parameter(operation, "parametric_split_highlights", 0.75);
    clamp_rgb_curve(parametric);
    const bool apply_parametric =
        mode == kRgbLevelsModeLinked && !rgb_curve_parametric_is_identity(parametric);
    if (display_srgb &&
        (mode != kRgbLevelsModeIndependent || preserve != kToneCurvePreserveColorsNone ||
         compensate || apply_parametric))
    {
        return make_error(
            ErrorCode::kValidation,
            "Display-sRGB RGB curves require independent channels without scene compensation",
            {{"reason", "unsupported_display_srgb_curve_policy"}});
    }
    const int lut_count = mode == kRgbLevelsModeIndependent ? 3 : 1;
    for (int channel = 0; channel < lut_count; ++channel)
    {
        if (apply_parametric && channel == 0)
        {
            auto &lut = tables[0];
            lut.assign(static_cast<std::size_t>(kToneCurveLut), 0.0F);
            for (int k = 0; k < kToneCurveLut; ++k)
            {
                const double x = static_cast<double>(k) / static_cast<double>(kToneCurveLut);
                lut[static_cast<std::size_t>(k)] = static_cast<float>(evaluate_tone_curve(
                    *channels[0], evaluate_rgb_curve_parametric(parametric, x), interpolation));
            }
        }
        else
        {
            auto built = build_unit_lut(*channels[static_cast<std::size_t>(channel)],
                                        tables[static_cast<std::size_t>(channel)], interpolation);
            if (!built)
            {
                return built.error();
            }
        }
        const auto &points = *channels[static_cast<std::size_t>(channel)];
        xm[static_cast<std::size_t>(channel)] =
            points.empty() ? 1.0F : static_cast<float>(points.back().x);
        const float x_l[4] = {0.7F * xm[static_cast<std::size_t>(channel)],
                              0.8F * xm[static_cast<std::size_t>(channel)],
                              0.9F * xm[static_cast<std::size_t>(channel)],
                              1.0F * xm[static_cast<std::size_t>(channel)]};
        float y_l[4]{};
        for (int i = 0; i < 4; ++i)
        {
            const int index =
                std::clamp(static_cast<int>(x_l[i] * kToneCurveLut), 0, kToneCurveLut - 1);
            y_l[i] = tables[static_cast<std::size_t>(channel)][static_cast<std::size_t>(index)];
        }
        estimate_exp(x_l, y_l, 4, unbounded[static_cast<std::size_t>(channel)].data());
    }
    if (mode == kRgbLevelsModeLinked)
    {
        tables[1] = tables[0];
        tables[2] = tables[0];
        unbounded[1] = unbounded[0];
        unbounded[2] = unbounded[0];
        xm[1] = xm[2] = xm[0];
    }
    const auto apply_channel = [&](const float sample, const int channel) -> float
    {
        return lookup_curve_lut(tables[static_cast<std::size_t>(channel)], sample,
                                xm[static_cast<std::size_t>(channel)],
                                unbounded[static_cast<std::size_t>(channel)].data());
    };
    const auto apply_sample = [&](const float sample, const int channel) -> float
    {
        if (!display_srgb)
        {
            return apply_channel(sample, channel);
        }
        return display_srgb_decode(apply_channel(display_srgb_encode(sample), channel));
    };
    const bool independent_or_none =
        mode == kRgbLevelsModeIndependent || preserve == kToneCurvePreserveColorsNone;
    const auto rows = for_each_row(
        image.height, cancellation,
        [&](const std::uint32_t row)
        {
            const std::size_t begin = static_cast<std::size_t>(row) * image.width * 3U;
            const std::size_t end = begin + static_cast<std::size_t>(image.width) * 3U;
            for (std::size_t index = begin; index < end; index += 3U)
            {
                if (independent_or_none)
                {
                    image.rgb[index] = apply_sample(image.rgb[index], 0);
                    image.rgb[index + 1U] = apply_sample(image.rgb[index + 1U],
                                                         mode == kRgbLevelsModeIndependent ? 1 : 0);
                    image.rgb[index + 2U] = apply_sample(image.rgb[index + 2U],
                                                         mode == kRgbLevelsModeIndependent ? 2 : 0);
                    continue;
                }
                const float rgb[3]{image.rgb[index], image.rgb[index + 1U], image.rgb[index + 2U]};
                const float lum = rgb_norm(rgb, preserve);
                if (lum > 0.0F)
                {
                    const float curve_lum = apply_channel(lum, 0);
                    const float ratio = curve_lum / lum;
                    image.rgb[index] *= ratio;
                    image.rgb[index + 1U] *= ratio;
                    image.rgb[index + 2U] *= ratio;
                }
            }
        });
    if (!rows)
    {
        return rows.error();
    }
    return {};
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

Result<void> apply_vignette(WorkingImage &image, const double amount, const double midpoint,
                            const double falloff, const double shape, const double center_x,
                            const double center_y, const CancellationToken &cancellation)
{
    if (std::abs(amount) <= 1.0e-8 || image.width == 0 || image.height == 0)
    {
        return {};
    }
    const float brightness = -0.5F * static_cast<float>(amount);
    const float saturation = -static_cast<float>(amount) * 0.5F;
    const float dscale = static_cast<float>(std::clamp(midpoint, 0.0, 1.0));
    const float fscale = std::max(0.05F, static_cast<float>(falloff));
    const float shape_clamped = std::clamp(static_cast<float>(shape), 0.5F, 5.0F);
    const float exp1 = 2.0F / shape_clamped;
    const float exp2 = shape_clamped / 2.0F;
    const float xscale = 2.0F / static_cast<float>(image.width);
    const float yscale = 2.0F / static_cast<float>(image.height);
    const float cx = 1.0F + static_cast<float>(std::clamp(center_x, -1.0, 1.0));
    const float cy = 1.0F + static_cast<float>(std::clamp(center_y, -1.0, 1.0));
    return for_each_row(image.height, cancellation,
                        [&](const std::uint32_t y)
                        {
                            for (std::uint32_t x = 0; x < image.width; ++x)
                            {
                                const float pv_x = std::abs(static_cast<float>(x) * xscale - cx);
                                const float pv_y = std::abs(static_cast<float>(y) * yscale - cy);
                                const float cplen =
                                    std::pow(std::pow(pv_x, exp1) + std::pow(pv_y, exp1), exp2);
                                float weight = 0.0F;
                                if (cplen >= dscale)
                                {
                                    weight = std::clamp((cplen - dscale) / fscale, 0.0F, 1.0F);
                                }
                                if (weight <= 0.0F)
                                {
                                    continue;
                                }
                                const std::size_t index =
                                    (static_cast<std::size_t>(y) * image.width + x) * 3U;
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
                        });
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

void rgb_to_hsl(const float r, const float g, const float b, float &h, float &s, float &l)
{
    hsl::rgb_to_hsl(r, g, b, h, s, l);
}

void hsl_to_rgb(const float h, const float s, const float l, float &r, float &g, float &b)
{
    hsl::hsl_to_rgb(h, s, l, r, g, b);
}

} // namespace ravo::image_ops_internal
