#include "ravo/recipe/develop.h"
#include "ravo/recipe/develop_mask.h"

#include "develop_internal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <iomanip>
#include <map>
#include <new>
#include <numbers>
#include <set>
#include <sstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ravo::develop_internal
{
[[nodiscard]] bool develop_set_field_accepts(const std::string_view name, const double value)
{
    DevelopParams params;
    return static_cast<bool>(apply_develop_field_strict(params, name, value));
}

[[nodiscard]] std::optional<double> first_accepted_develop_set_value(const std::string_view name)
{
    static constexpr double kSeeds[] = {0.0,  1.0,   0.5,   -1.0,  2.0,    -0.5, 0.1,   0.01, 0.2,
                                        3.0,  4.0,   5.0,   8.0,   10.0,   18.0, -18.0, 45.0, -45.0,
                                        90.0, 100.0, 180.0, 360.0, -100.0, 0.25, -0.25, 1.5};
    for (const double seed : kSeeds)
    {
        if (develop_set_field_accepts(name, seed))
        {
            return seed;
        }
    }
    for (int index = 0; index <= 32; ++index)
    {
        const double seed = static_cast<double>(index);
        if (develop_set_field_accepts(name, seed))
        {
            return seed;
        }
    }
    return std::nullopt;
}

[[nodiscard]] double develop_set_field_extreme(const std::string_view name, const double seed,
                                               const double direction, const bool integer)
{
    double accepted = seed;
    double step = integer ? 1.0 : std::max(1.0e-3, std::abs(seed) * 0.25 + 1.0e-3);
    for (int grow = 0; grow < 40; ++grow)
    {
        const double candidate = accepted + direction * step;
        if (develop_set_field_accepts(name, candidate))
        {
            accepted = candidate;
            step *= 2.0;
            if (std::abs(accepted) > 1.0e7)
            {
                return accepted;
            }
            continue;
        }
        double low = direction > 0.0 ? accepted : candidate;
        double high = direction > 0.0 ? candidate : accepted;
        for (int refine = 0; refine < 48; ++refine)
        {
            double mid = (low + high) * 0.5;
            if (integer)
            {
                mid = direction > 0.0 ? std::floor(mid) : std::ceil(mid);
            }
            if (mid == low || mid == high)
            {
                break;
            }
            if (develop_set_field_accepts(name, mid))
            {
                if (direction > 0.0)
                {
                    low = mid;
                }
                else
                {
                    high = mid;
                }
            }
            else if (direction > 0.0)
            {
                high = mid;
            }
            else
            {
                low = mid;
            }
        }
        const double edge = direction > 0.0 ? low : high;
        return develop_set_field_accepts(name, edge) ? edge : accepted;
    }
    return accepted;
}

[[nodiscard]] std::vector<std::string> candidate_develop_set_names()
{
    static constexpr std::string_view kQuoted[] = {
        "blacks",
        "bloom",
        "blueMapping",
        "canvasBottom",
        "canvasColorIndex",
        "canvasEnabled",
        "canvasLeft",
        "canvasRight",
        "canvasTop",
        "channelMixerBB",
        "channelMixerBG",
        "channelMixerBR",
        "channelMixerGB",
        "channelMixerGG",
        "channelMixerGR",
        "channelMixerRB",
        "channelMixerRG",
        "channelMixerRR",
        "clarity",
        "colorBalanceFormula",
        "colorCheckerEnabled",
        "colorCheckerPatch",
        "colorCheckerPreset",
        "colorCheckerSourceA",
        "colorCheckerSourceB",
        "colorCheckerSourceL",
        "colorCheckerTargetA",
        "colorCheckerTargetB",
        "colorCheckerTargetL",
        "colorContrastEnabled",
        "colorContrastUnbound",
        "colorCorrectionEnabled",
        "colorEqBand",
        "colorEqHue",
        "colorEqLight",
        "colorEqSat",
        "colorHarmonizerAnchorHueDegrees",
        "colorHarmonizerCustomNodeCount",
        "colorHarmonizerEnabled",
        "colorHarmonizerRuleIndex",
        "colorReconstructionEnabled",
        "colorReconstructionHueDegrees",
        "colorReconstructionPrecedenceIndex",
        "colorZonesBandIndex",
        "colorZonesChroma",
        "colorZonesChromaInterpolationIndex",
        "colorZonesEnabled",
        "colorZonesHue",
        "colorZonesHueInterpolationIndex",
        "colorZonesLightness",
        "colorZonesLightnessInterpolationIndex",
        "colorZonesSelectByIndex",
        "colorZonesStrength",
        "contrast",
        "cropHeight",
        "cropWidth",
        "cropX",
        "cropY",
        "dehaze",
        "dehazeAdaptive",
        "dehazeDistance",
        "demosaicModeIndex",
        "denoise",
        "denoiseChroma",
        "denoiseRadius",
        "exposure",
        "exposureBlack",
        "exposureCompensateBias",
        "exposureCompensateHighlight",
        "exposureDeflickerPercentile",
        "exposureDeflickerTarget",
        "exposureMode",
        "gamma",
        "gamutNormalize",
        "graduatedDensity",
        "graduatedHardness",
        "graduatedOffset",
        "graduatedRotation",
        "grain",
        "highlights",
        "hotPixelsPermissive",
        "hotPixelsStrength",
        "hotPixelsThreshold",
        "inputProfile",
        "legacyColorBalanceMode",
        "lensFocal",
        "lensK1",
        "lensK2",
        "lensMode",
        "lensTcaB",
        "lensTcaR",
        "lensVignetting",
        "lut3dEnabled",
        "lut3dInputSpaceIndex",
        "lut3dInterpolationIndex",
        "lut3dOutputSpaceIndex",
        "lut3dStrength",
        "monochrome",
        "monochromeEnabled",
        "monochromeFilterA",
        "monochromeFilterB",
        "monochromeHighlights",
        "monochromeMix",
        "monochromeSize",
        "outputBlackPointCompensation",
        "rapidrawBlacks",
        "rapidrawContrast",
        "rapidrawEvShift",
        "rapidrawExposure",
        "rapidrawHighlights",
        "rapidrawShadows",
        "rapidrawWhites",
        "outputDitherDamping",
        "outputDitherEnabled",
        "outputDitherMethodIndex",
        "outputFrameAspect",
        "outputFrameBasisIndex",
        "outputFrameBorderBlue",
        "outputFrameBorderGreen",
        "outputFrameBorderRed",
        "outputFrameEnabled",
        "outputFrameLineBlue",
        "outputFrameLineGreen",
        "outputFrameLineOffset",
        "outputFrameLineRed",
        "outputFrameLineSize",
        "outputFrameOrientationIndex",
        "outputFramePositionH",
        "outputFramePositionV",
        "outputFrameSize",
        "outputProfile",
        "outputRenderingIntent",
        "perspectiveConstrainCrop",
        "perspectiveHorizontal",
        "perspectiveInterpolationIndex",
        "perspectiveShear",
        "perspectiveVertical",
        "primariesAchromaticHueDegrees",
        "primariesAchromaticPurity",
        "primariesBlueHueDegrees",
        "primariesBluePurity",
        "primariesGreenHueDegrees",
        "primariesGreenPurity",
        "primariesRedHueDegrees",
        "primariesRedPurity",
        "profileGammaDynamicRange",
        "profileGammaEnabled",
        "profileGammaGamma",
        "profileGammaGreyPoint",
        "profileGammaLinear",
        "profileGammaModeIndex",
        "profileGammaSecurityFactor",
        "profileGammaShadowsRange",
        "proofIntent",
        "proofMode",
        "proofProfile",
        "rawCaAvoidShift",
        "rawCaIterations",
        "rawDenoiseThreshold",
        "rawHighlights",
        "rawHighlightsClip",
        "rawHighlightsMode",
        "renderingIntent",
        "rgbCurveCompensate",
        "rgbCurveDarks",
        "rgbCurveHighlights",
        "rgbCurveInterpolation",
        "rgbCurveLights",
        "rgbCurveMode",
        "rgbCurvePreserve",
        "rgbCurveShadows",
        "rgbCurveSplit0",
        "rgbCurveSplit1",
        "rgbCurveSplit2",
        "rgbLevelsMode",
        "rgbLevelsPreserve",
        "saturation",
        "shadows",
        "sharpen",
        "sharpenRadius",
        "sharpenThreshold",
        "sigmoidContrast",
        "sigmoidHuePreservation",
        "sigmoidSkew",
        "soften",
        "splitAmount",
        "splitBalance",
        "splitCompress",
        "splitHighlightSaturation",
        "splitHighlightsHue",
        "splitMix",
        "splitShadowSaturation",
        "splitShadowsHue",
        "splitToningEnabled",
        "straighten",
        "toneCurveChannelMode",
        "toneCurveInterpolation",
        "toneCurvePreserve",
        "toneCurveWorkingSpace",
        "toneMapperIndex",
        "toneEqBlacks",
        "toneEqHighlights",
        "toneEqMidtones",
        "toneEqShadows",
        "toneEqWhites",
        "texture",
        "textureDetailThreshold",
        "textureIterations",
        "velvia",
        "velviaBias",
        "velviaEnabled",
        "velviaStrength",
        "vibrance",
        "vignette",
        "vignetteCenterX",
        "vignetteCenterY",
        "vignetteFalloff",
        "vignetteMidpoint",
        "vignetteShape",
        "watermarkAlignmentIndex",
        "watermarkBlue",
        "watermarkEnabled",
        "watermarkGreen",
        "watermarkOffsetX",
        "watermarkOffsetY",
        "watermarkOpacity",
        "watermarkRed",
        "watermarkRotation",
        "watermarkScale",
        "whiteBalanceBlue",
        "whiteBalanceFourth",
        "whiteBalanceGreen",
        "whiteBalanceMode",
        "whiteBalanceRed",
        "whites",
        "workingProfile",
    };
    std::vector<std::string> names;
    names.reserve(std::size(kQuoted) + 80U);
    for (const auto name : kQuoted)
    {
        names.emplace_back(name);
    }
    append_develop_numeric_field_names(names);
    append_color_balance_develop_names(names);
    append_legacy_color_balance_develop_names(names);
    for (int band = 0; band <= 7; ++band)
    {
        names.push_back("colorEqHue" + std::to_string(band));
        names.push_back("colorEqSat" + std::to_string(band));
        names.push_back("colorEqLight" + std::to_string(band));
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

} // namespace ravo::develop_internal
