#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ravo/recipe/color_input.h"
#include "ravo/recipe/color_output.h"
#include "ravo/recipe/recipe.h"

namespace ravo
{

inline constexpr double kDevelopGammaDefault = 1.0;
inline constexpr double kDevelopStraightenMin = -45.0;
inline constexpr double kDevelopStraightenMax = 45.0;
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
inline constexpr std::string_view kToneCurveChannelModeRgb = "rgb";
inline constexpr std::string_view kToneCurveChannelModeIndependent = "independent";
inline constexpr std::string_view kToneCurvePreserveColorsAverage = "average";
inline constexpr std::string_view kToneCurvePreserveColorsNone = "none";
inline constexpr std::string_view kToneCurvePreserveColorsLuminance = "luminance";
inline constexpr std::string_view kToneCurvePreserveColorsMax = "max";
inline constexpr std::string_view kToneCurvePreserveColorsSum = "sum";
inline constexpr std::string_view kToneCurvePreserveColorsNorm = "norm";
inline constexpr std::string_view kToneCurvePreserveColorsPower = "power";
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

struct DevelopParams
{
    TemperatureParams temperature;
    InputColorParams input_color;
    OutputColorParams output_color;
    ChannelMixerParams channel_mixer;
    double exposure_ev = 0.0;
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
    double crop_x = 0.0;
    double crop_y = 0.0;
    double crop_width = 1.0;
    double crop_height = 1.0;
    double sharpen = 0.0;
    double sharpen_radius = 1.0;
    double clarity = 0.0;
    double vignette = 0.0;
    double grain = 0.0;
    double bloom = 0.0;
    double soften = 0.0;
    double dehaze = 0.0;
    double velvia = 0.0;
    ColorBalanceRgbParams color_balance_rgb;
    double color_contrast = 0.0;
    double monochrome = 0.0;
    double split_shadows_hue = 0.55;
    double split_highlights_hue = 0.08;
    double split_balance = 0.5;
    double split_amount = 0.0;
    double gamma = kDevelopGammaDefault;
    std::vector<ToneCurvePoint> tone_curve;
    std::string tone_curve_working_space{std::string(kToneCurveWorkingSpaceSrgb)};
    bool sigmoid_enabled = false;
    double sigmoid_contrast = kSigmoidContrastDefault;
    double sigmoid_skew = kSigmoidSkewDefault;
    double sigmoid_display_white = kSigmoidDisplayWhiteDefault;
    double sigmoid_display_black = kSigmoidDisplayBlackDefault;
    double sigmoid_hue_preservation = kSigmoidHuePreservationDefault;
    double raw_highlights = 0.0;
    double raw_highlights_clip = 0.987;
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
    double tone_eq_blacks = 0.0;
    double tone_eq_shadows = 0.0;
    double tone_eq_midtones = 0.0;
    double tone_eq_highlights = 0.0;
    double tone_eq_whites = 0.0;

    [[nodiscard]] bool is_identity() const noexcept;
    [[nodiscard]] bool operator==(const DevelopParams &) const noexcept = default;
};

[[nodiscard]] bool tone_curve_is_identity(const std::vector<ToneCurvePoint> &points) noexcept;
void clamp_tone_curve(std::vector<ToneCurvePoint> &points) noexcept;
[[nodiscard]] double evaluate_tone_curve(const std::vector<ToneCurvePoint> &points,
                                         double x) noexcept;
[[nodiscard]] Result<ToneCurveWorkingSpace> parse_tone_curve_working_space(std::string_view text);
[[nodiscard]] std::string_view tone_curve_working_space_name(ToneCurveWorkingSpace space) noexcept;
[[nodiscard]] Result<std::vector<ToneCurvePoint>>
parse_tone_curve_points(const ParameterValue &value);
[[nodiscard]] ParameterValue
tone_curve_points_to_parameter(const std::vector<ToneCurvePoint> &points);
[[nodiscard]] Result<void> validate_tone_curve_parameters(
    const std::map<std::string, ParameterValue, std::less<>> &parameters);
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
[[nodiscard]] Result<void> validate_color_balance_rgb_parameters(
    const std::map<std::string, ParameterValue, std::less<>> &parameters);
[[nodiscard]] Result<ColorBalanceRgbParams> color_balance_rgb_from_parameters(
    const std::map<std::string, ParameterValue, std::less<>> &parameters);
[[nodiscard]] std::map<std::string, ParameterValue, std::less<>>
color_balance_rgb_to_parameters(const ColorBalanceRgbParams &params);

void clamp_develop(DevelopParams &params) noexcept;
[[nodiscard]] bool apply_develop_field(DevelopParams &params, std::string_view name, double value);
[[nodiscard]] bool reset_develop_field(DevelopParams &params, std::string_view name);
[[nodiscard]] bool reset_develop_section(DevelopParams &params, std::string_view section);
[[nodiscard]] bool apply_crop_aspect(DevelopParams &params, std::string_view aspect);
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
