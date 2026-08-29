#include "canvas_frame.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#include "ravo/recipe/color_input.h"

namespace ravo
{
namespace
{

[[nodiscard]] Result<std::size_t> checked_channels(const std::uint32_t width,
                                                   const std::uint32_t height,
                                                   const std::string_view owner)
{
    if (width == 0U || height == 0U)
        return make_error(ErrorCode::kValidation, std::string(owner) + " dimensions are invalid");
    const std::uint64_t pixels = static_cast<std::uint64_t>(width) * height;
    if (pixels > std::vector<float>{}.max_size() / 3U)
    {
        return make_error(ErrorCode::kValidation, std::string(owner) + " dimensions overflow",
                          {{"reason", "dimensions_overflow"}});
    }
    return static_cast<std::size_t>(pixels) * 3U;
}

[[nodiscard]] Result<void> validate_finite_samples(const std::vector<float> &samples,
                                                   const std::uint32_t width,
                                                   const std::string_view owner,
                                                   const std::string_view reason,
                                                   const CancellationToken &cancellation)
{
    const std::size_t row_channels = static_cast<std::size_t>(width) * 3U;
    for (std::size_t row_start = 0U; row_start < samples.size(); row_start += row_channels)
    {
        auto active = cancellation.check();
        if (!active)
            return active.error();
        const std::size_t row_end = std::min(row_start + row_channels, samples.size());
        for (std::size_t index = row_start; index < row_end; ++index)
        {
            if (!std::isfinite(samples[index]))
            {
                return make_error(
                    ErrorCode::kValidation, std::string(owner) + " input contains NaN or infinity",
                    {{"sample_index", std::to_string(index)}, {"reason", std::string(reason)}});
            }
        }
    }
    return {};
}

[[nodiscard]] std::array<float, 3> canvas_color(const CanvasColor color) noexcept
{
    switch (color)
    {
    case CanvasColor::kGreen:
        return {0.0F, 1.0F, 0.0F};
    case CanvasColor::kRed:
        return {1.0F, 0.0F, 0.0F};
    case CanvasColor::kBlue:
        return {0.0F, 0.0F, 1.0F};
    case CanvasColor::kBlack:
        return {0.0F, 0.0F, 0.0F};
    case CanvasColor::kWhite:
        return {1.0F, 1.0F, 1.0F};
    }
    return {};
}

struct BorderInfo
{
    int border_top = 0;
    int frame_top = 0;
    int image_top = 0;
    int border_left = 0;
    int frame_left = 0;
    int image_left = 0;
    int image_right = 0;
    int frame_right = 0;
    int border_right = 0;
    int width = 0;
    int image_bottom = 0;
    int frame_bottom = 0;
    int border_bottom = 0;
    int height = 0;
};

[[nodiscard]] BorderInfo border_info(const std::uint32_t input_width,
                                     const std::uint32_t input_height, const FrameLayout &layout,
                                     const FrameParams &params)
{
    const int output_width = static_cast<int>(layout.output_width);
    const int output_height = static_cast<int>(layout.output_height);
    const int width = static_cast<int>(input_width);
    const int height = static_cast<int>(input_height);
    const int total_width = output_width - width;
    const int total_height = output_height - height;
    const float position_h = static_cast<float>(params.position_h);
    const float position_v = static_cast<float>(params.position_v);
    const bool has_left = position_h > 0.0F;
    const bool has_right = position_h < 1.0F;
    const bool has_top = position_v > 0.0F;
    const bool has_bottom = position_v < 1.0F;
    const int border_top_size =
        has_top ? static_cast<int>(static_cast<float>(total_height) * position_v) : 0;
    const int border_bottom_size = has_bottom ? total_height - border_top_size : 0;
    const int border_left_size =
        has_left ? static_cast<int>(static_cast<float>(total_width) * position_h) : 0;
    const int border_right_size = has_right ? total_width - border_left_size : 0;
    int image_x = has_right ? std::clamp(border_left_size, 0, output_width) : total_width;
    int image_y = has_bottom ? std::clamp(border_top_size, 0, output_height) : total_height;
    const int image_right = has_right ? image_x + width : output_width;
    const int image_bottom = has_bottom ? image_y + height : output_height;

    BorderInfo info;
    info.border_top = image_y;
    info.frame_top = image_y;
    info.image_top = image_y;
    info.border_left = image_x;
    info.frame_left = image_x;
    info.image_left = image_x;
    info.image_right = image_right;
    info.frame_right = output_width;
    info.border_right = output_width;
    info.width = output_width;
    info.image_bottom = image_bottom;
    info.frame_bottom = output_height;
    info.border_bottom = output_height;
    info.height = output_height;

    const int minimum_border = std::min(std::min(border_top_size, border_bottom_size),
                                        std::min(border_left_size, border_right_size));
    const int frame_size = static_cast<int>(static_cast<float>(minimum_border) *
                                            static_cast<float>(params.frame_size));
    if (frame_size <= 0)
        return info;

    const int frame_space = minimum_border - frame_size;
    const int frame_offset =
        static_cast<int>(static_cast<float>(frame_space) * static_cast<float>(params.frame_offset));
    const int frame_tl_in_x = std::max(image_x - frame_offset, 0);
    const int frame_tl_out_x = std::max(frame_tl_in_x - frame_size, 0);
    const int frame_tl_in_y = std::max(image_y - frame_offset, 0);
    const int frame_tl_out_y = std::max(frame_tl_in_y - frame_size, 0);
    info.border_top = frame_tl_out_y;
    info.frame_top = frame_tl_in_y;
    info.border_left = std::clamp(frame_tl_out_x, 0, output_width);
    info.frame_left = std::clamp(frame_tl_in_x, 0, output_width);
    const int frame_in_width = static_cast<int>(
        std::floor(static_cast<float>(width) + static_cast<float>(frame_offset) * 2.0F));
    const int frame_in_height = static_cast<int>(
        std::floor(static_cast<float>(height) + static_cast<float>(frame_offset) * 2.0F));
    const int frame_out_width = frame_in_width + frame_size * 2;
    const int frame_out_height = frame_in_height + frame_size * 2;
    const int frame_br_in_x =
        std::clamp(image_x - frame_offset + frame_in_width - 1, 0, output_width - 1);
    const int frame_br_in_y =
        std::clamp(image_y - frame_offset + frame_in_height - 1, 0, output_height - 1);
    const int frame_br_out_x =
        params.frame_offset == 1.0 &&
                (std::min(border_left_size, border_right_size) - minimum_border < 2) ?
            output_width :
            std::clamp(image_x - frame_offset - frame_size + frame_out_width - 1, 0,
                       output_width - 1);
    const int frame_br_out_y =
        params.frame_offset == 1.0 &&
                (std::min(border_top_size, border_bottom_size) - minimum_border < 2) ?
            output_height :
            std::clamp(image_y - frame_offset - frame_size + frame_out_height - 1, 0,
                       output_height - 1);
    info.frame_right = frame_br_in_x;
    info.border_right = frame_br_out_x;
    info.frame_bottom = frame_br_in_y;
    info.border_bottom = frame_br_out_y;
    return info;
}

void fill_pixels(float *output, const int count, const std::array<float, 3> &color) noexcept
{
    for (int pixel = 0; pixel < count; ++pixel)
    {
        output[pixel * 3] = color[0];
        output[pixel * 3 + 1] = color[1];
        output[pixel * 3 + 2] = color[2];
    }
}

void checkpoint(const detail::CanvasFrameControl &control,
                const detail::CanvasFrameCheckpoint stage, const std::uint32_t progress) noexcept
{
    if (control.checkpoint_callback != nullptr)
        control.checkpoint_callback(control.context, stage, progress);
}

[[nodiscard]] int rounded_dimension(const float value, const int maximum) noexcept
{
    if (!std::isfinite(value) || value >= static_cast<float>(maximum))
        return maximum;
    if (value <= 1.0F)
        return 1;
    return static_cast<int>(std::round(value));
}

} // namespace

Result<CanvasLayout> compute_canvas_layout(const std::uint32_t width, const std::uint32_t height,
                                           const CanvasParams &params)
{
    auto canonical = canvas_to_parameters(params);
    if (!canonical)
        return canonical.error();
    if (width == 0U || height == 0U ||
        width > static_cast<std::uint32_t>(std::numeric_limits<int>::max() / 3) ||
        height > static_cast<std::uint32_t>(std::numeric_limits<int>::max() / 3))
    {
        return make_error(ErrorCode::kValidation, "Canvas input dimensions are unsupported",
                          {{"reason", "invalid_canvas_dimensions"}});
    }
    const int left = static_cast<int>(static_cast<float>(width) *
                                      static_cast<float>(params.percent_left) / 100.0F);
    const int right = static_cast<int>(static_cast<float>(width) *
                                       static_cast<float>(params.percent_right) / 100.0F);
    const int top = static_cast<int>(static_cast<float>(height) *
                                     static_cast<float>(params.percent_top) / 100.0F);
    const int bottom = static_cast<int>(static_cast<float>(height) *
                                        static_cast<float>(params.percent_bottom) / 100.0F);
    const auto output_width = static_cast<std::uint32_t>(std::clamp<std::uint64_t>(
        static_cast<std::uint64_t>(width) + static_cast<std::uint64_t>(std::max(left, 0)) +
            static_cast<std::uint64_t>(std::max(right, 0)),
        5U, static_cast<std::uint64_t>(width) * 3U));
    const auto output_height = static_cast<std::uint32_t>(std::clamp<std::uint64_t>(
        static_cast<std::uint64_t>(height) + static_cast<std::uint64_t>(std::max(top, 0)) +
            static_cast<std::uint64_t>(std::max(bottom, 0)),
        5U, static_cast<std::uint64_t>(height) * 3U));
    const double horizontal_total = params.percent_left + params.percent_right;
    const double vertical_total = params.percent_top + params.percent_bottom;
    const float position_h =
        horizontal_total > 0.0 ? static_cast<float>(params.percent_left / horizontal_total) : 0.5F;
    const float position_v =
        vertical_total > 0.0 ? static_cast<float>(params.percent_top / vertical_total) : 0.5F;
    const auto image_x = static_cast<std::uint32_t>(static_cast<float>(output_width - width) *
                                                    std::clamp(position_h, 0.0F, 1.0F));
    const auto image_y = static_cast<std::uint32_t>(static_cast<float>(output_height - height) *
                                                    std::clamp(position_v, 0.0F, 1.0F));
    return CanvasLayout{output_width, output_height, image_x, image_y};
}

Result<WorkingImage> detail::apply_canvas_controlled(WorkingImage input, const CanvasParams &params,
                                                     const CancellationToken &cancellation,
                                                     const CanvasFrameControl control)
try
{
    auto active = cancellation.check();
    if (!active)
        return active.error();
    if (input.color_profile.model != ColorModel::kRgb ||
        input.color_profile.identifier != kInputProfileLinearRec709)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Canvas requires declared linear Rec709 working pixels",
                          {{"reason", "unsupported_canvas_working_space"},
                           {"profile", input.color_profile.identifier}});
    }
    if (input.mask_attached_frame.has_value())
    {
        return make_error(ErrorCode::kUnsupported, "Nested canvas operations are unsupported",
                          {{"reason", "nested_canvas_unsupported"}});
    }
    auto input_channels = checked_channels(input.width, input.height, "Canvas input");
    if (!input_channels || input.rgb.size() != input_channels.value())
        return !input_channels ? input_channels.error() :
                                 make_error(ErrorCode::kValidation,
                                            "Canvas input buffer does not match its dimensions");
    auto finite = validate_finite_samples(input.rgb, input.width, "Canvas",
                                          "nonfinite_canvas_input", cancellation);
    if (!finite)
        return finite.error();
    auto layout = compute_canvas_layout(input.width, input.height, params);
    if (!layout)
        return layout.error();
    auto output_channels = checked_channels(layout.value().output_width,
                                            layout.value().output_height, "Canvas output");
    if (!output_channels)
        return output_channels.error();
    WorkingImage output;
    output.width = layout.value().output_width;
    output.height = layout.value().output_height;
    output.color_profile = input.color_profile;
    output.exposure_analysis = input.exposure_analysis;
    output.canonical_roi_scale = input.canonical_roi_scale;
    output.mask_attached_frame = AttachedPixelFrame{layout.value().image_x, layout.value().image_y,
                                                    input.width, input.height};
    output.rgb.resize(output_channels.value());
    const auto fill = canvas_color(params.color);
    for (std::uint32_t row = 0U; row < output.height; ++row)
    {
        checkpoint(control, CanvasFrameCheckpoint::kCanvasRow, row);
        active = cancellation.check();
        if (!active)
            return active.error();
        float *destination = output.rgb.data() + static_cast<std::size_t>(row) * output.width * 3U;
        fill_pixels(destination, static_cast<int>(output.width), fill);
        if (row < layout.value().image_y || row >= layout.value().image_y + input.height)
            continue;
        const std::size_t source_row = row - layout.value().image_y;
        std::copy_n(input.rgb.data() + source_row * input.width * 3U,
                    static_cast<std::size_t>(input.width) * 3U,
                    destination + static_cast<std::size_t>(layout.value().image_x) * 3U);
    }
    checkpoint(control, CanvasFrameCheckpoint::kBeforePublication, 0U);
    active = cancellation.check();
    return active ? Result<WorkingImage>{std::move(output)} : active.error();
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Canvas allocation failed",
                      {{"reason", "allocation_failed"}});
}

Result<WorkingImage> apply_canvas(WorkingImage input, const CanvasParams &params,
                                  const CancellationToken &cancellation)
{
    return detail::apply_canvas_controlled(std::move(input), params, cancellation);
}

Result<WorkingImage> apply_canvas(WorkingImage input, const OperationInstance &operation,
                                  const CancellationToken &cancellation)
{
    if (operation.id != kCanvasOperationId ||
        operation.schema_version != kCanvasOperationSchemaVersion)
        return make_error(ErrorCode::kValidation, "Operation is not canonical Canvas");
    if (operation.mask_id.has_value())
        return make_error(ErrorCode::kUnsupported, "Canvas masks are unsupported",
                          {{"reason", "unsupported_canvas_mask"}});
    auto params = canvas_from_parameters(operation.parameters);
    return params ? apply_canvas(std::move(input), params.value(), cancellation) : params.error();
}

Result<FrameLayout> compute_frame_layout(const std::uint32_t width, const std::uint32_t height,
                                         const FrameParams &params)
{
    auto canonical = frame_to_parameters(params);
    if (!canonical)
        return canonical.error();
    if (width == 0U || height == 0U ||
        std::max(width, height) > static_cast<std::uint32_t>(std::numeric_limits<int>::max() / 3))
        return make_error(ErrorCode::kValidation, "Frame input dimensions are unsupported",
                          {{"reason", "invalid_frame_dimensions"}});
    const int maximum = static_cast<int>(std::max(width, height) * 3U);
    const float size = std::fabs(static_cast<float>(params.size));
    const bool constant = params.aspect == -1.0;
    FrameBasis basis = params.basis;
    if (basis == FrameBasis::kAuto)
        basis = constant ? FrameBasis::kLonger : FrameBasis::kWidth;
    if (basis == FrameBasis::kLonger)
        basis = width > height ? FrameBasis::kWidth : FrameBasis::kHeight;
    else if (basis == FrameBasis::kShorter)
        basis = width < height ? FrameBasis::kWidth : FrameBasis::kHeight;
    bool width_basis = basis == FrameBasis::kWidth;
    int basis_in = static_cast<int>(width_basis ? width : height);
    int other_in = static_cast<int>(width_basis ? height : width);
    int basis_out = basis_in;
    int other_out = other_in;
    if (constant)
    {
        basis_out = rounded_dimension(static_cast<float>(basis_in) / (1.0F - size), maximum);
        other_out = std::clamp(other_in + basis_out - basis_in, 1, maximum);
    }
    else
    {
        const float image_aspect = static_cast<float>(width) / static_cast<float>(height);
        float aspect = params.aspect == 0.0 ? image_aspect : static_cast<float>(params.aspect);
        if (params.orientation == FrameOrientation::kAuto)
        {
            if ((image_aspect < 1.0F && aspect > 1.0F) || (image_aspect > 1.0F && aspect < 1.0F))
                aspect = 1.0F / aspect;
        }
        else if (params.orientation == FrameOrientation::kLandscape && aspect < 1.0F)
            aspect = 1.0F / aspect;
        else if (params.orientation == FrameOrientation::kPortrait && aspect > 1.0F)
            aspect = 1.0F / aspect;
        const float border_width = static_cast<float>(basis_in) * (1.0F / (1.0F - size) - 1.0F);
        if ((width_basis && image_aspect < 1.0F) || (!width_basis && image_aspect > 1.0F))
            width_basis = !width_basis;
        if ((width_basis && image_aspect < aspect) || (!width_basis && image_aspect > aspect))
            width_basis = !width_basis;
        basis_in = static_cast<int>(width_basis ? width : height);
        other_in = static_cast<int>(width_basis ? height : width);
        if (!width_basis)
            aspect = 1.0F / aspect;
        basis_out = rounded_dimension(static_cast<float>(basis_in) + border_width, maximum);
        other_out = rounded_dimension(static_cast<float>(basis_out) / aspect, maximum);
    }
    int output_width = width_basis ? basis_out : other_out;
    int output_height = width_basis ? other_out : basis_out;
    output_width = std::clamp(output_width, 1, maximum);
    output_height = std::clamp(output_height, 1, maximum);
    if (output_width < static_cast<int>(width) || output_height < static_cast<int>(height))
        return make_error(ErrorCode::kValidation, "Frame layout would crop the source image",
                          {{"reason", "invalid_frame_layout"}});
    const auto image_x =
        static_cast<std::uint32_t>(static_cast<float>(output_width - static_cast<int>(width)) *
                                   static_cast<float>(params.position_h));
    const auto image_y =
        static_cast<std::uint32_t>(static_cast<float>(output_height - static_cast<int>(height)) *
                                   static_cast<float>(params.position_v));
    return FrameLayout{static_cast<std::uint32_t>(output_width),
                       static_cast<std::uint32_t>(output_height), image_x, image_y};
}

Result<ProfiledOutputBuffer> detail::apply_frame_controlled(ProfiledOutputBuffer input,
                                                            const FrameParams &params,
                                                            const CancellationToken &cancellation,
                                                            const CanvasFrameControl control)
try
{
    auto active = cancellation.check();
    if (!active)
        return active.error();
    auto input_channels = checked_channels(input.width, input.height, "Frame input");
    if (!input_channels || input.channels.size() != input_channels.value() ||
        input.color_profile.model != ColorModel::kRgb)
        return make_error(ErrorCode::kValidation, "Frame input is invalid",
                          {{"reason", "invalid_frame_input"}});
    auto finite = validate_finite_samples(input.channels, input.width, "Frame",
                                          "nonfinite_frame_input", cancellation);
    if (!finite)
        return finite.error();
    auto layout = compute_frame_layout(input.width, input.height, params);
    if (!layout)
        return layout.error();
    auto output_channels =
        checked_channels(layout.value().output_width, layout.value().output_height, "Frame output");
    if (!output_channels)
        return output_channels.error();
    ProfiledOutputBuffer output;
    output.width = layout.value().output_width;
    output.height = layout.value().output_height;
    output.color_profile = input.color_profile;
    output.channels.resize(output_channels.value());
    const std::array<float, 3> border{static_cast<float>(params.border_color[0]),
                                      static_cast<float>(params.border_color[1]),
                                      static_cast<float>(params.border_color[2])};
    const std::array<float, 3> frame{static_cast<float>(params.frame_color[0]),
                                     static_cast<float>(params.frame_color[1]),
                                     static_cast<float>(params.frame_color[2])};
    const BorderInfo info = border_info(input.width, input.height, layout.value(), params);
    const int image_width = info.image_right - info.image_left;
    for (int row = 0; row < info.height; ++row)
    {
        checkpoint(control, CanvasFrameCheckpoint::kFrameRow, static_cast<std::uint32_t>(row));
        active = cancellation.check();
        if (!active)
            return active.error();
        float *out = output.channels.data() +
                     static_cast<std::size_t>(row) * static_cast<std::size_t>(info.width) * 3U;
        if (row < info.border_top || row >= info.border_bottom)
            fill_pixels(out, info.width, border);
        else if (row < info.frame_top || row >= info.frame_bottom)
        {
            fill_pixels(out, info.border_left, border);
            fill_pixels(out + info.border_left * 3, info.border_right - info.border_left, frame);
            fill_pixels(out + info.border_right * 3, info.width - info.border_right, border);
        }
        else if (row < info.image_top || row >= info.image_bottom)
        {
            fill_pixels(out, info.border_left, border);
            fill_pixels(out + info.border_left * 3, info.frame_left - info.border_left, frame);
            fill_pixels(out + info.frame_left * 3, info.frame_right - info.frame_left, border);
            fill_pixels(out + info.frame_right * 3, info.border_right - info.frame_right, frame);
            fill_pixels(out + info.border_right * 3, info.width - info.border_right, border);
        }
        else
        {
            fill_pixels(out, info.border_left, border);
            if (info.image_left > info.border_left)
            {
                fill_pixels(out + info.border_left * 3, info.frame_left - info.border_left, frame);
                fill_pixels(out + info.frame_left * 3, info.image_left - info.frame_left, border);
            }
            const std::size_t source_row = static_cast<std::size_t>(row - info.image_top);
            std::copy_n(input.channels.data() + source_row * input.width * 3U,
                        static_cast<std::size_t>(image_width) * 3U, out + info.image_left * 3);
            fill_pixels(out + info.image_right * 3, info.frame_right - info.image_right, border);
            if (info.width > info.frame_right)
            {
                fill_pixels(out + info.frame_right * 3, info.border_right - info.frame_right,
                            frame);
                fill_pixels(out + info.border_right * 3, info.width - info.border_right, border);
            }
        }
    }
    checkpoint(control, CanvasFrameCheckpoint::kBeforePublication, 0U);
    active = cancellation.check();
    return active ? Result<ProfiledOutputBuffer>{std::move(output)} : active.error();
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Frame allocation failed", {{"reason", "allocation_failed"}});
}

Result<ProfiledOutputBuffer> apply_frame(ProfiledOutputBuffer input, const FrameParams &params,
                                         const CancellationToken &cancellation)
{
    return detail::apply_frame_controlled(std::move(input), params, cancellation);
}

Result<ProfiledOutputBuffer> apply_frame(ProfiledOutputBuffer input,
                                         const OperationInstance &operation,
                                         const CancellationToken &cancellation)
{
    if (operation.id != kFrameOperationId ||
        operation.schema_version != kFrameOperationSchemaVersion)
        return make_error(ErrorCode::kValidation, "Operation is not canonical Frame");
    if (operation.mask_id.has_value())
        return make_error(ErrorCode::kUnsupported, "Frame masks are unsupported",
                          {{"reason", "unsupported_frame_mask"}});
    auto params = frame_from_parameters(operation.parameters);
    return params ? apply_frame(std::move(input), params.value(), cancellation) : params.error();
}

} // namespace ravo
