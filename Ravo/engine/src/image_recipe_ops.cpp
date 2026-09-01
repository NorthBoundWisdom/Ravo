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

Result<WorkingImage> apply_recipe_ops(WorkingImage image, const Recipe &recipe,
                                      const CancellationToken &cancellation)
{
    for (auto iterator = recipe.operations.cbegin(); iterator != recipe.operations.cend();
         ++iterator)
    {
        const auto &operation = *iterator;
        auto cancelled = cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
        if (!operation.enabled || absorbed_operation(operation.id))
        {
            continue;
        }
        if (operation.mask_id.has_value() && operation.id != kColorHarmonizerOperationId &&
            operation.id != kColorZonesOperationId && operation.id != kMonochromeOperationId &&
            operation.id != kSplitToningOperationId && operation.id != kVelviaOperationId &&
            operation.id != "ravo.color.colorbalancergb" &&
            operation.id != "ravo.effect.graduatednd" && operation.id != kExposureOperationId &&
            operation.id != "ravo.color.rgbcurve" && operation.id != "ravo.core.tonecurve")
        {
            return make_error(ErrorCode::kUnsupported,
                              "Operation does not support canonical mask evaluation",
                              {{"operation_id", operation.id},
                               {"mask_id", *operation.mask_id},
                               {"reason", "unsupported_operation_mask"}});
        }
        if (operation.id == kPrimariesOperationId)
        {
            auto transformed = apply_primaries(image, operation, cancellation);
            if (!transformed)
            {
                return transformed.error();
            }
            image = std::move(transformed).value();
            continue;
        }
        if (operation.id == "ravo.color.temperature")
        {
            auto balanced = apply_temperature_rgb(image, operation, cancellation);
            if (!balanced)
            {
                return balanced.error();
            }
            continue;
        }
        if (operation.id == "ravo.color.channelmixerrgb")
        {
            auto mixed = apply_channel_mixer_rgb(image, operation, cancellation);
            if (!mixed)
            {
                return mixed.error();
            }
            continue;
        }
        if (operation.id == kExposureOperationId)
        {
            auto exposed =
                operation.mask_id.has_value() ?
                    apply_masked_exposure(std::move(image), recipe, operation, cancellation) :
                    apply_exposure(image, operation, cancellation);
            if (!exposed)
            {
                return exposed.error();
            }
            image = std::move(exposed).value();
            continue;
        }
        if (operation.id == "ravo.core.contrast")
        {
            apply_contrast(image, parameter(operation, "amount", 0.0));
            continue;
        }
        if (light_control_rank(operation.id) >= 0)
        {
            LightControlAmounts amounts;
            int previous_rank = -1;
            auto candidate = iterator;
            auto last = iterator;
            for (; candidate != recipe.operations.cend(); ++candidate)
            {
                const int rank = light_control_rank(candidate->id);
                if (rank < 0 || rank <= previous_rank ||
                    (candidate->enabled && candidate->mask_id.has_value()))
                {
                    break;
                }
                previous_rank = rank;
                last = candidate;
                if (!candidate->enabled)
                {
                    continue;
                }
                const double amount = parameter(*candidate, "amount", 0.0);
                if (rank == 0)
                {
                    amounts.highlights = amount;
                }
                else if (rank == 1)
                {
                    amounts.shadows = amount;
                }
                else if (rank == 2)
                {
                    amounts.whites = amount;
                }
                else
                {
                    amounts.blacks = amount;
                }
            }
            iterator = last;
            auto adjusted = apply_light_controls(image, amounts, cancellation);
            if (!adjusted)
            {
                return adjusted.error();
            }
            continue;
        }
        if (operation.id == "ravo.color.vibrance")
        {
            double saturation = 0.0;
            const auto next = std::next(iterator);
            if (next != recipe.operations.cend() && next->enabled &&
                next->id == "ravo.color.saturation")
            {
                saturation = parameter(*next, "amount", 0.0);
                iterator = next;
            }
            auto adjusted = apply_vibrance_saturation(image, parameter(operation, "amount", 0.0),
                                                      saturation, cancellation);
            if (!adjusted)
            {
                return adjusted.error();
            }
            continue;
        }
        if (operation.id == "ravo.color.saturation")
        {
            double vibrance = 0.0;
            const auto next = std::next(iterator);
            if (next != recipe.operations.cend() && next->enabled &&
                next->id == "ravo.color.vibrance")
            {
                vibrance = parameter(*next, "amount", 0.0);
                iterator = next;
            }
            auto adjusted = apply_vibrance_saturation(
                image, vibrance, parameter(operation, "amount", 0.0), cancellation);
            if (!adjusted)
            {
                return adjusted.error();
            }
            continue;
        }
        if (operation.id == "ravo.geometry.rotate")
        {
            auto rotated = rotate_working(std::move(image),
                                          static_cast<int>(parameter(operation, "quarters", 0.0)));
            if (!rotated)
            {
                return rotated.error();
            }
            image = std::move(rotated).value();
            continue;
        }
        if (operation.id == "ravo.geometry.crop")
        {
            auto cropped = crop_working(
                std::move(image), parameter(operation, "x", 0.0), parameter(operation, "y", 0.0),
                parameter(operation, "width", 1.0), parameter(operation, "height", 1.0));
            if (!cropped)
            {
                return cropped.error();
            }
            image = std::move(cropped).value();
            continue;
        }
        if (operation.id == "ravo.geometry.flip")
        {
            auto flipped =
                flip_working(std::move(image), parameter(operation, "horizontal", 0.0) != 0.0,
                             parameter(operation, "vertical", 0.0) != 0.0);
            if (!flipped)
            {
                return flipped.error();
            }
            image = std::move(flipped).value();
            continue;
        }
        if (operation.id == "ravo.geometry.straighten")
        {
            auto straightened =
                straighten_working(std::move(image), parameter(operation, "degrees", 0.0));
            if (!straightened)
            {
                return straightened.error();
            }
            image = std::move(straightened).value();
            continue;
        }
        if (operation.id == kPerspectiveOperationId)
        {
            auto params = perspective_from_parameters(operation.parameters);
            if (!params)
                return params.error();
            auto transformed = apply_perspective(image, params.value(), cancellation);
            if (!transformed)
                return transformed.error();
            image = std::move(transformed).value();
            continue;
        }
        if (operation.id == kCanvasOperationId)
        {
            auto expanded = apply_canvas(std::move(image), operation, cancellation);
            if (!expanded)
                return expanded.error();
            image = std::move(expanded).value();
            continue;
        }
        if (operation.id == "ravo.core.gamma")
        {
            apply_gamma(image, parameter(operation, "gamma", 1.0));
            continue;
        }
        if (operation.id == "ravo.color.rgblevels")
        {
            auto leveled = apply_rgb_levels(image, operation);
            if (!leveled)
            {
                return leveled.error();
            }
            continue;
        }
        if (operation.id == "ravo.color.rgbcurve")
        {
            if (operation.mask_id.has_value())
            {
                auto curved = apply_masked_rgb_curve(std::move(image), recipe, operation,
                                                     cancellation);
                if (!curved)
                    return curved.error();
                image = std::move(curved).value();
            }
            else
            {
                auto curved = apply_rgb_curve(image, operation, cancellation);
                if (!curved)
                    return curved.error();
            }
            continue;
        }
        if (operation.id == "ravo.core.tonecurve")
        {
            if (operation.mask_id.has_value())
            {
                auto curved = apply_masked_tone_curve(std::move(image), recipe, operation,
                                                      cancellation);
                if (!curved)
                    return curved.error();
                image = std::move(curved).value();
            }
            else
            {
                auto curved = apply_tone_curve(image, operation, cancellation);
                if (!curved)
                    return curved.error();
            }
            continue;
        }
        if (operation.id == kColorBalanceOperationId)
        {
            auto balanced = apply_color_balance(image, operation, cancellation);
            if (!balanced)
            {
                return balanced.error();
            }
            image = std::move(balanced).value();
            continue;
        }
        if (operation.id == kColorCheckerOperationId)
        {
            auto corrected = apply_color_checker(image, operation, cancellation);
            if (!corrected)
            {
                return corrected.error();
            }
            image = std::move(corrected).value();
            continue;
        }
        if (operation.id == kColorHarmonizerOperationId)
        {
            if (operation.mask_id.has_value())
            {
                auto harmonized = apply_masked_color_harmonizer(std::move(image), recipe, operation,
                                                                cancellation);
                if (!harmonized)
                {
                    return harmonized.error();
                }
                image = std::move(harmonized).value();
                continue;
            }
            auto harmonized = apply_color_harmonizer(image, operation, cancellation);
            if (!harmonized)
            {
                return harmonized.error();
            }
            image = std::move(harmonized).value();
            continue;
        }
        if (operation.id == kColorZonesOperationId)
        {
            auto zones =
                operation.mask_id.has_value() ?
                    apply_masked_color_zones(std::move(image), recipe, operation, cancellation) :
                    apply_color_zones(std::move(image), operation, cancellation);
            if (!zones)
                return zones.error();
            image = std::move(zones).value();
            continue;
        }
        if (operation.id == kColorCorrectionOperationId)
        {
            auto corrected = apply_color_correction(image, operation, cancellation);
            if (!corrected)
            {
                return corrected.error();
            }
            image = std::move(corrected).value();
            continue;
        }
        if (operation.id == "ravo.color.colorbalancergb")
        {
            if (operation.mask_id.has_value())
            {
                auto balanced = apply_masked_color_balance_rgb(std::move(image), recipe, operation,
                                                               cancellation);
                if (!balanced)
                    return balanced.error();
                image = std::move(balanced).value();
            }
            else
            {
                auto balanced = apply_color_balance_rgb(image, operation, cancellation);
                if (!balanced)
                    return balanced.error();
            }
            continue;
        }
        if (operation.id == kColorContrastOperationId)
        {
            auto contrasted = apply_color_contrast(image, operation, cancellation);
            if (!contrasted)
            {
                return contrasted.error();
            }
            image = std::move(contrasted).value();
            continue;
        }
        if (operation.id == kColorReconstructionOperationId)
        {
            auto reconstructed = apply_color_reconstruction(image, operation, cancellation);
            if (!reconstructed)
            {
                return reconstructed.error();
            }
            image = std::move(reconstructed).value();
            continue;
        }
        if (operation.id == kVelviaOperationId)
        {
            auto velvia =
                operation.mask_id.has_value() ?
                    apply_masked_velvia(std::move(image), recipe, operation, cancellation) :
                    apply_velvia(std::move(image), operation, cancellation);
            if (!velvia)
            {
                return velvia.error();
            }
            image = std::move(velvia).value();
            continue;
        }
        if (operation.id == kLut3dOperationId)
        {
            auto mapped =
                apply_lut3d(std::move(image), operation, process_lut3d_cache(), cancellation);
            if (!mapped)
                return mapped.error();
            image = std::move(mapped).value();
            continue;
        }
        if (operation.id == kMonochromeOperationId)
        {
            auto monochrome =
                operation.mask_id.has_value() ?
                    apply_masked_monochrome(std::move(image), recipe, operation, cancellation) :
                    apply_monochrome(std::move(image), operation, cancellation);
            if (!monochrome)
                return monochrome.error();
            image = std::move(monochrome).value();
            continue;
        }
        if (operation.id == kSplitToningOperationId)
        {
            auto split =
                operation.mask_id.has_value() ?
                    apply_masked_split_toning(std::move(image), recipe, operation, cancellation) :
                    apply_split_toning(std::move(image), operation, cancellation);
            if (!split)
                return split.error();
            image = std::move(split).value();
            continue;
        }
        if (operation.id == kSharpenOperationId)
        {
            auto sharpened = apply_sharpen(image, operation, cancellation);
            if (!sharpened)
            {
                return sharpened.error();
            }
            image = std::move(sharpened).value();
            continue;
        }
        if (operation.id == kTextureOperationId)
        {
            auto textured = apply_texture(image, operation, cancellation);
            if (!textured)
            {
                return textured.error();
            }
            image = std::move(textured).value();
            continue;
        }
        if (operation.id == kRetouchOperationId)
        {
            auto retouched = apply_retouch(std::move(image), recipe, operation, cancellation);
            if (!retouched)
            {
                return retouched.error();
            }
            image = std::move(retouched).value();
            continue;
        }
        if (operation.id == "ravo.detail.clarity")
        {
            apply_clarity(image, parameter(operation, "amount", 0.0));
            continue;
        }
        if (operation.id == "ravo.effect.vignette")
        {
            auto vignette = apply_vignette(
                image, parameter(operation, "amount", 0.0), parameter(operation, "midpoint", 0.8),
                parameter(operation, "falloff", 0.5), parameter(operation, "shape", 1.0),
                parameter(operation, "center_x", 0.0), parameter(operation, "center_y", 0.0),
                cancellation);
            if (!vignette)
            {
                return vignette.error();
            }
            continue;
        }
        if (operation.id == "ravo.effect.grain")
        {
            apply_grain(image, parameter(operation, "amount", 0.0));
            continue;
        }
        if (operation.id == "ravo.effect.bloom")
        {
            apply_bloom(image, parameter(operation, "amount", 0.0));
            continue;
        }
        if (operation.id == "ravo.effect.soften")
        {
            apply_soften(image, parameter(operation, "amount", 0.0));
            continue;
        }
        if (operation.id == kDehazeOperationId)
        {
            return make_error(
                ErrorCode::kUnsupported, "Dehaze must execute on the source-linear RAW buffer",
                {{"operation_id", operation.id}, {"reason", "dehaze_source_stage_required"}});
        }
        if (operation.id == "ravo.display.sigmoid")
        {
            auto transformed = apply_sigmoid(image, operation, cancellation);
            if (!transformed)
            {
                return transformed.error();
            }
            continue;
        }
        if (operation.id == "ravo.raw.hotpixels")
        {
            return make_error(ErrorCode::kUnsupported,
                              "Hot pixel correction requires a Bayer CFA working buffer",
                              {{"operation_id", operation.id}});
        }
        if (operation.id == "ravo.raw.cacorrect")
        {
            return make_error(
                ErrorCode::kUnsupported,
                "RAW chromatic aberration correction requires a Bayer CFA working buffer",
                {{"operation_id", operation.id}});
        }
        if (operation.id == "ravo.raw.highlights")
        {
            return make_error(ErrorCode::kUnsupported,
                              "RAW highlight reconstruction requires a Bayer CFA working buffer",
                              {{"operation_id", operation.id}});
        }
        if (operation.id == "ravo.raw.denoise")
        {
            return make_error(ErrorCode::kUnsupported,
                              "RAW denoise requires a Bayer CFA working buffer",
                              {{"operation_id", operation.id}});
        }
        if (operation.id == "ravo.detail.denoiseprofile")
        {
            auto denoised = apply_denoise_profile(image, operation, cancellation);
            if (!denoised)
            {
                return denoised.error();
            }
            continue;
        }
        if (operation.id == "ravo.geometry.lens")
        {
            auto corrected = apply_lens_correction(image, operation, cancellation);
            if (!corrected)
            {
                return corrected.error();
            }
            continue;
        }
        if (operation.id == "ravo.color.colorequal")
        {
            auto equalized = apply_color_equalizer(image, operation, cancellation);
            if (!equalized)
            {
                return equalized.error();
            }
            continue;
        }
        if (operation.id == "ravo.effect.graduatednd")
        {
            if (operation.mask_id.has_value())
            {
                auto graduated =
                    apply_masked_graduated_nd(std::move(image), recipe, operation, cancellation);
                if (!graduated)
                {
                    return graduated.error();
                }
                image = std::move(graduated).value();
                continue;
            }
            auto graduated = apply_graduated_nd(image, operation, cancellation);
            if (!graduated)
            {
                return graduated.error();
            }
            continue;
        }
        if (operation.id == "ravo.core.toneequal")
        {
            auto equalized = apply_tone_equalizer(image, operation, cancellation);
            if (!equalized)
            {
                return equalized.error();
            }
            continue;
        }
        return make_error(ErrorCode::kUnsupported, "Operation has no CPU implementation",
                          {{"operation_id", operation.id}});
    }
    return image;
}

} // namespace ravo
