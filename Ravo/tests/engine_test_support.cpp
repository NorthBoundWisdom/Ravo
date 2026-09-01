#include "engine_test_support.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <png.h>

#include "ravo/recipe/color_input.h"
#include "ravo/recipe/color_output.h"

#include "color_balance_fixture.h"
#include "color_balance_rgb.h"
#include "input_color.h"
#include "raw_temperature.h"

namespace ravo::engine_test_support
{

[[nodiscard]] std::string mire1_path()
{
    const auto path = std::filesystem::path(RAVO_REPOSITORY_ROOT) / "Ravo" / "tests" / "fixtures" /
                      "frozen" / "images" / "mire1.cr2";
    const auto utf8 = path.generic_u8string();
    return {utf8.begin(), utf8.end()};
}

[[nodiscard]] std::string mire1_xtrans_path()
{
    const auto path = std::filesystem::path(RAVO_REPOSITORY_ROOT) / "Ravo" / "tests" / "fixtures" /
                      "frozen" / "images" / "mire1-xtrans.raf";
    const auto utf8 = path.generic_u8string();
    return {utf8.begin(), utf8.end()};
}

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
                  RawExposureMetadata metadata)
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
                                                        std::string instance_id)
{
    return {"ravo.color.channelmixerrgb",        1,           std::move(instance_id), true,
            channel_mixer_to_parameters(params), std::nullopt};
}

[[nodiscard]] OperationInstance color_balance_rgb_operation(const ColorBalanceRgbParams &params,
                                                            std::string instance_id)
{
    return {"ravo.color.colorbalancergb",
            1,
            std::move(instance_id),
            true,
            color_balance_rgb_to_parameters(params),
            std::nullopt};
}

[[nodiscard]] OperationInstance legacy_color_balance_operation(const ColorBalanceParams &params,
                                                               std::string instance_id)
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

[[nodiscard]] OperationInstance temperature_operation(const TemperatureParams &params,
                                                      std::string instance_id)
{
    return {"ravo.color.temperature",          1,           std::move(instance_id), true,
            temperature_to_parameters(params), std::nullopt};
}

[[nodiscard]] OperationInstance hot_pixels_operation(const bool permissive)
{
    return {"ravo.raw.hotpixels",
            1,
            "hotpixels-1",
            true,
            {{"strength", ParameterValue{0.25}},
             {"threshold", ParameterValue{0.05}},
             {"permissive", ParameterValue{permissive}}},
            std::nullopt};
}

[[nodiscard]] OperationInstance raw_ca_operation(const std::int64_t iterations,
                                                 const bool avoid_color_shift)
{
    return {"ravo.raw.cacorrect",
            1,
            "cacorrect-1",
            true,
            {{"iterations", ParameterValue{iterations}},
             {"avoid_color_shift", ParameterValue{avoid_color_shift}}},
            std::nullopt};
}

[[nodiscard]] DecodedRaw synthetic_bayer_raw()
{
    DecodedRaw raw;
    raw.width = 9;
    raw.height = 9;
    raw.cfa_width = 2;
    raw.cfa_height = 2;
    raw.black_level = 0;
    raw.white_level = 1000;
    raw.has_as_shot_white_balance = true;
    raw.has_camera_reference_white_balance = true;
    declare_linear_srgb_matrix(raw);
    raw.cfa_channels = {0, 1, 1, 2};
    raw.pixels.assign(static_cast<std::size_t>(raw.width) * raw.height, 100);
    return raw;
}

} // namespace ravo::engine_test_support
