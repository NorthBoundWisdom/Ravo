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

namespace ravo
{
using namespace image_ops_internal;

Result<AlphaPlane> apply_recipe_geometry_to_alpha(AlphaPlane alpha, const Recipe &recipe,
                                                  const CancellationToken &cancellation)
try
{
    auto active = cancellation.check();
    if (!active)
        return active.error();
    const std::uint64_t pixels = static_cast<std::uint64_t>(alpha.width) * alpha.height;
    if (alpha.width == 0U || alpha.height == 0U || pixels != alpha.alpha.size() ||
        pixels > std::vector<float>{}.max_size() / 3U)
    {
        return make_error(ErrorCode::kValidation, "Mask overlay alpha plane is invalid",
                          {{"reason", "invalid_mask_overlay_alpha"}});
    }

    WorkingImage image;
    image.width = alpha.width;
    image.height = alpha.height;
    image.rgb.resize(static_cast<std::size_t>(pixels) * 3U);
    for (std::uint32_t row = 0U; row < alpha.height; ++row)
    {
        active = cancellation.check();
        if (!active)
            return active.error();
        for (std::uint32_t column = 0U; column < alpha.width; ++column)
        {
            const std::size_t pixel = static_cast<std::size_t>(row) * alpha.width + column;
            const float value = alpha.alpha[pixel];
            if (!std::isfinite(value))
                return make_error(ErrorCode::kValidation,
                                  "Mask overlay alpha contains NaN or infinity",
                                  {{"reason", "nonfinite_mask_overlay_alpha"},
                                   {"sample_index", std::to_string(pixel)}});
            image.rgb[pixel * 3U] = value;
            image.rgb[pixel * 3U + 1U] = value;
            image.rgb[pixel * 3U + 2U] = value;
        }
    }

    std::size_t replay_begin = 0U;
    for (std::size_t index = 0U; index < recipe.operations.size(); ++index)
    {
        if (recipe.operations[index].enabled && recipe.operations[index].id == kCanvasOperationId)
            replay_begin = index + 1U;
    }
    for (std::size_t index = replay_begin; index < recipe.operations.size(); ++index)
    {
        const auto &operation = recipe.operations[index];
        active = cancellation.check();
        if (!active)
            return active.error();
        if (!operation.enabled)
            continue;
        if (operation.id == "ravo.geometry.rotate")
        {
            auto transformed = rotate_working(
                std::move(image), static_cast<int>(parameter(operation, "quarters", 0.0)));
            if (!transformed)
                return transformed.error();
            image = std::move(transformed).value();
        }
        else if (operation.id == "ravo.geometry.flip")
        {
            auto transformed =
                flip_working(std::move(image), parameter(operation, "horizontal", 0.0) != 0.0,
                             parameter(operation, "vertical", 0.0) != 0.0);
            if (!transformed)
                return transformed.error();
            image = std::move(transformed).value();
        }
        else if (operation.id == "ravo.geometry.straighten")
        {
            auto transformed =
                straighten_working(std::move(image), parameter(operation, "degrees", 0.0));
            if (!transformed)
                return transformed.error();
            image = std::move(transformed).value();
        }
        else if (operation.id == kPerspectiveOperationId)
        {
            auto params = perspective_from_parameters(operation.parameters);
            if (!params)
                return params.error();
            params.value().interpolation = std::string(kPerspectiveInterpolationBilinear);
            auto transformed = apply_perspective(image, params.value(), cancellation);
            if (!transformed)
                return transformed.error();
            image = std::move(transformed).value();
        }
        else if (operation.id == "ravo.geometry.crop")
        {
            auto transformed = crop_working(
                std::move(image), parameter(operation, "x", 0.0), parameter(operation, "y", 0.0),
                parameter(operation, "width", 1.0), parameter(operation, "height", 1.0));
            if (!transformed)
                return transformed.error();
            image = std::move(transformed).value();
        }
    }

    AlphaPlane output;
    output.width = image.width;
    output.height = image.height;
    output.alpha.resize(static_cast<std::size_t>(image.width) * image.height);
    for (std::uint32_t row = 0U; row < image.height; ++row)
    {
        active = cancellation.check();
        if (!active)
            return active.error();
        for (std::uint32_t column = 0U; column < image.width; ++column)
        {
            const std::size_t pixel = static_cast<std::size_t>(row) * image.width + column;
            const float value = image.rgb[pixel * 3U];
            if (!std::isfinite(value))
                return make_error(ErrorCode::kValidation,
                                  "Transformed mask overlay alpha is non-finite",
                                  {{"reason", "nonfinite_mask_overlay_alpha"},
                                   {"sample_index", std::to_string(pixel)}});
            output.alpha[pixel] = std::clamp(value, 0.0F, 1.0F);
        }
    }
    return output;
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Mask overlay geometry allocation failed",
                      {{"reason", "allocation_failed"}});
}

Result<std::vector<std::uint8_t>> encode_png_bytes(const RenderedImage &image, const bool fast)
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
    png.flags = (standard_srgb ? 0U : PNG_IMAGE_FLAG_COLORSPACE_NOT_sRGB) |
                (fast ? PNG_IMAGE_FLAG_FAST : 0U);
    const png_alloc_size_t capacity = PNG_IMAGE_PNG_SIZE_MAX(png);
    if (capacity == 0U || static_cast<std::size_t>(capacity) < image.rgb.size())
    {
        return make_error(ErrorCode::kValidation, "PNG output bound is invalid");
    }
    std::vector<std::uint8_t> encoded(static_cast<std::size_t>(capacity));
    png_alloc_size_t encoded_size = capacity;
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
