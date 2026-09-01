#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ravo/engine/engine.h"
#include "ravo/recipe/color_input.h"
#include "ravo/recipe/color_output.h"
#include "ravo/recipe/recipe.h"
#include "ravo/recipe/sharpen.h"
#include "ravo/recipe/texture.h"

#include "image_ops.h"
#include "sharpen.h"
#include "texture.h"

namespace ravo
{
namespace
{

// This test-only prototype independently models the two bounded CPU candidates
// named by ADR-0096. It is deliberately not linked into Engine production code:
// the measured decision precedes product schema and shared-primitive ownership.
struct Plane
{
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::vector<float> samples;
};

struct CandidateResult
{
    Plane output;
    std::size_t scratch_bytes = 0U;
};

[[nodiscard]] std::size_t pixel_count(const Plane &plane)
{
    return static_cast<std::size_t>(plane.width) * plane.height;
}

[[nodiscard]] Plane make_plane(const std::uint32_t width, const std::uint32_t height,
                               const float value = 0.0F)
{
    return {width, height, std::vector<float>(static_cast<std::size_t>(width) * height, value)};
}

[[nodiscard]] std::size_t offset(const Plane &plane, const std::uint32_t x, const std::uint32_t y)
{
    return static_cast<std::size_t>(y) * plane.width + x;
}

[[nodiscard]] Plane box_blur(const Plane &input, const int radius)
{
    if (input.width == 0U || input.height == 0U || input.samples.size() != pixel_count(input) ||
        radius < 0)
    {
        throw std::invalid_argument("invalid research box blur input");
    }
    if (radius == 0)
    {
        return input;
    }
    Plane horizontal = make_plane(input.width, input.height);
    Plane output = make_plane(input.width, input.height);
    const double window = static_cast<double>(radius * 2 + 1);
    for (std::uint32_t y = 0U; y < input.height; ++y)
    {
        double sum = 0.0;
        for (int dx = -radius; dx <= radius; ++dx)
        {
            const auto x =
                static_cast<std::uint32_t>(std::clamp(dx, 0, static_cast<int>(input.width) - 1));
            sum += input.samples[offset(input, x, y)];
        }
        for (std::uint32_t x = 0U; x < input.width; ++x)
        {
            horizontal.samples[offset(horizontal, x, y)] = static_cast<float>(sum / window);
            const auto drop = static_cast<std::uint32_t>(
                std::clamp(static_cast<int>(x) - radius, 0, static_cast<int>(input.width) - 1));
            const auto add = static_cast<std::uint32_t>(
                std::clamp(static_cast<int>(x) + radius + 1, 0, static_cast<int>(input.width) - 1));
            sum += input.samples[offset(input, add, y)] - input.samples[offset(input, drop, y)];
        }
    }
    for (std::uint32_t x = 0U; x < input.width; ++x)
    {
        double sum = 0.0;
        for (int dy = -radius; dy <= radius; ++dy)
        {
            const auto y =
                static_cast<std::uint32_t>(std::clamp(dy, 0, static_cast<int>(input.height) - 1));
            sum += horizontal.samples[offset(horizontal, x, y)];
        }
        for (std::uint32_t y = 0U; y < input.height; ++y)
        {
            output.samples[offset(output, x, y)] = static_cast<float>(sum / window);
            const auto drop = static_cast<std::uint32_t>(
                std::clamp(static_cast<int>(y) - radius, 0, static_cast<int>(input.height) - 1));
            const auto add = static_cast<std::uint32_t>(std::clamp(
                static_cast<int>(y) + radius + 1, 0, static_cast<int>(input.height) - 1));
            sum += horizontal.samples[offset(horizontal, x, add)] -
                   horizontal.samples[offset(horizontal, x, drop)];
        }
    }
    return output;
}

[[nodiscard]] Plane self_guided_filter(const Plane &input, const int radius, const float epsilon)
{
    Plane mean = box_blur(input, radius);
    Plane squared = make_plane(input.width, input.height);
    for (std::size_t index = 0U; index < squared.samples.size(); ++index)
    {
        squared.samples[index] = input.samples[index] * input.samples[index];
    }
    Plane correlation = box_blur(squared, radius);
    Plane a = make_plane(input.width, input.height);
    Plane b = make_plane(input.width, input.height);
    for (std::size_t index = 0U; index < a.samples.size(); ++index)
    {
        const float variance =
            std::max(0.0F, correlation.samples[index] - mean.samples[index] * mean.samples[index]);
        a.samples[index] = variance / (variance + epsilon);
        b.samples[index] = mean.samples[index] * (1.0F - a.samples[index]);
    }
    Plane mean_a = box_blur(a, radius);
    Plane mean_b = box_blur(b, radius);
    Plane output = make_plane(input.width, input.height);
    for (std::size_t index = 0U; index < output.samples.size(); ++index)
    {
        output.samples[index] =
            mean_a.samples[index] * input.samples[index] + mean_b.samples[index];
    }
    return output;
}

[[nodiscard]] CandidateResult texture_boost(const Plane &input, const float strength,
                                            const float detail_threshold)
{
    const float full_radius = detail_threshold * 3.5F;
    const int radius = std::max(static_cast<int>(full_radius + 0.5F), 1);
    constexpr float epsilon = 0.001F;
    const float shaped = strength >= 0.0F ? std::pow(strength / 2.0F, 0.3F) * 2.0F : strength;
    const float fine_gain = shaped >= 0.0F ? 1.0F + shaped : 1.0F / (1.0F - shaped);
    const float coarse_gain = shaped >= 0.0F ? 1.0F + shaped / 4.0F : 1.0F / (1.0F - shaped / 2.0F);

    Plane bounded = input;
    float minimum = std::numeric_limits<float>::infinity();
    for (float &sample : bounded.samples)
    {
        minimum = std::min(minimum, sample);
        sample = std::clamp(sample, 1.0e-5F, 32.0F);
    }
    Plane middle = self_guided_filter(bounded, radius, epsilon);
    Plane base = self_guided_filter(middle, radius * 4, epsilon / 10.0F);
    Plane output = make_plane(input.width, input.height);
    for (std::size_t index = 0U; index < output.samples.size(); ++index)
    {
        const float fine = (input.samples[index] - middle.samples[index]) * fine_gain;
        const float coarse = (middle.samples[index] - base.samples[index]) * coarse_gain;
        output.samples[index] = std::max(base.samples[index] + fine + coarse, minimum);
    }
    // The conservative peak includes the input-normalization, two guided-filter
    // coefficient sets, both filtered scales, and publication plane.
    return {std::move(output), pixel_count(input) * sizeof(float) * 10U};
}

[[nodiscard]] Plane gaussian_reduce(const Plane &input)
{
    constexpr std::array<float, 5> weights{1.0F, 4.0F, 6.0F, 4.0F, 1.0F};
    const std::uint32_t width = (input.width + 1U) / 2U;
    const std::uint32_t height = (input.height + 1U) / 2U;
    Plane horizontal = make_plane(width, input.height);
    for (std::uint32_t y = 0U; y < input.height; ++y)
    {
        for (std::uint32_t x = 0U; x < width; ++x)
        {
            float value = 0.0F;
            for (int tap = -2; tap <= 2; ++tap)
            {
                const auto source_x = static_cast<std::uint32_t>(std::clamp(
                    static_cast<int>(x * 2U) + tap, 0, static_cast<int>(input.width) - 1));
                value += weights[static_cast<std::size_t>(tap + 2)] *
                         input.samples[offset(input, source_x, y)];
            }
            horizontal.samples[offset(horizontal, x, y)] = value / 16.0F;
        }
    }
    Plane output = make_plane(width, height);
    for (std::uint32_t y = 0U; y < height; ++y)
    {
        for (std::uint32_t x = 0U; x < width; ++x)
        {
            float value = 0.0F;
            for (int tap = -2; tap <= 2; ++tap)
            {
                const auto source_y = static_cast<std::uint32_t>(std::clamp(
                    static_cast<int>(y * 2U) + tap, 0, static_cast<int>(input.height) - 1));
                value += weights[static_cast<std::size_t>(tap + 2)] *
                         horizontal.samples[offset(horizontal, x, source_y)];
            }
            output.samples[offset(output, x, y)] = value / 16.0F;
        }
    }
    return output;
}

[[nodiscard]] Plane expand_bilinear(const Plane &input, const std::uint32_t width,
                                    const std::uint32_t height)
{
    Plane output = make_plane(width, height);
    for (std::uint32_t y = 0U; y < height; ++y)
    {
        const float source_y = static_cast<float>(y) * 0.5F;
        const auto y0 = std::min(static_cast<std::uint32_t>(source_y), input.height - 1U);
        const auto y1 = std::min(y0 + 1U, input.height - 1U);
        const float fy = source_y - static_cast<float>(y0);
        for (std::uint32_t x = 0U; x < width; ++x)
        {
            const float source_x = static_cast<float>(x) * 0.5F;
            const auto x0 = std::min(static_cast<std::uint32_t>(source_x), input.width - 1U);
            const auto x1 = std::min(x0 + 1U, input.width - 1U);
            const float fx = source_x - static_cast<float>(x0);
            const float top = input.samples[offset(input, x0, y0)] * (1.0F - fx) +
                              input.samples[offset(input, x1, y0)] * fx;
            const float bottom = input.samples[offset(input, x0, y1)] * (1.0F - fx) +
                                 input.samples[offset(input, x1, y1)] * fx;
            output.samples[offset(output, x, y)] = top * (1.0F - fy) + bottom * fy;
        }
    }
    return output;
}

[[nodiscard]] std::vector<Plane> gaussian_pyramid(Plane input)
{
    std::vector<Plane> result;
    result.push_back(std::move(input));
    while (result.size() < 12U && std::min(result.back().width, result.back().height) > 2U)
    {
        result.push_back(gaussian_reduce(result.back()));
    }
    return result;
}

[[nodiscard]] float local_laplacian_curve(const float input, const float gamma, const float sigma,
                                          const float clarity)
{
    const float distance = input - gamma;
    float value = input;
    if (distance > 0.0F && distance <= 2.0F * sigma)
    {
        const float t = std::clamp(distance / (2.0F * sigma), 0.0F, 1.0F);
        value = gamma + sigma * 2.0F * (1.0F - t) * t + t * t * (2.0F * sigma);
    }
    else if (distance <= 0.0F && distance >= -2.0F * sigma)
    {
        const float t = std::clamp(-distance / (2.0F * sigma), 0.0F, 1.0F);
        value = gamma - sigma * 2.0F * (1.0F - t) * t + t * t * (-2.0F * sigma);
    }
    value += clarity * distance * std::exp(-(distance * distance) / (2.0F * sigma * sigma / 3.0F));
    return value;
}

[[nodiscard]] std::size_t pyramid_samples(const std::vector<Plane> &pyramid)
{
    return std::accumulate(pyramid.begin(), pyramid.end(), std::size_t{0U},
                           [](const std::size_t total, const Plane &plane)
                           { return total + plane.samples.size(); });
}

[[nodiscard]] CandidateResult local_laplacian(const Plane &input, const float clarity)
{
    constexpr std::size_t gamma_count = 6U;
    constexpr float sigma = 0.2F;
    auto original = gaussian_pyramid(input);
    std::array<std::vector<Plane>, gamma_count> laplacians;
    for (std::size_t gamma_index = 0U; gamma_index < gamma_count; ++gamma_index)
    {
        const float gamma =
            (static_cast<float>(gamma_index) + 0.5F) / static_cast<float>(gamma_count);
        Plane remapped = make_plane(input.width, input.height);
        for (std::size_t index = 0U; index < input.samples.size(); ++index)
        {
            remapped.samples[index] =
                local_laplacian_curve(input.samples[index], gamma, sigma, clarity);
        }
        laplacians[gamma_index] = gaussian_pyramid(std::move(remapped));
        for (std::size_t level = 0U; level + 1U < laplacians[gamma_index].size(); ++level)
        {
            Plane expanded = expand_bilinear(laplacians[gamma_index][level + 1U],
                                             laplacians[gamma_index][level].width,
                                             laplacians[gamma_index][level].height);
            for (std::size_t index = 0U; index < expanded.samples.size(); ++index)
            {
                laplacians[gamma_index][level].samples[index] -= expanded.samples[index];
            }
        }
    }

    Plane reconstructed = original.back();
    for (std::size_t level = original.size() - 1U; level-- > 0U;)
    {
        Plane fine = expand_bilinear(reconstructed, original[level].width, original[level].height);
        for (std::size_t index = 0U; index < fine.samples.size(); ++index)
        {
            const float value = original[level].samples[index];
            std::size_t high = 1U;
            while (high + 1U < gamma_count &&
                   (static_cast<float>(high) + 0.5F) / static_cast<float>(gamma_count) <= value)
            {
                ++high;
            }
            const std::size_t low = high - 1U;
            const float low_gamma =
                (static_cast<float>(low) + 0.5F) / static_cast<float>(gamma_count);
            const float high_gamma =
                (static_cast<float>(high) + 0.5F) / static_cast<float>(gamma_count);
            const float blend =
                std::clamp((value - low_gamma) / (high_gamma - low_gamma), 0.0F, 1.0F);
            fine.samples[index] += laplacians[low][level].samples[index] * (1.0F - blend) +
                                   laplacians[high][level].samples[index] * blend;
        }
        reconstructed = std::move(fine);
    }

    std::size_t samples = pyramid_samples(original);
    for (const auto &pyramid : laplacians)
    {
        samples += pyramid_samples(pyramid);
    }
    samples += pixel_count(input) * 2U;
    return {std::move(reconstructed), samples * sizeof(float)};
}

[[nodiscard]] Plane step_fixture(const std::uint32_t width, const std::uint32_t height)
{
    Plane result = make_plane(width, height);
    for (std::uint32_t y = 0U; y < height; ++y)
    {
        for (std::uint32_t x = 0U; x < width; ++x)
        {
            result.samples[offset(result, x, y)] = x < width / 2U ? 0.2F : 0.8F;
        }
    }
    return result;
}

[[nodiscard]] Plane texture_fixture(const std::uint32_t width, const std::uint32_t height)
{
    Plane result = make_plane(width, height);
    for (std::uint32_t y = 0U; y < height; ++y)
    {
        for (std::uint32_t x = 0U; x < width; ++x)
        {
            const float checker = ((x / 2U + y / 2U) & 1U) == 0U ? -0.012F : 0.012F;
            const float wave = 0.008F * std::sin(static_cast<float>(x) * 0.37F) *
                               std::cos(static_cast<float>(y) * 0.23F);
            result.samples[offset(result, x, y)] = 0.32F + checker + wave;
        }
    }
    return result;
}

[[nodiscard]] double standard_deviation(const Plane &plane)
{
    const double mean = std::accumulate(plane.samples.begin(), plane.samples.end(), 0.0) /
                        static_cast<double>(plane.samples.size());
    double squared = 0.0;
    for (const float value : plane.samples)
    {
        const double delta = static_cast<double>(value) - mean;
        squared += delta * delta;
    }
    return std::sqrt(squared / static_cast<double>(plane.samples.size()));
}

[[nodiscard]] double step_halo(const Plane &plane)
{
    const std::uint32_t quarter = plane.width / 4U;
    double left = 0.0;
    double right = 0.0;
    std::size_t samples = 0U;
    for (std::uint32_t y = 0U; y < plane.height; ++y)
    {
        for (std::uint32_t x = 0U; x < quarter; ++x)
        {
            left += plane.samples[offset(plane, x, y)];
            right += plane.samples[offset(plane, plane.width - 1U - x, y)];
            ++samples;
        }
    }
    left /= static_cast<double>(samples);
    right /= static_cast<double>(samples);
    const double low = std::min(left, right);
    const double high = std::max(left, right);
    double halo = 0.0;
    for (const float value : plane.samples)
    {
        halo = std::max(halo, std::max(low - value, static_cast<double>(value) - high));
    }
    return std::max(0.0, halo);
}

[[nodiscard]] bool all_finite(const Plane &plane)
{
    return std::all_of(plane.samples.begin(), plane.samples.end(),
                       [](const float value) { return std::isfinite(value); });
}

[[nodiscard]] std::uint64_t sample_hash(const Plane &plane)
{
    std::uint64_t result = 1469598103934665603ULL;
    for (const float value : plane.samples)
    {
        const auto quantized = static_cast<std::int64_t>(std::llround(value * 1.0e7F));
        for (std::size_t byte = 0U; byte < sizeof(quantized); ++byte)
        {
            result ^= static_cast<std::uint8_t>(static_cast<std::uint64_t>(quantized) >>
                                                static_cast<unsigned>(byte * 8U));
            result *= 1099511628211ULL;
        }
    }
    return result;
}

[[nodiscard]] Plane luminance_plane(const LinearWorkingBuffer &input)
{
    Plane output = make_plane(input.width, input.height);
    for (std::size_t pixel = 0U; pixel < output.samples.size(); ++pixel)
    {
        output.samples[pixel] = 0.2126F * input.rgb[pixel * 3U] +
                                0.7152F * input.rgb[pixel * 3U + 1U] +
                                0.0722F * input.rgb[pixel * 3U + 2U];
    }
    return output;
}

[[nodiscard]] WorkingImage working_image(const Plane &input)
{
    WorkingImage output;
    output.width = input.width;
    output.height = input.height;
    output.color_profile.kind = ColorProfileKind::kBuiltin;
    output.color_profile.model = ColorModel::kRgb;
    output.color_profile.identifier = std::string(kInputProfileLinearRec709);
    output.exposure_analysis = std::make_shared<ExposureAnalysisContext>();
    output.canonical_roi_scale = CanonicalRoiScale::from_scaled_dimensions(
        input.width, input.height, input.width, input.height);
    output.rgb.reserve(input.samples.size() * 3U);
    for (const float value : input.samples)
    {
        output.rgb.insert(output.rgb.end(), 3U, value);
    }
    return output;
}

[[nodiscard]] Plane accepted_sharpen(const Plane &input)
{
    auto sharpened =
        apply_sharpen(working_image(input), SharpenParams{2.0, 0.75, 0.0}, CancellationToken{});
    if (!sharpened)
    {
        throw std::runtime_error(sharpened.error().message);
    }
    return luminance_plane(sharpened.value());
}

[[nodiscard]] Plane accepted_tone_equalizer(const Plane &input)
{
    Recipe recipe;
    recipe.operations.push_back({"ravo.core.toneequal",
                                 1,
                                 "local-detail-overlap-tone-equalizer",
                                 true,
                                 {{"blacks", ParameterValue{0.0}},
                                  {"shadows", ParameterValue{0.0}},
                                  {"midtones", ParameterValue{0.0}},
                                  {"highlights", ParameterValue{0.25}},
                                  {"whites", ParameterValue{0.5}}},
                                 std::nullopt});
    auto equalized = apply_recipe_ops(working_image(input), recipe, CancellationToken{});
    if (!equalized)
    {
        throw std::runtime_error(equalized.error().message);
    }
    return luminance_plane(equalized.value());
}

[[nodiscard]] double mean(const Plane &plane)
{
    return std::accumulate(plane.samples.begin(), plane.samples.end(), 0.0) /
           static_cast<double>(plane.samples.size());
}

[[nodiscard]] double mean_delta(const Plane &input, const Plane &output)
{
    return mean(output) - mean(input);
}

void declare_input(Recipe &recipe)
{
    recipe.operations.push_back({"ravo.color.input", 1, "local-detail-input", true,
                                 input_color_to_parameters(InputColorParams{}), std::nullopt});
    recipe.operations.push_back({"ravo.color.output", 1, "local-detail-output", true,
                                 output_color_to_parameters(OutputColorParams{}), std::nullopt});
}

[[nodiscard]] std::string fixture_path(const std::string_view name)
{
    return (std::filesystem::path(RAVO_REPOSITORY_ROOT) / "Ravo" / "tests" / "fixtures" / "frozen" / "images" / name)
        .string();
}

template <typename Operation>
[[nodiscard]] std::pair<double, CandidateResult> measure_candidate(const Plane &input,
                                                                   Operation operation)
{
    constexpr std::size_t runs = 3U;
    std::array<double, runs> elapsed_ms{};
    CandidateResult result;
    for (std::size_t run = 0U; run < runs; ++run)
    {
        const auto started = std::chrono::steady_clock::now();
        auto next = operation(input);
        const auto finished = std::chrono::steady_clock::now();
        elapsed_ms[run] = std::chrono::duration<double, std::milli>(finished - started).count();
        result = std::move(next);
    }
    std::sort(elapsed_ms.begin(), elapsed_ms.end());
    return {elapsed_ms[runs / 2U], std::move(result)};
}

TEST(LocalDetailResearchTest, CandidatesIncreaseTextureWithoutBroadStepHalos)
{
    const Plane step = step_fixture(256U, 128U);
    const Plane texture = texture_fixture(256U, 128U);
    const auto texture_step = texture_boost(step, 0.75F, 4.0F);
    const auto laplacian_step = local_laplacian(step, 0.5F);
    const auto texture_detail = texture_boost(texture, 0.75F, 4.0F);
    const auto laplacian_detail = local_laplacian(texture, 0.5F);

    ASSERT_TRUE(all_finite(texture_step.output));
    ASSERT_TRUE(all_finite(laplacian_step.output));
    ASSERT_TRUE(all_finite(texture_detail.output));
    ASSERT_TRUE(all_finite(laplacian_detail.output));
    EXPECT_LT(step_halo(texture_step.output), 0.01);
    EXPECT_LT(step_halo(laplacian_step.output), 0.03);
    EXPECT_GT(standard_deviation(texture_detail.output), standard_deviation(texture) * 1.05);
    EXPECT_GT(standard_deviation(laplacian_detail.output), standard_deviation(texture) * 1.05);
    EXPECT_GT(laplacian_detail.scratch_bytes, texture_detail.scratch_bytes);
    EXPECT_EQ(sample_hash(texture_detail.output),
              sample_hash(texture_boost(texture, 0.75F, 4.0F).output));
    EXPECT_EQ(sample_hash(laplacian_detail.output),
              sample_hash(local_laplacian(texture, 0.5F).output));
}

TEST(LocalDetailResearchTest, SelectedCandidateDoesNotDuplicateAcceptedToneOrSharpenSemantics)
{
    const Plane texture = texture_fixture(256U, 128U);
    const auto selected = texture_boost(texture, 0.75F, 4.0F).output;
    const Plane sharpened = accepted_sharpen(texture);
    const Plane tone_equalized = accepted_tone_equalizer(texture);

    ASSERT_TRUE(all_finite(selected));
    ASSERT_TRUE(all_finite(sharpened));
    ASSERT_TRUE(all_finite(tone_equalized));
    const double selected_gain = standard_deviation(selected) / standard_deviation(texture);
    const double sharpen_gain = standard_deviation(sharpened) / standard_deviation(texture);
    const double tone_gain = standard_deviation(tone_equalized) / standard_deviation(texture);
    EXPECT_GT(selected_gain, 1.25);
    EXPECT_GT(sharpen_gain, 1.01);
    EXPECT_GT(selected_gain, sharpen_gain * 1.10);
    EXPECT_GT(std::abs(mean_delta(texture, tone_equalized)), 0.02);
    EXPECT_LT(std::abs(mean_delta(texture, selected)),
              std::abs(mean_delta(texture, tone_equalized)) * 0.1);
    EXPECT_LT(tone_gain, selected_gain * 0.8);
}

TEST(LocalDetailResearchProbe, MeasuresCommittedRawCorpus)
{
    if (std::getenv("RAVO_LOCAL_DETAIL_RESEARCH") == nullptr)
    {
        GTEST_SKIP() << "set RAVO_LOCAL_DETAIL_RESEARCH=1 to run the Release decision probe";
    }

    auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    double texture_total_ms = 0.0;
    double laplacian_total_ms = 0.0;
    double product_texture_total_ms = 0.0;
    double product_texture_max_ms = 0.0;
    std::size_t texture_peak = 0U;
    std::size_t laplacian_peak = 0U;
    std::uint64_t texture_hash = 0U;
    std::uint64_t laplacian_hash = 0U;
    for (const std::string_view name :
         {std::string_view{"mire1.cr2"}, std::string_view{"mire1-xtrans.raf"}})
    {
        const std::string path = fixture_path(name);
        auto decoded = engine.value().decode_raw_frame(path, CancellationToken{});
        ASSERT_TRUE(decoded) << decoded.error().message;
        Recipe recipe;
        recipe.asset = {std::string(name), path, std::nullopt};
        declare_input(recipe);
        auto working = engine.value().linear_working_from_raw(decoded.value(), recipe, 960U, 640U,
                                                              CancellationToken{});
        ASSERT_TRUE(working) << working.error().message;
        working.value().canonical_roi_scale = CanonicalRoiScale::from_scaled_dimensions(
            working.value().width, working.value().height, working.value().width,
            working.value().height);
        const Plane input = luminance_plane(working.value());
        const auto [texture_ms, texture_result] = measure_candidate(
            input, [](const Plane &plane) { return texture_boost(plane, 0.75F, 4.0F); });
        const auto [laplacian_ms, laplacian_result] = measure_candidate(
            input, [](const Plane &plane) { return local_laplacian(plane, 0.5F); });
        std::array<double, 3> product_elapsed_ms{};
        Result<WorkingImage> product_result = make_error(ErrorCode::kInternal, "not measured");
        for (std::size_t run = 0U; run < product_elapsed_ms.size(); ++run)
        {
            const auto started = std::chrono::steady_clock::now();
            product_result =
                apply_texture(working.value(), TextureParams{0.75, 4.0, 1}, CancellationToken{});
            const auto finished = std::chrono::steady_clock::now();
            ASSERT_TRUE(product_result) << product_result.error().message;
            product_elapsed_ms[run] =
                std::chrono::duration<double, std::milli>(finished - started).count();
        }
        std::sort(product_elapsed_ms.begin(), product_elapsed_ms.end());
        const double product_ms = product_elapsed_ms[product_elapsed_ms.size() / 2U];
        ASSERT_TRUE(all_finite(texture_result.output));
        ASSERT_TRUE(all_finite(laplacian_result.output));
        texture_total_ms += texture_ms;
        laplacian_total_ms += laplacian_ms;
        product_texture_total_ms += product_ms;
        product_texture_max_ms = std::max(product_texture_max_ms, product_ms);
        texture_peak = std::max(texture_peak, texture_result.scratch_bytes);
        laplacian_peak = std::max(laplacian_peak, laplacian_result.scratch_bytes);
        texture_hash ^= sample_hash(texture_result.output);
        laplacian_hash ^= sample_hash(laplacian_result.output);
        std::cerr << "local_detail_fixture=" << name << " pixels=" << input.samples.size()
                  << " texture_boost_median_ms=" << texture_ms
                  << " local_laplacian_median_ms=" << laplacian_ms
                  << " product_texture_median_ms=" << product_ms << '\n';
    }

    const Plane step = step_fixture(960U, 320U);
    const Plane texture = texture_fixture(960U, 320U);
    const auto texture_step = texture_boost(step, 0.75F, 4.0F);
    const auto laplacian_step = local_laplacian(step, 0.5F);
    const auto texture_detail = texture_boost(texture, 0.75F, 4.0F);
    const auto laplacian_detail = local_laplacian(texture, 0.5F);
    const Plane sharpened = accepted_sharpen(texture);
    const Plane tone_equalized = accepted_tone_equalizer(texture);
    std::cerr << "local_detail_summary texture_boost_total_median_ms=" << texture_total_ms
              << " texture_boost_scratch_bytes=" << texture_peak
              << " texture_boost_halo=" << step_halo(texture_step.output)
              << " texture_boost_texture_gain="
              << standard_deviation(texture_detail.output) / standard_deviation(texture)
              << " texture_boost_hash=" << texture_hash
              << " local_laplacian_total_median_ms=" << laplacian_total_ms
              << " local_laplacian_scratch_bytes=" << laplacian_peak
              << " local_laplacian_halo=" << step_halo(laplacian_step.output)
              << " local_laplacian_texture_gain="
              << standard_deviation(laplacian_detail.output) / standard_deviation(texture)
              << " local_laplacian_hash=" << laplacian_hash << " accepted_sharpen_texture_gain="
              << standard_deviation(sharpened) / standard_deviation(texture)
              << " accepted_sharpen_mean_delta=" << mean_delta(texture, sharpened)
              << " accepted_tone_equalizer_texture_gain="
              << standard_deviation(tone_equalized) / standard_deviation(texture)
              << " accepted_tone_equalizer_mean_delta=" << mean_delta(texture, tone_equalized)
              << " product_texture_total_median_ms=" << product_texture_total_ms
              << " product_texture_max_median_ms=" << product_texture_max_ms << '\n';
    EXPECT_LT(product_texture_max_ms, 30.0)
        << "the selected Texture operation exhausted the complete interactive response budget";
}

} // namespace
} // namespace ravo
