#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ravo/recipe/color_input.h"
#include "ravo/recipe/color_checker.h"
#include "ravo/recipe/canvas_frame.h"
#include "ravo/recipe/color_contrast.h"
#include "ravo/recipe/color_correction.h"
#include "ravo/recipe/color_harmonizer.h"
#include "ravo/recipe/color_output.h"
#include "ravo/recipe/color_reconstruction.h"
#include "ravo/recipe/color_zones.h"
#include "ravo/recipe/dehaze.h"
#include "ravo/recipe/lut3d.h"
#include "ravo/recipe/monochrome.h"
#include "ravo/recipe/operation.h"
#include "ravo/recipe/output_dither.h"
#include "ravo/recipe/perspective.h"
#include "ravo/recipe/profile_gamma.h"
#include "ravo/recipe/primaries.h"
#include "ravo/recipe/recipe.h"
#include "ravo/recipe/retouch.h"
#include "ravo/recipe/sharpen.h"
#include "ravo/recipe/split_toning.h"
#include "ravo/recipe/texture.h"
#include "ravo/recipe/velvia.h"
#include "ravo/recipe/watermark.h"

namespace ravo
{

inline constexpr double kDevelopGammaDefault = 1.0;
inline constexpr double kDevelopStraightenMin = -45.0;
inline constexpr double kDevelopStraightenMax = 45.0;
// Short-edge floor is min(300px, half the source short side).
inline constexpr double kDevelopCropMinShortEdgePixels = 300.0;
inline constexpr double kDevelopCropMinShortEdgeFraction = 0.5;
inline constexpr double kSigmoidMiddleGrey = 0.1845;
inline constexpr double kSigmoidContrastDefault = 1.5;
inline constexpr double kSigmoidContrastMin = 0.1;
inline constexpr double kSigmoidContrastMax = 10.0;
inline constexpr double kSigmoidSkewDefault = 0.0;
inline constexpr double kSigmoidSkewMin = -1.0;
inline constexpr double kSigmoidSkewMax = 1.0;
inline constexpr double kSigmoidDisplayWhiteDefault = 100.0;
inline constexpr double kSigmoidDisplayWhiteMin = 20.0;
inline constexpr double kSigmoidDisplayWhiteMax = 1600.0;
inline constexpr double kSigmoidDisplayBlackDefault = 0.0152;
inline constexpr double kSigmoidDisplayBlackMin = 0.0;
inline constexpr double kSigmoidDisplayBlackMax = 15.0;
inline constexpr double kSigmoidHuePreservationDefault = 1.0;
inline constexpr std::string_view kSigmoidWorkingSpaceLinearSrgb = "linear_srgb";
inline constexpr std::string_view kSigmoidColorProcessingPerChannel = "per_channel";
inline constexpr std::string_view kSigmoidColorProcessingRgbRatio = "rgb_ratio";
inline constexpr std::size_t kToneCurveMinPoints = 2;
inline constexpr std::size_t kToneCurveMaxPoints = 20;
inline constexpr std::string_view kToneCurveWorkingSpaceSrgb = "srgb";
inline constexpr std::string_view kToneCurveWorkingSpaceLinearRgb = "linear_rgb";
inline constexpr std::string_view kToneCurveWorkingSpaceRgb = "rgb";
inline constexpr std::string_view kToneCurveWorkingSpaceLab = "lab";
inline constexpr std::string_view kToneCurveWorkingSpaceXyz = "xyz";
inline constexpr std::string_view kToneCurveWorkingSpaceLabIndependent = "lab_independent";
inline constexpr std::string_view kToneCurveInterpolationMonotoneHermite = "monotone_hermite";
inline constexpr std::string_view kToneCurveInterpolationCatmullRom = "catmull_rom";
inline constexpr std::string_view kToneCurveInterpolationCubicSpline = "cubic_spline";
inline constexpr std::string_view kToneCurveChannelModeRgb = "rgb";
inline constexpr std::string_view kToneCurveChannelModeIndependent = "independent";
inline constexpr std::string_view kToneCurvePreserveColorsAverage = "average";
inline constexpr std::string_view kToneCurvePreserveColorsNone = "none";
inline constexpr std::string_view kToneCurvePreserveColorsLuminance = "luminance";
inline constexpr std::string_view kToneCurvePreserveColorsMax = "max";
inline constexpr std::string_view kToneCurvePreserveColorsSum = "sum";
inline constexpr std::string_view kToneCurvePreserveColorsNorm = "norm";
inline constexpr std::string_view kToneCurvePreserveColorsPower = "power";
inline constexpr std::string_view kRgbLevelsModeLinked = "linked";
inline constexpr std::string_view kRgbLevelsModeIndependent = "independent";
inline constexpr std::string_view kRgbCurveApplicationSpaceSceneLinear = "scene_linear";
inline constexpr std::string_view kRgbCurveApplicationSpaceDisplaySrgb = "display_srgb";
inline constexpr std::size_t kColorEqualizerBandCount = 8;
inline constexpr std::size_t kChannelMixerChannelCount = 3;
inline constexpr std::string_view kChannelMixerWorkingSpaceLinearSrgbD50 = "linear_srgb_d50";
inline constexpr std::string_view kChannelMixerAlgorithmV3 = "v3";
inline constexpr std::string_view kChannelMixerAdaptationRgb = "rgb";
inline constexpr std::string_view kChannelMixerAdaptationCat16 = "cat16";
inline constexpr std::string_view kChannelMixerAdaptationLinearBradford = "linear_bradford";
inline constexpr std::string_view kChannelMixerAdaptationFullBradford = "full_bradford";
inline constexpr std::string_view kChannelMixerAdaptationXyz = "xyz";
inline constexpr std::size_t kTemperatureChannelCount = 4;
inline constexpr std::string_view kTemperatureWorkingSpaceCameraCfaOrLinearRgb =
    "camera_cfa_or_linear_rgb";
inline constexpr std::string_view kTemperatureAlgorithmChannelScaleV4 = "channel_scale_v4";
inline constexpr std::string_view kTemperatureModeAsShot = "as_shot";
inline constexpr std::string_view kTemperatureModeCameraReference = "camera_reference";
inline constexpr std::string_view kTemperatureModeAsShotToReference = "as_shot_to_reference";
inline constexpr std::string_view kTemperatureModeManual = "manual";
inline constexpr std::string_view kColorBalanceOperationId = "ravo.color.colorbalance";
inline constexpr std::int64_t kColorBalanceOperationSchemaVersion = 1;
inline constexpr std::string_view kColorBalanceWorkingSpaceLinearSrgbD50 = "linear_srgb_d50";
inline constexpr std::string_view kColorBalanceAlgorithmLabD50ProPhotoV4 = "lab_d50_prophoto_v4";
inline constexpr std::string_view kColorBalanceModeLiftGammaGain = "lift_gamma_gain";
inline constexpr std::string_view kColorBalanceModeSlopeOffsetPower = "slope_offset_power";
inline constexpr std::size_t kColorBalanceChannelCount = 4;
inline constexpr std::string_view kColorBalanceRgbWorkingSpaceLinearSrgbD50 = "linear_srgb_d50";
inline constexpr std::string_view kColorBalanceRgbAlgorithmFilmlightYchV5 = "filmlight_ych_v5";
inline constexpr std::string_view kColorBalanceRgbFormulaDtUcs2022 = "dt_ucs_2022";
inline constexpr std::string_view kColorBalanceRgbFormulaJzAzBz2021 = "jzazbz_2021";
inline constexpr std::string_view kRawHighlightsModeClip = "clip";
inline constexpr std::string_view kRawHighlightsModeInpaint = "inpaint";
inline constexpr std::string_view kRawHighlightsModeOpposed = "opposed";
inline constexpr std::string_view kRawHighlightsModeLch = "lch";
inline constexpr std::string_view kLensModeManual = "manual";
inline constexpr std::string_view kLensModeLookup = "lookup";

struct ToneCurvePoint
{
    double x = 0.0;
    double y = 0.0;

    [[nodiscard]] bool operator==(const ToneCurvePoint &) const noexcept = default;
};

enum class ToneCurveWorkingSpace
{
    kSrgb,
    kLinearRgb,
    kRgb,
    kLab,
    kXyz,
    kLabIndependent,
};

struct ChannelMixerParams
{
    std::array<double, kChannelMixerChannelCount> red{1.0, 0.0, 0.0};
    std::array<double, kChannelMixerChannelCount> green{0.0, 1.0, 0.0};
    std::array<double, kChannelMixerChannelCount> blue{0.0, 0.0, 1.0};
    std::array<double, kChannelMixerChannelCount> saturation{};
    std::array<double, kChannelMixerChannelCount> lightness{};
    std::array<double, kChannelMixerChannelCount> grey{};
    bool normalize_red = false;
    bool normalize_green = false;
    bool normalize_blue = false;
    bool normalize_saturation = false;
    bool normalize_lightness = false;
    bool normalize_grey = true;
    std::string adaptation{std::string(kChannelMixerAdaptationRgb)};
    double illuminant_x = 0.34567;
    double illuminant_y = 0.35850;
    double gamut = 0.0;
    bool clip = false;

    [[nodiscard]] bool is_identity() const noexcept;
    [[nodiscard]] bool operator==(const ChannelMixerParams &) const noexcept = default;
};

struct TemperatureParams
{
    std::string mode{std::string(kTemperatureModeAsShot)};
    std::optional<std::array<double, kTemperatureChannelCount>> coefficients;

    [[nodiscard]] bool is_identity() const noexcept;
    [[nodiscard]] bool operator==(const TemperatureParams &) const noexcept = default;
};

struct ColorBalanceParams
{
    std::string mode{std::string(kColorBalanceModeSlopeOffsetPower)};
    // Legacy channel order is factor, red, green, blue.
    std::array<double, kColorBalanceChannelCount> lift{1.0, 1.0, 1.0, 1.0};
    std::array<double, kColorBalanceChannelCount> gamma{1.0, 1.0, 1.0, 1.0};
    std::array<double, kColorBalanceChannelCount> gain{1.0, 1.0, 1.0, 1.0};
    double input_saturation = 1.0;
    double contrast = 1.0;
    double grey_fulcrum_percent = 18.0;
    double output_saturation = 1.0;

    [[nodiscard]] bool is_identity() const noexcept;
    [[nodiscard]] bool operator==(const ColorBalanceParams &) const noexcept = default;
};

struct ColorBalanceRgbParams
{
    double shadows_y = 0.0;
    double shadows_chroma = 0.0;
    double shadows_hue = 0.0;
    double midtones_y = 0.0;
    double midtones_chroma = 0.0;
    double midtones_hue = 0.0;
    double highlights_y = 0.0;
    double highlights_chroma = 0.0;
    double highlights_hue = 0.0;
    double global_y = 0.0;
    double global_chroma = 0.0;
    double global_hue = 0.0;
    double shadows_falloff = 1.0;
    double white_fulcrum_ev = 0.0;
    double highlights_falloff = 1.0;
    double chroma_shadows = 0.0;
    double chroma_highlights = 0.0;
    double chroma_global = 0.0;
    double chroma_midtones = 0.0;
    double saturation_global = 0.0;
    double saturation_highlights = 0.0;
    double saturation_midtones = 0.0;
    double saturation_shadows = 0.0;
    double hue_rotation = 0.0;
    double brilliance_global = 0.0;
    double brilliance_highlights = 0.0;
    double brilliance_midtones = 0.0;
    double brilliance_shadows = 0.0;
    double mask_grey_fulcrum = 0.1845;
    double vibrance = 0.0;
    double grey_fulcrum = 0.1845;
    double contrast = 0.0;
    std::string saturation_formula{std::string(kColorBalanceRgbFormulaDtUcs2022)};

    [[nodiscard]] bool is_identity() const noexcept;
    [[nodiscard]] bool operator==(const ColorBalanceRgbParams &) const noexcept = default;
};

struct RgbLevelsParams
{
    std::string mode{std::string(kRgbLevelsModeLinked)};
    std::string preserve_colors{std::string(kToneCurvePreserveColorsLuminance)};
    std::array<std::array<double, 3>, 3> levels{
        {{0.0, 0.5, 1.0}, {0.0, 0.5, 1.0}, {0.0, 0.5, 1.0}}};

    [[nodiscard]] bool is_identity() const noexcept;
    [[nodiscard]] bool operator==(const RgbLevelsParams &) const noexcept = default;
};

struct RgbCurveParams
{
    std::string mode{std::string(kRgbLevelsModeLinked)};
    std::string preserve_colors{std::string(kToneCurvePreserveColorsLuminance)};
    std::string interpolation{std::string(kToneCurveInterpolationMonotoneHermite)};
    std::string application_space{std::string(kRgbCurveApplicationSpaceSceneLinear)};
    bool compensate_middle_grey = false;
    std::array<std::vector<ToneCurvePoint>, 3> channels{
        {{ToneCurvePoint{0.0, 0.0}, ToneCurvePoint{1.0, 1.0}},
         {ToneCurvePoint{0.0, 0.0}, ToneCurvePoint{1.0, 1.0}},
         {ToneCurvePoint{0.0, 0.0}, ToneCurvePoint{1.0, 1.0}}}};
    double parametric_shadows = 0.0;
    double parametric_darks = 0.0;
    double parametric_lights = 0.0;
    double parametric_highlights = 0.0;
    double parametric_split_shadows = 0.25;
    double parametric_split_mid = 0.50;
    double parametric_split_highlights = 0.75;

    [[nodiscard]] bool is_identity() const noexcept;
    [[nodiscard]] bool operator==(const RgbCurveParams &) const noexcept = default;
};

struct DevelopParams
{
    // Canonical graph/attachment state is typed Develop state. S3.2's recipe
    // helper may author bounded Studio-owned leaves, while ordinary edits,
    // previews, saves, undo, and reopen preserve every valid attachment.
    std::vector<Mask> masks;
    // Structural RAW choice: an absent recipe operation means the RCD default.
    // Unlike raw_effect_enabled this cannot be bypassed, because every Bayer
    // source still needs exactly one demosaic owner.
    std::string demosaic_mode{std::string(kDemosaicModeRcd)};
    TemperatureParams temperature;
    bool profile_gamma_enabled = false;
    ProfileGammaParams profile_gamma;
    InputColorParams input_color;
    OutputColorParams output_color;
    PrimariesParams primaries;
    ChannelMixerParams channel_mixer;
    std::string exposure_mode{std::string(kExposureModeManual)};
    double exposure_black = 0.0;
    double exposure_ev = 0.0;
    double exposure_deflicker_percentile = kExposureDeflickerPercentileDefault;
    double exposure_deflicker_target_ev = kExposureDeflickerTargetEvDefault;
    bool exposure_compensate_exposure_bias = false;
    bool exposure_compensate_highlight_preservation = false;
    std::optional<std::string> exposure_mask_id;
    std::int64_t exposure_mask_child_index = 0;
    std::int64_t exposure_mask_point_index = 0;
    double contrast = 0.0;
    double highlights = 0.0;
    double shadows = 0.0;
    double whites = 0.0;
    double blacks = 0.0;
    double vibrance = 0.0;
    double saturation = 0.0;
    std::int64_t rotate_quarters = 0;
    std::int64_t flip_horizontal = 0;
    std::int64_t flip_vertical = 0;
    double straighten_degrees = 0.0;
    double perspective_vertical = 0.0;
    double perspective_horizontal = 0.0;
    double perspective_shear = 0.0;
    bool perspective_constrain_crop = true;
    std::int64_t perspective_interpolation_index = 2;
    double crop_x = 0.0;
    double crop_y = 0.0;
    double crop_width = 1.0;
    double crop_height = 1.0;
    bool canvas_present = false;
    bool canvas_enabled = false;
    CanvasParams canvas;
    double sharpen = 0.0;
    double sharpen_radius = 2.0;
    double sharpen_threshold = 0.5;
    TextureParams texture;
    RetouchParams retouch;
    double clarity = 0.0;
    double vignette = 0.0;
    double vignette_midpoint = 0.8;
    double vignette_falloff = 0.5;
    double vignette_shape = 1.0;
    double vignette_center_x = 0.0;
    double vignette_center_y = 0.0;
    double grain = 0.0;
    double bloom = 0.0;
    double soften = 0.0;
    double dehaze = 0.0;
    double dehaze_distance = 0.2;
    bool dehaze_adaptive = true;
    bool output_dither_present = false;
    bool output_dither_enabled = false;
    OutputDitherParams output_dither;
    bool frame_present = false;
    bool frame_enabled = false;
    FrameParams frame;
    bool watermark_present = false;
    bool watermark_enabled = false;
    WatermarkParams watermark;
    bool velvia_present = false;
    bool velvia_enabled = false;
    VelviaParams velvia;
    std::optional<std::string> velvia_mask_id;
    bool lut3d_present = false;
    bool lut3d_enabled = false;
    Lut3dParams lut3d;
    bool color_balance_enabled = false;
    ColorBalanceParams color_balance;
    bool color_checker_enabled = false;
    ColorCheckerParams color_checker;
    std::int64_t color_checker_patch = 0;
    ColorBalanceRgbParams color_balance_rgb;
    std::optional<std::string> color_balance_rgb_mask_id;
    std::int64_t color_balance_rgb_mask_child_index = 0;
    std::int64_t color_balance_rgb_mask_point_index = 0;
    bool color_correction_enabled = false;
    ColorCorrectionParams color_correction;
    bool color_contrast_enabled = false;
    ColorContrastParams color_contrast;
    bool color_reconstruction_enabled = false;
    ColorReconstructionParams color_reconstruction;
    bool color_zones_present = false;
    bool color_zones_enabled = false;
    ColorZonesParams color_zones;
    std::optional<std::string> color_zones_mask_id;
    std::int64_t color_zones_band = 0;
    bool color_harmonizer_present = false;
    bool color_harmonizer_enabled = false;
    ColorHarmonizerParams color_harmonizer;
    std::optional<std::string> color_harmonizer_mask_id;
    std::int64_t color_harmonizer_mask_child_index = 0;
    std::int64_t color_harmonizer_mask_point_index = 0;
    bool monochrome_present = false;
    bool monochrome_enabled = false;
    MonochromeParams monochrome;
    std::optional<std::string> monochrome_mask_id;
    bool split_toning_present = false;
    bool split_toning_enabled = false;
    SplitToningParams split_toning;
    std::optional<std::string> split_toning_mask_id;
    double gamma = kDevelopGammaDefault;
    RgbLevelsParams rgb_levels;
    RgbCurveParams rgb_curve;
    std::vector<ToneCurvePoint> tone_curve;
    std::vector<ToneCurvePoint> tone_curve_a;
    std::vector<ToneCurvePoint> tone_curve_b;
    std::string tone_curve_working_space{std::string(kToneCurveWorkingSpaceSrgb)};
    std::string tone_curve_interpolation{std::string(kToneCurveInterpolationMonotoneHermite)};
    std::string tone_curve_channel_mode{std::string(kToneCurveChannelModeRgb)};
    std::string tone_curve_preserve_colors{std::string(kToneCurvePreserveColorsAverage)};
    bool sigmoid_enabled = false;
    double sigmoid_contrast = kSigmoidContrastDefault;
    double sigmoid_skew = kSigmoidSkewDefault;
    double sigmoid_display_white = kSigmoidDisplayWhiteDefault;
    double sigmoid_display_black = kSigmoidDisplayBlackDefault;
    double sigmoid_hue_preservation = kSigmoidHuePreservationDefault;
    double raw_highlights = 0.0;
    double raw_highlights_clip = 0.987;
    double raw_denoise_threshold = 0.0;
    std::array<std::array<double, 5>, 4> raw_denoise_bands{{
        {{0.5, 0.5, 0.5, 0.5, 0.5}},
        {{0.5, 0.5, 0.5, 0.5, 0.5}},
        {{0.5, 0.5, 0.5, 0.5, 0.5}},
        {{0.5, 0.5, 0.5, 0.5, 0.5}},
    }};
    std::string raw_highlights_mode{std::string(kRawHighlightsModeOpposed)};
    double hot_pixels_strength = 0.0;
    double hot_pixels_threshold = 0.05;
    bool hot_pixels_permissive = false;
    std::int64_t raw_ca_iterations = 0;
    bool raw_ca_avoid_shift = false;
    double denoise = 0.0;
    double denoise_chroma = 1.0;
    double denoise_radius = 1.0;
    double lens_k1 = 0.0;
    double lens_k2 = 0.0;
    double lens_tca_r = 1.0;
    double lens_tca_b = 1.0;
    double lens_vignetting = 0.0;
    std::string lens_mode{std::string(kLensModeManual)};
    std::string lens_make;
    std::string lens_model;
    std::string lens_name;
    double lens_focal_mm = 50.0;
    std::array<double, kColorEqualizerBandCount> color_eq_hue{};
    std::array<double, kColorEqualizerBandCount> color_eq_sat{};
    std::array<double, kColorEqualizerBandCount> color_eq_light{};
    std::int64_t color_eq_band = 0;
    double graduated_density = 0.0;
    double graduated_hardness = 0.5;
    double graduated_rotation = 0.0;
    double graduated_offset = 0.0;
    bool graduated_present = false;
    bool graduated_enabled = false;
    std::optional<std::string> graduated_mask_id;
    std::int64_t graduated_mask_child_index = 0;
    std::int64_t graduated_mask_point_index = 0;
    double tone_eq_blacks = 0.0;
    double tone_eq_shadows = 0.0;
    double tone_eq_midtones = 0.0;
    double tone_eq_highlights = 0.0;
    double tone_eq_whites = 0.0;
    // Panel-lamp bypass. Identity sections stay omitted; a modified section with
    // effect_enabled=false keeps parameters and writes operation.enabled=false.
    bool geometry_effect_enabled = true;
    bool input_profile_effect_enabled = true;
    bool output_profile_effect_enabled = true;
    bool white_balance_effect_enabled = true;
    bool calibration_effect_enabled = true;
    bool primaries_effect_enabled = true;
    bool light_effect_enabled = true;
    bool color_effect_enabled = true;
    bool detail_effect_enabled = true;
    bool effects_effect_enabled = true;
    bool raw_effect_enabled = true;
    bool tone_equal_effect_enabled = true;
    bool graduated_effect_enabled = true;
    bool color_eq_effect_enabled = true; // Color Equalizer is independent of Graduated ND.
    bool curves_effect_enabled = true;

    [[nodiscard]] bool is_identity() const noexcept;
    [[nodiscard]] bool operator==(const DevelopParams &) const noexcept = default;
};

[[nodiscard]] bool tone_curve_is_identity(const std::vector<ToneCurvePoint> &points) noexcept;
void clamp_tone_curve(std::vector<ToneCurvePoint> &points) noexcept;
[[nodiscard]] bool curve_interpolation_is_supported(std::string_view interpolation) noexcept;
[[nodiscard]] int curve_interpolation_index(std::string_view interpolation) noexcept;
[[nodiscard]] std::string_view curve_interpolation_from_index(int index) noexcept;
[[nodiscard]] Result<std::string_view> parse_curve_interpolation(std::string_view interpolation);
[[nodiscard]] double evaluate_tone_curve(const std::vector<ToneCurvePoint> &points,
                                         double x) noexcept;
[[nodiscard]] double evaluate_tone_curve(const std::vector<ToneCurvePoint> &points, double x,
                                         std::string_view interpolation) noexcept;
// Samples x = index / sample_count after preparing interpolation coefficients once.
// This is the shared owner for dense Engine/UI LUT construction; callers must not
// reimplement curve interpolation or rebuild a spline for every sample.
[[nodiscard]] Result<std::vector<float>>
build_tone_curve_lut(const std::vector<ToneCurvePoint> &points, std::string_view interpolation,
                     std::size_t sample_count);
[[nodiscard]] bool rgb_curve_parametric_is_identity(const RgbCurveParams &params) noexcept;
[[nodiscard]] double evaluate_rgb_curve_parametric(const RgbCurveParams &params, double x) noexcept;
void clamp_rgb_curve(RgbCurveParams &params) noexcept;
[[nodiscard]] Result<ToneCurveWorkingSpace> parse_tone_curve_working_space(std::string_view text);
[[nodiscard]] std::string_view tone_curve_working_space_name(ToneCurveWorkingSpace space) noexcept;
[[nodiscard]] Result<std::vector<ToneCurvePoint>>
parse_tone_curve_points(const ParameterValue &value);
[[nodiscard]] Result<std::vector<ToneCurvePoint>>
parse_rgb_curve_points(const ParameterValue &value);
[[nodiscard]] ParameterValue
tone_curve_points_to_parameter(const std::vector<ToneCurvePoint> &points);
[[nodiscard]] Result<void> validate_tone_curve_parameters(
    const std::map<std::string, ParameterValue, std::less<>> &parameters);
[[nodiscard]] Result<void>
validate_rgb_curve_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters);
[[nodiscard]] Result<void>
validate_sigmoid_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters);
[[nodiscard]] Result<void> validate_channel_mixer_parameters(
    const std::map<std::string, ParameterValue, std::less<>> &parameters);
[[nodiscard]] Result<ChannelMixerParams>
channel_mixer_from_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters);
[[nodiscard]] std::map<std::string, ParameterValue, std::less<>>
channel_mixer_to_parameters(const ChannelMixerParams &params);
[[nodiscard]] Result<void> validate_temperature_parameters(
    const std::map<std::string, ParameterValue, std::less<>> &parameters);
[[nodiscard]] Result<TemperatureParams>
temperature_from_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters);
[[nodiscard]] std::map<std::string, ParameterValue, std::less<>>
temperature_to_parameters(const TemperatureParams &params);
[[nodiscard]] Result<void> validate_color_balance_parameters(
    const std::map<std::string, ParameterValue, std::less<>> &parameters);
[[nodiscard]] Result<ColorBalanceParams>
color_balance_from_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters);
[[nodiscard]] std::map<std::string, ParameterValue, std::less<>>
color_balance_to_parameters(const ColorBalanceParams &params);
[[nodiscard]] Result<void> validate_color_balance_rgb_parameters(
    const std::map<std::string, ParameterValue, std::less<>> &parameters);
[[nodiscard]] Result<ColorBalanceRgbParams> color_balance_rgb_from_parameters(
    const std::map<std::string, ParameterValue, std::less<>> &parameters);
[[nodiscard]] std::map<std::string, ParameterValue, std::less<>>
color_balance_rgb_to_parameters(const ColorBalanceRgbParams &params);

struct LeftoverFlipGeometry
{
    std::int64_t rotate_quarters = 0;
    std::int64_t flip_horizontal = 0;
    std::int64_t flip_vertical = 0;

    [[nodiscard]] bool is_identity() const noexcept
    {
        return rotate_quarters == 0 && flip_horizontal == 0 && flip_vertical == 0;
    }
};

// Maps leftover flip.c orientation bits (ORIENTATION_NULL=-1 through TRANSVERSE=7)
// onto canonical rotate-then-flip. NULL and NONE are identity because Ravo applies
// camera EXIF at decode.
[[nodiscard]] Result<LeftoverFlipGeometry>
leftover_flip_orientation_to_geometry(std::int32_t orientation);

struct LeftoverCropBox
{
    double x = 0.0;
    double y = 0.0;
    double width = 1.0;
    double height = 1.0;

    [[nodiscard]] bool is_identity() const noexcept;
};

// Maps leftover crop.c left/top/right/bottom (cx, cy, cw, ch) onto canonical
// x/y/width/height. Full-frame 0,0,1,1 is identity. ratio_n/ratio_d export
// snapping stays later G7 work.
[[nodiscard]] Result<LeftoverCropBox> leftover_crop_box_to_geometry(float left, float top,
                                                                    float right, float bottom);

// Maps the leftover ashift generic lens model onto canonical perspective. The
// adapter remains responsible for rejecting legacy-only lens/crop modes before
// calling this bounded head conversion.
[[nodiscard]] Result<PerspectiveParams>
leftover_ashift_to_perspective(float rotation, float lensshift_v, float lensshift_h, float shear,
                               bool constrain_crop);

[[nodiscard]] Result<RgbLevelsParams>
leftover_rgblevels_from_v1(std::int32_t autoscale, std::int32_t preserve_colors,
                           const std::array<float, 9> &levels);
[[nodiscard]] std::map<std::string, ParameterValue, std::less<>>
rgb_levels_to_parameters(const RgbLevelsParams &params);
[[nodiscard]] Result<RgbCurveParams>
leftover_rgbcurve_from_v1(const std::vector<std::uint8_t> &payload);
[[nodiscard]] std::map<std::string, ParameterValue, std::less<>>
rgb_curve_to_parameters(const RgbCurveParams &params);
[[nodiscard]] Result<void> leftover_rawdenoise_from_v2(const std::vector<std::uint8_t> &payload,
                                                       double &threshold,
                                                       std::array<std::array<double, 5>, 4> &bands);
[[nodiscard]] std::map<std::string, ParameterValue, std::less<>>
raw_denoise_to_parameters(double threshold, const std::array<std::array<double, 5>, 4> &bands);

void clamp_develop(DevelopParams &params) noexcept;
enum class DevelopSetFieldKind : std::uint8_t
{
    Number = 0,
    Integer = 1,
    Toggle = 2,
    Text = 3,
};

// Names accepted by catalog develop/probe `--set`, plus `watermarkText`.
// Bounds come from `apply_develop_field_strict` on an identity DevelopParams.
struct DevelopSetField
{
    std::string name;
    DevelopSetFieldKind kind = DevelopSetFieldKind::Number;
    std::optional<double> minimum;
    std::optional<double> maximum;
};

[[nodiscard]] std::string_view develop_set_field_kind_name(DevelopSetFieldKind kind) noexcept;
[[nodiscard]] std::vector<DevelopSetField> list_develop_set_fields();
[[nodiscard]] std::vector<std::string_view> develop_set_field_prefixes() noexcept;
[[nodiscard]] bool apply_develop_field(DevelopParams &params, std::string_view name, double value);
[[nodiscard]] Result<void> apply_develop_field_strict(DevelopParams &params, std::string_view name,
                                                      double value);
[[nodiscard]] Result<void> apply_develop_text_field_strict(DevelopParams &params,
                                                           std::string_view name,
                                                           std::string_view value);
[[nodiscard]] bool reset_develop_field(DevelopParams &params, std::string_view name);
[[nodiscard]] bool reset_develop_section(DevelopParams &params, std::string_view section);
[[nodiscard]] bool develop_section_modified(const DevelopParams &params, std::string_view section);
[[nodiscard]] bool develop_section_effect_enabled(const DevelopParams &params,
                                                  std::string_view section);
[[nodiscard]] bool set_develop_section_effect_enabled(DevelopParams &params,
                                                      std::string_view section, bool enabled);

struct DevelopChange
{
    std::string field;
    std::string value;
};

[[nodiscard]] std::vector<DevelopChange> develop_change_summary(const DevelopParams &before,
                                                                const DevelopParams &after);
// Stable logical fields shared by selective recipe styles and Studio's session
// clipboard. These are intentionally coarser than UI widget state: compound
// operations (curves, masks, Retouch, output layout, and profile state) remain
// atomic so an overlay cannot manufacture an invalid partial operation payload.
[[nodiscard]] std::span<const std::string_view> develop_selectable_field_names() noexcept;
[[nodiscard]] bool is_develop_selectable_field(std::string_view field) noexcept;
[[nodiscard]] std::vector<DevelopChange> develop_modified_fields(const DevelopParams &before,
                                                                 const DevelopParams &after);
[[nodiscard]] Result<void> apply_develop_selected_fields(DevelopParams &destination,
                                                         const DevelopParams &source,
                                                         const std::vector<std::string> &fields);
[[nodiscard]] bool apply_crop_aspect(DevelopParams &params, std::string_view aspect);
[[nodiscard]] double develop_crop_min_short_edge_pixels(double source_width,
                                                        double source_height) noexcept;
void clamp_develop_crop_min_extent(DevelopParams &params, double source_width,
                                   double source_height) noexcept;
void transform_crop_for_quarter_turns(DevelopParams &params, int turns_cw) noexcept;
void transform_crop_for_flip(DevelopParams &params, bool horizontal, bool vertical) noexcept;
[[nodiscard]] double working_image_aspect(std::int64_t rotate_quarters,
                                          double source_aspect) noexcept;
void map_straighten_normalized(double x, double y, double straighten_degrees, double working_aspect,
                               bool inverse, double &ox, double &oy) noexcept;
void straightened_source_quad(double straighten_degrees, double working_aspect,
                              std::array<double, 8> &corners) noexcept;
void inscribed_crop_for_straighten(double straighten_degrees, double working_aspect,
                                   double crop_aspect_norm, double &x, double &y, double &width,
                                   double &height) noexcept;
void constrain_crop_to_straighten(DevelopParams &params, double working_aspect) noexcept;
void fit_crop_to_straighten(DevelopParams &params, double working_aspect) noexcept;
void strip_crop_operations(Recipe &recipe);
void strip_straighten_operations(Recipe &recipe);

[[nodiscard]] Result<Recipe> recipe_from_develop(AssetDescriptor asset,
                                                 const DevelopParams &params);
[[nodiscard]] Result<DevelopParams> develop_from_recipe(const Recipe &recipe);

} // namespace ravo
