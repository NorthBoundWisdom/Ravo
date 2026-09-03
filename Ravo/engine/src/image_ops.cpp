#include "image_ops.h"

#include "bayer_demosaic.h"
#include "dng_opcodes.h"
#include "xtrans_demosaic.h"

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
#include <vector>

#include <png.h>
#include <zlib.h>

#include "capability_ops.h"
#include "canvas_frame.h"
#include "color_balance_rgb.h"
#include "color_contrast.h"
#include "color_correction.h"
#include "color_checker.h"
#include "color_harmonizer.h"
#include "color_reconstruction.h"
#include "color_zones.h"
#include "d50_lab.h"
#include "dehaze.h"
#include "gpu_adapter.h"
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

using detail::for_each_row;

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

Result<WorkingImage> apply_exposure_gpu(const WorkingImage &input, const ExposureParams &params,
                                        const GpuAdapter &gpu,
                                        const CancellationToken &cancellation)
try
{
    auto affine = prepare_exposure_affine(input, params, cancellation);
    if (!affine)
    {
        return affine.error();
    }
    const auto scale = static_cast<float>(affine.value().scale);
    const auto black = static_cast<float>(affine.value().black);
    if (!std::isfinite(scale) || !std::isfinite(black))
    {
        return make_error(ErrorCode::kValidation, "Exposure scale is not representable",
                          {{"reason", "invalid_exposure_denominator"}});
    }
    WorkingImage output;
    output.width = input.width;
    output.height = input.height;
    output.color_profile = input.color_profile;
    output.exposure_analysis = input.exposure_analysis;
    output.canonical_roi_scale = input.canonical_roi_scale;
    output.mask_attached_frame = input.mask_attached_frame;
    output.rgb.resize(input.rgb.size());
    auto applied = gpu.apply_affine_rgb(input.rgb, output.rgb, scale, black, cancellation);
    if (!applied)
    {
        return applied.error();
    }
    for (std::size_t index = 0; index < output.rgb.size(); ++index)
    {
        if (!std::isfinite(output.rgb[index]))
        {
            return make_error(ErrorCode::kValidation, "Exposure produced an unrepresentable sample",
                              {{"reason", "unrepresentable_exposure_sample"},
                               {"sample_index", std::to_string(index)}});
        }
    }
    return output;
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
    output.canonical_roi_scale = input.canonical_roi_scale;
    output.mask_attached_frame = input.mask_attached_frame;
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
    const std::string_view mode = raw.cfa_width == 6U && raw.cfa_height == 6U ?
                                      kXTransDemosaicModeMarkesteijn3 :
                                      kBayerDemosaicModeRcd;
    return working_from_raw(raw, width, height, white_balance, mode, cancellation);
}

Result<WorkingImage> working_from_raw(const DecodedRaw &raw, const std::uint32_t width,
                                      const std::uint32_t height,
                                      const std::array<float, 4> &white_balance,
                                      const std::string_view demosaic_mode,
                                      const CancellationToken &cancellation)
{
    if (width == 0 || height == 0)
    {
        return make_error(ErrorCode::kInvalidArgument, "Render output dimensions must be non-zero");
    }
    const bool bayer = raw.cfa_width == 2U && raw.cfa_height == 2U;
    const bool xtrans = raw.cfa_width == 6U && raw.cfa_height == 6U;
    if (!bayer && !xtrans)
    {
        return make_error(ErrorCode::kUnsupported,
                          "RAW demosaic requires a supported Bayer or X-Trans CFA",
                          {{"reason", "unsupported_raw_sensor"}});
    }
    const int turns = normalized_rotate_quarters(raw.rotate_quarters);
    std::uint32_t original_display_width = raw.width;
    std::uint32_t original_display_height = raw.height;
    apply_display_rotation_to_size(original_display_width, original_display_height, turns);
    std::uint32_t demosaic_width = width;
    std::uint32_t demosaic_height = height;
    apply_display_rotation_to_size(demosaic_width, demosaic_height, turns);
    const bool defer_white_balance = dng_list3_requires_deferred_white_balance(raw.dng_opcodes);
    Result<WorkingImage> image =
        make_error(ErrorCode::kUnsupported, "RAW demosaic mode does not match the sensor",
                   {{"reason", "demosaic_sensor_mismatch"}});
    if (bayer)
    {
        auto mode = parse_bayer_demosaic_mode(demosaic_mode);
        if (!mode)
        {
            if (demosaic_mode == kXTransDemosaicModeMarkesteijn1 ||
                demosaic_mode == kXTransDemosaicModeMarkesteijn3)
            {
                return make_error(ErrorCode::kUnsupported,
                                  "Markesteijn demosaic requires an X-Trans 6x6 CFA",
                                  {{"demosaic_mode", std::string(demosaic_mode)},
                                   {"reason", "demosaic_sensor_mismatch"},
                                   {"sensor", "bayer"}});
            }
            return mode.error();
        }
        image = demosaic_bayer(raw, demosaic_width, demosaic_height, white_balance, mode.value(),
                               cancellation);
    }
    else
    {
        auto mode = parse_xtrans_demosaic_mode(demosaic_mode);
        if (!mode)
        {
            if (demosaic_mode == kBayerDemosaicModeRcd || demosaic_mode == kBayerDemosaicModePpg)
            {
                return make_error(ErrorCode::kUnsupported,
                                  "RCD and PPG demosaic require a Bayer 2x2 CFA",
                                  {{"demosaic_mode", std::string(demosaic_mode)},
                                   {"reason", "demosaic_sensor_mismatch"},
                                   {"sensor", "xtrans"}});
            }
            return mode.error();
        }
        image = demosaic_xtrans(raw, demosaic_width, demosaic_height, white_balance, mode.value(),
                                cancellation);
    }
    if (!image)
        return image.error();
    auto corrected = raw.dng_opcodes ? apply_dng_opcode_list3(std::move(image).value(),
                                                              *raw.dng_opcodes, cancellation) :
                                       Result<WorkingImage>(std::move(image).value());
    if (!corrected)
    {
        return corrected.error();
    }
    if (defer_white_balance)
    {
        auto rows = for_each_row(
            corrected.value().height, cancellation,
            [&](const std::uint32_t y)
            {
                for (std::uint32_t x = 0U; x < corrected.value().width; ++x)
                {
                    const std::size_t base =
                        (static_cast<std::size_t>(y) * corrected.value().width + x) * 3U;
                    corrected.value().rgb[base] *= white_balance[0];
                    corrected.value().rgb[base + 1U] *= white_balance[1];
                    corrected.value().rgb[base + 2U] *= white_balance[2];
                }
            });
        if (!rows)
        {
            return rows.error();
        }
    }
    auto oriented = turns == 0 ? Result<WorkingImage>(std::move(corrected).value()) :
                                 rotate_working(std::move(corrected).value(), turns);
    if (!oriented)
    {
        return oriented.error();
    }
    // width/height callers follow the existing display-oriented render
    // contract.  Establish the scale from the actual final buffer so a quarter
    // turn cannot accidentally validate the pre-rotation geometry instead.
    oriented.value().canonical_roi_scale =
        CanonicalRoiScale::from_scaled_dimensions(oriented.value().width, oriented.value().height,
                                                  original_display_width, original_display_height);
    return oriented;
}

Result<WorkingImage> working_from_raw_window(const DecodedRaw &raw, const std::uint32_t origin_x,
                                             const std::uint32_t origin_y,
                                             const std::uint32_t width, const std::uint32_t height,
                                             const std::array<float, 4> &white_balance,
                                             const std::string_view demosaic_mode,
                                             const CancellationToken &cancellation,
                                             const GpuAdapter *gpu)
{
    if (width == 0 || height == 0)
    {
        return make_error(ErrorCode::kInvalidArgument, "RAW window dimensions must be non-zero");
    }
    const bool bayer = raw.cfa_width == 2U && raw.cfa_height == 2U;
    if (!bayer)
    {
        return make_error(ErrorCode::kUnsupported, "Preview ROI demosaic requires a Bayer CFA",
                          {{"reason", "preview_roi_sensor_unsupported"}});
    }
    auto mode = parse_bayer_demosaic_mode(demosaic_mode);
    if (!mode)
    {
        return mode.error();
    }
    const bool defer_white_balance = dng_list3_requires_deferred_white_balance(raw.dng_opcodes);
    auto image = demosaic_bayer_window(raw, origin_x, origin_y, width, height, white_balance,
                                       mode.value(), cancellation, gpu);
    if (!image)
    {
        return image.error();
    }
    auto corrected = raw.dng_opcodes ? apply_dng_opcode_list3(std::move(image).value(),
                                                              *raw.dng_opcodes, cancellation) :
                                       Result<WorkingImage>(std::move(image).value());
    if (!corrected)
    {
        return corrected.error();
    }
    if (defer_white_balance)
    {
        auto rows = for_each_row(
            corrected.value().height, cancellation,
            [&](const std::uint32_t y)
            {
                for (std::uint32_t x = 0U; x < corrected.value().width; ++x)
                {
                    const std::size_t base =
                        (static_cast<std::size_t>(y) * corrected.value().width + x) * 3U;
                    corrected.value().rgb[base] *= white_balance[0];
                    corrected.value().rgb[base + 1U] *= white_balance[1];
                    corrected.value().rgb[base + 2U] *= white_balance[2];
                }
            });
        if (!rows)
        {
            return rows.error();
        }
    }
    const int turns = normalized_rotate_quarters(raw.rotate_quarters);
    auto oriented = turns == 0 ? Result<WorkingImage>(std::move(corrected).value()) :
                                 rotate_working(std::move(corrected).value(), turns);
    if (!oriented)
    {
        return oriented.error();
    }
    oriented.value().canonical_roi_scale =
        CanonicalRoiScale::from_scaled_dimensions(oriented.value().width, oriented.value().height,
                                                  oriented.value().width, oriented.value().height);
    return oriented;
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
    image.canonical_roi_scale = CanonicalRoiScale::from_scaled_dimensions(
        raster.width, raster.height, raster.source_width, raster.source_height);
    for (std::size_t index = 0; index < image.rgb.size(); ++index)
    {
        image.rgb[index] = static_cast<float>(raster.srgb[index]) / 255.0F;
    }
    return image;
}

Result<WorkingImage> scale_working_image(const WorkingImage &input, const std::uint32_t width,
                                         const std::uint32_t height,
                                         const std::uint32_t original_width,
                                         const std::uint32_t original_height,
                                         const CancellationToken &cancellation)
try
{
    auto cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    if (width == 0U || height == 0U || width > std::numeric_limits<std::size_t>::max() / height)
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "Linear working scale dimensions must be non-zero and bounded",
                          {{"reason", "invalid_linear_working_scale"}});
    }
    const std::size_t expected = static_cast<std::size_t>(input.width) * input.height * 3U;
    if (input.width == 0U || input.height == 0U || input.rgb.size() != expected)
    {
        return make_error(ErrorCode::kValidation, "Linear working buffer is empty or undersized",
                          {{"reason", "invalid_linear_working_buffer"}});
    }
    if (width > input.width || height > input.height)
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "Linear working scale cannot enlarge the source",
                          {{"reason", "linear_working_upscale_unsupported"},
                           {"source_width", std::to_string(input.width)},
                           {"source_height", std::to_string(input.height)},
                           {"width", std::to_string(width)},
                           {"height", std::to_string(height)}});
    }

    WorkingImage output;
    output.width = width;
    output.height = height;
    output.color_profile = input.color_profile;
    output.exposure_analysis = input.exposure_analysis;
    output.canonical_roi_scale =
        CanonicalRoiScale::from_scaled_dimensions(width, height, original_width, original_height);
    if (input.mask_attached_frame.has_value())
    {
        const auto &frame = *input.mask_attached_frame;
        AttachedPixelFrame scaled;
        scaled.x = frame.x * width / input.width;
        scaled.y = frame.y * height / input.height;
        scaled.width = std::max(1U, frame.width * width / input.width);
        scaled.height = std::max(1U, frame.height * height / input.height);
        if (scaled.x >= width)
        {
            scaled.x = width - 1U;
        }
        if (scaled.y >= height)
        {
            scaled.y = height - 1U;
        }
        scaled.width = std::min(scaled.width, width - scaled.x);
        scaled.height = std::min(scaled.height, height - scaled.y);
        output.mask_attached_frame = scaled;
    }
    output.rgb.resize(static_cast<std::size_t>(width) * height * 3U);
    if (width == input.width && height == input.height)
    {
        output.rgb = input.rgb;
        return output;
    }

    std::atomic_bool invalid_sample{false};
    const auto rows = detail::for_each_row(
        height, cancellation,
        [&](const std::uint32_t output_y)
        {
            if (invalid_sample.load(std::memory_order_relaxed))
            {
                return;
            }
            const std::uint32_t source_top = static_cast<std::uint32_t>(
                static_cast<std::uint64_t>(output_y) * input.height / height);
            const std::uint32_t source_bottom = std::max(
                source_top + 1U,
                static_cast<std::uint32_t>(
                    (static_cast<std::uint64_t>(output_y + 1U) * input.height + height - 1U) /
                    height));
            const std::uint32_t y_end = std::min(source_bottom, input.height);
            for (std::uint32_t output_x = 0U; output_x < width; ++output_x)
            {
                const std::uint32_t source_left = static_cast<std::uint32_t>(
                    static_cast<std::uint64_t>(output_x) * input.width / width);
                const std::uint32_t source_right = std::max(
                    source_left + 1U,
                    static_cast<std::uint32_t>(
                        (static_cast<std::uint64_t>(output_x + 1U) * input.width + width - 1U) /
                        width));
                const std::uint32_t x_end = std::min(source_right, input.width);
                double sum_r = 0.0;
                double sum_g = 0.0;
                double sum_b = 0.0;
                std::uint32_t count = 0U;
                for (std::uint32_t source_y = source_top; source_y < y_end; ++source_y)
                {
                    const std::size_t row =
                        (static_cast<std::size_t>(source_y) * input.width + source_left) * 3U;
                    for (std::uint32_t source_x = source_left; source_x < x_end; ++source_x)
                    {
                        const std::size_t base =
                            row + static_cast<std::size_t>(source_x - source_left) * 3U;
                        sum_r += static_cast<double>(input.rgb[base]);
                        sum_g += static_cast<double>(input.rgb[base + 1U]);
                        sum_b += static_cast<double>(input.rgb[base + 2U]);
                        ++count;
                    }
                }
                if (count == 0U)
                {
                    invalid_sample.store(true, std::memory_order_relaxed);
                    return;
                }
                const auto dest = (static_cast<std::size_t>(output_y) * width + output_x) * 3U;
                output.rgb[dest] = static_cast<float>(sum_r / count);
                output.rgb[dest + 1U] = static_cast<float>(sum_g / count);
                output.rgb[dest + 2U] = static_cast<float>(sum_b / count);
            }
        });
    if (!rows)
    {
        return rows.error();
    }
    if (invalid_sample.load(std::memory_order_relaxed))
    {
        return make_error(ErrorCode::kValidation,
                          "Linear working scale produced an empty source bin",
                          {{"reason", "empty_linear_working_scale_bin"}});
    }
    return output;
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Linear working scale allocation failed",
                      {{"reason", "allocation_failed"}});
}

[[nodiscard]] Result<AlphaPlane> evaluate_operation_mask(const WorkingImage &input,
                                                         const WorkingImage &operation_output,
                                                         const Recipe &recipe,
                                                         const std::string_view mask_id,
                                                         const CancellationToken &cancellation)
{
    if (input.width == 0U || input.height == 0U || operation_output.width != input.width ||
        operation_output.height != input.height ||
        input.rgb.size() != operation_output.rgb.size() ||
        input.width > std::numeric_limits<std::uint32_t>::max() / 3U)
    {
        return make_error(
            ErrorCode::kValidation, "Masked operation buffers are incompatible",
            {{"reason", "invalid_masked_operation_buffers"}, {"mask_id", std::string(mask_id)}});
    }
    const std::uint32_t stride = input.width * 3U;
    MaskEvaluationRequest request{
        .full_width = input.width,
        .full_height = input.height,
        .roi_x = 0U,
        .roi_y = 0U,
        .roi_width = input.width,
        .roi_height = input.height,
        .input = MaskRgbPlaneView{input.rgb, stride},
        .operation_output = MaskRgbPlaneView{operation_output.rgb, stride},
        .attached_frame = input.mask_attached_frame,
        .cancellation = cancellation,
    };
    return evaluate_canonical_mask(recipe.masks, mask_id, request);
}

[[nodiscard]] Result<WorkingImage>
apply_masked_color_harmonizer(WorkingImage image, const Recipe &recipe,
                              const OperationInstance &operation,
                              const CancellationToken &cancellation)
try
{
    WorkingImage pre_operation = std::move(image);
    OperationInstance unmasked = operation;
    unmasked.mask_id.reset();
    auto operation_output = apply_color_harmonizer(pre_operation, unmasked, cancellation);
    if (!operation_output)
    {
        return operation_output.error();
    }
    auto alpha = evaluate_operation_mask(pre_operation, operation_output.value(), recipe,
                                         *operation.mask_id, cancellation);
    if (!alpha)
    {
        return alpha.error();
    }
    auto mixed = normal_mask_mix(pre_operation.rgb, operation_output.value().rgb, alpha.value(),
                                 cancellation);
    if (!mixed)
    {
        return mixed.error();
    }
    return std::move(operation_output).value();
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Masked Color Harmonizer allocation failed",
                      {{"operation_id", operation.id}, {"reason", "allocation_failed"}});
}

[[nodiscard]] Result<WorkingImage> apply_masked_color_zones(WorkingImage image,
                                                            const Recipe &recipe,
                                                            const OperationInstance &operation,
                                                            const CancellationToken &cancellation)
try
{
    WorkingImage pre_operation = std::move(image);
    OperationInstance unmasked = operation;
    unmasked.mask_id.reset();
    auto operation_output = apply_color_zones(pre_operation, unmasked, cancellation);
    if (!operation_output)
        return operation_output.error();
    auto alpha = evaluate_operation_mask(pre_operation, operation_output.value(), recipe,
                                         *operation.mask_id, cancellation);
    if (!alpha)
        return alpha.error();
    auto mixed = normal_mask_mix(pre_operation.rgb, operation_output.value().rgb, alpha.value(),
                                 cancellation);
    if (!mixed)
        return mixed.error();
    return std::move(operation_output).value();
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Masked Color Zones allocation failed",
                      {{"operation_id", operation.id}, {"reason", "allocation_failed"}});
}

[[nodiscard]] Result<WorkingImage> apply_masked_monochrome(WorkingImage image, const Recipe &recipe,
                                                           const OperationInstance &operation,
                                                           const CancellationToken &cancellation)
try
{
    WorkingImage pre_operation = std::move(image);
    OperationInstance unmasked = operation;
    unmasked.mask_id.reset();
    auto operation_output = apply_monochrome(pre_operation, unmasked, cancellation);
    if (!operation_output)
        return operation_output.error();
    auto alpha = evaluate_operation_mask(pre_operation, operation_output.value(), recipe,
                                         *operation.mask_id, cancellation);
    if (!alpha)
        return alpha.error();
    auto mixed = normal_mask_mix(pre_operation.rgb, operation_output.value().rgb, alpha.value(),
                                 cancellation);
    if (!mixed)
        return mixed.error();
    return std::move(operation_output).value();
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Masked Monochrome allocation failed",
                      {{"operation_id", operation.id}, {"reason", "allocation_failed"}});
}

[[nodiscard]] Result<WorkingImage> apply_masked_split_toning(WorkingImage image,
                                                             const Recipe &recipe,
                                                             const OperationInstance &operation,
                                                             const CancellationToken &cancellation)
try
{
    WorkingImage pre_operation = std::move(image);
    OperationInstance unmasked = operation;
    unmasked.mask_id.reset();
    auto operation_output = apply_split_toning(pre_operation, unmasked, cancellation);
    if (!operation_output)
        return operation_output.error();
    auto alpha = evaluate_operation_mask(pre_operation, operation_output.value(), recipe,
                                         *operation.mask_id, cancellation);
    if (!alpha)
        return alpha.error();
    auto mixed = normal_mask_mix(pre_operation.rgb, operation_output.value().rgb, alpha.value(),
                                 cancellation);
    if (!mixed)
        return mixed.error();
    return std::move(operation_output).value();
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Masked Split Toning allocation failed",
                      {{"operation_id", operation.id}, {"reason", "allocation_failed"}});
}

[[nodiscard]] Result<WorkingImage> apply_masked_velvia(WorkingImage image, const Recipe &recipe,
                                                       const OperationInstance &operation,
                                                       const CancellationToken &cancellation)
try
{
    WorkingImage pre_operation = std::move(image);
    OperationInstance unmasked = operation;
    unmasked.mask_id.reset();
    auto operation_output = apply_velvia(pre_operation, unmasked, cancellation);
    if (!operation_output)
        return operation_output.error();
    auto alpha = evaluate_operation_mask(pre_operation, operation_output.value(), recipe,
                                         *operation.mask_id, cancellation);
    if (!alpha)
        return alpha.error();
    auto mixed = normal_mask_mix(pre_operation.rgb, operation_output.value().rgb, alpha.value(),
                                 cancellation);
    if (!mixed)
        return mixed.error();
    return std::move(operation_output).value();
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Masked Velvia allocation failed",
                      {{"operation_id", operation.id}, {"reason", "allocation_failed"}});
}

[[nodiscard]] Result<WorkingImage>
apply_masked_color_balance_rgb(WorkingImage image, const Recipe &recipe,
                               const OperationInstance &operation,
                               const CancellationToken &cancellation)
try
{
    WorkingImage pre_operation = std::move(image);
    WorkingImage operation_output = pre_operation;
    OperationInstance unmasked = operation;
    unmasked.mask_id.reset();
    auto balanced = apply_color_balance_rgb(operation_output, unmasked, cancellation);
    if (!balanced)
        return balanced.error();
    auto alpha = evaluate_operation_mask(pre_operation, operation_output, recipe,
                                         *operation.mask_id, cancellation);
    if (!alpha)
        return alpha.error();
    auto mixed =
        normal_mask_mix(pre_operation.rgb, operation_output.rgb, alpha.value(), cancellation);
    if (!mixed)
        return mixed.error();
    return operation_output;
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Masked Color Balance RGB allocation failed",
                      {{"operation_id", operation.id}, {"reason", "allocation_failed"}});
}

[[nodiscard]] Result<WorkingImage> apply_masked_exposure(WorkingImage image, const Recipe &recipe,
                                                         const OperationInstance &operation,
                                                         const CancellationToken &cancellation)
try
{
    WorkingImage pre_operation = std::move(image);
    OperationInstance unmasked = operation;
    unmasked.mask_id.reset();
    auto operation_output = apply_exposure(pre_operation, unmasked, cancellation);
    if (!operation_output)
        return operation_output.error();
    auto alpha = evaluate_operation_mask(pre_operation, operation_output.value(), recipe,
                                         *operation.mask_id, cancellation);
    if (!alpha)
        return alpha.error();
    auto mixed = normal_mask_mix(pre_operation.rgb, operation_output.value().rgb, alpha.value(),
                                 cancellation);
    if (!mixed)
        return mixed.error();
    return std::move(operation_output).value();
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Masked Exposure allocation failed",
                      {{"operation_id", operation.id}, {"reason", "allocation_failed"}});
}

[[nodiscard]] Result<WorkingImage> apply_masked_rgb_curve(WorkingImage image, const Recipe &recipe,
                                                          const OperationInstance &operation,
                                                          const CancellationToken &cancellation)
try
{
    WorkingImage pre_operation = std::move(image);
    WorkingImage operation_output = pre_operation;
    OperationInstance unmasked = operation;
    unmasked.mask_id.reset();
    auto curved = apply_rgb_curve(operation_output, unmasked, cancellation);
    if (!curved)
        return curved.error();
    auto alpha = evaluate_operation_mask(pre_operation, operation_output, recipe,
                                         *operation.mask_id, cancellation);
    if (!alpha)
        return alpha.error();
    auto mixed =
        normal_mask_mix(pre_operation.rgb, operation_output.rgb, alpha.value(), cancellation);
    if (!mixed)
        return mixed.error();
    return operation_output;
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Masked RGB Curve allocation failed",
                      {{"operation_id", operation.id}, {"reason", "allocation_failed"}});
}

[[nodiscard]] Result<WorkingImage> apply_masked_tone_curve(WorkingImage image, const Recipe &recipe,
                                                           const OperationInstance &operation,
                                                           const CancellationToken &cancellation)
try
{
    WorkingImage pre_operation = std::move(image);
    WorkingImage operation_output = pre_operation;
    OperationInstance unmasked = operation;
    unmasked.mask_id.reset();
    auto curved = apply_tone_curve(operation_output, unmasked, cancellation);
    if (!curved)
        return curved.error();
    auto alpha = evaluate_operation_mask(pre_operation, operation_output, recipe,
                                         *operation.mask_id, cancellation);
    if (!alpha)
        return alpha.error();
    auto mixed =
        normal_mask_mix(pre_operation.rgb, operation_output.rgb, alpha.value(), cancellation);
    if (!mixed)
        return mixed.error();
    return operation_output;
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Masked Tone Curve allocation failed",
                      {{"operation_id", operation.id}, {"reason", "allocation_failed"}});
}

[[nodiscard]] Result<WorkingImage> apply_masked_light_control(WorkingImage image,
                                                              const Recipe &recipe,
                                                              const OperationInstance &operation,
                                                              const CancellationToken &cancellation)
try
{
    WorkingImage pre_operation = std::move(image);
    WorkingImage operation_output = pre_operation;
    LightControlAmounts amounts;
    const double amount = parameter(operation, "amount", 0.0);
    switch (light_control_rank(operation.id))
    {
    case 0:
        amounts.highlights = amount;
        break;
    case 1:
        amounts.shadows = amount;
        break;
    case 2:
        amounts.whites = amount;
        break;
    case 3:
        amounts.blacks = amount;
        break;
    default:
        return make_error(ErrorCode::kUnsupported,
                          "Operation does not support canonical mask evaluation",
                          {{"operation_id", operation.id},
                           {"mask_id", operation.mask_id.value_or(std::string{})},
                           {"reason", "unsupported_operation_mask"}});
    }
    auto adjusted = apply_light_controls(operation_output, amounts, cancellation);
    if (!adjusted)
        return adjusted.error();
    auto alpha = evaluate_operation_mask(pre_operation, operation_output, recipe,
                                         *operation.mask_id, cancellation);
    if (!alpha)
        return alpha.error();
    auto mixed =
        normal_mask_mix(pre_operation.rgb, operation_output.rgb, alpha.value(), cancellation);
    if (!mixed)
        return mixed.error();
    return operation_output;
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Masked Light control allocation failed",
                      {{"operation_id", operation.id}, {"reason", "allocation_failed"}});
}

[[nodiscard]] Result<WorkingImage> apply_masked_graduated_nd(WorkingImage image,
                                                             const Recipe &recipe,
                                                             const OperationInstance &operation,
                                                             const CancellationToken &cancellation)
try
{
    WorkingImage pre_operation = std::move(image);
    // The caller-visible input remains in `pre_operation`; the legacy-derived
    // graduated operation receives a separately owned output image.
    WorkingImage operation_output = pre_operation;
    OperationInstance unmasked = operation;
    unmasked.mask_id.reset();
    auto graduated = apply_graduated_nd(operation_output, unmasked, cancellation);
    if (!graduated)
    {
        return graduated.error();
    }
    auto alpha = evaluate_operation_mask(pre_operation, operation_output, recipe,
                                         *operation.mask_id, cancellation);
    if (!alpha)
    {
        return alpha.error();
    }
    auto mixed =
        normal_mask_mix(pre_operation.rgb, operation_output.rgb, alpha.value(), cancellation);
    if (!mixed)
    {
        return mixed.error();
    }
    return operation_output;
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Masked Graduated ND allocation failed",
                      {{"operation_id", operation.id}, {"reason", "allocation_failed"}});
}

} // namespace ravo
