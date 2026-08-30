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

namespace ravo
{
namespace
{

// This is a test-only, Ravo-owned decision prototype for the physical stages
// reviewed in Filmulator commit 57fbaec57555432d86d3aa632990cd8fa09114ad.
// It deliberately has no Recipe, Engine, service, CLI, or Studio entry point.
struct FilmResearchParams
{
    float initial_developer_concentration = 1.0F;
    float reservoir_thickness = 1000.0F;
    float active_layer_thickness = 0.1F;
    float crystals_per_pixel = 500.0F;
    float initial_crystal_radius = 0.00001F;
    float initial_silver_salt_density = 1.0F;
    float developer_consumption = 2000000.0F;
    float crystal_growth = 0.00001F;
    float silver_salt_consumption = 2000000.0F;
    float development_time = 100.0F;
    int agitation_count = 1;
    int development_steps = 12;
    float film_area = 864.0F;
    float diffusion_sigma = 0.2F;
    float layer_mix = 0.2F;
    float layer_time_divisor = 20.0F;
    float rolloff_boundary = 51275.0F;
    float toe_boundary = 0.0F;
};

struct FilmResearchTiming
{
    double exposure_ms = 0.0;
    double reaction_ms = 0.0;
    double diffusion_ms = 0.0;
    double reservoir_ms = 0.0;
    double agitation_ms = 0.0;
    double density_ms = 0.0;
};

struct FilmResearchResult
{
    std::vector<float> density;
    FilmResearchTiming timing;
    std::size_t additional_peak_bytes = 0U;
};

[[nodiscard]] double elapsed_ms(const std::chrono::steady_clock::time_point started)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
        .count();
}

[[nodiscard]] std::size_t rgb_offset(const std::size_t pixel, const std::size_t channel)
{
    return pixel * 3U + channel;
}

[[nodiscard]] int reflected_index(int index, const int size)
{
    if (size <= 1)
    {
        return 0;
    }
    while (index < 0 || index >= size)
    {
        index = index < 0 ? -index : 2 * size - 2 - index;
    }
    return index;
}

void repeated_box_diffusion(std::vector<float> &plane, const std::uint32_t width,
                            const std::uint32_t height, const int radius,
                            std::vector<float> &horizontal, std::vector<float> &vertical)
{
    if (radius <= 0)
    {
        return;
    }
    constexpr int passes = 7;
    const double divisor = static_cast<double>(radius * 2 + 1);
    for (int pass = 0; pass < passes; ++pass)
    {
        for (std::uint32_t y = 0U; y < height; ++y)
        {
            double sum = 0.0;
            for (int dx = -radius; dx <= radius; ++dx)
            {
                const auto x =
                    static_cast<std::uint32_t>(reflected_index(dx, static_cast<int>(width)));
                sum += plane[static_cast<std::size_t>(y) * width + x];
            }
            for (std::uint32_t x = 0U; x < width; ++x)
            {
                horizontal[static_cast<std::size_t>(y) * width + x] =
                    static_cast<float>(sum / divisor);
                const auto drop = static_cast<std::uint32_t>(
                    reflected_index(static_cast<int>(x) - radius, static_cast<int>(width)));
                const auto add = static_cast<std::uint32_t>(
                    reflected_index(static_cast<int>(x) + radius + 1, static_cast<int>(width)));
                sum += plane[static_cast<std::size_t>(y) * width + add] -
                       plane[static_cast<std::size_t>(y) * width + drop];
            }
        }

        for (std::uint32_t x = 0U; x < width; ++x)
        {
            double sum = 0.0;
            for (int dy = -radius; dy <= radius; ++dy)
            {
                const auto y =
                    static_cast<std::uint32_t>(reflected_index(dy, static_cast<int>(height)));
                sum += horizontal[static_cast<std::size_t>(y) * width + x];
            }
            for (std::uint32_t y = 0U; y < height; ++y)
            {
                vertical[static_cast<std::size_t>(y) * width + x] =
                    static_cast<float>(sum / divisor);
                const auto drop = static_cast<std::uint32_t>(
                    reflected_index(static_cast<int>(y) - radius, static_cast<int>(height)));
                const auto add = static_cast<std::uint32_t>(
                    reflected_index(static_cast<int>(y) + radius + 1, static_cast<int>(height)));
                sum += horizontal[static_cast<std::size_t>(add) * width + x] -
                       horizontal[static_cast<std::size_t>(drop) * width + x];
            }
        }
        plane.swap(vertical);
    }
}

[[nodiscard]] float activate_crystals(const float code_value, const FilmResearchParams &params)
{
    const float toe = std::clamp(params.toe_boundary, 0.0F, params.rolloff_boundary / 2.0F);
    const float rolloff = std::clamp(params.rolloff_boundary - toe, 1.0F, 65535.0F);
    const float headroom = std::max(65535.0F - toe - rolloff, 1.0F / 65535.0F);
    const float input = std::max(code_value, 0.0F);
    float output = input - toe + toe * toe / (input + toe + 1.0F / 65535.0F);
    if (output > rolloff)
    {
        output = 65535.0F - headroom * headroom / (output + headroom - rolloff);
    }
    return std::max(output, 0.0F) * params.crystals_per_pixel * 0.00015387105F;
}

[[nodiscard]] FilmResearchResult
film_development_prototype(const std::vector<float> &input_code_rgb, const std::uint32_t width,
                           const std::uint32_t height, const FilmResearchParams &params = {})
{
    const std::size_t pixels = static_cast<std::size_t>(width) * height;
    if (width == 0U || height == 0U || input_code_rgb.size() != pixels * 3U ||
        params.development_steps < 1 || params.film_area <= 0.0F ||
        params.active_layer_thickness <= 0.0F || params.reservoir_thickness <= 0.0F ||
        params.layer_time_divisor <= 0.0F)
    {
        throw std::invalid_argument("invalid film-development research input");
    }

    FilmResearchResult result;
    const auto exposure_started = std::chrono::steady_clock::now();
    std::vector<float> active(input_code_rgb.size());
    for (std::size_t index = 0U; index < active.size(); ++index)
    {
        active[index] = activate_crystals(input_code_rgb[index], params);
    }
    result.timing.exposure_ms = elapsed_ms(exposure_started);

    std::vector<float> radius(input_code_rgb.size(), params.initial_crystal_radius);
    std::vector<float> silver(input_code_rgb.size(), params.initial_silver_salt_density);
    std::vector<float> developer(pixels, params.initial_developer_concentration);
    std::vector<float> horizontal(pixels);
    std::vector<float> vertical(pixels);
    result.additional_peak_bytes = (active.size() + radius.size() + silver.size() +
                                    developer.size() + horizontal.size() + vertical.size()) *
                                   sizeof(float);

    const float reservoir_thickness = params.reservoir_thickness * params.film_area / 864.0F;
    float reservoir_concentration = params.initial_developer_concentration;
    const float pixels_per_millimeter = std::sqrt(static_cast<float>(pixels) / params.film_area);
    const float timestep = params.development_time / static_cast<float>(params.development_steps);
    const int agitation_period =
        params.agitation_count > 0 ?
            std::max(params.development_steps / params.agitation_count, 1) :
            params.development_steps * 3;
    const int half_agitation_period = agitation_period / 2;
    const float reaction_growth = params.crystal_growth * timestep;
    const float developer_consumption =
        2.0F * params.developer_consumption / (params.active_layer_thickness * 3.0F);
    const float silver_consumption = params.silver_salt_consumption * 2.0F;
    const float sigma =
        std::sqrt(timestep * std::pow(params.diffusion_sigma * pixels_per_millimeter, 2.0F));
    int convolution_length =
        static_cast<int>(std::floor(std::sqrt(sigma * sigma * (12.0F / 7.0F) + 1.0F)));
    if (convolution_length % 2 == 0)
    {
        ++convolution_length;
    }
    const int diffusion_radius = std::max((convolution_length - 1) / 2, 0);

    for (int step = 0; step <= params.development_steps; ++step)
    {
        const auto reaction_started = std::chrono::steady_clock::now();
        for (std::size_t pixel = 0U; pixel < pixels; ++pixel)
        {
            float developer_delta = 0.0F;
            for (std::size_t channel = 0U; channel < 3U; ++channel)
            {
                const std::size_t index = rgb_offset(pixel, channel);
                const float radius_delta = developer[pixel] * silver[index] * reaction_growth;
                const float volume_delta =
                    radius_delta * radius[index] * radius[index] * active[index];
                radius[index] += radius_delta;
                silver[index] -= silver_consumption * volume_delta;
                developer_delta += volume_delta;
            }
            developer[pixel] =
                std::max(developer[pixel] - developer_consumption * developer_delta, 0.0F);
        }
        result.timing.reaction_ms += elapsed_ms(reaction_started);

        const auto diffusion_started = std::chrono::steady_clock::now();
        repeated_box_diffusion(developer, width, height, diffusion_radius, horizontal, vertical);
        result.timing.diffusion_ms += elapsed_ms(diffusion_started);

        const auto reservoir_started = std::chrono::steady_clock::now();
        const float retained = std::pow(params.layer_mix, timestep / params.layer_time_divisor);
        const float reservoir_portion = (1.0F - retained) * reservoir_concentration;
        double transferred = 0.0;
        for (float &sample : developer)
        {
            const float mixed = sample * retained + reservoir_portion;
            transferred += static_cast<double>(mixed - sample);
            sample = mixed;
        }
        reservoir_concentration -=
            static_cast<float>(transferred) * params.active_layer_thickness /
            (pixels_per_millimeter * pixels_per_millimeter * reservoir_thickness);
        result.timing.reservoir_ms += elapsed_ms(reservoir_started);

        const auto agitation_started = std::chrono::steady_clock::now();
        if ((step + half_agitation_period) % agitation_period == 0)
        {
            const double layer_total = std::accumulate(developer.begin(), developer.end(), 0.0) *
                                       params.active_layer_thickness /
                                       (pixels_per_millimeter * pixels_per_millimeter);
            const double total =
                layer_total + static_cast<double>(reservoir_concentration) * reservoir_thickness;
            const double contact_volume = static_cast<double>(pixels) *
                                          params.active_layer_thickness /
                                          (pixels_per_millimeter * pixels_per_millimeter);
            reservoir_concentration =
                static_cast<float>(total / (reservoir_thickness + contact_volume));
            std::fill(developer.begin(), developer.end(), reservoir_concentration);
        }
        result.timing.agitation_ms += elapsed_ms(agitation_started);
    }

    const auto density_started = std::chrono::steady_clock::now();
    for (std::size_t index = 0U; index < radius.size(); ++index)
    {
        radius[index] = radius[index] * radius[index] * active[index];
    }
    result.timing.density_ms = elapsed_ms(density_started);
    result.density = std::move(radius);
    return result;
}

[[nodiscard]] std::vector<float>
context_fixture(const std::uint32_t width, const std::uint32_t height, const bool bright_surround)
{
    const float surround = bright_surround ? 0.7F : 0.15F;
    std::vector<float> result(static_cast<std::size_t>(width) * height * 3U, surround * 65535.0F);
    const std::uint32_t cx = width / 2U;
    const std::uint32_t cy = height / 2U;
    for (std::uint32_t y = cy - 2U; y <= cy + 2U; ++y)
    {
        for (std::uint32_t x = cx - 2U; x <= cx + 2U; ++x)
        {
            const std::size_t pixel = static_cast<std::size_t>(y) * width + x;
            for (std::size_t channel = 0U; channel < 3U; ++channel)
            {
                result[rgb_offset(pixel, channel)] = 0.7F * 65535.0F;
            }
        }
    }
    return result;
}

[[nodiscard]] bool all_finite_nonnegative(const std::vector<float> &values)
{
    return std::all_of(values.begin(), values.end(),
                       [](const float value) { return std::isfinite(value) && value >= 0.0F; });
}

[[nodiscard]] std::uint64_t sample_hash(const std::vector<float> &values)
{
    std::uint64_t result = 1469598103934665603ULL;
    for (const float value : values)
    {
        const auto quantized = static_cast<std::int64_t>(std::llround(value * 1.0e5F));
        for (std::size_t byte = 0U; byte < sizeof(quantized); ++byte)
        {
            result ^= static_cast<std::uint8_t>(static_cast<std::uint64_t>(quantized) >>
                                                static_cast<unsigned>(byte * 8U));
            result *= 1099511628211ULL;
        }
    }
    return result;
}

[[nodiscard]] double normalized_mean_absolute_delta(const std::vector<float> &left,
                                                    const std::vector<float> &right)
{
    if (left.size() != right.size() || left.empty())
    {
        throw std::invalid_argument("incompatible film-development research outputs");
    }
    double difference = 0.0;
    double reference = 0.0;
    for (std::size_t index = 0U; index < left.size(); ++index)
    {
        difference += std::abs(static_cast<double>(left[index]) - right[index]);
        reference += std::abs(static_cast<double>(left[index]));
    }
    return difference / std::max(reference, std::numeric_limits<double>::epsilon());
}

[[nodiscard]] double total_ms(const FilmResearchTiming &timing)
{
    return timing.exposure_ms + timing.reaction_ms + timing.diffusion_ms + timing.reservoir_ms +
           timing.agitation_ms + timing.density_ms;
}

[[nodiscard]] double median(std::array<double, 3U> values)
{
    std::sort(values.begin(), values.end());
    return values[1U];
}

[[nodiscard]] std::string fixture_path(const std::string_view name)
{
    return (std::filesystem::path(RAVO_REPOSITORY_ROOT) / "legacy" / "tests" / "images" / name)
        .string();
}

void declare_input(Recipe &recipe)
{
    recipe.operations.push_back({"ravo.color.input", 1, "film-research-input", true,
                                 input_color_to_parameters(InputColorParams{}), std::nullopt});
    recipe.operations.push_back({"ravo.color.output", 1, "film-research-output", true,
                                 output_color_to_parameters(OutputColorParams{}), std::nullopt});
}

[[nodiscard]] std::vector<float> code_values(const LinearWorkingBuffer &working)
{
    std::vector<float> output(working.rgb.size());
    std::transform(working.rgb.begin(), working.rgb.end(), output.begin(),
                   [](const float value) { return std::max(value, 0.0F) * 65535.0F; });
    return output;
}

TEST(FilmDevelopmentResearchTest, PhysicalStagesAreDeterministicAndSpatiallyDistinct)
{
    constexpr std::uint32_t width = 128U;
    constexpr std::uint32_t height = 96U;
    const auto dark_context = context_fixture(width, height, false);
    const auto bright_context = context_fixture(width, height, true);
    const auto dark_copy = dark_context;
    const auto dark_result = film_development_prototype(dark_context, width, height);
    const auto repeated = film_development_prototype(dark_context, width, height);
    const auto bright_result = film_development_prototype(bright_context, width, height);

    ASSERT_TRUE(all_finite_nonnegative(dark_result.density));
    ASSERT_TRUE(all_finite_nonnegative(bright_result.density));
    EXPECT_EQ(dark_context, dark_copy);
    EXPECT_EQ(sample_hash(dark_result.density), sample_hash(repeated.density));
    const std::size_t center =
        rgb_offset(static_cast<std::size_t>(height / 2U) * width + width / 2U, 1U);
    EXPECT_GT(std::abs(dark_result.density[center] - bright_result.density[center]), 1.0e-5F)
        << "equal input values must respond to different developer-depletion neighborhoods";

    FilmResearchParams no_agitation;
    no_agitation.agitation_count = 0;
    const auto unagitated = film_development_prototype(dark_context, width, height, no_agitation);
    EXPECT_NE(sample_hash(dark_result.density), sample_hash(unagitated.density));
    EXPECT_GT(normalized_mean_absolute_delta(dark_result.density, unagitated.density), 1.0e-4);
    EXPECT_EQ(dark_result.additional_peak_bytes,
              static_cast<std::size_t>(width) * height * 12U * sizeof(float));
}

TEST(FilmDevelopmentResearchProbe, MeasuresCommittedRawCorpus)
{
    if (std::getenv("RAVO_FILM_DEVELOPMENT_RESEARCH") == nullptr)
    {
        GTEST_SKIP() << "set RAVO_FILM_DEVELOPMENT_RESEARCH=1 to run the Release decision probe";
    }

    auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    double corpus_total_ms = 0.0;
    double corpus_max_ms = 0.0;
    std::size_t peak_bytes = 0U;
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
        const auto input = code_values(working.value());
        const auto input_hash = sample_hash(input);
        std::array<double, 3U> totals{};
        std::array<double, 3U> reactions{};
        std::array<double, 3U> diffusions{};
        std::array<double, 3U> reservoirs{};
        FilmResearchResult measured;
        for (std::size_t run = 0U; run < totals.size(); ++run)
        {
            measured =
                film_development_prototype(input, working.value().width, working.value().height);
            totals[run] = total_ms(measured.timing);
            reactions[run] = measured.timing.reaction_ms;
            diffusions[run] = measured.timing.diffusion_ms;
            reservoirs[run] = measured.timing.reservoir_ms;
        }
        ASSERT_TRUE(all_finite_nonnegative(measured.density));
        EXPECT_EQ(sample_hash(input), input_hash);
        const double fixture_total = median(totals);
        corpus_total_ms += fixture_total;
        corpus_max_ms = std::max(corpus_max_ms, fixture_total);
        peak_bytes = std::max(peak_bytes, measured.additional_peak_bytes);
        std::cerr << "film_development_fixture=" << name << " pixels="
                  << static_cast<std::size_t>(working.value().width) * working.value().height
                  << " total_median_ms=" << fixture_total
                  << " reaction_median_ms=" << median(reactions)
                  << " diffusion_median_ms=" << median(diffusions)
                  << " reservoir_median_ms=" << median(reservoirs)
                  << " additional_peak_bytes=" << measured.additional_peak_bytes
                  << " output_hash=" << sample_hash(measured.density) << '\n';
    }
    constexpr std::uint32_t context_width = 128U;
    constexpr std::uint32_t context_height = 96U;
    const auto dark_context = context_fixture(context_width, context_height, false);
    const auto bright_context = context_fixture(context_width, context_height, true);
    const auto dark_result =
        film_development_prototype(dark_context, context_width, context_height);
    const auto bright_result =
        film_development_prototype(bright_context, context_width, context_height);
    FilmResearchParams no_agitation;
    no_agitation.agitation_count = 0;
    const auto unagitated =
        film_development_prototype(dark_context, context_width, context_height, no_agitation);
    const std::size_t center = rgb_offset(
        static_cast<std::size_t>(context_height / 2U) * context_width + context_width / 2U, 1U);
    std::cerr << "film_development_summary total_median_ms=" << corpus_total_ms
              << " max_median_ms=" << corpus_max_ms << " additional_peak_bytes=" << peak_bytes
              << " equal_input_context_relative_delta="
              << std::abs(static_cast<double>(dark_result.density[center]) -
                          bright_result.density[center]) /
                     std::max(std::abs(static_cast<double>(dark_result.density[center])),
                              std::numeric_limits<double>::epsilon())
              << " agitation_normalized_mean_delta="
              << normalized_mean_absolute_delta(dark_result.density, unagitated.density) << '\n';
}

} // namespace
} // namespace ravo
