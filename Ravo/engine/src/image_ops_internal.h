#pragma once

#include "ravo/engine/engine.h"
#include "ravo/recipe/operation.h"

namespace ravo::image_ops_internal
{

struct LightControlAmounts
{
    double highlights = 0.0;
    double shadows = 0.0;
    double whites = 0.0;
    double blacks = 0.0;
};

inline constexpr int kToneCurveLut = 0x10000;

[[nodiscard]] int light_control_rank(std::string_view id) noexcept;

[[nodiscard]] bool absorbed_operation(std::string_view id) noexcept;
[[nodiscard]] double parameter(const OperationInstance &operation, std::string_view name,
                               double fallback);
[[nodiscard]] std::string parameter_string(const OperationInstance &operation,
                                           std::string_view name, const std::string &fallback);
void linear_rgb_to_xyz_d50(float r, float g, float b, float xyz[3]) noexcept;
void xyz_d50_to_linear_rgb(const float xyz[3], float &r, float &g, float &b) noexcept;
void xyz_d50_to_lab(const float xyz[3], float lab[3]) noexcept;
void lab_to_xyz_d50(const float lab[3], float xyz[3]) noexcept;
void lab_to_prophoto(const float lab[3], float rgb[3]) noexcept;
void prophoto_to_lab(const float rgb[3], float lab[3]) noexcept;
void xyz_to_prophoto(const float xyz[3], float rgb[3]) noexcept;
void prophoto_to_xyz(const float rgb[3], float xyz[3]) noexcept;
[[nodiscard]] float rgb_norm(const float rgb[3], std::string_view preserve) noexcept;
void estimate_exp(const float *x, const float *y, int count, float coeff[3]) noexcept;
[[nodiscard]] float lookup_curve_lut(const std::vector<float> &lut, float x, float xm,
                                     const float coeff[3]) noexcept;
[[nodiscard]] float display_srgb_encode(float linear) noexcept;
[[nodiscard]] float display_srgb_decode(float encoded) noexcept;
[[nodiscard]] Result<void>
build_unit_lut(const std::vector<ToneCurvePoint> &points, std::vector<float> &lut,
               std::string_view interpolation = kToneCurveInterpolationMonotoneHermite);
void rgb_to_lab(float r, float g, float b, float &lightness, float &a, float &b_channel);
void lab_to_rgb(float lightness, float a, float b_channel, float &r, float &g, float &b);
void rgb_to_hsl(float r, float g, float b, float &h, float &s, float &l);
void hsl_to_rgb(float h, float s, float l, float &r, float &g, float &b);
[[nodiscard]] Result<WorkingImage> apply_exposure_impl(const WorkingImage &input,
                                                       const ExposureParams &params,
                                                       const CancellationToken &cancellation);
[[nodiscard]] Result<WorkingImage> rotate_working(WorkingImage image, int quarters);
[[nodiscard]] Result<WorkingImage> crop_working(WorkingImage image, double x, double y,
                                                double width, double height);
[[nodiscard]] Result<WorkingImage> flip_working(WorkingImage image, bool horizontal, bool vertical);
[[nodiscard]] Result<WorkingImage> straighten_working(WorkingImage image, double degrees);
[[nodiscard]] Result<void> apply_rgb_levels(WorkingImage &image,
                                            const OperationInstance &operation);
[[nodiscard]] Result<void> apply_rgb_curve(WorkingImage &image, const OperationInstance &operation,
                                           const CancellationToken &cancellation);
[[nodiscard]] Result<void> apply_tone_curve(WorkingImage &image, const OperationInstance &operation,
                                            const CancellationToken &cancellation);
[[nodiscard]] Result<void> apply_sigmoid(WorkingImage &image, const OperationInstance &operation,
                                         const CancellationToken &cancellation);
void apply_contrast(WorkingImage &image, double amount);
[[nodiscard]] Result<void> apply_light_controls(WorkingImage &image,
                                                const LightControlAmounts &amounts,
                                                const CancellationToken &cancellation);
[[nodiscard]] Result<void> apply_vibrance_saturation(WorkingImage &image, double vibrance,
                                                     double saturation,
                                                     const CancellationToken &cancellation);
void apply_clarity(WorkingImage &image, double amount);
void apply_soften(WorkingImage &image, double amount);
void apply_bloom(WorkingImage &image, double amount);
[[nodiscard]] Result<void> apply_vignette(WorkingImage &image, double amount, double midpoint,
                                          double falloff, double shape, double center_x,
                                          double center_y, const CancellationToken &cancellation);
void apply_grain(WorkingImage &image, double amount);
void apply_gamma(WorkingImage &image, double gamma);

} // namespace ravo::image_ops_internal

namespace ravo
{
[[nodiscard]] Result<WorkingImage>
apply_masked_color_harmonizer(WorkingImage image, const Recipe &recipe,
                              const OperationInstance &operation,
                              const CancellationToken &cancellation);
[[nodiscard]] Result<WorkingImage> apply_masked_color_zones(WorkingImage image,
                                                            const Recipe &recipe,
                                                            const OperationInstance &operation,
                                                            const CancellationToken &cancellation);
[[nodiscard]] Result<WorkingImage> apply_masked_monochrome(WorkingImage image, const Recipe &recipe,
                                                           const OperationInstance &operation,
                                                           const CancellationToken &cancellation);
[[nodiscard]] Result<WorkingImage> apply_masked_split_toning(WorkingImage image,
                                                             const Recipe &recipe,
                                                             const OperationInstance &operation,
                                                             const CancellationToken &cancellation);
[[nodiscard]] Result<WorkingImage> apply_masked_velvia(WorkingImage image, const Recipe &recipe,
                                                       const OperationInstance &operation,
                                                       const CancellationToken &cancellation);
[[nodiscard]] Result<WorkingImage>
apply_masked_color_balance_rgb(WorkingImage image, const Recipe &recipe,
                               const OperationInstance &operation,
                               const CancellationToken &cancellation);
[[nodiscard]] Result<WorkingImage> apply_masked_exposure(WorkingImage image, const Recipe &recipe,
                                                         const OperationInstance &operation,
                                                         const CancellationToken &cancellation);
[[nodiscard]] Result<WorkingImage> apply_masked_graduated_nd(WorkingImage image,
                                                             const Recipe &recipe,
                                                             const OperationInstance &operation,
                                                             const CancellationToken &cancellation);
} // namespace ravo
