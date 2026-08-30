#include "perspective_transform.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <numbers>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "parallel_rows.h"

namespace ravo
{
namespace
{

// Adapted under GPL-3.0-or-later from ART commit
// 6f511409afe28b2096c38483a6dfa3afcf167f5b,
// rtengine/perspectivecorrection.{cc,h}, copyright (c) 2019 Alberto Griggio.
// Those files derive their ShiftN formulation from darktable ashift, copyright
// (c) 2016 Ulrich Pegelow, with acknowledgement to ShiftN author Marcus Hebel.
// Ravo owns bounds, cancellation, allocation, row scheduling, deterministic
// crop search, interpolation, analysis, and publication.
constexpr double kReferenceFocalLength = 28.0;
constexpr double kDenominatorEpsilon = 1.0e-10;
constexpr double kDimensionGrowthLimit = 4.0;
constexpr std::size_t kSafeCropSamples = 192U;

using Matrix = std::array<double, 9>;

[[nodiscard]] Matrix identity_matrix() noexcept
{
    return {1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
}

[[nodiscard]] Matrix multiply(const Matrix &left, const Matrix &right) noexcept
{
    Matrix result{};
    for (std::size_t row = 0U; row < 3U; ++row)
    {
        for (std::size_t column = 0U; column < 3U; ++column)
        {
            for (std::size_t inner = 0U; inner < 3U; ++inner)
                result[row * 3U + column] +=
                    left[row * 3U + inner] * right[inner * 3U + column];
        }
    }
    return result;
}

[[nodiscard]] Result<Matrix> inverse(const Matrix &matrix)
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
    const double determinant =
        a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!std::isfinite(determinant) || std::abs(determinant) < kDenominatorEpsilon)
        return make_error(ErrorCode::kValidation, "Perspective homography is singular",
                          {{"reason", "degenerate_perspective_homography"}});
    const double scale = 1.0 / determinant;
    Matrix result{
        (e * i - f * h) * scale, (c * h - b * i) * scale, (b * f - c * e) * scale,
        (f * g - d * i) * scale, (a * i - c * g) * scale, (c * d - a * f) * scale,
        (d * h - e * g) * scale, (b * g - a * h) * scale, (a * e - b * d) * scale,
    };
    if (!std::all_of(result.begin(), result.end(), [](const double value)
                     { return std::isfinite(value); }))
        return make_error(ErrorCode::kValidation, "Perspective inverse is non-finite",
                          {{"reason", "degenerate_perspective_homography"}});
    return result;
}

[[nodiscard]] Result<PerspectivePoint> transform(const Matrix &matrix, const double x,
                                                 const double y)
{
    const double denominator = matrix[6] * x + matrix[7] * y + matrix[8];
    if (!std::isfinite(denominator) || std::abs(denominator) < kDenominatorEpsilon)
        return make_error(ErrorCode::kValidation, "Perspective transform crosses infinity",
                          {{"reason", "degenerate_perspective_homography"}});
    PerspectivePoint result{(matrix[0] * x + matrix[1] * y + matrix[2]) / denominator,
                            (matrix[3] * x + matrix[4] * y + matrix[5]) / denominator};
    if (!std::isfinite(result.x) || !std::isfinite(result.y))
        return make_error(ErrorCode::kValidation, "Perspective transform is non-finite",
                          {{"reason", "degenerate_perspective_homography"}});
    return result;
}

[[nodiscard]] Matrix reference_homography(const std::uint32_t width, const std::uint32_t height,
                                          const PerspectiveParams &params) noexcept
{
    const double u = static_cast<double>(width);
    const double v = static_cast<double>(height);
    const double phi = params.rotation_degrees * std::numbers::pi / 180.0;
    const double cosine = std::cos(phi);
    const double sine = std::sin(phi);
    const double vertical_exponential = std::exp(params.vertical_shift);
    const double horizontal_exponential = std::exp(params.horizontal_shift);

    Matrix current{0.0, 1.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0};
    const Matrix rotation{cosine,
                          -sine,
                          -0.5 * v * cosine + 0.5 * u * sine + 0.5 * v,
                          sine,
                          cosine,
                          -0.5 * v * sine - 0.5 * u * cosine + 0.5 * u,
                          0.0,
                          0.0,
                          1.0};
    current = multiply(rotation, current);
    const Matrix shear{1.0, params.shear, 0.0, params.shear, 1.0, 0.0, 0.0, 0.0, 1.0};
    current = multiply(shear, current);

    const Matrix vertical{
        vertical_exponential,
        0.0,
        0.0,
        0.5 * (vertical_exponential - 1.0) * u / v,
        2.0 * vertical_exponential / (vertical_exponential + 1.0),
        -0.5 * (vertical_exponential - 1.0) * u / (vertical_exponential + 1.0),
        (vertical_exponential - 1.0) / v,
        0.0,
        1.0};
    current = multiply(vertical, current);

    const Matrix swap{0.0, 1.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0};
    current = multiply(swap, current);

    const Matrix horizontal{
        horizontal_exponential,
        0.0,
        0.0,
        0.5 * (horizontal_exponential - 1.0) * v / u,
        2.0 * horizontal_exponential / (horizontal_exponential + 1.0),
        -0.5 * (horizontal_exponential - 1.0) * v / (horizontal_exponential + 1.0),
        (horizontal_exponential - 1.0) / u,
        0.0,
        1.0};
    current = multiply(horizontal, current);

    // Generic ashift mode makes orthographic compression and aspect scaling
    // identities. Retain the focal constant in the contract/provenance even
    // though it cancels in that mode.
    static_cast<void>(kReferenceFocalLength);
    return current;
}

[[nodiscard]] std::optional<std::pair<double, double>>
horizontal_interval(const std::array<PerspectivePoint, 4> &quad, const double y) noexcept
{
    std::array<double, 8> intersections{};
    std::size_t count = 0U;
    for (std::size_t index = 0U; index < quad.size(); ++index)
    {
        const PerspectivePoint first = quad[index];
        const PerspectivePoint second = quad[(index + 1U) % quad.size()];
        const double low = std::min(first.y, second.y);
        const double high = std::max(first.y, second.y);
        if (y < low - 1.0e-9 || y > high + 1.0e-9)
            continue;
        if (std::abs(second.y - first.y) <= 1.0e-12)
        {
            intersections[count++] = first.x;
            intersections[count++] = second.x;
            continue;
        }
        const double fraction = (y - first.y) / (second.y - first.y);
        if (fraction >= -1.0e-9 && fraction <= 1.0 + 1.0e-9)
            intersections[count++] = first.x + fraction * (second.x - first.x);
    }
    if (count < 2U)
        return std::nullopt;
    const auto bounds = std::minmax_element(intersections.begin(), intersections.begin() +
                                                                          static_cast<std::ptrdiff_t>(count));
    if (!std::isfinite(*bounds.first) || !std::isfinite(*bounds.second) ||
        *bounds.second <= *bounds.first)
        return std::nullopt;
    return std::pair<double, double>{*bounds.first, *bounds.second};
}

[[nodiscard]] double crop_area(const std::array<PerspectivePoint, 4> &quad, const double top,
                               const double bottom, PerspectiveRect *rectangle = nullptr) noexcept
{
    if (!(bottom > top))
        return 0.0;
    const auto top_interval = horizontal_interval(quad, top);
    const auto bottom_interval = horizontal_interval(quad, bottom);
    if (!top_interval || !bottom_interval)
        return 0.0;
    const double left = std::max(top_interval->first, bottom_interval->first);
    const double right = std::min(top_interval->second, bottom_interval->second);
    if (!(right > left))
        return 0.0;
    if (rectangle != nullptr)
        *rectangle = {left, top, right, bottom};
    return (right - left) * (bottom - top);
}

[[nodiscard]] Result<PerspectiveRect>
maximal_safe_crop(const std::array<PerspectivePoint, 4> &quad)
{
    double minimum_y = std::numeric_limits<double>::max();
    double maximum_y = std::numeric_limits<double>::lowest();
    for (const auto &point : quad)
    {
        minimum_y = std::min(minimum_y, point.y);
        maximum_y = std::max(maximum_y, point.y);
    }
    if (!std::isfinite(minimum_y) || !std::isfinite(maximum_y) ||
        maximum_y - minimum_y < 1.0e-6)
        return make_error(ErrorCode::kValidation, "Perspective safe crop is empty",
                          {{"reason", "empty_perspective_safe_crop"}});

    PerspectiveRect best{};
    double best_area = 0.0;
    const double initial_step = (maximum_y - minimum_y) / static_cast<double>(kSafeCropSamples);
    for (std::size_t top_index = 0U; top_index < kSafeCropSamples; ++top_index)
    {
        const double top = minimum_y + static_cast<double>(top_index) * initial_step;
        for (std::size_t bottom_index = top_index + 1U; bottom_index <= kSafeCropSamples;
             ++bottom_index)
        {
            const double bottom =
                minimum_y + static_cast<double>(bottom_index) * initial_step;
            PerspectiveRect candidate{};
            const double area = crop_area(quad, top, bottom, &candidate);
            if (area > best_area)
            {
                best_area = area;
                best = candidate;
            }
        }
    }
    if (!(best_area > 0.0))
        return make_error(ErrorCode::kValidation, "Perspective safe crop has no solution",
                          {{"reason", "empty_perspective_safe_crop"}});

    double step = initial_step;
    for (std::size_t iteration = 0U; iteration < 64U && step > 1.0e-7; ++iteration)
    {
        bool improved = false;
        for (const double top_delta : {-step, 0.0, step})
        {
            for (const double bottom_delta : {-step, 0.0, step})
            {
                const double top = std::clamp(best.top + top_delta, minimum_y, maximum_y);
                const double bottom = std::clamp(best.bottom + bottom_delta, minimum_y, maximum_y);
                PerspectiveRect candidate{};
                const double area = crop_area(quad, top, bottom, &candidate);
                if (area > best_area + 1.0e-12)
                {
                    best_area = area;
                    best = candidate;
                    improved = true;
                }
            }
        }
        if (!improved)
            step *= 0.5;
    }
    return best;
}

[[nodiscard]] double sinc(const double value) noexcept
{
    if (std::abs(value) < 1.0e-12)
        return 1.0;
    const double angle = std::numbers::pi * value;
    return std::sin(angle) / angle;
}

[[nodiscard]] double lanczos_weight(const double value, const int radius) noexcept
{
    const double absolute = std::abs(value);
    return absolute < static_cast<double>(radius) ? sinc(value) * sinc(value / radius) : 0.0;
}

void sample_bilinear(const LinearWorkingBuffer &input, const double x, const double y,
                     float *output) noexcept
{
    const auto x0 = static_cast<std::uint32_t>(std::floor(x));
    const auto y0 = static_cast<std::uint32_t>(std::floor(y));
    const auto x1 = std::min(x0 + 1U, input.width - 1U);
    const auto y1 = std::min(y0 + 1U, input.height - 1U);
    const float tx = static_cast<float>(x - static_cast<double>(x0));
    const float ty = static_cast<float>(y - static_cast<double>(y0));
    const std::size_t i00 = (static_cast<std::size_t>(y0) * input.width + x0) * 3U;
    const std::size_t i10 = (static_cast<std::size_t>(y0) * input.width + x1) * 3U;
    const std::size_t i01 = (static_cast<std::size_t>(y1) * input.width + x0) * 3U;
    const std::size_t i11 = (static_cast<std::size_t>(y1) * input.width + x1) * 3U;
    for (std::size_t channel = 0U; channel < 3U; ++channel)
    {
        const float top = input.rgb[i00 + channel] * (1.0F - tx) + input.rgb[i10 + channel] * tx;
        const float bottom =
            input.rgb[i01 + channel] * (1.0F - tx) + input.rgb[i11 + channel] * tx;
        output[channel] = top * (1.0F - ty) + bottom * ty;
    }
}

void sample_lanczos(const LinearWorkingBuffer &input, const double x, const double y,
                    const int radius, float *output) noexcept
{
    const int center_x = static_cast<int>(std::floor(x));
    const int center_y = static_cast<int>(std::floor(y));
    std::array<double, 3> sums{};
    double weight_sum = 0.0;
    for (int sample_y = center_y - radius + 1; sample_y <= center_y + radius; ++sample_y)
    {
        if (sample_y < 0 || sample_y >= static_cast<int>(input.height))
            continue;
        const double wy = lanczos_weight(y - static_cast<double>(sample_y), radius);
        for (int sample_x = center_x - radius + 1; sample_x <= center_x + radius; ++sample_x)
        {
            if (sample_x < 0 || sample_x >= static_cast<int>(input.width))
                continue;
            const double weight =
                wy * lanczos_weight(x - static_cast<double>(sample_x), radius);
            const std::size_t index =
                (static_cast<std::size_t>(sample_y) * input.width +
                 static_cast<std::size_t>(sample_x)) *
                3U;
            for (std::size_t channel = 0U; channel < sums.size(); ++channel)
                sums[channel] += weight * static_cast<double>(input.rgb[index + channel]);
            weight_sum += weight;
        }
    }
    if (std::abs(weight_sum) < 1.0e-12)
    {
        sample_bilinear(input, x, y, output);
        return;
    }
    for (std::size_t channel = 0U; channel < sums.size(); ++channel)
        output[channel] = static_cast<float>(sums[channel] / weight_sum);
}

[[nodiscard]] Result<void> validate_input(const LinearWorkingBuffer &input,
                                          const CancellationToken &cancellation)
{
    if (input.width == 0U || input.height == 0U)
        return make_error(ErrorCode::kValidation, "Perspective input dimensions are invalid",
                          {{"reason", "invalid_dimensions"}});
    const std::uint64_t expected = static_cast<std::uint64_t>(input.width) * input.height * 3U;
    if (expected != input.rgb.size())
        return make_error(ErrorCode::kValidation, "Perspective input channel count is invalid",
                          {{"reason", "channel_count_mismatch"}});
    const std::size_t row_channels = static_cast<std::size_t>(input.width) * 3U;
    for (std::uint32_t row = 0U; row < input.height; ++row)
    {
        auto active = cancellation.check();
        if (!active)
            return active.error();
        const std::size_t begin = static_cast<std::size_t>(row) * row_channels;
        for (std::size_t index = begin; index < begin + row_channels; ++index)
        {
            if (!std::isfinite(input.rgb[index]))
                return make_error(ErrorCode::kValidation,
                                  "Perspective input contains NaN or infinity",
                                  {{"reason", "nonfinite_perspective_input"},
                                   {"sample_index", std::to_string(index)}});
        }
    }
    return {};
}

constexpr std::uint32_t kPerspectiveAnalysisMaxEdge = 900U;
constexpr std::size_t kPerspectiveAnalysisMaxEdgePoints = 48000U;
constexpr std::size_t kPerspectiveAnalysisMaxLines = 48U;
constexpr std::size_t kPerspectiveThetaBins = 180U;

struct EdgePoint
{
    double x = 0.0;
    double y = 0.0;
    double magnitude = 0.0;
    double normal_angle = 0.0;
};

struct HoughPeak
{
    std::size_t theta = 0U;
    std::int32_t rho = 0;
    double vote = 0.0;
};

[[nodiscard]] double circular_angle_distance(double first, double second) noexcept
{
    first = std::fmod(std::abs(first - second), std::numbers::pi);
    return std::min(first, std::numbers::pi - first);
}

[[nodiscard]] Result<std::vector<PerspectiveGuideLine>>
detect_perspective_lines(const RasterBuffer &raster, std::uint32_t &analysis_width,
                         std::uint32_t &analysis_height, const CancellationToken &cancellation)
try
{
    if (raster.width < 16U || raster.height < 16U)
        return make_error(ErrorCode::kValidation, "Perspective analysis image is too small",
                          {{"reason", "perspective_analysis_image_too_small"}});
    const std::uint64_t expected = static_cast<std::uint64_t>(raster.width) * raster.height * 3U;
    if (expected != raster.srgb.size())
        return make_error(ErrorCode::kValidation,
                          "Perspective analysis raster channel count is invalid",
                          {{"reason", "channel_count_mismatch"}});
    const double scale = std::min(
        1.0, static_cast<double>(kPerspectiveAnalysisMaxEdge) /
                 static_cast<double>(std::max(raster.width, raster.height)));
    analysis_width = std::max<std::uint32_t>(
        16U, static_cast<std::uint32_t>(std::lround(static_cast<double>(raster.width) * scale)));
    analysis_height = std::max<std::uint32_t>(
        16U, static_cast<std::uint32_t>(std::lround(static_cast<double>(raster.height) * scale)));
    const std::size_t pixels = static_cast<std::size_t>(analysis_width) * analysis_height;
    std::vector<double> luma(pixels, 0.0);
    for (std::uint32_t y = 0U; y < analysis_height; ++y)
    {
        auto active = cancellation.check();
        if (!active)
            return active.error();
        const auto source_y = std::min(
            raster.height - 1U,
            static_cast<std::uint32_t>((static_cast<std::uint64_t>(y) * raster.height) /
                                       analysis_height));
        for (std::uint32_t x = 0U; x < analysis_width; ++x)
        {
            const auto source_x = std::min(
                raster.width - 1U,
                static_cast<std::uint32_t>((static_cast<std::uint64_t>(x) * raster.width) /
                                           analysis_width));
            const std::size_t source =
                (static_cast<std::size_t>(source_y) * raster.width + source_x) * 3U;
            luma[static_cast<std::size_t>(y) * analysis_width + x] =
                (0.2126 * raster.srgb[source] + 0.7152 * raster.srgb[source + 1U] +
                 0.0722 * raster.srgb[source + 2U]) /
                255.0;
        }
    }

    std::vector<double> magnitude(pixels, 0.0);
    std::vector<double> gradient_angle(pixels, 0.0);
    std::vector<double> positive_magnitudes;
    positive_magnitudes.reserve(pixels / 3U);
    double maximum_magnitude = 0.0;
    for (std::uint32_t y = 1U; y + 1U < analysis_height; ++y)
    {
        auto active = cancellation.check();
        if (!active)
            return active.error();
        for (std::uint32_t x = 1U; x + 1U < analysis_width; ++x)
        {
            const auto sample = [&](const int dx, const int dy)
            {
                return luma[(static_cast<std::size_t>(static_cast<std::int32_t>(y) + dy) *
                             analysis_width) +
                            static_cast<std::size_t>(static_cast<std::int32_t>(x) + dx)];
            };
            const double gx = -sample(-1, -1) + sample(1, -1) - 2.0 * sample(-1, 0) +
                              2.0 * sample(1, 0) - sample(-1, 1) + sample(1, 1);
            const double gy = -sample(-1, -1) - 2.0 * sample(0, -1) - sample(1, -1) +
                              sample(-1, 1) + 2.0 * sample(0, 1) + sample(1, 1);
            const double value = std::hypot(gx, gy);
            const std::size_t index = static_cast<std::size_t>(y) * analysis_width + x;
            magnitude[index] = value;
            double angle = std::atan2(gy, gx);
            if (angle < 0.0)
                angle += std::numbers::pi;
            if (angle >= std::numbers::pi)
                angle -= std::numbers::pi;
            gradient_angle[index] = angle;
            if (value > 1.0e-8)
                positive_magnitudes.push_back(value);
            maximum_magnitude = std::max(maximum_magnitude, value);
        }
    }
    if (positive_magnitudes.size() < 32U || maximum_magnitude < 1.0e-5)
        return make_error(ErrorCode::kNotFound, "Perspective analysis found no usable edges",
                          {{"reason", "no_perspective_lines"}});
    const std::size_t percentile = positive_magnitudes.size() * 7U / 10U;
    std::nth_element(positive_magnitudes.begin(), positive_magnitudes.begin() +
                                                      static_cast<std::ptrdiff_t>(percentile),
                     positive_magnitudes.end());
    const double threshold =
        std::max({positive_magnitudes[percentile], maximum_magnitude * 0.08, 0.015});
    std::vector<EdgePoint> edges;
    edges.reserve(std::min(pixels / 4U, kPerspectiveAnalysisMaxEdgePoints));
    for (std::uint32_t y = 2U; y + 2U < analysis_height; ++y)
    {
        for (std::uint32_t x = 2U; x + 2U < analysis_width; ++x)
        {
            const std::size_t index = static_cast<std::size_t>(y) * analysis_width + x;
            const double value = magnitude[index];
            if (value < threshold)
                continue;
            const double angle = gradient_angle[index];
            const bool horizontal_normal = std::abs(std::cos(angle)) >= std::abs(std::sin(angle));
            const double before = horizontal_normal ? magnitude[index - 1U] :
                                                      magnitude[index - analysis_width];
            const double after = horizontal_normal ? magnitude[index + 1U] :
                                                     magnitude[index + analysis_width];
            if (value < before || value < after)
                continue;
            edges.push_back({static_cast<double>(x), static_cast<double>(y), value, angle});
        }
    }
    if (edges.size() > kPerspectiveAnalysisMaxEdgePoints)
    {
        std::sort(edges.begin(), edges.end(), [](const EdgePoint &left, const EdgePoint &right)
                  {
                      return std::tie(left.magnitude, left.y, left.x) >
                             std::tie(right.magnitude, right.y, right.x);
                  });
        edges.resize(kPerspectiveAnalysisMaxEdgePoints);
    }
    if (edges.size() < 24U)
        return make_error(ErrorCode::kNotFound,
                          "Perspective analysis found too few usable edge points",
                          {{"reason", "no_perspective_lines"}});

    const auto rho_limit = static_cast<std::int32_t>(
        std::ceil(std::hypot(static_cast<double>(analysis_width - 1U),
                             static_cast<double>(analysis_height - 1U))));
    const std::size_t rho_bins = static_cast<std::size_t>(rho_limit) * 2U + 1U;
    std::vector<double> accumulator(kPerspectiveThetaBins * rho_bins, 0.0);
    constexpr std::array<double, 5> angular_weights{0.35, 0.7, 1.0, 0.7, 0.35};
    for (const auto &edge : edges)
    {
        const auto center = static_cast<std::int32_t>(std::lround(
            edge.normal_angle / std::numbers::pi * static_cast<double>(kPerspectiveThetaBins)));
        for (std::int32_t delta = -2; delta <= 2; ++delta)
        {
            const auto wrapped = (center + delta + static_cast<std::int32_t>(kPerspectiveThetaBins)) %
                                 static_cast<std::int32_t>(kPerspectiveThetaBins);
            const double theta = static_cast<double>(wrapped) * std::numbers::pi /
                                 static_cast<double>(kPerspectiveThetaBins);
            const auto rho = static_cast<std::int32_t>(
                std::lround(edge.x * std::cos(theta) + edge.y * std::sin(theta)));
            if (rho < -rho_limit || rho > rho_limit)
                continue;
            accumulator[static_cast<std::size_t>(wrapped) * rho_bins +
                        static_cast<std::size_t>(rho + rho_limit)] +=
                edge.magnitude * angular_weights[static_cast<std::size_t>(delta + 2)];
        }
    }
    const double maximum_vote = *std::max_element(accumulator.begin(), accumulator.end());
    const double vote_cutoff = std::max(maximum_vote * 0.10,
                                        threshold * std::min(analysis_width, analysis_height) * 0.08);
    std::vector<HoughPeak> candidates;
    for (std::size_t theta = 0U; theta < kPerspectiveThetaBins; ++theta)
    {
        for (std::size_t rho = 0U; rho < rho_bins; ++rho)
        {
            const double vote = accumulator[theta * rho_bins + rho];
            if (vote >= vote_cutoff)
                candidates.push_back({theta, static_cast<std::int32_t>(rho) - rho_limit, vote});
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](const HoughPeak &left,
                                                       const HoughPeak &right)
              {
                  return std::tie(left.vote, left.theta, left.rho) >
                         std::tie(right.vote, right.theta, right.rho);
              });
    std::vector<HoughPeak> peaks;
    for (const auto &candidate : candidates)
    {
        const bool duplicate = std::any_of(peaks.begin(), peaks.end(), [&](const HoughPeak &peak)
        {
            const std::size_t direct = candidate.theta > peak.theta ? candidate.theta - peak.theta :
                                                                      peak.theta - candidate.theta;
            const std::size_t theta_distance = std::min(direct, kPerspectiveThetaBins - direct);
            return theta_distance <= 4U && std::abs(candidate.rho - peak.rho) <= 8;
        });
        if (!duplicate)
            peaks.push_back(candidate);
        if (peaks.size() >= kPerspectiveAnalysisMaxLines * 2U)
            break;
    }

    std::vector<PerspectiveGuideLine> lines;
    const double minimum_length =
        std::max(12.0, static_cast<double>(std::min(analysis_width, analysis_height)) * 0.12);
    const double scale_x = static_cast<double>(raster.width - 1U) /
                           static_cast<double>(analysis_width - 1U);
    const double scale_y = static_cast<double>(raster.height - 1U) /
                           static_cast<double>(analysis_height - 1U);
    for (const auto &peak : peaks)
    {
        const double theta = static_cast<double>(peak.theta) * std::numbers::pi /
                             static_cast<double>(kPerspectiveThetaBins);
        const double cosine = std::cos(theta);
        const double sine = std::sin(theta);
        const double tangent_x = -sine;
        const double tangent_y = cosine;
        double minimum_projection = std::numeric_limits<double>::max();
        double maximum_projection = std::numeric_limits<double>::lowest();
        for (const auto &edge : edges)
        {
            if (std::abs(edge.x * cosine + edge.y * sine - peak.rho) > 1.75 ||
                circular_angle_distance(edge.normal_angle, theta) > 8.0 * std::numbers::pi / 180.0)
                continue;
            const double projection = edge.x * tangent_x + edge.y * tangent_y;
            minimum_projection = std::min(minimum_projection, projection);
            maximum_projection = std::max(maximum_projection, projection);
        }
        const double length = maximum_projection - minimum_projection;
        if (!std::isfinite(length) || length < minimum_length)
            continue;
        const double x1 = peak.rho * cosine + minimum_projection * tangent_x;
        const double y1 = peak.rho * sine + minimum_projection * tangent_y;
        const double x2 = peak.rho * cosine + maximum_projection * tangent_x;
        const double y2 = peak.rho * sine + maximum_projection * tangent_y;
        const double midpoint_x = (x1 + x2) * 0.5;
        const double midpoint_y = (y1 + y2) * 0.5;
        const bool vertical = std::abs(tangent_y) >= std::cos(30.0 * std::numbers::pi / 180.0);
        const bool horizontal = std::abs(tangent_x) >= std::cos(30.0 * std::numbers::pi / 180.0);
        if (!vertical && !horizontal)
            continue;
        if ((vertical && (midpoint_x < 3.0 || midpoint_x > analysis_width - 4.0)) ||
            (horizontal && (midpoint_y < 3.0 || midpoint_y > analysis_height - 4.0)))
            continue;
        lines.push_back({x1 * scale_x, y1 * scale_y, x2 * scale_x, y2 * scale_y,
                         std::max(1.0, length * peak.vote),
                         vertical ? PerspectiveGuideOrientation::kVertical :
                                    PerspectiveGuideOrientation::kHorizontal});
        if (lines.size() >= kPerspectiveAnalysisMaxLines)
            break;
    }
    if (lines.empty())
        return make_error(ErrorCode::kNotFound, "Perspective analysis found no usable lines",
                          {{"reason", "no_perspective_lines"}});
    return lines;
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Perspective line detection allocation failed",
                      {{"reason", "allocation_failed"}});
}

[[nodiscard]] bool analysis_mode_supported(const PerspectiveAnalysisMode mode) noexcept
{
    return mode == PerspectiveAnalysisMode::kVertical ||
           mode == PerspectiveAnalysisMode::kHorizontal || mode == PerspectiveAnalysisMode::kFull;
}

[[nodiscard]] double line_fit_objective(const std::uint32_t width, const std::uint32_t height,
                                        const std::vector<PerspectiveGuideLine> &lines,
                                        const PerspectiveAnalysisMode mode,
                                        const PerspectiveParams &params) noexcept
{
    const Matrix matrix = reference_homography(width, height, params);
    std::vector<std::pair<double, double>> errors;
    errors.reserve(lines.size());
    for (const auto &line : lines)
    {
        const bool selected = mode == PerspectiveAnalysisMode::kFull ||
                              (mode == PerspectiveAnalysisMode::kVertical &&
                               line.orientation == PerspectiveGuideOrientation::kVertical) ||
                              (mode == PerspectiveAnalysisMode::kHorizontal &&
                               line.orientation == PerspectiveGuideOrientation::kHorizontal);
        if (!selected)
            continue;
        auto first = transform(matrix, line.x1, line.y1);
        auto second = transform(matrix, line.x2, line.y2);
        if (!first || !second)
            return std::numeric_limits<double>::max();
        const double dx = second.value().x - first.value().x;
        const double dy = second.value().y - first.value().y;
        const double length = std::hypot(dx, dy);
        if (!std::isfinite(length) || length < 1.0e-9)
            return std::numeric_limits<double>::max();
        const double error = line.orientation == PerspectiveGuideOrientation::kVertical ?
                                 std::atan2(std::abs(dx), std::abs(dy)) :
                                 std::atan2(std::abs(dy), std::abs(dx));
        errors.emplace_back(error * error, line.weight);
    }
    if (errors.empty())
        return std::numeric_limits<double>::max();
    std::sort(errors.begin(), errors.end(), [](const auto &left, const auto &right)
              { return left.first < right.first; });
    const std::size_t retained = errors.size() <= 3U ? errors.size() :
                                                        (errors.size() * 3U + 3U) / 4U;
    double weighted_error = 0.0;
    double weights = 0.0;
    for (std::size_t index = 0U; index < retained; ++index)
    {
        const double weight = std::clamp(errors[index].second, 1.0, 1.0e12);
        weighted_error += errors[index].first * weight;
        weights += weight;
    }
    const double regularization =
        1.0e-9 * (params.rotation_degrees * params.rotation_degrees +
                  params.vertical_shift * params.vertical_shift +
                  params.horizontal_shift * params.horizontal_shift + params.shear * params.shear);
    return weighted_error / std::max(weights, 1.0) + regularization;
}

} // namespace

Result<PerspectiveLayout> compute_perspective_layout(const std::uint32_t width,
                                                     const std::uint32_t height,
                                                     const PerspectiveParams &params)
{
    auto parameter_map = perspective_to_parameters(params);
    if (!parameter_map)
        return parameter_map.error();
    if (width == 0U || height == 0U)
        return make_error(ErrorCode::kValidation, "Perspective dimensions are invalid",
                          {{"reason", "invalid_dimensions"}});
    if (params.is_identity())
    {
        PerspectiveLayout identity;
        identity.forward = identity_matrix();
        identity.inverse = identity_matrix();
        identity.source_quad = {{{0.0, 0.0},
                                 {static_cast<double>(width - 1U), 0.0},
                                 {static_cast<double>(width - 1U), static_cast<double>(height - 1U)},
                                 {0.0, static_cast<double>(height - 1U)}}};
        identity.safe_crop = {0.0, 0.0, static_cast<double>(width - 1U),
                              static_cast<double>(height - 1U)};
        identity.full_width = identity.output_width = width;
        identity.full_height = identity.output_height = height;
        return identity;
    }

    Matrix forward = reference_homography(width, height, params);
    constexpr std::array<std::array<double, 2>, 4> normalized_corners{{
        {0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.0, 1.0}}};
    std::array<PerspectivePoint, 4> quad{};
    double minimum_x = std::numeric_limits<double>::max();
    double minimum_y = std::numeric_limits<double>::max();
    double maximum_x = std::numeric_limits<double>::lowest();
    double maximum_y = std::numeric_limits<double>::lowest();
    for (std::size_t index = 0U; index < quad.size(); ++index)
    {
        auto point = transform(forward,
                               normalized_corners[index][0] * static_cast<double>(width - 1U),
                               normalized_corners[index][1] * static_cast<double>(height - 1U));
        if (!point)
            return point.error();
        quad[index] = point.value();
        minimum_x = std::min(minimum_x, point.value().x);
        minimum_y = std::min(minimum_y, point.value().y);
        maximum_x = std::max(maximum_x, point.value().x);
        maximum_y = std::max(maximum_y, point.value().y);
    }
    const double span_x = maximum_x - minimum_x;
    const double span_y = maximum_y - minimum_y;
    if (!std::isfinite(span_x) || !std::isfinite(span_y) || span_x < 1.0 || span_y < 1.0 ||
        span_x > static_cast<double>(width) * kDimensionGrowthLimit ||
        span_y > static_cast<double>(height) * kDimensionGrowthLimit ||
        span_x * span_y > static_cast<double>(width) * height * kDimensionGrowthLimit)
        return make_error(ErrorCode::kValidation, "Perspective output exceeds its geometry bound",
                          {{"reason", "perspective_growth_limit"}});

    const Matrix translation{1.0, 0.0, -minimum_x, 0.0, 1.0, -minimum_y, 0.0, 0.0, 1.0};
    forward = multiply(translation, forward);
    for (auto &point : quad)
    {
        point.x -= minimum_x;
        point.y -= minimum_y;
    }
    auto inverted = inverse(forward);
    if (!inverted)
        return inverted.error();
    auto safe = maximal_safe_crop(quad);
    if (!safe)
        return safe.error();

    const auto full_width = static_cast<std::uint64_t>(std::ceil(span_x)) + 1U;
    const auto full_height = static_cast<std::uint64_t>(std::ceil(span_y)) + 1U;
    if (full_width > std::numeric_limits<std::uint32_t>::max() ||
        full_height > std::numeric_limits<std::uint32_t>::max())
        return make_error(ErrorCode::kValidation, "Perspective dimensions overflow",
                          {{"reason", "dimensions_overflow"}});

    PerspectiveLayout layout;
    layout.forward = forward;
    layout.inverse = inverted.value();
    layout.source_quad = quad;
    layout.safe_crop = safe.value();
    layout.full_width = static_cast<std::uint32_t>(full_width);
    layout.full_height = static_cast<std::uint32_t>(full_height);
    if (params.constrain_crop)
    {
        const double left = std::ceil(std::max(0.0, safe.value().left));
        const double top = std::ceil(std::max(0.0, safe.value().top));
        const double right = std::floor(
            std::min(static_cast<double>(layout.full_width - 1U), safe.value().right));
        const double bottom = std::floor(
            std::min(static_cast<double>(layout.full_height - 1U), safe.value().bottom));
        if (!(right >= left) || !(bottom >= top))
            return make_error(ErrorCode::kValidation, "Perspective safe crop contains no pixels",
                              {{"reason", "empty_perspective_safe_crop"}});
        layout.output_left = static_cast<std::uint32_t>(left);
        layout.output_top = static_cast<std::uint32_t>(top);
        layout.output_width = static_cast<std::uint32_t>(right - left + 1.0);
        layout.output_height = static_cast<std::uint32_t>(bottom - top + 1.0);
    }
    else
    {
        layout.output_width = layout.full_width;
        layout.output_height = layout.full_height;
    }
    return layout;
}

Result<LinearWorkingBuffer> apply_perspective(const LinearWorkingBuffer &input,
                                              const PerspectiveParams &params,
                                              const CancellationToken &cancellation)
try
{
    auto valid = validate_input(input, cancellation);
    if (!valid)
        return valid.error();
    if (params.is_identity())
        return input;
    auto layout = compute_perspective_layout(input.width, input.height, params);
    if (!layout)
        return layout.error();
    const std::uint64_t output_channels = static_cast<std::uint64_t>(layout.value().output_width) *
                                          layout.value().output_height * 3U;
    if (output_channels > std::vector<float>{}.max_size())
        return make_error(ErrorCode::kValidation, "Perspective output dimensions overflow",
                          {{"reason", "dimensions_overflow"}});

    LinearWorkingBuffer output;
    output.width = layout.value().output_width;
    output.height = layout.value().output_height;
    output.color_profile = input.color_profile;
    output.exposure_analysis = input.exposure_analysis;
    output.canonical_roi_scale = input.canonical_roi_scale;
    output.mask_attached_frame.reset();
    output.rgb.assign(static_cast<std::size_t>(output_channels), 0.0F);
    const int lanczos_radius = params.interpolation == kPerspectiveInterpolationLanczos2 ? 2 : 3;
    auto rows = detail::for_each_row(
        output.height, cancellation,
        [&](const std::uint32_t row)
        {
            for (std::uint32_t column = 0U; column < output.width; ++column)
            {
                const double target_x =
                    static_cast<double>(layout.value().output_left + column);
                const double target_y = static_cast<double>(layout.value().output_top + row);
                const auto source = transform(layout.value().inverse, target_x, target_y);
                if (!source || source.value().x < 0.0 || source.value().y < 0.0 ||
                    source.value().x > static_cast<double>(input.width - 1U) ||
                    source.value().y > static_cast<double>(input.height - 1U))
                    continue;
                float *destination =
                    output.rgb.data() +
                    (static_cast<std::size_t>(row) * output.width + column) * 3U;
                if (params.interpolation == kPerspectiveInterpolationBilinear)
                    sample_bilinear(input, source.value().x, source.value().y, destination);
                else
                    sample_lanczos(input, source.value().x, source.value().y, lanczos_radius,
                                   destination);
            }
        });
    if (!rows)
        return rows.error();
    auto active = cancellation.check();
    if (!active)
        return active.error();
    return output;
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Perspective allocation failed",
                      {{"reason", "allocation_failed"}});
}

Result<PerspectiveAnalysis>
fit_perspective_guides(const std::uint32_t width, const std::uint32_t height,
                       const std::vector<PerspectiveGuideLine> &lines,
                       const PerspectiveAnalysisMode mode,
                       const CancellationToken &cancellation)
try
{
    if (!analysis_mode_supported(mode))
        return make_error(ErrorCode::kInvalidArgument, "Perspective analysis mode is unsupported",
                          {{"reason", "unsupported_perspective_analysis_mode"}});
    if (width < 2U || height < 2U || lines.size() > 128U)
        return make_error(ErrorCode::kValidation, "Perspective guide geometry is invalid",
                          {{"reason", "invalid_perspective_guides"}});
    std::uint32_t vertical_count = 0U;
    std::uint32_t horizontal_count = 0U;
    double minimum_vertical_x = std::numeric_limits<double>::max();
    double maximum_vertical_x = std::numeric_limits<double>::lowest();
    double minimum_horizontal_y = std::numeric_limits<double>::max();
    double maximum_horizontal_y = std::numeric_limits<double>::lowest();
    for (const auto &line : lines)
    {
        const bool finite = std::isfinite(line.x1) && std::isfinite(line.y1) &&
                            std::isfinite(line.x2) && std::isfinite(line.y2) &&
                            std::isfinite(line.weight) && line.weight > 0.0;
        const bool bounded = line.x1 >= -2.0 && line.y1 >= -2.0 && line.x2 >= -2.0 &&
                             line.y2 >= -2.0 && line.x1 <= width + 1.0 &&
                             line.x2 <= width + 1.0 && line.y1 <= height + 1.0 &&
                             line.y2 <= height + 1.0;
        if (!finite || !bounded || std::hypot(line.x2 - line.x1, line.y2 - line.y1) < 2.0)
            return make_error(ErrorCode::kValidation, "Perspective guide line is invalid",
                              {{"reason", "invalid_perspective_guides"}});
        if (line.orientation == PerspectiveGuideOrientation::kVertical)
        {
            ++vertical_count;
            const double midpoint = (line.x1 + line.x2) * 0.5;
            minimum_vertical_x = std::min(minimum_vertical_x, midpoint);
            maximum_vertical_x = std::max(maximum_vertical_x, midpoint);
        }
        else if (line.orientation == PerspectiveGuideOrientation::kHorizontal)
        {
            ++horizontal_count;
            const double midpoint = (line.y1 + line.y2) * 0.5;
            minimum_horizontal_y = std::min(minimum_horizontal_y, midpoint);
            maximum_horizontal_y = std::max(maximum_horizontal_y, midpoint);
        }
        else
        {
            return make_error(ErrorCode::kValidation,
                              "Perspective guide orientation is invalid",
                              {{"reason", "invalid_perspective_guides"}});
        }
    }
    const bool needs_vertical = mode != PerspectiveAnalysisMode::kHorizontal;
    const bool needs_horizontal = mode != PerspectiveAnalysisMode::kVertical;
    if ((needs_vertical && vertical_count < 2U) ||
        (needs_horizontal && horizontal_count < 2U))
        return make_error(ErrorCode::kNotFound, "Perspective analysis has too few guide lines",
                          {{"reason", "insufficient_perspective_lines"},
                           {"vertical_count", std::to_string(vertical_count)},
                           {"horizontal_count", std::to_string(horizontal_count)}});
    if ((needs_vertical && maximum_vertical_x - minimum_vertical_x < width * 0.05) ||
        (needs_horizontal && maximum_horizontal_y - minimum_horizontal_y < height * 0.05))
        return make_error(ErrorCode::kValidation, "Perspective guide lines are degenerate",
                          {{"reason", "degenerate_perspective_guides"}});

    PerspectiveParams best;
    best.constrain_crop = true;
    best.interpolation = std::string(kPerspectiveInterpolationLanczos3);
    double best_score = line_fit_objective(width, height, lines, mode, best);
    if (!std::isfinite(best_score))
        return make_error(ErrorCode::kValidation, "Perspective guide fit is degenerate",
                          {{"reason", "degenerate_perspective_guides"}});

    std::vector<int> variables{0};
    if (needs_vertical)
        variables.push_back(1);
    if (needs_horizontal)
        variables.push_back(2);
    if (mode == PerspectiveAnalysisMode::kFull)
        variables.push_back(3);
    std::array<double, 4> steps{3.0, 0.24, 0.24, 0.05};
    const auto adjusted = [](PerspectiveParams params, const int variable, const double delta)
    {
        switch (variable)
        {
        case 0:
            params.rotation_degrees = std::clamp(params.rotation_degrees + delta,
                                                 kPerspectiveRotationMin,
                                                 kPerspectiveRotationMax);
            break;
        case 1:
            params.vertical_shift = std::clamp(params.vertical_shift + delta,
                                               kPerspectiveShiftMin, kPerspectiveShiftMax);
            break;
        case 2:
            params.horizontal_shift = std::clamp(params.horizontal_shift + delta,
                                                 kPerspectiveShiftMin, kPerspectiveShiftMax);
            break;
        default:
            params.shear = std::clamp(params.shear + delta, kPerspectiveShearMin,
                                      kPerspectiveShearMax);
            break;
        }
        return params;
    };
    for (std::size_t level = 0U; level < 13U; ++level)
    {
        auto active = cancellation.check();
        if (!active)
            return active.error();
        for (std::size_t sweep = 0U; sweep < 8U; ++sweep)
        {
            bool improved = false;
            for (const int variable : variables)
            {
                PerspectiveParams selected = best;
                double selected_score = best_score;
                for (const double direction : {-1.0, 1.0})
                {
                    PerspectiveParams candidate =
                        adjusted(best, variable, direction * steps[static_cast<std::size_t>(variable)]);
                    const double score = line_fit_objective(width, height, lines, mode, candidate);
                    if (score + 1.0e-14 < selected_score)
                    {
                        selected = std::move(candidate);
                        selected_score = score;
                    }
                }
                if (selected_score + 1.0e-14 < best_score)
                {
                    best = std::move(selected);
                    best_score = selected_score;
                    improved = true;
                }
            }
            if (!improved)
                break;
        }
        for (double &step : steps)
            step *= 0.5;
    }
    auto layout = compute_perspective_layout(width, height, best);
    if (!layout)
        return make_error(ErrorCode::kValidation, "Perspective guide fit exceeds geometry bounds",
                          {{"reason", "degenerate_perspective_solution"}});

    PerspectiveAnalysis result;
    result.params = std::move(best);
    result.lines = lines;
    result.vertical_line_count = vertical_count;
    result.horizontal_line_count = horizontal_count;
    result.analyzed_width = width;
    result.analyzed_height = height;
    result.residual_degrees =
        std::sqrt(std::max(0.0, best_score)) * 180.0 / std::numbers::pi;
    return result;
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Perspective guide fit allocation failed",
                      {{"reason", "allocation_failed"}});
}

Result<PerspectiveAnalysis>
analyze_perspective_raster(const RasterBuffer &raster, const PerspectiveAnalysisMode mode,
                           const CancellationToken &cancellation)
try
{
    std::uint32_t analysis_width = 0U;
    std::uint32_t analysis_height = 0U;
    auto lines = detect_perspective_lines(raster, analysis_width, analysis_height, cancellation);
    if (!lines)
        return lines.error();
    auto fitted = fit_perspective_guides(raster.width, raster.height, lines.value(), mode,
                                         cancellation);
    if (!fitted)
        return fitted.error();
    fitted.value().analyzed_width = analysis_width;
    fitted.value().analyzed_height = analysis_height;
    return fitted;
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Perspective analysis allocation failed",
                      {{"reason", "allocation_failed"}});
}

} // namespace ravo
