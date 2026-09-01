#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "capability_ops.h"

namespace ravo::capability_internal
{

inline constexpr float kPi = std::numbers::pi_v<float>;
inline constexpr float kTwoPi = 2.0F * kPi;
inline constexpr int kColorNodes = 8;
inline constexpr int kUcsLutSize = 512;
inline constexpr int kToneAnchorCount = 9;
inline constexpr int kToneLutResolution = 10000;
inline constexpr float kToneLutMinEv = -8.0F;
inline constexpr float kToneLutMaxEv = 0.0F;
inline constexpr float kToneMaskRadiusOriginalPixels = 240.0F;
inline constexpr float kToneMaskEpsilonEv = 0.04F;
inline constexpr int kVignetteSplines = 512;
inline constexpr int kDenoiseBands = 7;
inline constexpr float kDenoisePFulcrum = 0.05F;
inline constexpr float kSatEffect = 2.0F;
inline constexpr float kBrightEffect = 8.0F;
inline constexpr float kUcsLStarRange = 2.098883786377F;
inline constexpr float kUcsLStarUpper = 2.09885F;
inline constexpr float kAngleShiftDeg = 20.0F;
inline constexpr float kGenericNoiseA = 1.0e-4F;
inline constexpr float kGenericNoiseB = 0.0F;
inline constexpr std::size_t kDenoiseNoiseSampleLimit = 1U << 18U;
inline constexpr float kDenoiseGaussianMad = 0.67448975F;
inline constexpr float kChannelMixerNormMin = 1.52587890625e-05F;
inline constexpr float kChannelMixerInverseSqrt3 = 0.5773502691896258F;

using ChannelVector = std::array<float, 3>;
using ChannelMatrix = std::array<ChannelVector, 3>;

inline constexpr ChannelMatrix kLinearSrgbToXyzD65 = {
    ChannelVector{0.4124564F, 0.3575761F, 0.1804375F},
    ChannelVector{0.2126729F, 0.7151522F, 0.0721750F},
    ChannelVector{0.0193339F, 0.1191920F, 0.9503041F}};
inline constexpr ChannelMatrix kXyzD65ToD50Cat16 = {
    ChannelVector{1.01085433F, 0.0407086103F, -0.0341425825F},
    ChannelVector{0.00542814201F, 0.993581926F, 0.00115592039F},
    ChannelVector{0.000250722468F, -0.0114918759F, 0.767964947F}};
inline constexpr ChannelMatrix kXyzToCat16 = {ChannelVector{0.401288F, 0.650173F, -0.051461F},
                                              ChannelVector{-0.250268F, 1.204414F, 0.045854F},
                                              ChannelVector{-0.002079F, 0.048952F, 0.953127F}};
inline constexpr ChannelMatrix kCat16ToXyz = {ChannelVector{1.862068F, -1.011255F, 0.149187F},
                                              ChannelVector{0.38752F, 0.621447F, -0.008974F},
                                              ChannelVector{-0.015841F, -0.034123F, 1.049964F}};
inline constexpr ChannelMatrix kXyzToBradford = {ChannelVector{0.8951F, 0.2664F, -0.1614F},
                                                 ChannelVector{-0.7502F, 1.7135F, 0.0367F},
                                                 ChannelVector{0.0389F, -0.0685F, 1.0296F}};
inline constexpr ChannelMatrix kBradfordToXyz = {ChannelVector{0.9870F, -0.1471F, 0.1600F},
                                                 ChannelVector{0.4323F, 0.5184F, 0.0493F},
                                                 ChannelVector{-0.0085F, 0.0400F, 0.9685F}};
inline constexpr ChannelMatrix kIdentityMatrix = {ChannelVector{1.0F, 0.0F, 0.0F},
                                                  ChannelVector{0.0F, 1.0F, 0.0F},
                                                  ChannelVector{0.0F, 0.0F, 1.0F}};

enum class ChannelAdaptation
{
    kRgb,
    kCat16,
    kLinearBradford,
    kFullBradford,
    kXyz,
};

struct LensCalibration
{
    std::string_view camera_make;
    std::string_view camera_model;
    std::string_view lens;
    double focal_mm = 50.0;
    double k1 = 0.0;
    double k2 = 0.0;
    double tca_r = 1.0;
    double tca_b = 1.0;
    double vignetting = 0.0;
};

[[nodiscard]] ChannelVector channel_matrix_apply(const ChannelMatrix &matrix,
                                                 const ChannelVector &input) noexcept;
[[nodiscard]] ChannelMatrix channel_matrix_multiply(const ChannelMatrix &left,
                                                    const ChannelMatrix &right) noexcept;
[[nodiscard]] bool channel_matrix_inverse(const ChannelMatrix &input,
                                          ChannelMatrix &output) noexcept;
[[nodiscard]] bool channel_vector_is_finite(const ChannelVector &value) noexcept;
[[nodiscard]] bool channel_gamut_map(const ChannelVector &input, float gamut, bool clip,
                                     ChannelVector &output) noexcept;
void channel_clip_negative(ChannelVector &value) noexcept;
void channel_downscale(ChannelVector &value, float scale) noexcept;
void channel_upscale(ChannelVector &value, float scale) noexcept;
[[nodiscard]] ChannelVector channel_luma_chroma(const ChannelVector &input,
                                                const ChannelVector &saturation,
                                                const ChannelVector &lightness) noexcept;

[[nodiscard]] double as_number(const ParameterValue &value, double fallback);
[[nodiscard]] double parameter(const OperationInstance &operation, std::string_view name,
                               double fallback);
[[nodiscard]] std::string parameter_string(const OperationInstance &operation,
                                           std::string_view name, const std::string &fallback);
[[nodiscard]] bool parameter_bool(const OperationInstance &operation, std::string_view name,
                                  bool fallback);
[[nodiscard]] Result<std::array<double, kColorEqualizerBandCount>>
parameter_band_array(const OperationInstance &operation, std::string_view name);
[[nodiscard]] std::uint8_t cfa_channel(const DecodedRaw &raw, std::uint32_t x,
                                       std::uint32_t y) noexcept;
[[nodiscard]] const LensCalibration *find_lens_calibration(std::string_view make,
                                                           std::string_view model,
                                                           std::string_view lens, double focal_mm);
[[nodiscard]] float sample_channel(const WorkingImage &image, float x, float y,
                                   std::size_t channel) noexcept;
[[nodiscard]] Result<void> blur_plane(std::vector<float> &plane, std::uint32_t width,
                                      std::uint32_t height, float sigma,
                                      const CancellationToken &cancellation);
[[nodiscard]] Result<std::vector<float>> raw_to_float(const DecodedRaw &raw);
void float_to_raw(DecodedRaw &raw, const std::vector<float> &buffer, double amount);
void process_highlights_clip(std::vector<float> &buffer, const DecodedRaw &raw,
                             const std::array<float, 3> &clips);
void process_highlights_lch(std::vector<float> &buffer, const DecodedRaw &raw, float clip);
void process_highlights_inpaint(std::vector<float> &buffer, const DecodedRaw &raw,
                                const std::array<float, 3> &clips);
void process_highlights_opposed(std::vector<float> &buffer, const DecodedRaw &raw,
                                const std::array<float, 3> &clips);
[[nodiscard]] Result<void> eaw_dn_decompose(std::vector<float> &coarse,
                                            const std::vector<float> &input,
                                            std::vector<float> &detail,
                                            std::array<float, 3> &sum_squared, int scale,
                                            float radius, float inv_sigma2, int width, int height,
                                            const CancellationToken &cancellation);
[[nodiscard]] Result<void> eaw_synthesize(std::vector<float> &accum,
                                          const std::vector<float> &detail,
                                          const std::array<float, 3> &threshold, int width,
                                          int height, const CancellationToken &cancellation);
[[nodiscard]] Result<std::array<float, 3>>
estimate_wavelet_noise_sigma(const std::vector<float> &detail, std::size_t pixel_count,
                             const CancellationToken &cancellation);
[[nodiscard]] bool invert_matrix3(const float input[3][3], float output[3][3]) noexcept;
void apply_matrix(const float matrix[3][3], const float input[3], float output[3]) noexcept;
void hsl_to_rgb(float h, float s, float l, float &r, float &g, float &b) noexcept;
[[nodiscard]] float compute_density(float dens, float length) noexcept;
[[nodiscard]] float gaussian_denom(float sigma) noexcept;
[[nodiscard]] float gaussian_func(float radius, float denominator) noexcept;
void rgb_to_xyz_d65(float r, float g, float b, float &x, float &y, float &z) noexcept;
void xyz_d65_to_rgb(float x, float y, float z, float &r, float &g, float &b) noexcept;
void xyz_to_xyy(float x, float y, float z, float &xyx, float &xyy, float &xy_y) noexcept;
void xyy_to_xyz(float xyx, float xyy, float xy_y, float &x, float &y, float &z) noexcept;
[[nodiscard]] float y_to_ucs_l_star(float y) noexcept;
void xyy_to_ucs_uv(float xyx, float xyy, float uv[2]) noexcept;
void ucs_luv_to_jch(float l_star, float l_white, const float uv[2], float jch[3]) noexcept;
void ucs_jch_to_hsb(const float jch[3], float hsb[3]) noexcept;
void ucs_hsb_to_jch(const float hsb[3], float jch[3]) noexcept;
void ucs_jch_to_xyy(const float jch[3], float l_white, float &xyx, float &xyy, float &xy_y);
void ucs_hsb_to_rgb(const float hsb[3], float l_white, float &r, float &g, float &b);
[[nodiscard]] float lookup_lut_periodic(const std::array<float, kUcsLutSize> &lut,
                                        float hue) noexcept;
void periodic_rbf_interpolate(std::array<float, kColorNodes> nodes, float smoothing,
                              std::array<float, kUcsLutSize> &lut, float hue_shift,
                              bool clip_positive);

} // namespace ravo::capability_internal
