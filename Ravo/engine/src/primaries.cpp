#include "primaries.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <tuple>

namespace ravo
{
namespace
{

using Matrix3 = std::array<double, 9>;

struct Chromaticity
{
    double x = 0.0;
    double y = 0.0;
};

struct WorkingChromaticities
{
    std::array<Chromaticity, 3> primaries{};
    Chromaticity whitepoint{};
};

constexpr double kMatrixEpsilon = 1.0e-12;
constexpr double kGeometryEpsilon = 1.0e-12;

[[nodiscard]] bool finite_matrix(const Matrix3 &matrix) noexcept
{
    return std::all_of(matrix.begin(), matrix.end(),
                       [](const double value) { return std::isfinite(value); });
}

[[nodiscard]] bool finite_chromaticity(const Chromaticity value) noexcept
{
    return std::isfinite(value.x) && std::isfinite(value.y);
}

[[nodiscard]] Result<void> validate_bounded(const double value, const double minimum,
                                            const double maximum, const std::string_view name)
{
    if (!std::isfinite(value))
    {
        return make_error(ErrorCode::kValidation, "RGB primaries parameter must be finite",
                          {{"parameter", std::string(name)}});
    }
    if (value < minimum || value > maximum)
    {
        return make_error(ErrorCode::kValidation,
                          "RGB primaries parameter is outside the permitted range",
                          {{"parameter", std::string(name)}});
    }
    return {};
}

[[nodiscard]] Result<void> validate_params(const PrimariesParams &params)
{
    const std::array<std::tuple<double, double, double, std::string_view>, 8> fields{
        {{params.achromatic_tint_hue, kPrimariesHueMin, kPrimariesHueMax, "achromatic_tint_hue"},
         {params.achromatic_tint_purity, kPrimariesAchromaticTintPurityMin,
          kPrimariesAchromaticTintPurityMax, "achromatic_tint_purity"},
         {params.red_hue, kPrimariesHueMin, kPrimariesHueMax, "red_hue"},
         {params.red_purity, kPrimariesPrimaryPurityMin, kPrimariesPrimaryPurityMax, "red_purity"},
         {params.green_hue, kPrimariesHueMin, kPrimariesHueMax, "green_hue"},
         {params.green_purity, kPrimariesPrimaryPurityMin, kPrimariesPrimaryPurityMax,
          "green_purity"},
         {params.blue_hue, kPrimariesHueMin, kPrimariesHueMax, "blue_hue"},
         {params.blue_purity, kPrimariesPrimaryPurityMin, kPrimariesPrimaryPurityMax,
          "blue_purity"}}};
    for (const auto &[value, minimum, maximum, name] : fields)
    {
        auto valid = validate_bounded(value, minimum, maximum, name);
        if (!valid)
        {
            return valid.error();
        }
    }
    return {};
}

[[nodiscard]] Result<Matrix3> working_to_xyz_matrix(const ColorProfileState &profile)
{
    if (profile.kind == ColorProfileKind::kMissing || profile.model != ColorModel::kRgb ||
        !profile.has_matrix)
    {
        return make_error(ErrorCode::kValidation,
                          "RGB primaries requires a declared matrix RGB working profile",
                          {{"profile", profile.identifier}});
    }

    Matrix3 matrix{};
    for (std::size_t index = 0; index < matrix.size(); ++index)
    {
        matrix[index] = static_cast<double>(profile.matrix_to_xyz_d50[index]);
    }
    if (!finite_matrix(matrix))
    {
        return make_error(ErrorCode::kValidation,
                          "RGB primaries working profile matrix contains a non-finite value",
                          {{"profile", profile.identifier}});
    }
    return matrix;
}

[[nodiscard]] Result<Matrix3> invert_matrix(const Matrix3 &matrix, const std::string_view label)
{
    if (!finite_matrix(matrix))
    {
        return make_error(ErrorCode::kValidation,
                          "RGB primaries matrix contains a non-finite value",
                          {{"matrix", std::string(label)}});
    }

    const double a = matrix[0];
    const double b = matrix[1];
    const double c = matrix[2];
    const double d = matrix[3];
    const double e = matrix[4];
    const double f = matrix[5];
    const double g = matrix[6];
    const double h = matrix[7];
    const double i = matrix[8];
    const double determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!std::isfinite(determinant) || std::abs(determinant) < kMatrixEpsilon)
    {
        return make_error(ErrorCode::kValidation, "RGB primaries matrix is singular",
                          {{"matrix", std::string(label)}});
    }

    const double inverse = 1.0 / determinant;
    const Matrix3 result{
        (e * i - f * h) * inverse, (c * h - b * i) * inverse, (b * f - c * e) * inverse,
        (f * g - d * i) * inverse, (a * i - c * g) * inverse, (c * d - a * f) * inverse,
        (d * h - e * g) * inverse, (b * g - a * h) * inverse, (a * e - b * d) * inverse,
    };
    if (!finite_matrix(result))
    {
        return make_error(ErrorCode::kValidation, "RGB primaries inverted matrix is non-finite",
                          {{"matrix", std::string(label)}});
    }
    return result;
}

[[nodiscard]] std::array<double, 3> apply_matrix(const Matrix3 &matrix,
                                                 const std::array<double, 3> &value) noexcept
{
    return {matrix[0] * value[0] + matrix[1] * value[1] + matrix[2] * value[2],
            matrix[3] * value[0] + matrix[4] * value[1] + matrix[5] * value[2],
            matrix[6] * value[0] + matrix[7] * value[1] + matrix[8] * value[2]};
}

[[nodiscard]] Matrix3 multiply_matrices(const Matrix3 &left, const Matrix3 &right) noexcept
{
    Matrix3 result{};
    for (std::size_t row = 0; row < 3U; ++row)
    {
        for (std::size_t column = 0; column < 3U; ++column)
        {
            for (std::size_t shared = 0; shared < 3U; ++shared)
            {
                result[row * 3U + column] += left[row * 3U + shared] * right[shared * 3U + column];
            }
        }
    }
    return result;
}

[[nodiscard]] Result<Chromaticity> chromaticity_from_xyz(const std::array<double, 3> &xyz,
                                                         const std::string_view label)
{
    if (!std::all_of(xyz.begin(), xyz.end(),
                     [](const double value) { return std::isfinite(value); }))
    {
        return make_error(ErrorCode::kValidation, "RGB primaries profile XYZ value is non-finite",
                          {{"component", std::string(label)}});
    }
    const double sum = xyz[0] + xyz[1] + xyz[2];
    if (!std::isfinite(sum) || std::abs(sum) < kGeometryEpsilon)
    {
        return make_error(ErrorCode::kValidation, "RGB primaries profile XYZ sum is invalid",
                          {{"component", std::string(label)}});
    }
    const Chromaticity result{xyz[0] / sum, xyz[1] / sum};
    if (!finite_chromaticity(result) || std::abs(result.y) < kGeometryEpsilon)
    {
        return make_error(ErrorCode::kValidation,
                          "RGB primaries profile chromaticity has an invalid Y division",
                          {{"component", std::string(label)}});
    }
    return result;
}

[[nodiscard]] double cross(const Chromaticity left, const Chromaticity right) noexcept
{
    return left.x * right.y - left.y * right.x;
}

[[nodiscard]] Chromaticity subtract(const Chromaticity left, const Chromaticity right) noexcept
{
    return {left.x - right.x, left.y - right.y};
}

[[nodiscard]] Result<WorkingChromaticities> derive_working_chromaticities(const Matrix3 &matrix)
{
    WorkingChromaticities result;
    for (std::size_t primary = 0; primary < result.primaries.size(); ++primary)
    {
        auto chromaticity =
            chromaticity_from_xyz({matrix[primary], matrix[3U + primary], matrix[6U + primary]},
                                  "primary_" + std::to_string(primary));
        if (!chromaticity)
        {
            return chromaticity.error();
        }
        result.primaries[primary] = chromaticity.value();
    }

    auto whitepoint =
        chromaticity_from_xyz({matrix[0] + matrix[1] + matrix[2], matrix[3] + matrix[4] + matrix[5],
                               matrix[6] + matrix[7] + matrix[8]},
                              "whitepoint");
    if (!whitepoint)
    {
        return whitepoint.error();
    }
    result.whitepoint = whitepoint.value();

    const Chromaticity first = subtract(result.primaries[1], result.primaries[0]);
    const Chromaticity second = subtract(result.primaries[2], result.primaries[0]);
    const double twice_area = cross(first, second);
    if (!std::isfinite(twice_area) || std::abs(twice_area) < kGeometryEpsilon)
    {
        return make_error(ErrorCode::kValidation, "RGB primaries working triangle is degenerate");
    }
    return result;
}

[[nodiscard]] Result<double> ray_triangle_edge_distance(const WorkingChromaticities &profile,
                                                        const Chromaticity direction,
                                                        const std::size_t primary_index)
{
    if (!finite_chromaticity(direction))
    {
        return make_error(ErrorCode::kValidation, "RGB primaries ray direction is non-finite",
                          {{"primary_index", std::to_string(primary_index)}});
    }

    bool saw_parallel = false;
    bool saw_backwards = false;
    double nearest = std::numeric_limits<double>::infinity();
    for (std::size_t edge_index = 0; edge_index < profile.primaries.size(); ++edge_index)
    {
        const Chromaticity edge_start = profile.primaries[edge_index];
        const Chromaticity edge_end = profile.primaries[(edge_index + 1U) % 3U];
        const Chromaticity edge = subtract(edge_end, edge_start);
        const Chromaticity origin_to_edge = subtract(edge_start, profile.whitepoint);
        const double denominator = cross(direction, edge);
        if (!std::isfinite(denominator))
        {
            return make_error(ErrorCode::kValidation,
                              "RGB primaries edge intersection is non-finite",
                              {{"primary_index", std::to_string(primary_index)}});
        }
        if (std::abs(denominator) < kGeometryEpsilon)
        {
            saw_parallel = true;
            continue;
        }

        const double distance = cross(origin_to_edge, edge) / denominator;
        if (!std::isfinite(distance))
        {
            return make_error(ErrorCode::kValidation,
                              "RGB primaries edge intersection is non-finite",
                              {{"primary_index", std::to_string(primary_index)}});
        }
        if (distance <= kGeometryEpsilon)
        {
            saw_backwards = true;
            continue;
        }
        nearest = std::min(nearest, distance);
    }

    if (!std::isfinite(nearest))
    {
        return make_error(ErrorCode::kValidation,
                          saw_backwards ?
                              "RGB primaries ray has only backwards edge intersections" :
                              "RGB primaries ray has no forward triangle-edge intersection",
                          {{"primary_index", std::to_string(primary_index)},
                           {"parallel_edges", saw_parallel ? "true" : "false"}});
    }
    return nearest;
}

[[nodiscard]] Result<Chromaticity> rotate_and_scale_primary(const WorkingChromaticities &profile,
                                                            const double scaling,
                                                            const double rotation,
                                                            const std::size_t primary_index)
{
    const Chromaticity offset = subtract(profile.primaries[primary_index], profile.whitepoint);
    const double angle = std::atan2(offset.y, offset.x) + rotation;
    const Chromaticity direction{std::cos(angle), std::sin(angle)};
    auto distance = ray_triangle_edge_distance(profile, direction, primary_index);
    if (!distance)
    {
        return distance.error();
    }
    const Chromaticity result{profile.whitepoint.x + scaling * distance.value() * direction.x,
                              profile.whitepoint.y + scaling * distance.value() * direction.y};
    if (!finite_chromaticity(result) || std::abs(result.y) < kGeometryEpsilon)
    {
        return make_error(ErrorCode::kValidation,
                          "RGB primaries rotated chromaticity has an invalid Y division",
                          {{"primary_index", std::to_string(primary_index)}});
    }
    return result;
}

[[nodiscard]] Result<Matrix3>
matrix_from_primaries_and_whitepoint(const std::array<Chromaticity, 3> &primaries,
                                     const Chromaticity whitepoint)
{
    Matrix3 primary_matrix{};
    for (std::size_t primary = 0; primary < primaries.size(); ++primary)
    {
        if (!finite_chromaticity(primaries[primary]) ||
            std::abs(primaries[primary].y) < kGeometryEpsilon)
        {
            return make_error(ErrorCode::kValidation,
                              "RGB primaries custom primary has an invalid Y division",
                              {{"primary_index", std::to_string(primary)}});
        }
        primary_matrix[primary] = primaries[primary].x / primaries[primary].y;
        primary_matrix[3U + primary] = 1.0;
        primary_matrix[6U + primary] =
            (1.0 - primaries[primary].x - primaries[primary].y) / primaries[primary].y;
    }
    if (!finite_matrix(primary_matrix) || !finite_chromaticity(whitepoint) ||
        std::abs(whitepoint.y) < kGeometryEpsilon)
    {
        return make_error(ErrorCode::kValidation,
                          "RGB primaries custom matrix has an invalid chromaticity division");
    }

    auto inverse = invert_matrix(primary_matrix, "custom_primary_chromaticities");
    if (!inverse)
    {
        return inverse.error();
    }
    const std::array<double, 3> white_xyz{whitepoint.x / whitepoint.y, 1.0,
                                          (1.0 - whitepoint.x - whitepoint.y) / whitepoint.y};
    if (!std::all_of(white_xyz.begin(), white_xyz.end(),
                     [](const double value) { return std::isfinite(value); }))
    {
        return make_error(ErrorCode::kValidation, "RGB primaries custom white point is non-finite");
    }
    const std::array<double, 3> scale = apply_matrix(inverse.value(), white_xyz);
    if (!std::all_of(scale.begin(), scale.end(),
                     [](const double value) { return std::isfinite(value); }))
    {
        return make_error(ErrorCode::kValidation,
                          "RGB primaries custom primary scale is non-finite");
    }

    Matrix3 rgb_to_xyz{};
    for (std::size_t row = 0; row < 3U; ++row)
    {
        for (std::size_t column = 0; column < 3U; ++column)
        {
            rgb_to_xyz[row * 3U + column] = primary_matrix[row * 3U + column] * scale[column];
        }
    }
    auto validated = invert_matrix(rgb_to_xyz, "custom_rgb_to_xyz_d50");
    if (!validated)
    {
        return validated.error();
    }
    return rgb_to_xyz;
}

[[nodiscard]] Result<void> validate_input(const WorkingImage &input,
                                          const CancellationToken &cancellation)
{
    const std::uint64_t pixels = static_cast<std::uint64_t>(input.width) * input.height;
    if (input.width == 0 || input.height == 0 ||
        pixels > std::numeric_limits<std::size_t>::max() / 3U ||
        input.rgb.size() != static_cast<std::size_t>(pixels * 3U))
    {
        return make_error(ErrorCode::kValidation,
                          "RGB primaries input buffer does not match its dimensions");
    }
    for (std::uint32_t row = 0; row < input.height; ++row)
    {
        auto active = cancellation.check();
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
                                  "RGB primaries input contains a non-finite sample",
                                  {{"sample_index", std::to_string(index)}});
            }
        }
    }
    return {};
}

[[nodiscard]] Result<WorkingImage> apply_primaries_impl(const WorkingImage &input,
                                                        const PrimariesParams &params,
                                                        const CancellationToken &cancellation)
{
    auto active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    auto valid_input = validate_input(input, cancellation);
    if (!valid_input)
    {
        return valid_input.error();
    }
    auto adjustment = primaries_adjustment_matrix(input.color_profile, params);
    if (!adjustment)
    {
        return adjustment.error();
    }

    WorkingImage output;
    output.width = input.width;
    output.height = input.height;
    output.color_profile = input.color_profile;
    output.exposure_analysis = input.exposure_analysis;
    output.canonical_roi_scale = input.canonical_roi_scale;
    output.mask_attached_frame = input.mask_attached_frame;
    output.rgb.resize(input.rgb.size());
    for (std::uint32_t row = 0; row < input.height; ++row)
    {
        active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
        const std::size_t begin = static_cast<std::size_t>(row) * input.width * 3U;
        const std::size_t end = begin + static_cast<std::size_t>(input.width) * 3U;
        for (std::size_t index = begin; index < end; index += 3U)
        {
            const std::array<double, 3> source{input.rgb[index], input.rgb[index + 1U],
                                               input.rgb[index + 2U]};
            const std::array<double, 3> transformed = apply_matrix(adjustment.value(), source);
            for (std::size_t channel = 0; channel < transformed.size(); ++channel)
            {
                if (!std::isfinite(transformed[channel]) ||
                    std::abs(transformed[channel]) > std::numeric_limits<float>::max())
                {
                    return make_error(ErrorCode::kValidation,
                                      "RGB primaries produced an unrepresentable sample",
                                      {{"sample_index", std::to_string(index + channel)}});
                }
                const float sample = static_cast<float>(transformed[channel]);
                output.rgb[index + channel] = sample;
            }
        }
    }
    return output;
}

} // namespace

Result<std::array<double, 9>> primaries_adjustment_matrix(const ColorProfileState &working_profile,
                                                          const PrimariesParams &params)
{
    auto valid_params = validate_params(params);
    if (!valid_params)
    {
        return valid_params.error();
    }
    auto working_to_xyz = working_to_xyz_matrix(working_profile);
    if (!working_to_xyz)
    {
        return working_to_xyz.error();
    }
    auto chromaticities = derive_working_chromaticities(working_to_xyz.value());
    if (!chromaticities)
    {
        return chromaticities.error();
    }
    auto xyz_to_working = invert_matrix(working_to_xyz.value(), "working_rgb_to_xyz_d50");
    if (!xyz_to_working)
    {
        return xyz_to_working.error();
    }
    if (params.is_identity())
    {
        return Matrix3{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
    }

    const std::array<double, 3> scaling{params.red_purity, params.green_purity, params.blue_purity};
    const std::array<double, 3> rotation{params.red_hue, params.green_hue, params.blue_hue};
    std::array<Chromaticity, 3> custom_primaries{};
    for (std::size_t primary = 0; primary < custom_primaries.size(); ++primary)
    {
        auto transformed = rotate_and_scale_primary(chromaticities.value(), scaling[primary],
                                                    rotation[primary], primary);
        if (!transformed)
        {
            return transformed.error();
        }
        custom_primaries[primary] = transformed.value();
    }
    auto custom_whitepoint = rotate_and_scale_primary(
        chromaticities.value(), params.achromatic_tint_purity, params.achromatic_tint_hue, 0U);
    if (!custom_whitepoint)
    {
        return custom_whitepoint.error();
    }

    auto custom_to_xyz =
        matrix_from_primaries_and_whitepoint(custom_primaries, custom_whitepoint.value());
    if (!custom_to_xyz)
    {
        return custom_to_xyz.error();
    }
    const Matrix3 adjustment = multiply_matrices(xyz_to_working.value(), custom_to_xyz.value());
    if (!finite_matrix(adjustment))
    {
        return make_error(ErrorCode::kValidation, "RGB primaries adjustment matrix is non-finite");
    }
    return adjustment;
}

Result<WorkingImage> apply_primaries(const WorkingImage &input, const PrimariesParams &params,
                                     const CancellationToken &cancellation)
try
{
    return apply_primaries_impl(input, params, cancellation);
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "RGB primaries output allocation failed",
                      {{"reason", "allocation_failed"}});
}

Result<WorkingImage> apply_primaries(const WorkingImage &input, const OperationInstance &operation,
                                     const CancellationToken &cancellation)
try
{
    if (operation.id != kPrimariesOperationId)
    {
        return make_error(ErrorCode::kValidation, "Operation is not RGB primaries",
                          {{"operation_id", operation.id}});
    }
    if (operation.schema_version != 1)
    {
        return make_error(ErrorCode::kValidation, "RGB primaries schema version is unsupported",
                          {{"schema_version", std::to_string(operation.schema_version)}});
    }
    if (operation.mask_id.has_value())
    {
        return make_error(ErrorCode::kUnsupported, "RGB primaries does not support masks");
    }
    if (!operation.enabled)
    {
        return input;
    }
    auto params = primaries_from_parameters(operation.parameters);
    if (!params)
    {
        return params.error();
    }
    return apply_primaries_impl(input, params.value(), cancellation);
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "RGB primaries operation allocation failed",
                      {{"reason", "allocation_failed"}});
}

} // namespace ravo
