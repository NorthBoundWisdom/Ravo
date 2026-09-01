#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cfenv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <memory>
#include <numeric>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <png.h>

#include <QBuffer>
#include <QColor>
#include <QFile>
#include <QImage>
#include <zlib.h>

#include "ravo/domain/types.h"
#include "ravo/engine/engine.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/operation.h"

#include "color_balance_fixture.h"
#include "capability_ops.h"
#include "capture_metadata_test_support.h"
#include "color_balance_rgb.h"
#include "color_checker.h"
#include "color_harmonizer.h"
#include "color_contrast.h"
#include "d50_lab.h"
#include "dt_ucs.h"
#include "harmony_geometry.h"
#include "image_ops.h"
#include "input_color.h"
#include "primaries.h"
#include "raw_pipeline.h"
#include "raw_temperature.h"
#include "recursive_gaussian.h"
#include "temperature_fixture.h"
#include "test_support.h"

namespace ravo
{
namespace
{

class RecordingProgressSink final : public ProgressSink
{
public:
    void on_progress(const ProgressEvent &event) override
    {
        events.push_back(event);
    }

    std::vector<ProgressEvent> events;
};

[[nodiscard]] std::string mire1_path()
{
    const auto path =
        std::filesystem::path(RAVO_REPOSITORY_ROOT) / "Ravo" / "tests" / "fixtures" / "frozen" / "images" / "mire1.cr2";
    const auto utf8 = path.generic_u8string();
    return {utf8.begin(), utf8.end()};
}

[[nodiscard]] std::string mire1_xtrans_path()
{
    const auto path = std::filesystem::path(RAVO_REPOSITORY_ROOT) / "Ravo" / "tests" / "fixtures" / "frozen" / "images" /
                      "mire1-xtrans.raf";
    const auto utf8 = path.generic_u8string();
    return {utf8.begin(), utf8.end()};
}

struct SourceFileSnapshot
{
    std::uintmax_t size = 0;
    std::filesystem::file_time_type modified;
    std::uint64_t content_hash = 1469598103934665603ULL;

    bool operator==(const SourceFileSnapshot &) const = default;
};

[[nodiscard]] std::optional<SourceFileSnapshot> source_file_snapshot(const std::string &path)
{
    std::error_code error;
    SourceFileSnapshot result;
    result.size = std::filesystem::file_size(path, error);
    if (error)
    {
        return std::nullopt;
    }
    result.modified = std::filesystem::last_write_time(path, error);
    if (error)
    {
        return std::nullopt;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        return std::nullopt;
    }
    std::array<char, 64U * 1024U> block{};
    while (input)
    {
        input.read(block.data(), static_cast<std::streamsize>(block.size()));
        const auto read = input.gcount();
        for (std::streamsize index = 0; index < read; ++index)
        {
            result.content_hash ^=
                static_cast<std::uint8_t>(block[static_cast<std::size_t>(index)]);
            result.content_hash *= 1099511628211ULL;
        }
    }
    if (!input.eof())
    {
        return std::nullopt;
    }
    return result;
}

struct DecodedPng
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<png_byte> pixels;
};

void declare_srgb(RasterBuffer &raster)
{
    raster.color_profile.kind = ColorProfileKind::kBuiltin;
    raster.color_profile.model = ColorModel::kRgb;
    raster.color_profile.identifier = "srgb";
}

void declare_linear_srgb_matrix(DecodedRaw &raw)
{
    raw.color_profile.kind = ColorProfileKind::kMatrix;
    raw.color_profile.model = ColorModel::kRgb;
    raw.color_profile.identifier = "enhanced_matrix";
    raw.color_profile.matrix_to_xyz_d50 = {0.4360747F, 0.3850649F, 0.1430804F,
                                           0.2225045F, 0.7168786F, 0.0606169F,
                                           0.0139322F, 0.0971045F, 0.7141733F};
    raw.color_profile.has_matrix = true;
    raw.color_profile.camera_input = true;
}

void declare_input(Recipe &recipe)
{
    recipe.operations.push_back({"ravo.color.input", 1, "color-input-1", true,
                                 input_color_to_parameters(InputColorParams{}), std::nullopt});
    recipe.operations.push_back({"ravo.color.output", 1, "color-output-1", true,
                                 output_color_to_parameters(OutputColorParams{}), std::nullopt});
}

[[nodiscard]] std::shared_ptr<const ExposureAnalysisContext>
exposure_analysis(const std::initializer_list<std::pair<std::uint16_t, std::uint32_t>> bins,
                  const std::uint32_t black_level, const std::uint32_t white_level,
                  RawExposureMetadata metadata = {})
{
    auto context = std::make_shared<ExposureAnalysisContext>();
    context->raw_histogram.assign(kExposureRawHistogramBins, 0U);
    for (const auto &[bin, count] : bins)
    {
        context->raw_histogram[bin] += count;
        context->raw_pixel_count += count;
    }
    context->raw_black_level = black_level;
    context->raw_white_level = white_level;
    context->metadata = std::move(metadata);
    return context;
}

[[nodiscard]] std::optional<DecodedPng> read_rgb_png(const std::filesystem::path &path)
{
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    if (png_image_begin_read_from_file(&image, path.string().c_str()) == 0)
    {
        return std::nullopt;
    }
    image.format = PNG_FORMAT_RGB;
    DecodedPng result{image.width, image.height, std::vector<png_byte>(PNG_IMAGE_SIZE(image))};
    if (png_image_finish_read(&image, nullptr, result.pixels.data(), 0, nullptr) == 0)
    {
        png_image_free(&image);
        return std::nullopt;
    }
    png_image_free(&image);
    return result;
}

[[nodiscard]] std::optional<DecodedPng> read_rgb_png(const std::vector<std::uint8_t> &encoded)
{
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    if (encoded.empty() ||
        png_image_begin_read_from_memory(&image, encoded.data(), encoded.size()) == 0)
    {
        return std::nullopt;
    }
    image.format = PNG_FORMAT_RGB;
    DecodedPng result{image.width, image.height, std::vector<png_byte>(PNG_IMAGE_SIZE(image))};
    if (png_image_finish_read(&image, nullptr, result.pixels.data(), 0, nullptr) == 0)
    {
        png_image_free(&image);
        return std::nullopt;
    }
    png_image_free(&image);
    return result;
}

[[nodiscard]] std::size_t png_chunk_count(const std::string &png_bytes,
                                          const std::string_view chunk_type)
{
    if (png_bytes.size() < 8U || chunk_type.size() != 4U)
    {
        return 0;
    }
    std::size_t offset = 8U;
    std::size_t count = 0;
    while (png_bytes.size() - offset >= 12U)
    {
        const auto byte = [&png_bytes, offset](const std::size_t index)
        {
            return static_cast<std::uint32_t>(
                static_cast<unsigned char>(png_bytes[offset + index]));
        };
        const auto length = (byte(0U) << 24U) | (byte(1U) << 16U) | (byte(2U) << 8U) | byte(3U);
        const auto remaining = png_bytes.size() - offset;
        if (static_cast<std::size_t>(length) > remaining - 12U)
        {
            return 0;
        }
        if (std::equal(chunk_type.begin(), chunk_type.end(), png_bytes.data() + offset + 4U))
        {
            ++count;
        }
        offset += 12U + static_cast<std::size_t>(length);
    }
    return offset == png_bytes.size() ? count : 0;
}

[[nodiscard]] RasterBuffer solid_raster(const std::uint32_t width, const std::uint32_t height,
                                        const std::uint8_t r, const std::uint8_t g,
                                        const std::uint8_t b)
{
    RasterBuffer raster;
    raster.width = width;
    raster.height = height;
    raster.source_width = width;
    raster.source_height = height;
    declare_srgb(raster);
    raster.srgb.resize(static_cast<std::size_t>(width) * height * 3U);
    for (std::size_t index = 0; index < raster.srgb.size(); index += 3)
    {
        raster.srgb[index] = r;
        raster.srgb[index + 1U] = g;
        raster.srgb[index + 2U] = b;
    }
    return raster;
}

[[nodiscard]] RasterBuffer gradient_raster()
{
    RasterBuffer raster;
    raster.width = 16;
    raster.height = 16;
    raster.source_width = 16;
    raster.source_height = 16;
    declare_srgb(raster);
    raster.srgb.resize(16U * 16U * 3U);
    for (std::uint32_t y = 0; y < 16; ++y)
    {
        for (std::uint32_t x = 0; x < 16; ++x)
        {
            const std::size_t index = (static_cast<std::size_t>(y) * 16U + x) * 3U;
            raster.srgb[index] = static_cast<std::uint8_t>(x * 16U);
            raster.srgb[index + 1U] = static_cast<std::uint8_t>(y * 16U);
            raster.srgb[index + 2U] = 80;
        }
    }
    return raster;
}

[[nodiscard]] std::uint64_t mean_luma(const RenderedImage &image)
{
    std::uint64_t sum = 0;
    for (std::size_t index = 0; index + 2 < image.rgb.size(); index += 3)
    {
        sum += static_cast<std::uint64_t>(image.rgb[index]) * 21U +
               static_cast<std::uint64_t>(image.rgb[index + 1U]) * 72U +
               static_cast<std::uint64_t>(image.rgb[index + 2U]) * 7U;
    }
    return sum / std::max<std::size_t>(1, image.rgb.size() / 3U);
}

[[nodiscard]] Result<RenderedImage> render_op(const EngineFacade &engine, RasterBuffer raster,
                                              OperationInstance operation)
{
    if (raster.color_profile.kind == ColorProfileKind::kMissing)
    {
        declare_srgb(raster);
    }
    Recipe recipe;
    recipe.asset = {"raster", "memory:raster", std::nullopt};
    declare_input(recipe);
    recipe.operations.push_back(std::move(operation));
    RenderRequest request;
    request.asset = recipe.asset;
    request.recipe = recipe;
    return engine.render_to_image(request, &raster);
}

[[nodiscard]] OperationInstance channel_mixer_operation(const ChannelMixerParams &params,
                                                        std::string instance_id = "calibration-1")
{
    return {"ravo.color.channelmixerrgb",        1,           std::move(instance_id), true,
            channel_mixer_to_parameters(params), std::nullopt};
}

[[nodiscard]] OperationInstance
color_balance_rgb_operation(const ColorBalanceRgbParams &params,
                            std::string instance_id = "colorbalancergb-1")
{
    return {"ravo.color.colorbalancergb",
            1,
            std::move(instance_id),
            true,
            color_balance_rgb_to_parameters(params),
            std::nullopt};
}

[[nodiscard]] OperationInstance
legacy_color_balance_operation(const ColorBalanceParams &params,
                               std::string instance_id = "colorbalance-1")
{
    return {std::string(kColorBalanceOperationId),
            kColorBalanceOperationSchemaVersion,
            std::move(instance_id),
            true,
            color_balance_to_parameters(params),
            std::nullopt};
}

[[nodiscard]] WorkingImage legacy_color_balance_working_fixture()
{
    ColorProfileState profile;
    profile.kind = ColorProfileKind::kIcc;
    profile.model = ColorModel::kRgb;
    profile.identifier = std::string(kInputProfileLinearRec709);
    profile.icc_bytes = {1U, 2U, 3U, 4U};
    auto analysis = std::make_shared<ExposureAnalysisContext>();
    analysis->raw_pixel_count = 6U;
    return {
        2U, 1U, {0.03F, 0.18F, 0.72F, 0.91F, 0.42F, 0.07F}, std::move(profile), std::move(analysis),
        {}, {}};
}

using FrozenD50Triplet = std::array<float, 3>;
inline constexpr float kPlatformLibmReferenceTolerance = 1.0e-5F;

// Independent scalar oracle transcribed from the frozen
// common/colorspaces_inline_conversions.h owner. In particular, it preserves
// the transposed-matrix addition order, the pre-rounded D50 reciprocals, and
// the Lab inverse scale/add order instead of calling the production seam.
[[nodiscard]] FrozenD50Triplet frozen_linear_rec709_to_xyz_d50(const FrozenD50Triplet &rgb) noexcept
{
    return {0.4360747F * rgb[0] + 0.3850649F * rgb[1] + 0.1430804F * rgb[2],
            0.2225045F * rgb[0] + 0.7168786F * rgb[1] + 0.0606169F * rgb[2],
            0.0139322F * rgb[0] + 0.0971045F * rgb[1] + 0.7141733F * rgb[2]};
}

[[nodiscard]] FrozenD50Triplet frozen_xyz_d50_to_linear_rec709(const FrozenD50Triplet &xyz) noexcept
{
    // Keep every negative coefficient as an added product, matching the frozen
    // dt_apply_transposed_color_matrix expression order rather than subtraction.
    return {3.1338561F * xyz[0] + (-1.6168667F) * xyz[1] + (-0.4906146F) * xyz[2],
            (-0.9787684F) * xyz[0] + 1.9161415F * xyz[1] + 0.0334540F * xyz[2],
            0.0719453F * xyz[0] + (-0.2289914F) * xyz[1] + 1.4052427F * xyz[2]};
}

[[nodiscard]] FrozenD50Triplet frozen_xyz_d50_to_lab(const FrozenD50Triplet &xyz) noexcept
{
    constexpr FrozenD50Triplet d50_inverse{1.0F / 0.9642F, 1.0F, 1.0F / 0.8249F};
    constexpr float epsilon = 216.0F / 24389.0F;
    constexpr float kappa = 24389.0F / 27.0F;
    FrozenD50Triplet transformed{};
    for (std::size_t channel = 0U; channel < transformed.size(); ++channel)
    {
        const float normalized = xyz[channel] * d50_inverse[channel];
        transformed[channel] =
            normalized > epsilon ? std::cbrt(normalized) : (kappa * normalized + 16.0F) / 116.0F;
    }
    return {116.0F * transformed[1] - 16.0F, 500.0F * (transformed[0] - transformed[1]),
            -200.0F * (transformed[2] - transformed[1])};
}

[[nodiscard]] FrozenD50Triplet frozen_lab_to_xyz_d50(const FrozenD50Triplet &lab) noexcept
{
    constexpr FrozenD50Triplet d50{0.9642F, 1.0F, 0.8249F};
    constexpr FrozenD50Triplet offset{0.0F, 16.0F, 0.0F};
    constexpr FrozenD50Triplet coefficient{1.0F / 500.0F, 1.0F / 116.0F, -1.0F / 200.0F};
    constexpr FrozenD50Triplet add_coefficient{1.0F, 0.0F, 1.0F};
    constexpr float epsilon = 0.20689655172413796F;
    constexpr float kappa = 24389.0F / 27.0F;
    const FrozenD50Triplet reordered{lab[1], lab[0], lab[2]};
    FrozenD50Triplet scaled{};
    for (std::size_t channel = 0U; channel < scaled.size(); ++channel)
    {
        scaled[channel] = (reordered[channel] + offset[channel]) * coefficient[channel];
    }
    FrozenD50Triplet xyz{};
    for (std::size_t channel = 0U; channel < xyz.size(); ++channel)
    {
        const float value = scaled[channel] + scaled[1] * add_coefficient[channel];
        const float inverse =
            value > epsilon ? value * value * value : (116.0F * value - 16.0F) / kappa;
        xyz[channel] = d50[channel] * inverse;
    }
    return xyz;
}

// Independent scalar oracle transcribed from frozen colorcontrast.c v2. It
// intentionally calls neither the production Color Contrast helper nor the
// production D50 bridge, preserving the source multiply/add and CLAMPS order.
[[nodiscard]] FrozenD50Triplet frozen_color_contrast_lab(const ColorContrastParams &params,
                                                         const FrozenD50Triplet &lab) noexcept
{
    const float a_steepness = static_cast<float>(params.a_steepness);
    const float a_offset = static_cast<float>(params.a_offset);
    const float b_steepness = static_cast<float>(params.b_steepness);
    const float b_offset = static_cast<float>(params.b_offset);
    float a = lab[1] * a_steepness + a_offset;
    float b = lab[2] * b_steepness + b_offset;
    if (!params.unbound)
    {
        a = a > -128.0F ? (a < 128.0F ? a : 128.0F) : -128.0F;
        b = b > -128.0F ? (b < 128.0F ? b : 128.0F) : -128.0F;
    }
    return {lab[0], a, b};
}

[[nodiscard]] FrozenD50Triplet frozen_color_contrast_rgb(const ColorContrastParams &params,
                                                         const FrozenD50Triplet &rgb) noexcept
{
    const auto lab = frozen_xyz_d50_to_lab(frozen_linear_rec709_to_xyz_d50(rgb));
    return frozen_xyz_d50_to_linear_rec709(
        frozen_lab_to_xyz_d50(frozen_color_contrast_lab(params, lab)));
}

[[nodiscard]] std::array<std::uint32_t, 3>
d50_triplet_bits(const FrozenD50Triplet &triplet) noexcept
{
    return {std::bit_cast<std::uint32_t>(triplet[0]), std::bit_cast<std::uint32_t>(triplet[1]),
            std::bit_cast<std::uint32_t>(triplet[2])};
}

void expect_frozen_d50_bits(const FrozenD50Triplet &actual, const FrozenD50Triplet &oracle,
                            const std::array<std::uint32_t, 3> &golden)
{
    EXPECT_EQ(d50_triplet_bits(oracle), golden);
    EXPECT_EQ(d50_triplet_bits(actual), golden);
}

void expect_frozen_d50_cbrt_reference(const FrozenD50Triplet &actual,
                                      const FrozenD50Triplet &oracle,
                                      const std::array<std::uint32_t, 3> &reference)
{
    EXPECT_EQ(d50_triplet_bits(actual), d50_triplet_bits(oracle));
    for (std::size_t channel = 0U; channel < reference.size(); ++channel)
    {
        // cbrtf is platform libm code. Preserve exact host-local source-order
        // agreement while retaining a tight, recorded cross-platform envelope.
        EXPECT_NEAR(actual[channel], std::bit_cast<float>(reference[channel]),
                    kPlatformLibmReferenceTolerance);
    }
}

// Independent scalar oracle transcribed from the frozen colorbalance.c
// commit_params(), _process_sop(), and _process_lgg() paths. It deliberately
// calls no production Color Balance helper, so the fixed goldens below do not
// merely restate apply_color_balance().
[[nodiscard]] std::vector<float>
frozen_legacy_color_balance_reference(const WorkingImage &input, const ColorBalanceParams &params)
{
    const auto linear_to_xyz = [](const std::array<float, 3> &rgb)
    {
        return std::array<float, 3>{0.4360747F * rgb[0] + 0.3850649F * rgb[1] + 0.1430804F * rgb[2],
                                    0.2225045F * rgb[0] + 0.7168786F * rgb[1] + 0.0606169F * rgb[2],
                                    0.0139322F * rgb[0] + 0.0971045F * rgb[1] +
                                        0.7141733F * rgb[2]};
    };
    const auto xyz_to_linear = [](const std::array<float, 3> &xyz)
    {
        return std::array<float, 3>{
            3.1338561F * xyz[0] - 1.6168667F * xyz[1] - 0.4906146F * xyz[2],
            -0.9787684F * xyz[0] + 1.9161415F * xyz[1] + 0.0334540F * xyz[2],
            0.0719453F * xyz[0] - 0.2289914F * xyz[1] + 1.4052427F * xyz[2]};
    };
    const auto xyz_to_lab = [](const std::array<float, 3> &xyz)
    {
        constexpr std::array<float, 3> d50{0.9642F, 1.0F, 0.8249F};
        constexpr float epsilon = 216.0F / 24389.0F;
        constexpr float kappa = 24389.0F / 27.0F;
        std::array<float, 3> f{};
        for (std::size_t channel = 0U; channel < f.size(); ++channel)
        {
            const float value = xyz[channel] / d50[channel];
            f[channel] = value > epsilon ? std::cbrt(value) : (kappa * value + 16.0F) / 116.0F;
        }
        return std::array<float, 3>{116.0F * f[1] - 16.0F, 500.0F * (f[0] - f[1]),
                                    200.0F * (f[1] - f[2])};
    };
    const auto lab_to_xyz = [](const std::array<float, 3> &lab)
    {
        constexpr std::array<float, 3> d50{0.9642F, 1.0F, 0.8249F};
        constexpr float epsilon = 0.20689655172413796F;
        constexpr float kappa = 24389.0F / 27.0F;
        const float fy = (lab[0] + 16.0F) / 116.0F;
        const std::array<float, 3> f{fy + lab[1] / 500.0F, fy, fy - lab[2] / 200.0F};
        std::array<float, 3> xyz{};
        for (std::size_t channel = 0U; channel < xyz.size(); ++channel)
        {
            const float value = f[channel] > epsilon ? f[channel] * f[channel] * f[channel] :
                                                       (116.0F * f[channel] - 16.0F) / kappa;
            xyz[channel] = d50[channel] * value;
        }
        return xyz;
    };
    const auto xyz_to_prophoto = [](const std::array<float, 3> &xyz)
    {
        return std::array<float, 3>{
            1.3459433F * xyz[0] - 0.2556075F * xyz[1] - 0.0511118F * xyz[2],
            -0.5445989F * xyz[0] + 1.5081673F * xyz[1] + 0.0205351F * xyz[2], 1.2118128F * xyz[2]};
    };
    const auto prophoto_to_xyz = [](const std::array<float, 3> &rgb)
    {
        return std::array<float, 3>{0.7976749F * rgb[0] + 0.1351917F * rgb[1] + 0.0313534F * rgb[2],
                                    0.2880402F * rgb[0] + 0.7118741F * rgb[1] + 0.0000857F * rgb[2],
                                    0.8252100F * rgb[2]};
    };
    const auto corrected = [&](const std::array<double, 4> &values)
    {
        const float red = static_cast<float>(values[1]);
        const float green = static_cast<float>(values[2]);
        const float blue = static_cast<float>(values[3]);
        const float luma = 0.2880402F * red + 0.7118741F * green + 0.0000857F * blue;
        return std::array<float, 4>{static_cast<float>(values[0]), red - luma + 1.0F,
                                    green - luma + 1.0F, blue - luma + 1.0F};
    };

    const auto lift = corrected(params.lift);
    const auto gamma = corrected(params.gamma);
    const auto gain = corrected(params.gain);
    const bool lgg = params.mode == kColorBalanceModeLiftGammaGain;
    std::array<float, 3> effective_lift{};
    std::array<float, 3> effective_gain{};
    std::array<float, 3> effective_power{};
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
    }
    const float input_saturation = static_cast<float>(params.input_saturation);
    const float output_saturation = static_cast<float>(params.output_saturation);
    const float contrast = static_cast<float>(params.contrast);
    const float contrast_power = 1.0F / contrast;
    const float grey = static_cast<float>(params.grey_fulcrum_percent / 100.0);
    const bool run_input_saturation = std::abs(input_saturation - 1.0F) > 1.0e-6F;
    const bool run_output_saturation = std::abs(output_saturation - 1.0F) > 1.0e-6F;
    const bool run_contrast = std::abs((lgg ? contrast_power : contrast) - 1.0F) > 1.0e-6F;

    std::vector<float> result(input.rgb.size());
    for (std::size_t index = 0U; index < input.rgb.size(); index += 3U)
    {
        const std::array<float, 3> source{input.rgb[index], input.rgb[index + 1U],
                                          input.rgb[index + 2U]};
        auto xyz = lab_to_xyz(xyz_to_lab(linear_to_xyz(source)));
        auto rgb = xyz_to_prophoto(xyz);
        if (run_input_saturation)
        {
            for (float &sample : rgb)
            {
                sample = xyz[1] + input_saturation * (sample - xyz[1]);
            }
        }
        for (std::size_t channel = 0U; channel < rgb.size(); ++channel)
        {
            if (lgg)
            {
                float value = std::pow(std::max(rgb[channel], 0.0F), 1.0F / 2.2F);
                value = ((value - 1.0F) * effective_lift[channel] + 1.0F) * effective_gain[channel];
                rgb[channel] = std::pow(std::max(value, 0.0F), effective_power[channel]);
            }
            else
            {
                rgb[channel] = std::pow(
                    std::max(effective_gain[channel] * rgb[channel] + effective_lift[channel],
                             0.0F),
                    effective_power[channel]);
            }
        }
        if (run_output_saturation)
        {
            const float luma = prophoto_to_xyz(rgb)[1];
            for (float &sample : rgb)
            {
                sample = luma + output_saturation * (sample - luma);
            }
        }
        if (run_contrast)
        {
            for (float &sample : rgb)
            {
                sample = std::pow(std::max(sample, 0.0F) / grey, contrast_power) * grey;
            }
        }
        const auto output = xyz_to_linear(lab_to_xyz(xyz_to_lab(prophoto_to_xyz(rgb))));
        std::copy(output.begin(), output.end(),
                  result.begin() + static_cast<std::ptrdiff_t>(index));
    }
    return result;
}
struct FrozenColorCheckerFit
{
    std::vector<std::array<float, 3>> sources;
    std::array<std::vector<float>, 3> coefficients;
};

[[nodiscard]] float frozen_color_checker_kernel_oracle(const std::array<float, 3> &left,
                                                       const std::array<float, 3> &right,
                                                       const bool use_libm = false)
{
    std::array<float, 3> squared{};
    for (std::size_t channel = 0U; channel < squared.size(); ++channel)
    {
        squared[channel] = left[channel] - right[channel];
        squared[channel] *= squared[channel];
    }
    const float radius_squared = squared[0] + squared[1] + squared[2];
    if (use_libm)
    {
        return radius_squared * std::log(std::max(1.0e-8F, radius_squared));
    }
    const float argument = std::max(1.0e-8F, radius_squared);
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(argument);
    const float mantissa = std::bit_cast<float>((bits & 0x007fffffU) | 0x3f000000U);
    float exponent = static_cast<float>(bits);
    exponent *= 1.1920928955078125e-7F;
    const float log2 = exponent - 124.22551499F - 1.498030302F * mantissa -
                       1.72587999F / (0.3520887068F + mantissa);
    return radius_squared * (0.69314718055994530942F * log2);
}

[[nodiscard]] bool frozen_color_checker_triangular(std::vector<double> &matrix,
                                                   std::vector<int> &pivots, const std::size_t size)
{
    pivots[size - 1U] = static_cast<int>(size - 1U);
    for (std::size_t column = 0U; column < size; ++column)
    {
        std::size_t best_row = column;
        for (std::size_t row = column + 1U; row < size; ++row)
        {
            if (std::fabs(matrix[row * size + column]) >
                std::fabs(matrix[best_row * size + column]))
            {
                best_row = row;
            }
        }
        pivots[column] = static_cast<int>(best_row);
        const double pivot = matrix[best_row * size + column];
        std::swap(matrix[best_row * size + column], matrix[column * size + column]);
        if (pivot == 0.0)
        {
            return false;
        }
        for (std::size_t row = column + 1U; row < size; ++row)
        {
            matrix[row * size + column] /= -pivot;
        }
        if (best_row != column)
        {
            for (std::size_t remaining = column + 1U; remaining < size; ++remaining)
            {
                std::swap(matrix[best_row * size + remaining], matrix[column * size + remaining]);
            }
        }
        for (std::size_t row = column + 1U; row < size; ++row)
        {
            for (std::size_t remaining = column + 1U; remaining < size; ++remaining)
            {
                matrix[row * size + remaining] +=
                    matrix[row * size + column] * matrix[column * size + remaining];
            }
        }
    }
    return true;
}

void frozen_color_checker_back_substitute(const std::vector<double> &matrix,
                                          const std::vector<int> &pivots,
                                          std::vector<double> &right, const std::size_t size)
{
    for (std::size_t column = 0U; column + 1U < size; ++column)
    {
        const std::size_t pivot = static_cast<std::size_t>(pivots[column]);
        const double value = right[pivot];
        std::swap(right[pivot], right[column]);
        for (std::size_t row = column + 1U; row < size; ++row)
        {
            right[row] += matrix[row * size + column] * value;
        }
    }
    for (std::size_t column = size - 1U; column > 0U; --column)
    {
        right[column] /= matrix[column * size + column];
        for (std::size_t row = 0U; row < column; ++row)
        {
            right[row] -= matrix[row * size + column] * right[column];
        }
    }
    right[0] /= matrix[0];
}

[[nodiscard]] bool frozen_color_checker_solve(std::vector<double> matrix,
                                              std::vector<double> &right)
{
    std::vector<int> pivots(right.size());
    if (!frozen_color_checker_triangular(matrix, pivots, right.size()))
    {
        return false;
    }
    frozen_color_checker_back_substitute(matrix, pivots, right, right.size());
    return true;
}

[[nodiscard]] FrozenColorCheckerFit
frozen_color_checker_fit_oracle(const ColorCheckerParams &params, const bool use_libm = false,
                                const bool promote_n3_sum = false)
{
    FrozenColorCheckerFit fit;
    fit.sources.reserve(params.patches.size());
    for (const auto &patch : params.patches)
    {
        fit.sources.push_back({static_cast<float>(patch.source_lab[0]),
                               static_cast<float>(patch.source_lab[1]),
                               static_cast<float>(patch.source_lab[2])});
    }
    const std::size_t count = fit.sources.size();
    for (auto &coefficients : fit.coefficients)
    {
        coefficients.assign(count + 4U, 0.0F);
    }
    fit.coefficients[0][count + 1U] = 1.0F;
    fit.coefficients[1][count + 2U] = 1.0F;
    fit.coefficients[2][count + 3U] = 1.0F;
    const auto target = [&](const std::size_t patch, const std::size_t channel)
    { return static_cast<float>(params.patches[patch].target_lab[channel]); };

    if (count == 0U)
    {
        return fit;
    }
    if (count == 1U)
    {
        for (std::size_t channel = 0U; channel < 3U; ++channel)
        {
            fit.coefficients[channel][count + channel + 1U] =
                target(0U, channel) / fit.sources[0][channel];
        }
        return fit;
    }
    if (count == 2U)
    {
        for (std::size_t channel = 0U; channel < 3U; ++channel)
        {
            std::vector<double> right{target(0U, channel), target(1U, channel)};
            if (!frozen_color_checker_solve(
                    {1.0, fit.sources[0][channel], 1.0, fit.sources[1][channel]}, right))
            {
                return fit;
            }
            fit.coefficients[channel][count] = static_cast<float>(right[0]);
            fit.coefficients[channel][count + channel + 1U] = static_cast<float>(right[1]);
        }
        return fit;
    }
    if (count == 3U)
    {
        for (std::size_t channel = 0U; channel < 3U; ++channel)
        {
            std::vector<double> matrix;
            for (std::size_t patch = 0U; patch < count; ++patch)
            {
                const std::size_t other0 = (channel + 1U) % 3U;
                const std::size_t other1 = (channel + 2U) % 3U;
                const double other_sum = promote_n3_sum ?
                                             static_cast<double>(fit.sources[patch][other0]) +
                                                 fit.sources[patch][other1] :
                                             static_cast<double>(fit.sources[patch][other0] +
                                                                 fit.sources[patch][other1]);
                matrix.insert(matrix.end(), {1.0, fit.sources[patch][channel], other_sum});
            }
            std::vector<double> right{target(0U, channel), target(1U, channel),
                                      target(2U, channel)};
            if (!frozen_color_checker_solve(std::move(matrix), right))
            {
                return fit;
            }
            fit.coefficients[channel][count] = static_cast<float>(right[0]);
            fit.coefficients[channel][count + channel + 1U] = static_cast<float>(right[1]);
            for (std::size_t input = 0U; input < 3U; ++input)
            {
                if (input != channel)
                {
                    fit.coefficients[channel][count + input + 1U] = static_cast<float>(right[2]);
                }
            }
        }
        return fit;
    }

    const std::size_t fit_size = count == 4U ? 4U : count + 4U;
    std::vector<double> matrix(fit_size * fit_size, 0.0);
    if (count == 4U)
    {
        for (std::size_t patch = 0U; patch < count; ++patch)
        {
            matrix[patch * fit_size] = 1.0;
            for (std::size_t channel = 0U; channel < 3U; ++channel)
            {
                matrix[patch * fit_size + channel + 1U] = fit.sources[patch][channel];
            }
        }
    }
    else
    {
        for (std::size_t row = 0U; row < count; ++row)
        {
            for (std::size_t column = 0U; column < count; ++column)
            {
                matrix[row * fit_size + column] = frozen_color_checker_kernel_oracle(
                    fit.sources[row], fit.sources[column], use_libm);
            }
            matrix[row * fit_size + count] = matrix[count * fit_size + row] = 1.0;
            for (std::size_t channel = 0U; channel < 3U; ++channel)
            {
                matrix[row * fit_size + count + channel + 1U] =
                    matrix[(count + channel + 1U) * fit_size + row] = fit.sources[row][channel];
            }
        }
    }
    std::vector<int> pivots(fit_size);
    if (!frozen_color_checker_triangular(matrix, pivots, fit_size))
    {
        return fit;
    }
    for (std::size_t channel = 0U; channel < 3U; ++channel)
    {
        std::vector<double> right(fit_size, 0.0);
        for (std::size_t patch = 0U; patch < count; ++patch)
        {
            right[patch] = target(patch, channel);
        }
        frozen_color_checker_back_substitute(matrix, pivots, right, fit_size);
        const std::size_t offset = count == 4U ? count : 0U;
        for (std::size_t index = 0U; index < fit_size; ++index)
        {
            fit.coefficients[channel][offset + index] = static_cast<float>(right[index]);
        }
    }
    return fit;
}

[[nodiscard]] std::array<float, 3>
frozen_color_checker_lab_reference(const ColorCheckerParams &params,
                                   const std::array<float, 3> &lab, const bool use_libm = false,
                                   const bool promote_n3_sum = false)
{
    const auto fit = frozen_color_checker_fit_oracle(params, use_libm, promote_n3_sum);
    const std::size_t count = fit.sources.size();
    std::array<float, 3> result{};
    for (std::size_t channel = 0U; channel < result.size(); ++channel)
    {
        const float term_l = fit.coefficients[channel][count + 1U] * lab[0];
        const float term_a = fit.coefficients[channel][count + 2U] * lab[1];
        const float term_b = fit.coefficients[channel][count + 3U] * lab[2];
        result[channel] = fit.coefficients[channel][count] + (term_l + term_a + term_b);
    }
    for (std::size_t patch = 0U; patch < count; ++patch)
    {
        const float phi = frozen_color_checker_kernel_oracle(lab, fit.sources[patch], use_libm);
        for (std::size_t channel = 0U; channel < result.size(); ++channel)
        {
            result[channel] += fit.coefficients[channel][patch] * phi;
        }
    }
    return result;
}

TEST(LegacyColorBalanceTest, SopAndLggModesPreserveFrozenMathAndOwnedPublication)
{
    const auto input = legacy_color_balance_working_fixture();
    const auto original = input;

    ColorBalanceParams sop;
    sop.lift = {0.96, 1.03, 0.98, 1.06};
    sop.gamma = {1.08, 0.91, 1.05, 0.97};
    sop.gain = {1.04, 1.12, 0.95, 1.08};
    sop.input_saturation = 0.84;
    sop.contrast = 1.16;
    sop.grey_fulcrum_percent = 18.0;
    sop.output_saturation = 1.09;
    auto sop_result = apply_color_balance(input, sop, CancellationToken{});
    ASSERT_TRUE(sop_result) << sop_result.error().message;
    ASSERT_EQ(sop_result.value().rgb.size(), input.rgb.size());
    const std::array<float, 6> expected_sop{0.10232526F, 0.15027370F, 0.66838688F,
                                            0.85773712F, 0.34731370F, 0.18906617F};
    const auto reference_sop = frozen_legacy_color_balance_reference(input, sop);
    for (std::size_t index = 0U; index < expected_sop.size(); ++index)
    {
        EXPECT_NEAR(reference_sop[index], expected_sop[index], 2.0e-5F) << index;
        EXPECT_NEAR(sop_result.value().rgb[index], expected_sop[index], 2.0e-5F) << index;
        EXPECT_NEAR(sop_result.value().rgb[index], reference_sop[index], 2.0e-5F) << index;
    }
    auto channel_order_perturbation = sop;
    std::swap(channel_order_perturbation.lift[1], channel_order_perturbation.lift[3]);
    const auto perturbed_reference =
        frozen_legacy_color_balance_reference(input, channel_order_perturbation);
    bool perturbation_detected = false;
    for (std::size_t index = 0U; index < reference_sop.size(); ++index)
    {
        perturbation_detected |=
            std::abs(reference_sop[index] - perturbed_reference[index]) > 1.0e-3F;
    }
    EXPECT_TRUE(perturbation_detected)
        << "the independent oracle must detect a frozen RGB channel-order perturbation";

    ColorBalanceParams lgg = sop;
    lgg.mode = std::string(kColorBalanceModeLiftGammaGain);
    auto lgg_result = apply_color_balance(input, lgg, CancellationToken{});
    ASSERT_TRUE(lgg_result) << lgg_result.error().message;
    const std::array<float, 6> expected_lgg{0.12932241F, 0.17394857F, 0.73170942F,
                                            1.06095791F, 0.34121433F, 0.19952966F};
    const auto reference_lgg = frozen_legacy_color_balance_reference(input, lgg);
    for (std::size_t index = 0U; index < expected_lgg.size(); ++index)
    {
        EXPECT_NEAR(reference_lgg[index], expected_lgg[index], 2.0e-5F) << index;
        EXPECT_NEAR(lgg_result.value().rgb[index], expected_lgg[index], 2.0e-5F) << index;
        EXPECT_NEAR(lgg_result.value().rgb[index], reference_lgg[index], 2.0e-5F) << index;
    }
    EXPECT_NE(lgg_result.value().rgb, sop_result.value().rgb);

    auto defaults = apply_color_balance(input, ColorBalanceParams{}, CancellationToken{});
    ASSERT_TRUE(defaults) << defaults.error().message;
    const auto reference_defaults =
        frozen_legacy_color_balance_reference(input, ColorBalanceParams{});
    // The frozen operation performs its Lab/ProPhoto conversion boundary even at defaults.
    EXPECT_NE(defaults.value().rgb, input.rgb);
    ASSERT_EQ(defaults.value().rgb.size(), reference_defaults.size());
    for (std::size_t index = 0U; index < reference_defaults.size(); ++index)
    {
        EXPECT_NEAR(defaults.value().rgb[index], reference_defaults[index], 2.0e-5F) << index;
    }
    EXPECT_EQ(sop_result.value().width, input.width);
    EXPECT_EQ(sop_result.value().height, input.height);
    EXPECT_EQ(sop_result.value().color_profile, input.color_profile);
    EXPECT_EQ(sop_result.value().exposure_analysis, input.exposure_analysis);
    EXPECT_NE(sop_result.value().rgb.data(), input.rgb.data());
    ASSERT_FALSE(sop_result.value().color_profile.icc_bytes.empty());
    EXPECT_NE(sop_result.value().color_profile.icc_bytes.data(),
              input.color_profile.icc_bytes.data());
    sop_result.value().rgb[0] = 42.0F;
    sop_result.value().color_profile.icc_bytes[0] = 99U;
    EXPECT_EQ(input.width, original.width);
    EXPECT_EQ(input.height, original.height);
    EXPECT_EQ(input.rgb, original.rgb);
    EXPECT_EQ(input.color_profile, original.color_profile);
    EXPECT_EQ(input.exposure_analysis, original.exposure_analysis);
}

TEST(ColorCheckerTest, ThinPlateKernelUsesTheFrozenFastLogApproximation)
{
    struct KernelCase
    {
        std::array<float, 3> point;
        float squared_distance;
        std::uint32_t expected_bits;
    };
    const std::array<KernelCase, 4> cases{{
        {{1.0F, 0.0F, 0.0F}, 1.0F, 0xb5ddce9eU},
        {{1.0F, 1.0F, 0.0F}, 2.0F, 0x3fb171fcU},
        {{3.0F, 1.0F, 0.0F}, 10.0F, 0x41b8340aU},
        {{100.0F, 0.0F, 0.0F}, 10000.0F, 0x47b3e369U},
    }};
    for (const auto &[point, squared_distance, expected_bits] : cases)
    {
        const float actual = color_checker_thin_plate_kernel(point, {});
        EXPECT_EQ(std::bit_cast<std::uint32_t>(actual), expected_bits) << squared_distance;
    }
}

TEST(ColorCheckerTest, TwoPatchGaussianOrientationMatchesTheFrozenScalarOracle)
{
    ColorCheckerParams params{{
        {{{1.0, 2.0, 4.0}}, {{3.0, 8.0, 20.0}}},
        {{{3.0, 5.0, 9.0}}, {{7.0, 20.0, 45.0}}},
    }};
    const std::array<float, 3> input{2.0F, 3.0F, 6.0F};
    const auto oracle = frozen_color_checker_lab_reference(params, input);
    const std::array<float, 3> golden{5.0F, 12.0F, 30.0F};
    EXPECT_EQ(oracle, golden);

    auto actual = apply_color_checker_lab(params, input, CancellationToken{});
    ASSERT_TRUE(actual) << actual.error().message;
    EXPECT_EQ(actual.value(), golden);
    EXPECT_EQ(actual.value(), oracle);
}

TEST(ColorCheckerTest, ThreePatchOtherChannelSumRoundsInFloatBeforePromotion)
{
    ColorCheckerParams params{{
        {{{-52.407073974609375, 16777224.0, -16777207.0}},
         {{-5.947298526763916, 67.29228973388672, -4.729358196258545}}},
        {{{-5.189292907714844, 16777200.0, -16777184.0}},
         {{27.813627243041992, -69.87671661376953, 26.972131729125977}}},
        {{{-6.1535325050354, 16777212.0, -16777192.0}},
         {{73.60906219482422, 4.636241912841797, 48.250370025634766}}},
    }};
    const std::array<float, 3> input{12.5F, 16777220.0F, -16777216.0F};
    const auto oracle = frozen_color_checker_lab_reference(params, input);
    const auto promoted = frozen_color_checker_lab_reference(params, input, false, true);
    EXPECT_NE(oracle, promoted)
        << "the independent oracle must detect promotion before the frozen float addition";
    EXPECT_FLOAT_EQ(oracle[1], 48.0F);
    EXPECT_FLOAT_EQ(promoted[1], 40.0F);

    auto actual = apply_color_checker_lab(params, input, CancellationToken{});
    ASSERT_TRUE(actual) << actual.error().message;
    EXPECT_EQ(actual.value(), oracle);
}

TEST(ColorCheckerTest, Real0098PayloadMatchesIndependentRbfOracleAndFixedLabGolden)
{
    ColorCheckerParams params;
    params.patches[7].target_lab = {92.74998474121094, 97.59593200683594, 82.81928253173828};
    params.patches[19].target_lab = {72.97999572753906, 43.90998840332031, 35.799983978271484};
    params.patches[22].target_lab = {45.439998626708984, -0.41999998688697815, 59.32999801635742};
    const std::array<float, 3> input{50.0F, 0.0F, 0.0F};
    // Fixed source-order results for the verbatim 0098 v2 payload. The
    // independent scalar oracle below protects the frozen kernel and solver
    // order without calling a production Ravo helper.
    const std::array<float, 3> golden{std::bit_cast<float>(0x4249cbc8U),
                                      std::bit_cast<float>(0x3eb8d900U),
                                      std::bit_cast<float>(0x404095c0U)};

    const auto oracle = frozen_color_checker_lab_reference(params, input);
    const auto libm_perturbation = frozen_color_checker_lab_reference(params, input, true);
    bool oracle_detects_libm = false;
    for (std::size_t channel = 0U; channel < golden.size(); ++channel)
    {
        EXPECT_EQ(std::bit_cast<std::uint32_t>(oracle[channel]),
                  std::bit_cast<std::uint32_t>(golden[channel]))
            << channel;
        oracle_detects_libm |= std::abs(oracle[channel] - libm_perturbation[channel]) > 1.0e-5F;
    }
    EXPECT_TRUE(oracle_detects_libm)
        << "the independent oracle must distinguish the frozen fastlog from libm";

    auto actual = apply_color_checker_lab(params, input, CancellationToken{});
    ASSERT_TRUE(actual) << actual.error().message;
    for (std::size_t channel = 0U; channel < golden.size(); ++channel)
    {
        EXPECT_EQ(std::bit_cast<std::uint32_t>(actual.value()[channel]),
                  std::bit_cast<std::uint32_t>(golden[channel]))
            << channel;
        EXPECT_EQ(std::bit_cast<std::uint32_t>(actual.value()[channel]),
                  std::bit_cast<std::uint32_t>(oracle[channel]))
            << channel;
    }
}

TEST(ColorCheckerTest, ZeroOneFourAndRbfPatchModesMatchTheIndependentOracle)
{
    const std::array<float, 3> input{0.25F, 0.5F, 0.75F};

    ColorCheckerParams zero{{}};
    auto actual = apply_color_checker_lab(zero, input, CancellationToken{});
    ASSERT_TRUE(actual) << actual.error().message;
    EXPECT_EQ(actual.value(), input);

    ColorCheckerParams one{{{{{2.0, 4.0, 5.0}}, {{6.0, 2.0, 10.0}}}}};
    actual = apply_color_checker_lab(one, {1.0F, 8.0F, 2.5F}, CancellationToken{});
    ASSERT_TRUE(actual) << actual.error().message;
    EXPECT_EQ(actual.value(), (std::array<float, 3>{3.0F, 4.0F, 5.0F}));

    ColorCheckerParams four{{
        {{{0.0, 0.0, 0.0}}, {{1.0, -2.0, 0.5}}},
        {{{1.0, 0.0, 0.0}}, {{3.0, -1.5, -0.5}}},
        {{{0.0, 1.0, 0.0}}, {{4.0, -1.0, 2.5}}},
        {{{0.0, 0.0, 1.0}}, {{5.0, -0.5, 4.5}}},
    }};
    const auto four_oracle = frozen_color_checker_lab_reference(four, input);
    EXPECT_EQ(four_oracle, (std::array<float, 3>{6.0F, -0.25F, 4.25F}));
    actual = apply_color_checker_lab(four, input, CancellationToken{});
    ASSERT_TRUE(actual) << actual.error().message;
    EXPECT_EQ(actual.value(), four_oracle);

    ColorCheckerParams five = four;
    five.patches.push_back({{{1.0, 1.0, 1.0}}, {{12.0, 3.0, -7.0}}});
    const auto five_oracle = frozen_color_checker_lab_reference(five, input);
    actual = apply_color_checker_lab(five, input, CancellationToken{});
    ASSERT_TRUE(actual) << actual.error().message;
    for (std::size_t channel = 0U; channel < 3U; ++channel)
    {
        EXPECT_NEAR(actual.value()[channel], five_oracle[channel], 2.0e-5F) << channel;
    }
    EXPECT_NE(actual.value(), input);

    auto expanded = color_checker_params_for_preset("expanded_color_checker");
    ASSERT_TRUE(expanded) << expanded.error().message;
    ASSERT_EQ(expanded.value().patches.size(), kColorCheckerMaxPatchCount);
    const std::array<float, 3> expanded_input{52.0F, 18.0F, -21.0F};
    const auto expanded_oracle =
        frozen_color_checker_lab_reference(expanded.value(), expanded_input);
    actual = apply_color_checker_lab(expanded.value(), expanded_input, CancellationToken{});
    ASSERT_TRUE(actual) << actual.error().message;
    for (std::size_t channel = 0U; channel < 3U; ++channel)
    {
        EXPECT_NEAR(actual.value()[channel], expanded_oracle[channel], 2.0e-5F) << channel;
    }
}

TEST(ColorCheckerTest, SingularFallbackPreservesFrozenSequentialAndSharedMatrixSemantics)
{
    ColorCheckerParams two{{
        {{{1.0, 2.0, 3.0}}, {{2.0, 7.0, 11.0}}},
        {{{3.0, 2.0, 6.0}}, {{10.0, 9.0, 17.0}}},
    }};
    const std::array<float, 3> input{2.0F, 5.0F, 7.0F};
    const auto two_oracle = frozen_color_checker_lab_reference(two, input);
    EXPECT_FLOAT_EQ(two_oracle[0], 6.0F);
    EXPECT_FLOAT_EQ(two_oracle[1], input[1]);
    EXPECT_FLOAT_EQ(two_oracle[2], input[2]);
    auto actual = apply_color_checker_lab(two, input, CancellationToken{});
    ASSERT_TRUE(actual) << actual.error().message;
    EXPECT_EQ(actual.value(), two_oracle);

    ColorCheckerParams three{{
        {{{1.0, 1.0, 5.0}}, {{2.0, -1.0, 12.0}}},
        {{{2.0, 3.0, 5.0}}, {{7.0, 4.0, 18.0}}},
        {{{4.0, 2.0, 5.0}}, {{11.0, 9.0, 24.0}}},
    }};
    const auto three_oracle = frozen_color_checker_lab_reference(three, input);
    EXPECT_NE(three_oracle[0], input[0]);
    EXPECT_NE(three_oracle[1], input[1]);
    EXPECT_FLOAT_EQ(three_oracle[2], input[2]);
    actual = apply_color_checker_lab(three, input, CancellationToken{});
    ASSERT_TRUE(actual) << actual.error().message;
    EXPECT_EQ(actual.value(), three_oracle);

    ColorCheckerParams four_singular{{
        {{{1.0, 2.0, 3.0}}, {{8.0, 9.0, 10.0}}},
        {{{1.0, 2.0, 3.0}}, {{11.0, 12.0, 13.0}}},
        {{{2.0, 3.0, 4.0}}, {{14.0, 15.0, 16.0}}},
        {{{3.0, 4.0, 5.0}}, {{17.0, 18.0, 19.0}}},
    }};
    actual = apply_color_checker_lab(four_singular, input, CancellationToken{});
    ASSERT_TRUE(actual) << actual.error().message;
    EXPECT_EQ(actual.value(), input);

    ColorCheckerParams rbf_singular = four_singular;
    rbf_singular.patches.push_back({{{4.0, 5.0, 6.0}}, {{20.0, 21.0, 22.0}}});
    actual = apply_color_checker_lab(rbf_singular, input, CancellationToken{});
    ASSERT_TRUE(actual) << actual.error().message;
    EXPECT_EQ(actual.value(), input);
}

TEST(ColorCheckerTest, WorkingPublicationIsOwnedImmutableAndRejectsEveryInvalidBoundary)
{
    WorkingImage input = legacy_color_balance_working_fixture();
    input.rgb.front() = -0.1F;
    input.rgb.back() = 1.2F;
    const WorkingImage original = input;

    auto output = apply_color_checker(input, ColorCheckerParams{}, CancellationToken{});
    ASSERT_TRUE(output) << output.error().message;
    EXPECT_EQ(output.value().width, input.width);
    EXPECT_EQ(output.value().height, input.height);
    EXPECT_EQ(output.value().color_profile, input.color_profile);
    EXPECT_EQ(output.value().exposure_analysis, input.exposure_analysis);
    EXPECT_NE(output.value().rgb.data(), input.rgb.data());
    ASSERT_FALSE(input.color_profile.icc_bytes.empty());
    EXPECT_NE(output.value().color_profile.icc_bytes.data(), input.color_profile.icc_bytes.data());
    output.value().rgb[0] = 42.0F;
    output.value().color_profile.icc_bytes[0] = 99U;
    EXPECT_EQ(input.width, original.width);
    EXPECT_EQ(input.height, original.height);
    EXPECT_EQ(input.rgb, original.rgb);
    EXPECT_EQ(input.color_profile, original.color_profile);
    EXPECT_EQ(input.exposure_analysis, original.exposure_analysis);

    auto operation_parameters = color_checker_to_parameters(ColorCheckerParams{});
    ASSERT_TRUE(operation_parameters) << operation_parameters.error().message;
    OperationInstance operation{std::string(kColorCheckerOperationId),
                                kColorCheckerOperationSchemaVersion,
                                "colorchecker-dispatch",
                                true,
                                operation_parameters.value(),
                                std::nullopt};
    auto dispatched = apply_color_checker(input, operation, CancellationToken{});
    ASSERT_TRUE(dispatched) << dispatched.error().message;
    auto direct = apply_color_checker(input, ColorCheckerParams{}, CancellationToken{});
    ASSERT_TRUE(direct) << direct.error().message;
    EXPECT_EQ(dispatched.value().rgb, direct.value().rgb);
    operation.enabled = false;
    auto disabled = apply_color_checker(input, operation, CancellationToken{});
    ASSERT_TRUE(disabled) << disabled.error().message;
    EXPECT_EQ(disabled.value().rgb, input.rgb);
    EXPECT_NE(disabled.value().rgb.data(), input.rgb.data());

    WorkingImage zero = input;
    zero.width = 0U;
    auto rejected = apply_color_checker(zero, ColorCheckerParams{}, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "invalid_colorchecker_dimensions");
    WorkingImage wrong_size = input;
    wrong_size.rgb.pop_back();
    rejected = apply_color_checker(wrong_size, ColorCheckerParams{}, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "invalid_colorchecker_buffer");
    WorkingImage wrong_model = input;
    wrong_model.color_profile.model = ColorModel::kLab;
    rejected = apply_color_checker(wrong_model, ColorCheckerParams{}, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "unsupported_colorchecker_working_space");
    WorkingImage wrong_profile = input;
    wrong_profile.color_profile.identifier = "srgb";
    rejected = apply_color_checker(wrong_profile, ColorCheckerParams{}, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "unsupported_colorchecker_working_space");
    for (const float invalid :
         {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
          -std::numeric_limits<float>::infinity()})
    {
        WorkingImage nonfinite = input;
        nonfinite.rgb[1] = invalid;
        const auto source = nonfinite.rgb;
        rejected = apply_color_checker(nonfinite, ColorCheckerParams{}, CancellationToken{});
        ASSERT_FALSE(rejected);
        EXPECT_EQ(rejected.error().context.at("reason"), "nonfinite_colorchecker_input");
        ASSERT_EQ(nonfinite.rgb.size(), source.size());
        for (std::size_t index = 0U; index < source.size(); ++index)
        {
            EXPECT_EQ(std::bit_cast<std::uint32_t>(nonfinite.rgb[index]),
                      std::bit_cast<std::uint32_t>(source[index]));
        }
    }

    ColorCheckerParams invalid_params;
    invalid_params.patches[0].target_lab[1] = std::numeric_limits<double>::infinity();
    auto invalid_fit =
        apply_color_checker_lab(invalid_params, {50.0F, 0.0F, 0.0F}, CancellationToken{});
    ASSERT_FALSE(invalid_fit);
    ColorCheckerParams zero_denominator{{{{{1.0, 0.0, 2.0}}, {{2.0, 1.0, 4.0}}}}};
    invalid_fit =
        apply_color_checker_lab(zero_denominator, {1.0F, 2.0F, 3.0F}, CancellationToken{});
    ASSERT_FALSE(invalid_fit);
    EXPECT_EQ(invalid_fit.error().context.at("reason"), "invalid_colorchecker_denominator");

    CancellationSource pre_cancelled;
    ASSERT_TRUE(pre_cancelled.cancel("colorchecker-pre"));
    rejected = apply_color_checker(input, ColorCheckerParams{}, pre_cancelled.token());
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kCancelled);
    EXPECT_EQ(input.width, original.width);
    EXPECT_EQ(input.height, original.height);
    EXPECT_EQ(input.rgb, original.rgb);
    EXPECT_EQ(input.color_profile, original.color_profile);
    EXPECT_EQ(input.exposure_analysis, original.exposure_analysis);

    auto canonical = color_checker_to_parameters(ColorCheckerParams{});
    ASSERT_TRUE(canonical) << canonical.error().message;
    OperationInstance masked{std::string(kColorCheckerOperationId),
                             kColorCheckerOperationSchemaVersion,
                             "colorchecker-mask",
                             true,
                             std::move(canonical).value(),
                             "mask-1"};
    rejected = apply_color_checker(input, masked, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "colorchecker_mask_graph_unavailable");
    EXPECT_EQ(input.width, original.width);
    EXPECT_EQ(input.height, original.height);
    EXPECT_EQ(input.rgb, original.rgb);
    EXPECT_EQ(input.color_profile, original.color_profile);
    EXPECT_EQ(input.exposure_analysis, original.exposure_analysis);
}

TEST(D50LabBridgeTest, MatricesAndD50WhiteBlackMatchFrozenBitGoldens)
{
    struct MatrixCase
    {
        FrozenD50Triplet input;
        std::array<std::uint32_t, 3> forward;
        std::array<std::uint32_t, 3> inverse;
    };
    const std::array cases{
        MatrixCase{{1.0F, 0.0F, 0.0F},
                   {0x3edf452fU, 0x3e63d838U, 0x3c6443e2U},
                   {0x40489119U, 0xbf7a9091U, 0x3d93580fU}},
        MatrixCase{{0.0F, 1.0F, 0.0F},
                   {0x3ec5273aU, 0x3f37855bU, 0x3dc6deb9U},
                   {0xbfcef57dU, 0x3ff54420U, 0xbe6a7cb9U}},
        MatrixCase{{0.0F, 0.0F, 1.0F},
                   {0x3e1283abU, 0x3d78496dU, 0x3f36d410U},
                   {0xbefb31d6U, 0x3d090710U, 0x3fb3defeU}},
    };
    for (const auto &[input, forward, inverse] : cases)
    {
        expect_frozen_d50_bits(d50_lab::linear_rec709_to_xyz(input),
                               frozen_linear_rec709_to_xyz_d50(input), forward);
        expect_frozen_d50_bits(d50_lab::xyz_to_linear_rec709(input),
                               frozen_xyz_d50_to_linear_rec709(input), inverse);
    }

    constexpr FrozenD50Triplet black_xyz{0.0F, 0.0F, 0.0F};
    constexpr FrozenD50Triplet d50_white{0.9642F, 1.0F, 0.8249F};
    constexpr FrozenD50Triplet black_lab{0.0F, 0.0F, 0.0F};
    constexpr FrozenD50Triplet white_lab{100.0F, 0.0F, 0.0F};
    expect_frozen_d50_bits(d50_lab::xyz_to_lab(black_xyz), frozen_xyz_d50_to_lab(black_xyz),
                           {0x00000000U, 0x00000000U, 0x80000000U});
    expect_frozen_d50_bits(d50_lab::xyz_to_lab(d50_white), frozen_xyz_d50_to_lab(d50_white),
                           {0x42c80000U, 0x00000000U, 0x80000000U});
    expect_frozen_d50_bits(d50_lab::lab_to_xyz(black_lab), frozen_lab_to_xyz_d50(black_lab),
                           {0x00000000U, 0x00000000U, 0x00000000U});
    expect_frozen_d50_bits(d50_lab::lab_to_xyz(white_lab), frozen_lab_to_xyz_d50(white_lab),
                           {0x3f76d5d0U, 0x3f800000U, 0x3f532ca5U});
}

TEST(D50LabBridgeTest, XyzToLabFreezesEpsilonAndReciprocalMultiplyOrder)
{
    constexpr float epsilon = 216.0F / 24389.0F;
    struct BranchCase
    {
        float y;
        std::array<std::uint32_t, 3> expected;
    };
    const std::array cases{
        BranchCase{epsilon * 0.99F, {0x40fd70a8U, 0xc2088d41U, 0x415a7b9bU}},
        BranchCase{epsilon, {0x41000000U, 0xc209ee59U, 0x415cb08fU}},
        BranchCase{epsilon * 1.01F, {0x41014698U, 0xc20b4e47U, 0x415ee3a5U}},
    };
    for (const auto &[y, expected] : cases)
    {
        const FrozenD50Triplet xyz{0.0F, y, 0.0F};
        expect_frozen_d50_cbrt_reference(d50_lab::xyz_to_lab(xyz), frozen_xyz_d50_to_lab(xyz),
                                         expected);
    }

    constexpr FrozenD50Triplet rgb{0.1938238604679151F, 0.36766030739017674F, 0.38827863670090734F};
    const auto xyz = frozen_linear_rec709_to_xyz_d50(rgb);
    const auto expected = frozen_xyz_d50_to_lab(xyz);
    expect_frozen_d50_cbrt_reference(d50_lab::xyz_to_lab(d50_lab::linear_rec709_to_xyz(rgb)),
                                     expected, {0x42805bf3U, 0xc15d8c10U, 0xc0deecf2U});
}

TEST(D50LabBridgeTest, LabToXyzFreezesInverseThresholdAndScaleMultiplyOrder)
{
    struct BranchCase
    {
        float lightness;
        std::array<std::uint32_t, 3> expected;
    };
    const std::array cases{
        BranchCase{7.99F, {0x3c0bbc07U, 0x3c10ec37U, 0x3bef17efU}},
        BranchCase{8.0F, {0x3c0be8cdU, 0x3c111aa6U, 0x3bef648aU}},
        BranchCase{8.01F, {0x3c0c1598U, 0x3c11491bU, 0x3befb12fU}},
    };
    for (const auto &[lightness, expected] : cases)
    {
        const FrozenD50Triplet lab{lightness, 0.0F, 0.0F};
        expect_frozen_d50_bits(d50_lab::lab_to_xyz(lab), frozen_lab_to_xyz_d50(lab), expected);
    }

    constexpr FrozenD50Triplet lab{50.0F, 20.0F, -30.0F};
    const auto expected = frozen_lab_to_xyz_d50(lab);
    expect_frozen_d50_bits(d50_lab::lab_to_xyz(lab), expected,
                           {0x3e5ef828U, 0x3e3c9b63U, 0x3e9cf659U});

    constexpr FrozenD50Triplet d50{0.9642F, 1.0F, 0.8249F};
    constexpr float threshold = 0.20689655172413796F;
    constexpr float kappa = 24389.0F / 27.0F;
    const float fy = (lab[0] + 16.0F) / 116.0F;
    const FrozenD50Triplet divided{fy + lab[1] / 500.0F, fy, fy - lab[2] / 200.0F};
    FrozenD50Triplet divide_perturbation{};
    for (std::size_t channel = 0U; channel < divide_perturbation.size(); ++channel)
    {
        const float value = divided[channel] > threshold ?
                                divided[channel] * divided[channel] * divided[channel] :
                                (116.0F * divided[channel] - 16.0F) / kappa;
        divide_perturbation[channel] = d50[channel] * value;
    }
    EXPECT_EQ(d50_triplet_bits(divide_perturbation),
              (std::array<std::uint32_t, 3>{0x3e5ef828U, 0x3e3c9b63U, 0x3e9cf65cU}));
    EXPECT_NE(d50_triplet_bits(divide_perturbation), d50_triplet_bits(expected));
}

TEST(D50LabBridgeTest, ExtendedRoundTripsAndNonFiniteValuesPreserveFrozenClassification)
{
    constexpr FrozenD50Triplet extended_rgb{-0.25F, 0.5F, 1.75F};
    const auto expected_lab = frozen_xyz_d50_to_lab(frozen_linear_rec709_to_xyz_d50(extended_rgb));
    expect_frozen_d50_bits(d50_lab::xyz_to_lab(d50_lab::linear_rec709_to_xyz(extended_rgb)),
                           expected_lab, {0x428c3252U, 0xc19ff315U, 0xc2a7fbbeU});

    constexpr FrozenD50Triplet extended_lab{-25.0F, 120.0F, -90.0F};
    expect_frozen_d50_bits(d50_lab::lab_to_xyz(extended_lab), frozen_lab_to_xyz_d50(extended_lab),
                           {0x3b46abe5U, 0xbce2b9a4U, 0x3d2e846eU});

    struct RoundTripCase
    {
        FrozenD50Triplet input;
        std::array<std::uint32_t, 3> expected;
    };
    const std::array cases{
        RoundTripCase{{0.25F, 0.5F, 0.75F}, {0x3e800006U, 0x3efffffcU, 0x3f400000U}},
        RoundTripCase{extended_rgb, {0xbe7ffffaU, 0x3efffffcU, 0x3fe00002U}},
    };
    for (const auto &[input, expected] : cases)
    {
        const auto oracle = frozen_xyz_d50_to_linear_rec709(
            frozen_lab_to_xyz_d50(frozen_xyz_d50_to_lab(frozen_linear_rec709_to_xyz_d50(input))));
        const auto actual = d50_lab::xyz_to_linear_rec709(
            d50_lab::lab_to_xyz(d50_lab::xyz_to_lab(d50_lab::linear_rec709_to_xyz(input))));
        expect_frozen_d50_cbrt_reference(actual, oracle, expected);
    }

    const std::array nonfinite{std::numeric_limits<float>::quiet_NaN(),
                               std::numeric_limits<float>::infinity(),
                               -std::numeric_limits<float>::infinity()};
    const auto all_nonfinite = [](const FrozenD50Triplet &value)
    {
        return std::ranges::all_of(value,
                                   [](const float sample) { return !std::isfinite(sample); });
    };
    for (const float sample : nonfinite)
    {
        const FrozenD50Triplet value{sample, sample, sample};
        EXPECT_TRUE(all_nonfinite(d50_lab::linear_rec709_to_xyz(value)));
        EXPECT_TRUE(all_nonfinite(d50_lab::xyz_to_linear_rec709(value)));
        EXPECT_TRUE(all_nonfinite(d50_lab::xyz_to_lab(value)));
        EXPECT_TRUE(all_nonfinite(d50_lab::lab_to_xyz(value)));
    }
}


} // namespace
} // namespace ravo
