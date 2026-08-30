#include "ravo/engine/noise_calibration.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <new>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

namespace ravo
{
namespace
{

constexpr double kMaximumSignal = 65535.0;
constexpr double kMaximumVariance = kMaximumSignal * kMaximumSignal;
constexpr std::uint64_t kMaximumSampleCount = 1'000'000'000ULL;
constexpr double kMadToSigma = 1.4826;
constexpr double kMadCutoff = 4.5;

[[nodiscard]] TaskError calibration_error(std::string message, std::string reason)
{
    return make_error(ErrorCode::kValidation, std::move(message), {{"reason", std::move(reason)}});
}

[[nodiscard]] Result<void> check_cancelled(const CancellationToken &cancellation,
                                           const std::string_view stage)
{
    auto active = cancellation.check();
    if (active)
        return {};
    auto error = std::move(active).error();
    error.context.insert_or_assign("stage", std::string(stage));
    return error;
}

[[nodiscard]] double median(std::vector<double> values)
{
    const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2U);
    std::nth_element(values.begin(), middle, values.end());
    if ((values.size() & 1U) != 0U)
        return *middle;
    const double upper = *middle;
    const double lower = *std::max_element(values.begin(), middle);
    return (lower + upper) * 0.5;
}

struct Candidate
{
    double gaussian = 0.0;
    double poisson = 0.0;
    double error = std::numeric_limits<double>::infinity();
};

[[nodiscard]] double weighted_error(const std::vector<CameraNoiseSample> &samples,
                                    const std::vector<std::size_t> &indices, const double gaussian,
                                    const double poisson)
{
    long double result = 0.0L;
    for (const auto index : indices)
    {
        const auto &sample = samples[index];
        const long double residual = static_cast<long double>(sample.variance) -
                                     static_cast<long double>(gaussian) -
                                     static_cast<long double>(poisson) * sample.signal_mean;
        result += static_cast<long double>(sample.count) * residual * residual;
    }
    return static_cast<double>(result);
}

[[nodiscard]] Result<Candidate>
weighted_nonnegative_fit(const std::vector<CameraNoiseSample> &samples,
                         const std::vector<std::size_t> &indices)
{
    long double weight_sum = 0.0L;
    long double weighted_x = 0.0L;
    long double weighted_y = 0.0L;
    for (const auto index : indices)
    {
        const auto &sample = samples[index];
        const long double weight = static_cast<long double>(sample.count);
        weight_sum += weight;
        weighted_x += weight * sample.signal_mean;
        weighted_y += weight * sample.variance;
    }
    if (!(weight_sum > 0.0L))
        return calibration_error("Camera noise samples have no positive weight", "empty_weight");

    const long double x_mean = weighted_x / weight_sum;
    const long double y_mean = weighted_y / weight_sum;
    long double xx = 0.0L;
    long double xy = 0.0L;
    long double x2 = 0.0L;
    for (const auto index : indices)
    {
        const auto &sample = samples[index];
        const long double weight = static_cast<long double>(sample.count);
        const long double centered_x = sample.signal_mean - x_mean;
        const long double centered_y = sample.variance - y_mean;
        xx += weight * centered_x * centered_x;
        xy += weight * centered_x * centered_y;
        x2 += weight * sample.signal_mean * sample.signal_mean;
    }
    if (!(xx > 0.0L))
        return calibration_error("Camera noise samples do not identify a signal slope",
                                 "degenerate_signal");

    std::vector<Candidate> candidates;
    candidates.reserve(3U);
    const double free_poisson = static_cast<double>(xy / xx);
    const double free_gaussian = static_cast<double>(y_mean - free_poisson * x_mean);
    if (free_gaussian >= 0.0 && free_poisson >= 0.0)
        candidates.push_back({free_gaussian, free_poisson, 0.0});

    const double zero_poisson_gaussian = static_cast<double>(weighted_y / weight_sum);
    candidates.push_back({std::max(0.0, zero_poisson_gaussian), 0.0, 0.0});
    if (x2 > 0.0L)
    {
        long double weighted_xy = 0.0L;
        for (const auto index : indices)
        {
            const auto &sample = samples[index];
            weighted_xy +=
                static_cast<long double>(sample.count) * sample.signal_mean * sample.variance;
        }
        candidates.push_back({0.0, std::max(0.0, static_cast<double>(weighted_xy / x2)), 0.0});
    }

    for (auto &candidate : candidates)
        candidate.error = weighted_error(samples, indices, candidate.gaussian, candidate.poisson);
    return *std::min_element(candidates.begin(), candidates.end(),
                             [](const Candidate &left, const Candidate &right)
                             { return left.error < right.error; });
}

} // namespace

Result<CameraNoiseFit> fit_camera_noise(const std::span<const CameraNoiseSample> input,
                                        const CancellationToken &cancellation)
{
    auto active = check_cancelled(cancellation, "validation");
    if (!active)
        return active.error();
    if (input.size() < kCameraNoiseMinimumSamples || input.size() > kCameraNoiseMaximumSamples)
        return calibration_error("Camera noise sample count is outside the supported range",
                                 "sample_count_out_of_range");

    try
    {
        std::vector<CameraNoiseSample> samples(input.begin(), input.end());
        for (std::size_t index = 0U; index < samples.size(); ++index)
        {
            const auto &sample = samples[index];
            if (!std::isfinite(sample.signal_mean) || sample.signal_mean < 0.0 ||
                sample.signal_mean > kMaximumSignal || !std::isfinite(sample.variance) ||
                sample.variance <= 0.0 || sample.variance > kMaximumVariance ||
                sample.count == 0U || sample.count > kMaximumSampleCount)
                return make_error(ErrorCode::kValidation, "Camera noise sample is invalid",
                                  {{"index", std::to_string(index)}, {"reason", "invalid_sample"}});
        }
        std::stable_sort(samples.begin(), samples.end(),
                         [](const CameraNoiseSample &left, const CameraNoiseSample &right)
                         { return left.signal_mean < right.signal_mean; });
        if (samples.back().signal_mean - samples.front().signal_mean <
            kCameraNoiseMinimumSignalSpan)
            return calibration_error("Camera noise samples do not span enough sensor signal",
                                     "insufficient_signal_span");

        std::vector<double> slopes;
        slopes.reserve(samples.size() * (samples.size() - 1U) / 2U);
        for (std::size_t left = 0U; left + 1U < samples.size(); ++left)
        {
            if ((left & 15U) == 0U)
            {
                active = check_cancelled(cancellation, "theil_sen");
                if (!active)
                    return active.error();
            }
            for (std::size_t right = left + 1U; right < samples.size(); ++right)
            {
                const double delta = samples[right].signal_mean - samples[left].signal_mean;
                if (delta > 0.0)
                    slopes.push_back((samples[right].variance - samples[left].variance) / delta);
            }
        }
        if (slopes.empty())
            return calibration_error("Camera noise samples need distinct signal levels",
                                     "degenerate_signal");

        const double robust_slope = median(std::move(slopes));
        if (!std::isfinite(robust_slope))
            return calibration_error("Camera noise robust slope is not finite", "nonfinite_fit");
        std::vector<double> intercepts;
        intercepts.reserve(samples.size());
        for (const auto &sample : samples)
            intercepts.push_back(sample.variance - robust_slope * sample.signal_mean);
        const double robust_intercept = median(std::move(intercepts));

        std::vector<double> residuals;
        residuals.reserve(samples.size());
        for (const auto &sample : samples)
            residuals.push_back(sample.variance -
                                (robust_intercept + robust_slope * sample.signal_mean));
        const double residual_center = median(residuals);
        std::vector<double> absolute_deviations;
        absolute_deviations.reserve(residuals.size());
        double residual_scale = 1.0;
        for (const auto residual : residuals)
        {
            absolute_deviations.push_back(std::abs(residual - residual_center));
            residual_scale = std::max(residual_scale, std::abs(residual));
        }
        const double sigma = kMadToSigma * median(std::move(absolute_deviations));
        const double threshold = std::max(
            kMadCutoff * sigma, residual_scale * 32.0 * std::numeric_limits<double>::epsilon());

        std::vector<std::size_t> retained;
        retained.reserve(samples.size());
        for (std::size_t index = 0U; index < residuals.size(); ++index)
            if (std::abs(residuals[index] - residual_center) <= threshold)
                retained.push_back(index);
        if (retained.size() < kCameraNoiseMinimumSamples || retained.size() * 2U < samples.size())
            return calibration_error("Too few camera noise samples remain after outlier rejection",
                                     "insufficient_inliers");

        active = check_cancelled(cancellation, "weighted_fit");
        if (!active)
            return active.error();
        auto fitted = weighted_nonnegative_fit(samples, retained);
        if (!fitted)
            return fitted.error();
        const auto candidate = fitted.value();
        if (!std::isfinite(candidate.gaussian) || !std::isfinite(candidate.poisson) ||
            candidate.gaussian > kMaximumVariance || candidate.poisson > kMaximumSignal)
            return calibration_error("Camera noise coefficients exceed the supported model bounds",
                                     "fit_out_of_range");

        long double weight_sum = 0.0L;
        long double weighted_y = 0.0L;
        for (const auto index : retained)
        {
            weight_sum += static_cast<long double>(samples[index].count);
            weighted_y += static_cast<long double>(samples[index].count) * samples[index].variance;
        }
        const long double y_mean = weighted_y / weight_sum;
        long double total_error = 0.0L;
        for (const auto index : retained)
        {
            const long double delta = samples[index].variance - y_mean;
            total_error += static_cast<long double>(samples[index].count) * delta * delta;
        }
        const double rmse = std::sqrt(candidate.error / static_cast<double>(weight_sum));
        const double r_squared =
            total_error > 0.0L ? 1.0 - candidate.error / static_cast<double>(total_error) : 1.0;
        if (!std::isfinite(rmse) || !std::isfinite(r_squared))
            return calibration_error("Camera noise fit statistics are not finite", "nonfinite_fit");

        active = check_cancelled(cancellation, "publication");
        if (!active)
            return active.error();
        return CameraNoiseFit{candidate.gaussian, candidate.poisson, rmse,
                              r_squared,          samples.size(),    retained.size()};
    }
    catch (const std::bad_alloc &)
    {
        return make_error(ErrorCode::kIo, "Unable to allocate camera noise calibration scratch",
                          {{"reason", "allocation_failed"}});
    }
}

} // namespace ravo
