#include "dng_opcodes.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <limits>
#include <new>
#include <string>
#include <type_traits>
#include <utility>

#include "parallel_rows.h"

namespace ravo
{
namespace
{

inline constexpr std::uint32_t kOpcodeWarpRectilinear = 1U;
inline constexpr std::uint32_t kOpcodeFixVignetteRadial = 3U;
inline constexpr std::uint32_t kOpcodeGainMap = 9U;
inline constexpr std::uint32_t kOpcodeOptionalFlag = 1U;
inline constexpr std::uint32_t kOpcodePreviewSkipFlag = 2U;
inline constexpr std::uint32_t kKnownOpcodeFlags =
    kOpcodeOptionalFlag | kOpcodePreviewSkipFlag;
inline constexpr std::uint32_t kSupportedDngVersion = 0x01070100U;
inline constexpr std::size_t kOpcodeHeaderBytes = 16U;
inline constexpr std::size_t kGainMapHeaderBytes = 76U;
inline constexpr std::size_t kMaxGainMapPointsPerAxis = 4096U;

[[nodiscard]] constexpr bool checked_add(const std::size_t left, const std::size_t right,
                                         std::size_t &result) noexcept
{
    if (right > std::numeric_limits<std::size_t>::max() - left)
    {
        return false;
    }
    result = left + right;
    return true;
}

[[nodiscard]] constexpr bool checked_multiply(const std::size_t left, const std::size_t right,
                                              std::size_t &result) noexcept
{
    if (left != 0U && right > std::numeric_limits<std::size_t>::max() / left)
    {
        return false;
    }
    result = left * right;
    return true;
}

class BigEndianReader
{
public:
    explicit BigEndianReader(const std::span<const std::uint8_t> bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - offset_; }
    [[nodiscard]] std::size_t offset() const noexcept { return offset_; }

    [[nodiscard]] bool read_u32(std::uint32_t &value) noexcept
    {
        if (remaining() < 4U)
        {
            return false;
        }
        value = (static_cast<std::uint32_t>(bytes_[offset_]) << 24U) |
                (static_cast<std::uint32_t>(bytes_[offset_ + 1U]) << 16U) |
                (static_cast<std::uint32_t>(bytes_[offset_ + 2U]) << 8U) |
                static_cast<std::uint32_t>(bytes_[offset_ + 3U]);
        offset_ += 4U;
        return true;
    }

    [[nodiscard]] bool read_float(float &value) noexcept
    {
        std::uint32_t bits = 0U;
        if (!read_u32(bits))
        {
            return false;
        }
        value = std::bit_cast<float>(bits);
        return true;
    }

    [[nodiscard]] bool read_double(double &value) noexcept
    {
        if (remaining() < 8U)
        {
            return false;
        }
        std::uint64_t bits = 0U;
        for (std::size_t index = 0U; index < 8U; ++index)
        {
            bits = (bits << 8U) | bytes_[offset_ + index];
        }
        offset_ += 8U;
        value = std::bit_cast<double>(bits);
        return true;
    }

    [[nodiscard]] bool read_bytes(const std::size_t count,
                                  std::span<const std::uint8_t> &value) noexcept
    {
        if (count > remaining())
        {
            return false;
        }
        value = bytes_.subspan(offset_, count);
        offset_ += count;
        return true;
    }

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t offset_ = 0U;
};

[[nodiscard]] TaskError invalid_list_error(const std::uint32_t list, const std::uint32_t index,
                                           const std::string_view reason,
                                           const std::string_view message)
{
    return make_error(ErrorCode::kValidation, std::string(message),
                      {{"dng_opcode_index", std::to_string(index)},
                       {"dng_opcode_list", std::to_string(list)},
                       {"reason", std::string(reason)}});
}

[[nodiscard]] Result<DngGainMap> parse_gain_map(const std::span<const std::uint8_t> payload,
                                                const std::uint32_t list,
                                                const std::uint32_t index)
{
    if (payload.size() < kGainMapHeaderBytes)
    {
        return invalid_list_error(list, index, "invalid_dng_gain_map_size",
                                  "DNG GainMap payload is shorter than its fixed header");
    }
    BigEndianReader reader(payload);
    DngGainMap map;
    if (!reader.read_u32(map.top) || !reader.read_u32(map.left) ||
        !reader.read_u32(map.bottom) || !reader.read_u32(map.right) ||
        !reader.read_u32(map.plane) || !reader.read_u32(map.planes) ||
        !reader.read_u32(map.row_pitch) || !reader.read_u32(map.column_pitch) ||
        !reader.read_u32(map.map_points_vertical) ||
        !reader.read_u32(map.map_points_horizontal) ||
        !reader.read_double(map.map_spacing_vertical) ||
        !reader.read_double(map.map_spacing_horizontal) ||
        !reader.read_double(map.map_origin_vertical) ||
        !reader.read_double(map.map_origin_horizontal) || !reader.read_u32(map.map_planes))
    {
        return invalid_list_error(list, index, "truncated_dng_gain_map",
                                  "DNG GainMap fixed fields are truncated");
    }

    std::size_t map_points = 0U;
    std::size_t all_points = 0U;
    std::size_t gain_bytes = 0U;
    std::size_t expected_size = 0U;
    if (map.map_points_vertical == 0U || map.map_points_horizontal == 0U ||
        map.map_points_vertical > kMaxGainMapPointsPerAxis ||
        map.map_points_horizontal > kMaxGainMapPointsPerAxis ||
        !checked_multiply(map.map_points_vertical, map.map_points_horizontal, map_points) ||
        !checked_multiply(map_points, map.map_planes, all_points) ||
        !checked_multiply(all_points, sizeof(float), gain_bytes) ||
        !checked_add(kGainMapHeaderBytes, gain_bytes, expected_size))
    {
        return invalid_list_error(list, index, "dng_gain_map_dimensions_overflow",
                                  "DNG GainMap dimensions exceed the bounded parser");
    }
    if (payload.size() != expected_size)
    {
        return invalid_list_error(list, index, "invalid_dng_gain_map_size",
                                  "DNG GainMap sample count does not match its payload size");
    }
    map.gains.resize(all_points);
    for (std::size_t point = 0U; point < all_points; ++point)
    {
        if (!reader.read_float(map.gains[point]))
        {
            return invalid_list_error(list, index, "truncated_dng_gain_map",
                                      "DNG GainMap samples are truncated");
        }
        if (!std::isfinite(map.gains[point]) || map.gains[point] <= 0.0F ||
            map.gains[point] > kMaxDngGain)
        {
            return invalid_list_error(list, index, "invalid_dng_gain_value",
                                      "DNG GainMap contains a non-finite or unsafe gain");
        }
    }
    return map;
}

[[nodiscard]] Result<DngWarpRectilinear>
parse_warp_rectilinear(const std::span<const std::uint8_t> payload,
                       const std::uint32_t index)
{
    BigEndianReader reader(payload);
    DngWarpRectilinear warp;
    if (!reader.read_u32(warp.planes) || (warp.planes != 1U && warp.planes != 3U))
    {
        return invalid_list_error(3U, index, "unsupported_dng_warp_planes",
                                  "DNG WarpRectilinear requires one or three planes");
    }
    std::size_t coefficient_count = 0U;
    std::size_t coefficient_bytes = 0U;
    std::size_t expected_size = 0U;
    if (!checked_multiply(static_cast<std::size_t>(warp.planes), 6U, coefficient_count) ||
        !checked_add(coefficient_count, 2U, coefficient_count) ||
        !checked_multiply(coefficient_count, sizeof(double), coefficient_bytes) ||
        !checked_add(4U, coefficient_bytes, expected_size) || payload.size() != expected_size)
    {
        return invalid_list_error(3U, index, "invalid_dng_warp_size",
                                  "DNG WarpRectilinear size does not match its plane count");
    }
    for (std::size_t plane = 0U; plane < warp.planes; ++plane)
    {
        for (double &coefficient : warp.coefficients[plane])
        {
            if (!reader.read_double(coefficient) || !std::isfinite(coefficient))
            {
                return invalid_list_error(3U, index, "invalid_dng_warp_coefficient",
                                          "DNG WarpRectilinear coefficient is not finite");
            }
        }
    }
    for (double &center : warp.center)
    {
        if (!reader.read_double(center) || !std::isfinite(center) || center < 0.0 || center > 1.0)
        {
            return invalid_list_error(3U, index, "invalid_dng_warp_center",
                                      "DNG WarpRectilinear center is outside [0, 1]");
        }
    }
    for (std::size_t plane = 0U; plane < warp.planes; ++plane)
    {
        const auto &coefficient = warp.coefficients[plane];
        for (std::uint32_t step = 0U; step <= 1024U; ++step)
        {
            const double radius = static_cast<double>(step) / 1024.0;
            const double radius2 = radius * radius;
            const double derivative =
                coefficient[0] +
                radius2 * (3.0 * coefficient[1] +
                           radius2 * (5.0 * coefficient[2] +
                                      radius2 * 7.0 * coefficient[3]));
            if (!std::isfinite(derivative) || derivative <= 0.0)
            {
                return invalid_list_error(
                    3U, index, "non_monotonic_dng_warp",
                    "DNG WarpRectilinear radial function is not strictly increasing");
            }
        }
    }
    return warp;
}

[[nodiscard]] Result<DngFixVignetteRadial>
parse_fix_vignette_radial(const std::span<const std::uint8_t> payload,
                          const std::uint32_t index)
{
    if (payload.size() != 7U * sizeof(double))
    {
        return invalid_list_error(3U, index, "invalid_dng_vignette_size",
                                  "DNG FixVignetteRadial requires seven doubles");
    }
    BigEndianReader reader(payload);
    DngFixVignetteRadial vignette;
    for (double &coefficient : vignette.coefficients)
    {
        if (!reader.read_double(coefficient) || !std::isfinite(coefficient))
        {
            return invalid_list_error(3U, index, "invalid_dng_vignette_coefficient",
                                      "DNG FixVignetteRadial coefficient is not finite");
        }
    }
    for (double &center : vignette.center)
    {
        if (!reader.read_double(center) || !std::isfinite(center) || center < 0.0 || center > 1.0)
        {
            return invalid_list_error(3U, index, "invalid_dng_vignette_center",
                                      "DNG FixVignetteRadial center is outside [0, 1]");
        }
    }
    return vignette;
}

[[nodiscard]] Result<void> validate_gain_map(const DngGainMap &map,
                                             const std::uint32_t list,
                                             const std::uint32_t index,
                                             const std::uint32_t raw_width,
                                             const std::uint32_t raw_height)
{
    const bool bounds_valid = map.top < map.bottom && map.left < map.right &&
                              map.bottom <= raw_height && map.right <= raw_width;
    const bool grid_valid = map.map_points_vertical > 0U &&
                            map.map_points_horizontal > 0U && map.map_planes > 0U &&
                            std::isfinite(map.map_spacing_vertical) &&
                            std::isfinite(map.map_spacing_horizontal) &&
                            std::isfinite(map.map_origin_vertical) &&
                            std::isfinite(map.map_origin_horizontal) &&
                            map.map_spacing_vertical > 0.0 &&
                            map.map_spacing_horizontal > 0.0;
    const bool pitch_valid = map.row_pitch > 0U && map.column_pitch > 0U;
    if (!bounds_valid || !grid_valid || !pitch_valid)
    {
        return invalid_list_error(list, index, "unsupported_dng_gain_map_geometry",
                                  "DNG GainMap geometry is outside the bounded image contract");
    }

    if (list == 2U)
    {
        if (map.plane != 0U || map.planes != 1U || map.map_planes != 1U)
        {
            return invalid_list_error(
                list, index, "unsupported_dng_list2_gain_map_planes",
                "DNG OpcodeList2 GainMap must address the single CFA plane");
        }
    }
    else
    {
        if (map.plane >= 3U || map.planes == 0U || map.planes > 3U - map.plane ||
            map.map_planes > map.planes || map.row_pitch != 1U ||
            map.column_pitch != 1U)
        {
            return invalid_list_error(
                list, index, "unsupported_dng_list3_gain_map_planes",
                "DNG OpcodeList3 GainMap must address camera RGB with unit pitch");
        }
    }
    return {};
}

[[nodiscard]] Result<void> parse_opcode_list(const std::uint32_t list_number,
                                             const DngOpcodeListView view,
                                             DngOpcodeMetadata &metadata,
                                             const std::uint32_t raw_width,
                                             const std::uint32_t raw_height)
{
    if (!view.present)
    {
        return {};
    }
    if (view.bytes.empty())
    {
        return invalid_list_error(list_number, 0U, "empty_dng_opcode_list",
                                  "DNG opcode list is present but empty");
    }
    if (view.bytes.size() > kMaxDngOpcodeListBytes)
    {
        return make_error(ErrorCode::kUnsupported, "DNG opcode list exceeds the byte limit",
                          {{"bytes", std::to_string(view.bytes.size())},
                           {"dng_opcode_list", std::to_string(list_number)},
                           {"reason", "oversized_dng_opcode_list"}});
    }

    BigEndianReader reader(view.bytes);
    std::uint32_t count = 0U;
    if (!reader.read_u32(count) || count > reader.remaining() / kOpcodeHeaderBytes)
    {
        return invalid_list_error(list_number, 0U, "invalid_dng_opcode_count",
                                  "DNG opcode count exceeds the list envelope");
    }
    for (std::uint32_t index = 0U; index < count; ++index)
    {
        std::uint32_t id = 0U;
        std::uint32_t minimum_version = 0U;
        std::uint32_t flags = 0U;
        std::uint32_t payload_size = 0U;
        if (!reader.read_u32(id) || !reader.read_u32(minimum_version) || !reader.read_u32(flags) ||
            !reader.read_u32(payload_size))
        {
            return invalid_list_error(list_number, index, "truncated_dng_opcode_header",
                                      "DNG opcode header is truncated");
        }
        std::span<const std::uint8_t> payload;
        if (!reader.read_bytes(payload_size, payload))
        {
            return invalid_list_error(list_number, index, "truncated_dng_opcode_payload",
                                      "DNG opcode payload exceeds the list envelope");
        }

        const bool optional = (flags & kOpcodeOptionalFlag) != 0U;
        if (minimum_version > kSupportedDngVersion || (flags & ~kKnownOpcodeFlags) != 0U)
        {
            if (!optional)
            {
                return make_error(
                    ErrorCode::kUnsupported,
                    "DNG opcode requires unsupported version or flag semantics",
                    {{"dng_opcode_flags", std::to_string(flags)},
                     {"dng_opcode_id", std::to_string(id)},
                     {"dng_opcode_index", std::to_string(index)},
                     {"dng_opcode_list", std::to_string(list_number)},
                     {"dng_opcode_version", std::to_string(minimum_version)},
                     {"reason", "unsupported_mandatory_dng_opcode"}});
            }
            metadata.skipped_optional.push_back(
                {list_number, index, id, minimum_version, flags});
            continue;
        }

        bool supported = false;
        if ((list_number == 2U || list_number == 3U) && id == kOpcodeGainMap)
        {
            auto map = parse_gain_map(payload, list_number, index);
            if (!map)
            {
                return map.error();
            }
            auto valid = validate_gain_map(map.value(), list_number, index, raw_width, raw_height);
            if (!valid)
            {
                return valid.error();
            }
            if (list_number == 2U)
            {
                metadata.list2_gain_maps.push_back(std::move(map).value());
            }
            else
            {
                metadata.list3_operations.emplace_back(std::move(map).value());
            }
            supported = true;
        }
        else if (list_number == 3U && id == kOpcodeWarpRectilinear)
        {
            auto warp = parse_warp_rectilinear(payload, index);
            if (!warp)
            {
                return warp.error();
            }
            metadata.list3_operations.emplace_back(std::move(warp).value());
            supported = true;
        }
        else if (list_number == 3U && id == kOpcodeFixVignetteRadial)
        {
            auto vignette = parse_fix_vignette_radial(payload, index);
            if (!vignette)
            {
                return vignette.error();
            }
            metadata.list3_operations.emplace_back(std::move(vignette).value());
            supported = true;
        }

        if (!supported)
        {
            if ((flags & kOpcodeOptionalFlag) == 0U)
            {
                return make_error(ErrorCode::kUnsupported,
                                  "DNG opcode list contains an unsupported mandatory opcode",
                                  {{"dng_opcode_id", std::to_string(id)},
                                   {"dng_opcode_index", std::to_string(index)},
                                   {"dng_opcode_list", std::to_string(list_number)},
                                   {"reason", "unsupported_mandatory_dng_opcode"}});
            }
            metadata.skipped_optional.push_back(
                {list_number, index, id, minimum_version, flags});
        }
    }
    if (reader.remaining() != 0U)
    {
        return invalid_list_error(list_number, count, "dng_opcode_trailing_bytes",
                                  "DNG opcode list has bytes after its declared operations");
    }
    return {};
}

[[nodiscard]] double radial_extent(const std::uint32_t width, const std::uint32_t height,
                                   const std::array<double, 2> center) noexcept
{
    const double last_x = static_cast<double>(width - 1U);
    const double last_y = static_cast<double>(height - 1U);
    const double center_x = center[0] * last_x;
    const double center_y = center[1] * last_y;
    const double maximum_x = std::max(center_x, last_x - center_x);
    const double maximum_y = std::max(center_y, last_y - center_y);
    return std::sqrt(maximum_x * maximum_x + maximum_y * maximum_y);
}

[[nodiscard]] bool warp_source_coordinate(const DngWarpRectilinear &warp,
                                          const std::uint32_t plane,
                                          const std::uint32_t width,
                                          const std::uint32_t height, const double output_x,
                                          const double output_y, double &source_x,
                                          double &source_y) noexcept
{
    const double center_x = warp.center[0] * static_cast<double>(width - 1U);
    const double center_y = warp.center[1] * static_cast<double>(height - 1U);
    const double extent = radial_extent(width, height, warp.center);
    if (!std::isfinite(extent) || extent <= 0.0)
    {
        return false;
    }
    const double dx = (output_x - center_x) / extent;
    const double dy = (output_y - center_y) / extent;
    const double dx2 = dx * dx;
    const double dy2 = dy * dy;
    const double radius2 = dx2 + dy2;
    const auto &coefficient = warp.coefficients[plane];
    const double radial = coefficient[0] +
                          radius2 * (coefficient[1] +
                                     radius2 * (coefficient[2] + radius2 * coefficient[3]));
    const double twice_xy = 2.0 * dx * dy;
    const double tangential_x =
        coefficient[4] * twice_xy + coefficient[5] * (radius2 + 2.0 * dx2);
    const double tangential_y =
        coefficient[5] * twice_xy + coefficient[4] * (radius2 + 2.0 * dy2);
    source_x = center_x + extent * (radial * dx + tangential_x);
    source_y = center_y + extent * (radial * dy + tangential_y);
    return std::isfinite(source_x) && std::isfinite(source_y);
}

[[nodiscard]] bool gain_map_affects(const DngGainMap &map, const std::uint32_t x,
                                    const std::uint32_t y,
                                    const std::uint32_t plane) noexcept
{
    return y >= map.top && y < map.bottom && x >= map.left && x < map.right &&
           plane >= map.plane && plane < map.plane + map.planes &&
           (y - map.top) % map.row_pitch == 0U &&
           (x - map.left) % map.column_pitch == 0U;
}

[[nodiscard]] float interpolate_gain_map(const DngGainMap &map, const double x,
                                         const double y, const std::uint32_t width,
                                         const std::uint32_t height,
                                         const std::uint32_t plane) noexcept
{
    const double maximum_x = static_cast<double>(map.map_points_horizontal - 1U);
    const double maximum_y = static_cast<double>(map.map_points_vertical - 1U);
    const double mapped_x = std::clamp(
        ((x / static_cast<double>(width)) - map.map_origin_horizontal) /
            map.map_spacing_horizontal,
        0.0, maximum_x);
    const double mapped_y = std::clamp(
        ((y / static_cast<double>(height)) - map.map_origin_vertical) /
            map.map_spacing_vertical,
        0.0, maximum_y);
    const auto x0 = static_cast<std::uint32_t>(mapped_x);
    const auto y0 = static_cast<std::uint32_t>(mapped_y);
    const auto x1 = std::min(x0 + 1U, map.map_points_horizontal - 1U);
    const auto y1 = std::min(y0 + 1U, map.map_points_vertical - 1U);
    const float fraction_x = static_cast<float>(mapped_x - x0);
    const float fraction_y = static_cast<float>(mapped_y - y0);
    const std::uint32_t map_plane = std::min(plane - map.plane, map.map_planes - 1U);
    const auto sample = [&](const std::uint32_t sample_x, const std::uint32_t sample_y)
    {
        return map.gains[(static_cast<std::size_t>(sample_y) * map.map_points_horizontal +
                          sample_x) *
                             map.map_planes +
                         map_plane];
    };
    const float top = (1.0F - fraction_x) * sample(x0, y0) + fraction_x * sample(x1, y0);
    const float bottom =
        (1.0F - fraction_x) * sample(x0, y1) + fraction_x * sample(x1, y1);
    return (1.0F - fraction_y) * top + fraction_y * bottom;
}

[[nodiscard]] float cubic_weight(const double distance) noexcept
{
    const double value = std::abs(distance);
    if (value < 1.0)
    {
        return static_cast<float>(((1.5 * value - 2.5) * value) * value + 1.0);
    }
    if (value < 2.0)
    {
        return static_cast<float>((((-0.5 * value + 2.5) * value - 4.0) * value) + 2.0);
    }
    return 0.0F;
}

[[nodiscard]] float cubic_sample(const WorkingImage &input, const double source_x,
                                 const double source_y, const std::uint32_t channel) noexcept
{
    const int floor_x = static_cast<int>(std::floor(source_x));
    const int floor_y = static_cast<int>(std::floor(source_y));
    double value = 0.0;
    double weight_sum = 0.0;
    for (int offset_y = -1; offset_y <= 2; ++offset_y)
    {
        const int unclamped_y = floor_y + offset_y;
        const auto sample_y = static_cast<std::uint32_t>(
            std::clamp(unclamped_y, 0, static_cast<int>(input.height) - 1));
        const double weight_y = cubic_weight(source_y - unclamped_y);
        for (int offset_x = -1; offset_x <= 2; ++offset_x)
        {
            const int unclamped_x = floor_x + offset_x;
            const auto sample_x = static_cast<std::uint32_t>(
                std::clamp(unclamped_x, 0, static_cast<int>(input.width) - 1));
            const double weight =
                weight_y * cubic_weight(source_x - static_cast<double>(unclamped_x));
            value += weight *
                     input.rgb[(static_cast<std::size_t>(sample_y) * input.width + sample_x) *
                                   3U +
                               channel];
            weight_sum += weight;
        }
    }
    return static_cast<float>(weight_sum != 0.0 ? value / weight_sum : value);
}

[[nodiscard]] [[maybe_unused]] Result<WorkingImage>
apply_warp(WorkingImage input, const DngWarpRectilinear &warp,
           const CancellationToken &cancellation)
try
{
    WorkingImage output;
    output.width = input.width;
    output.height = input.height;
    output.rgb.resize(input.rgb.size());
    output.color_profile = input.color_profile;
    output.exposure_analysis = input.exposure_analysis;
    output.canonical_roi_scale = input.canonical_roi_scale;
    output.mask_attached_frame = input.mask_attached_frame;

    std::atomic_bool invalid_coordinate{false};
    std::atomic_bool non_finite_output{false};
    auto rows = detail::for_each_row(
        output.height, cancellation,
        [&](const std::uint32_t y)
        {
            if (invalid_coordinate.load(std::memory_order_relaxed) ||
                non_finite_output.load(std::memory_order_relaxed))
            {
                return;
            }
            for (std::uint32_t x = 0U; x < output.width; ++x)
            {
                for (std::uint32_t channel = 0U; channel < 3U; ++channel)
                {
                    const std::uint32_t plane = warp.planes == 1U ? 0U : channel;
                    double source_x = 0.0;
                    double source_y = 0.0;
                    if (!warp_source_coordinate(warp, plane, input.width, input.height, x, y,
                                                source_x, source_y) ||
                        source_x < -0.5 || source_y < -0.5 ||
                        source_x > static_cast<double>(input.width) - 0.5 ||
                        source_y > static_cast<double>(input.height) - 0.5)
                    {
                        invalid_coordinate.store(true, std::memory_order_relaxed);
                        return;
                    }
                    source_x = std::clamp(source_x, 0.0, static_cast<double>(input.width - 1U));
                    source_y = std::clamp(source_y, 0.0, static_cast<double>(input.height - 1U));
                    const float value = cubic_sample(input, source_x, source_y, channel);
                    if (!std::isfinite(value))
                    {
                        non_finite_output.store(true, std::memory_order_relaxed);
                        return;
                    }
                    output.rgb[(static_cast<std::size_t>(y) * output.width + x) * 3U + channel] =
                        std::clamp(value, 0.0F, 1.0F);
                }
            }
        });
    if (!rows)
    {
        return rows.error();
    }
    if (invalid_coordinate.load(std::memory_order_relaxed))
    {
        return make_error(ErrorCode::kUnsupported,
                          "DNG WarpRectilinear maps output outside its source frame",
                          {{"reason", "dng_warp_out_of_bounds"}});
    }
    if (non_finite_output.load(std::memory_order_relaxed))
    {
        return make_error(ErrorCode::kValidation, "DNG WarpRectilinear output is not finite",
                          {{"reason", "non_finite_dng_warp_output"}});
    }
    return output;
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "DNG WarpRectilinear allocation failed",
                      {{"reason", "allocation_failed"}});
}

[[nodiscard]] Result<void> apply_vignette(WorkingImage &input,
                                          const DngFixVignetteRadial &vignette,
                                          const DngOpcodeMetadata &metadata,
                                          const CancellationToken &cancellation)
{
    const double center_x = vignette.center[0] * static_cast<double>(metadata.source_width - 1U);
    const double center_y = vignette.center[1] * static_cast<double>(metadata.source_height - 1U);
    const double extent = radial_extent(metadata.source_width, metadata.source_height,
                                        vignette.center);
    if (!std::isfinite(extent) || extent <= 0.0)
    {
        return make_error(ErrorCode::kValidation, "DNG vignette radius is invalid",
                          {{"reason", "invalid_dng_vignette_radius"}});
    }
    const double inverse_extent_squared = 1.0 / (extent * extent);
    std::atomic_bool invalid{false};
    auto rows = detail::for_each_row(
        input.height, cancellation,
        [&](const std::uint32_t y)
        {
            if (invalid.load(std::memory_order_relaxed))
            {
                return;
            }
            const double source_y = static_cast<double>(metadata.active_origin_y) +
                                    static_cast<double>(y) * metadata.active_height /
                                        static_cast<double>(input.height);
            const double dy = source_y - center_y;
            for (std::uint32_t x = 0U; x < input.width; ++x)
            {
                const double source_x = static_cast<double>(metadata.active_origin_x) +
                                        static_cast<double>(x) * metadata.active_width /
                                            static_cast<double>(input.width);
                const double dx = source_x - center_x;
                const double radius2 = (dx * dx + dy * dy) * inverse_extent_squared;
                const auto &coefficient = vignette.coefficients;
                const double gain =
                    1.0 + radius2 *
                              (coefficient[0] +
                               radius2 *
                                   (coefficient[1] +
                                    radius2 *
                                        (coefficient[2] +
                                         radius2 * (coefficient[3] +
                                                    radius2 * coefficient[4]))));
                if (!std::isfinite(gain) || gain <= 0.0 || gain > kMaxDngGain)
                {
                    invalid.store(true, std::memory_order_relaxed);
                    return;
                }
                const std::size_t base = (static_cast<std::size_t>(y) * input.width + x) * 3U;
                for (std::size_t channel = 0U; channel < 3U; ++channel)
                {
                    const float value =
                        input.rgb[base + channel] * static_cast<float>(gain);
                    if (!std::isfinite(value))
                    {
                        invalid.store(true, std::memory_order_relaxed);
                        return;
                    }
                    input.rgb[base + channel] = std::clamp(value, 0.0F, 1.0F);
                }
            }
        });
    if (!rows)
    {
        return rows.error();
    }
    if (invalid.load(std::memory_order_relaxed))
    {
        return make_error(ErrorCode::kValidation,
                          "DNG FixVignetteRadial produced an invalid gain or sample",
                          {{"reason", "invalid_dng_vignette_output"}});
    }
    return {};
}

[[nodiscard]] Result<void> apply_gain_map(WorkingImage &input, const DngGainMap &map,
                                          const DngOpcodeMetadata &metadata,
                                          const CancellationToken &cancellation)
{
    std::atomic_bool invalid{false};
    const auto rows = detail::for_each_row(
        input.height, cancellation,
        [&](const std::uint32_t y)
        {
            if (invalid.load(std::memory_order_relaxed))
            {
                return;
            }
            const std::uint32_t source_y = std::min(
                metadata.active_origin_y + metadata.active_height - 1U,
                metadata.active_origin_y +
                    static_cast<std::uint32_t>(static_cast<std::uint64_t>(y) *
                                               metadata.active_height / input.height));
            for (std::uint32_t x = 0U; x < input.width; ++x)
            {
                const std::uint32_t source_x = std::min(
                    metadata.active_origin_x + metadata.active_width - 1U,
                    metadata.active_origin_x +
                        static_cast<std::uint32_t>(static_cast<std::uint64_t>(x) *
                                                   metadata.active_width / input.width));
                if (source_y < map.top || source_y >= map.bottom || source_x < map.left ||
                    source_x >= map.right)
                {
                    continue;
                }
                const std::size_t base = (static_cast<std::size_t>(y) * input.width + x) * 3U;
                for (std::uint32_t channel = map.plane; channel < map.plane + map.planes;
                     ++channel)
                {
                    const float gain = interpolate_gain_map(
                        map, static_cast<double>(source_x), static_cast<double>(source_y),
                        metadata.source_width, metadata.source_height, channel);
                    const float value = input.rgb[base + channel] * gain;
                    if (!std::isfinite(value))
                    {
                        invalid.store(true, std::memory_order_relaxed);
                        return;
                    }
                    input.rgb[base + channel] = std::clamp(value, 0.0F, 1.0F);
                }
            }
        });
    if (!rows)
    {
        return rows.error();
    }
    if (invalid.load(std::memory_order_relaxed))
    {
        return make_error(ErrorCode::kValidation, "DNG GainMap produced a non-finite sample",
                          {{"reason", "non_finite_dng_gain_map_output"}});
    }
    return {};
}

} // namespace

Result<std::shared_ptr<const DngOpcodeMetadata>>
parse_dng_opcode_metadata(const DngOpcodeListView list2, const DngOpcodeListView list3,
                          const std::uint32_t raw_width, const std::uint32_t raw_height,
                          const std::uint32_t active_origin_x,
                          const std::uint32_t active_origin_y,
                          const std::uint32_t active_width,
                          const std::uint32_t active_height)
try
{
    if (!list2.present && !list3.present)
    {
        return std::shared_ptr<const DngOpcodeMetadata>{};
    }
    if (raw_width == 0U || raw_height == 0U)
    {
        return make_error(ErrorCode::kValidation,
                          "DNG opcode metadata requires non-zero RAW dimensions",
                          {{"reason", "invalid_dng_opcode_dimensions"}});
    }
    const std::uint32_t resolved_width = active_width == 0U ? raw_width : active_width;
    const std::uint32_t resolved_height = active_height == 0U ? raw_height : active_height;
    if (active_origin_x > raw_width || active_origin_y > raw_height || resolved_width == 0U ||
        resolved_height == 0U || resolved_width > raw_width - active_origin_x ||
        resolved_height > raw_height - active_origin_y)
    {
        return make_error(ErrorCode::kValidation,
                          "DNG opcode active frame is outside the RAW dimensions",
                          {{"reason", "invalid_dng_opcode_active_frame"}});
    }
    auto metadata = std::make_shared<DngOpcodeMetadata>();
    metadata->list2_present = list2.present;
    metadata->list3_present = list3.present;
    metadata->source_width = raw_width;
    metadata->source_height = raw_height;
    metadata->active_origin_x = active_origin_x;
    metadata->active_origin_y = active_origin_y;
    metadata->active_width = resolved_width;
    metadata->active_height = resolved_height;
    auto parsed = parse_opcode_list(2U, list2, *metadata, raw_width, raw_height);
    if (!parsed)
    {
        return parsed.error();
    }
    parsed = parse_opcode_list(3U, list3, *metadata, raw_width, raw_height);
    if (!parsed)
    {
        return parsed.error();
    }
    std::shared_ptr<const DngOpcodeMetadata> published = std::move(metadata);
    return published;
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "DNG opcode allocation failed",
                      {{"reason", "allocation_failed"}});
}

float apply_dng_opcode_list2_sample(const DngOpcodeMetadata &metadata, const std::uint32_t x,
                                    const std::uint32_t y, const std::uint32_t raw_width,
                                    const std::uint32_t raw_height,
                                    const float normalized_sample) noexcept
{
    float value = std::clamp(normalized_sample, 0.0F, 1.0F);
    if (raw_width == 0U || raw_height == 0U || metadata.source_width == 0U ||
        metadata.source_height == 0U || metadata.active_width == 0U ||
        metadata.active_height == 0U)
    {
        return value;
    }
    const std::uint32_t source_x = std::min(
        metadata.active_origin_x + metadata.active_width - 1U,
        metadata.active_origin_x +
            static_cast<std::uint32_t>(static_cast<std::uint64_t>(x) * metadata.active_width /
                                       raw_width));
    const std::uint32_t source_y = std::min(
        metadata.active_origin_y + metadata.active_height - 1U,
        metadata.active_origin_y +
            static_cast<std::uint32_t>(static_cast<std::uint64_t>(y) * metadata.active_height /
                                       raw_height));
    for (const auto &map : metadata.list2_gain_maps)
    {
        if (gain_map_affects(map, source_x, source_y, 0U))
        {
            value = std::clamp(value * interpolate_gain_map(
                                           map, static_cast<double>(source_x),
                                           static_cast<double>(source_y), metadata.source_width,
                                           metadata.source_height, 0U),
                               0.0F, 1.0F);
        }
    }
    return value;
}

std::size_t dng_gain_map_count(const DngOpcodeMetadata &metadata) noexcept
{
    return metadata.list2_gain_maps.size() +
           static_cast<std::size_t>(std::count_if(
               metadata.list3_operations.begin(), metadata.list3_operations.end(),
               [](const DngOpcodeList3Operation &operation)
               { return std::holds_alternative<DngGainMap>(operation); }));
}

Result<WorkingImage> apply_dng_opcode_list3(WorkingImage input,
                                            const DngOpcodeMetadata &metadata,
                                            const CancellationToken &cancellation)
{
    if (metadata.list3_operations.empty())
    {
        return input;
    }
    if (input.width == 0U || input.height == 0U || metadata.source_width == 0U ||
        metadata.source_height == 0U ||
        input.rgb.size() != static_cast<std::size_t>(input.width) * input.height * 3U)
    {
        return make_error(ErrorCode::kValidation,
                          "DNG OpcodeList3 input does not match its dimensions",
                          {{"reason", "invalid_dng_list3_input"}});
    }
    if (input.color_profile.model != ColorModel::kRgb)
    {
        return make_error(ErrorCode::kUnsupported, "DNG OpcodeList3 requires camera RGB",
                          {{"reason", "unsupported_dng_list3_color_model"}});
    }
    if (std::any_of(input.rgb.begin(), input.rgb.end(),
                    [](const float value) { return !std::isfinite(value); }))
    {
        return make_error(ErrorCode::kValidation, "DNG OpcodeList3 input is not finite",
                          {{"reason", "non_finite_dng_list3_input"}});
    }
    auto active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    for (const auto &operation : metadata.list3_operations)
    {
        if (std::holds_alternative<DngWarpRectilinear>(operation))
        {
            // Default colour decode does not apply DNG lens geometry. Inspect still
            // reports WarpRectilinear. darktable keeps this opcode for the lens
            // module (off by default); RapidRAW uses optional lensfun instead.
            continue;
        }
        if (const auto *vignette = std::get_if<DngFixVignetteRadial>(&operation))
        {
            auto corrected = apply_vignette(input, *vignette, metadata, cancellation);
            if (!corrected)
            {
                return corrected.error();
            }
        }
        else
        {
            auto corrected =
                apply_gain_map(input, std::get<DngGainMap>(operation), metadata, cancellation);
            if (!corrected)
            {
                return corrected.error();
            }
        }
    }
    active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    return input;
}

std::uint64_t estimate_dng_opcode_memory(const DngOpcodeMetadata &metadata) noexcept
{
    std::uint64_t bytes = sizeof(DngOpcodeMetadata);
    const auto add_gain_bytes = [&bytes](const DngGainMap &map) noexcept
    {
        const std::uint64_t gain_bytes =
            static_cast<std::uint64_t>(map.gains.capacity()) * sizeof(float);
        bytes = gain_bytes > std::numeric_limits<std::uint64_t>::max() - bytes ?
                    std::numeric_limits<std::uint64_t>::max() :
                    bytes + gain_bytes;
    };
    for (const auto &map : metadata.list2_gain_maps)
    {
        add_gain_bytes(map);
    }
    for (const auto &operation : metadata.list3_operations)
    {
        if (const auto *map = std::get_if<DngGainMap>(&operation))
        {
            add_gain_bytes(*map);
        }
    }
    const std::uint64_t list2_bytes =
        static_cast<std::uint64_t>(metadata.list2_gain_maps.capacity()) * sizeof(DngGainMap);
    bytes = list2_bytes > std::numeric_limits<std::uint64_t>::max() - bytes ?
                std::numeric_limits<std::uint64_t>::max() :
                bytes + list2_bytes;
    const std::uint64_t operation_bytes =
        static_cast<std::uint64_t>(metadata.list3_operations.capacity()) *
        sizeof(DngOpcodeList3Operation);
    bytes = operation_bytes > std::numeric_limits<std::uint64_t>::max() - bytes ?
                std::numeric_limits<std::uint64_t>::max() :
                bytes + operation_bytes;
    const std::uint64_t skipped_bytes =
        static_cast<std::uint64_t>(metadata.skipped_optional.capacity()) *
        sizeof(DngSkippedOptionalOpcode);
    return skipped_bytes > std::numeric_limits<std::uint64_t>::max() - bytes ?
               std::numeric_limits<std::uint64_t>::max() :
               bytes + skipped_bytes;
}

} // namespace ravo
