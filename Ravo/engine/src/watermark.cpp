#include "watermark.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace ravo
{
namespace
{

using Glyph = std::array<std::uint8_t, 7>;

[[nodiscard]] Glyph glyph_rows(char character) noexcept
{
    if (character >= 'a' && character <= 'z')
        character = static_cast<char>(character - 'a' + 'A');
    switch (character)
    {
    case 'A':
        return {14, 17, 17, 31, 17, 17, 17};
    case 'B':
        return {30, 17, 17, 30, 17, 17, 30};
    case 'C':
        return {14, 17, 16, 16, 16, 17, 14};
    case 'D':
        return {30, 17, 17, 17, 17, 17, 30};
    case 'E':
        return {31, 16, 16, 30, 16, 16, 31};
    case 'F':
        return {31, 16, 16, 30, 16, 16, 16};
    case 'G':
        return {14, 17, 16, 23, 17, 17, 15};
    case 'H':
        return {17, 17, 17, 31, 17, 17, 17};
    case 'I':
        return {31, 4, 4, 4, 4, 4, 31};
    case 'J':
        return {7, 2, 2, 2, 18, 18, 12};
    case 'K':
        return {17, 18, 20, 24, 20, 18, 17};
    case 'L':
        return {16, 16, 16, 16, 16, 16, 31};
    case 'M':
        return {17, 27, 21, 21, 17, 17, 17};
    case 'N':
        return {17, 25, 21, 19, 17, 17, 17};
    case 'O':
        return {14, 17, 17, 17, 17, 17, 14};
    case 'P':
        return {30, 17, 17, 30, 16, 16, 16};
    case 'Q':
        return {14, 17, 17, 17, 21, 18, 13};
    case 'R':
        return {30, 17, 17, 30, 20, 18, 17};
    case 'S':
        return {15, 16, 16, 14, 1, 1, 30};
    case 'T':
        return {31, 4, 4, 4, 4, 4, 4};
    case 'U':
        return {17, 17, 17, 17, 17, 17, 14};
    case 'V':
        return {17, 17, 17, 17, 17, 10, 4};
    case 'W':
        return {17, 17, 17, 21, 21, 21, 10};
    case 'X':
        return {17, 17, 10, 4, 10, 17, 17};
    case 'Y':
        return {17, 17, 10, 4, 4, 4, 4};
    case 'Z':
        return {31, 1, 2, 4, 8, 16, 31};
    case '0':
        return {14, 17, 19, 21, 25, 17, 14};
    case '1':
        return {4, 12, 4, 4, 4, 4, 14};
    case '2':
        return {14, 17, 1, 2, 4, 8, 31};
    case '3':
        return {30, 1, 1, 14, 1, 1, 30};
    case '4':
        return {2, 6, 10, 18, 31, 2, 2};
    case '5':
        return {31, 16, 16, 30, 1, 1, 30};
    case '6':
        return {14, 16, 16, 30, 17, 17, 14};
    case '7':
        return {31, 1, 2, 4, 8, 8, 8};
    case '8':
        return {14, 17, 17, 14, 17, 17, 14};
    case '9':
        return {14, 17, 17, 15, 1, 1, 14};
    case '.':
        return {0, 0, 0, 0, 0, 12, 12};
    case ',':
        return {0, 0, 0, 0, 0, 12, 8};
    case ':':
        return {0, 12, 12, 0, 12, 12, 0};
    case ';':
        return {0, 12, 12, 0, 12, 8, 0};
    case '!':
        return {4, 4, 4, 4, 4, 0, 4};
    case '?':
        return {14, 17, 1, 2, 4, 0, 4};
    case '-':
        return {0, 0, 0, 31, 0, 0, 0};
    case '_':
        return {0, 0, 0, 0, 0, 0, 31};
    case '/':
        return {1, 2, 2, 4, 8, 8, 16};
    case '+':
        return {0, 4, 4, 31, 4, 4, 0};
    case '(':
        return {2, 4, 8, 8, 8, 4, 2};
    case ')':
        return {8, 4, 2, 2, 2, 4, 8};
    case '[':
        return {14, 8, 8, 8, 8, 8, 14};
    case ']':
        return {14, 2, 2, 2, 2, 2, 14};
    case '#':
        return {10, 31, 10, 10, 31, 10, 0};
    case '@':
        return {14, 17, 23, 21, 23, 16, 14};
    case '&':
        return {12, 18, 20, 8, 21, 18, 13};
    case '%':
        return {17, 2, 4, 8, 17, 0, 0};
    case '\'':
        return {4, 4, 8, 0, 0, 0, 0};
    case ' ':
        return {};
    default:
        return {};
    }
}

struct Layout
{
    std::vector<std::string_view> lines;
    std::uint32_t cell = 1U;
    double width = 0.0;
    double height = 0.0;
    double center_x = 0.0;
    double center_y = 0.0;
    double cosine = 1.0;
    double sine = 0.0;
};

void checkpoint(const detail::WatermarkControl &control, const detail::WatermarkCheckpoint stage,
                const std::uint32_t progress) noexcept
{
    if (control.checkpoint_callback != nullptr)
        control.checkpoint_callback(control.context, stage, progress);
}

[[nodiscard]] Result<void> validate_input(const ProfiledOutputBuffer &input)
{
    auto valid = validate_profiled_output_for_pack(input, 12U);
    if (!valid)
        return valid.error();
    for (std::size_t index = 0U; index < input.channels.size(); ++index)
    {
        if (!std::isfinite(input.channels[index]))
            return make_error(
                ErrorCode::kValidation, "Watermark input contains NaN or infinity",
                {{"sample_index", std::to_string(index)}, {"reason", "nonfinite_watermark_input"}});
    }
    return {};
}

[[nodiscard]] Result<Layout> make_layout(const std::string &text, const std::uint32_t width,
                                         const std::uint32_t height, const WatermarkParams &params)
{
    Layout layout;
    std::size_t begin = 0U;
    std::size_t maximum_characters = 0U;
    while (begin <= text.size())
    {
        const std::size_t end = text.find('\n', begin);
        const std::size_t count = (end == std::string::npos ? text.size() : end) - begin;
        if (count == 0U || count > kWatermarkLineMaxCharacters)
            return make_error(ErrorCode::kValidation, "Expanded watermark line is invalid",
                              {{"reason", "invalid_watermark_layout"}});
        layout.lines.emplace_back(text.data() + begin, count);
        maximum_characters = std::max(maximum_characters, count);
        if (end == std::string::npos)
            break;
        begin = end + 1U;
    }
    if (layout.lines.empty() || layout.lines.size() > kWatermarkMaxLines)
        return make_error(ErrorCode::kValidation, "Expanded watermark line count is invalid",
                          {{"reason", "invalid_watermark_layout"}});
    const double desired_height =
        static_cast<double>(std::min(width, height)) * params.scale_percent / 100.0;
    layout.cell = std::max(1U, static_cast<std::uint32_t>(std::round(desired_height / 7.0)));
    const std::uint64_t logical_width =
        (static_cast<std::uint64_t>(maximum_characters) * 6U - 1U) * layout.cell;
    const std::uint64_t logical_height =
        (static_cast<std::uint64_t>(layout.lines.size()) * 8U - 1U) * layout.cell;
    if (logical_width == 0U || logical_height == 0U ||
        logical_width > std::numeric_limits<std::uint32_t>::max() ||
        logical_height > std::numeric_limits<std::uint32_t>::max())
        return make_error(ErrorCode::kValidation, "Watermark layout dimensions overflow",
                          {{"reason", "invalid_watermark_layout"}});
    layout.width = static_cast<double>(logical_width);
    layout.height = static_cast<double>(logical_height);
    constexpr double pi = 3.14159265358979323846;
    const double angle = params.rotation_degrees * pi / 180.0;
    layout.cosine = std::cos(angle);
    layout.sine = std::sin(angle);
    if (!std::isfinite(layout.cosine) || !std::isfinite(layout.sine))
        return make_error(ErrorCode::kValidation, "Watermark rotation is non-finite",
                          {{"reason", "invalid_watermark_layout"}});
    const double bounding_width =
        std::abs(layout.width * layout.cosine) + std::abs(layout.height * layout.sine);
    const double bounding_height =
        std::abs(layout.width * layout.sine) + std::abs(layout.height * layout.cosine);
    const int column = static_cast<int>(params.alignment) % 3;
    const int row = static_cast<int>(params.alignment) / 3;
    const double left = column == 0 ? 0.0 :
                        column == 1 ? (static_cast<double>(width) - bounding_width) * 0.5 :
                                      static_cast<double>(width) - bounding_width;
    const double top = row == 0 ? 0.0 :
                       row == 1 ? (static_cast<double>(height) - bounding_height) * 0.5 :
                                  static_cast<double>(height) - bounding_height;
    layout.center_x = left + bounding_width * 0.5 + params.x_offset * width;
    layout.center_y = top + bounding_height * 0.5 + params.y_offset * height;
    return layout;
}

[[nodiscard]] bool glyph_sample(const Layout &layout, const double x, const double y) noexcept
{
    const double dx = x - layout.center_x;
    const double dy = y - layout.center_y;
    const double local_x = dx * layout.cosine + dy * layout.sine + layout.width * 0.5;
    const double local_y = -dx * layout.sine + dy * layout.cosine + layout.height * 0.5;
    if (local_x < 0.0 || local_y < 0.0 || local_x >= layout.width || local_y >= layout.height)
        return false;
    const auto cell_x = static_cast<std::uint64_t>(local_x) / layout.cell;
    const auto cell_y = static_cast<std::uint64_t>(local_y) / layout.cell;
    const std::size_t line = static_cast<std::size_t>(cell_y / 8U);
    const std::uint64_t glyph_row = cell_y % 8U;
    const std::size_t character = static_cast<std::size_t>(cell_x / 6U);
    const std::uint64_t glyph_column = cell_x % 6U;
    if (line >= layout.lines.size() || character >= layout.lines[line].size() || glyph_row >= 7U ||
        glyph_column >= 5U)
        return false;
    const Glyph rows = glyph_rows(layout.lines[line][character]);
    return (rows[static_cast<std::size_t>(glyph_row)] & (1U << (4U - glyph_column))) != 0U;
}

[[nodiscard]] float coverage(const Layout &layout, const std::uint32_t x,
                             const std::uint32_t y) noexcept
{
    constexpr std::array<double, 2> samples{0.25, 0.75};
    int active = 0;
    for (const double sy : samples)
        for (const double sx : samples)
            active +=
                glyph_sample(layout, static_cast<double>(x) + sx, static_cast<double>(y) + sy) ? 1 :
                                                                                                 0;
    return static_cast<float>(active) * 0.25F;
}

} // namespace

Result<ProfiledOutputBuffer> detail::apply_watermark_controlled(
    ProfiledOutputBuffer input, const WatermarkParams &params, const AssetDescriptor &asset,
    const CancellationToken &cancellation, const WatermarkControl control)
try
{
    auto active = cancellation.check();
    if (!active)
        return active.error();
    auto canonical = watermark_to_parameters(params);
    if (!canonical)
        return canonical.error();
    auto valid = validate_input(input);
    if (!valid)
        return valid.error();
    auto text = expand_watermark_text(params.text, asset);
    if (!text)
        return text.error();
    auto layout = make_layout(text.value(), input.width, input.height, params);
    if (!layout)
        return layout.error();
    const std::array<float, 3> color{static_cast<float>(params.color[0]),
                                     static_cast<float>(params.color[1]),
                                     static_cast<float>(params.color[2])};
    const float opacity = static_cast<float>(params.opacity);
    for (std::uint32_t row = 0U; row < input.height; ++row)
    {
        checkpoint(control, WatermarkCheckpoint::kProcessRow, row);
        active = cancellation.check();
        if (!active)
            return active.error();
        for (std::uint32_t column = 0U; column < input.width; ++column)
        {
            const float alpha = coverage(layout.value(), column, row) * opacity;
            if (alpha == 0.0F)
                continue;
            const std::size_t pixel = (static_cast<std::size_t>(row) * input.width + column) * 3U;
            for (std::size_t channel = 0U; channel < 3U; ++channel)
                input.channels[pixel + channel] =
                    (1.0F - alpha) * input.channels[pixel + channel] + alpha * color[channel];
        }
    }
    checkpoint(control, WatermarkCheckpoint::kBeforePublication, 0U);
    active = cancellation.check();
    return active ? Result<ProfiledOutputBuffer>{std::move(input)} : active.error();
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Watermark allocation failed",
                      {{"reason", "allocation_failed"}});
}

Result<ProfiledOutputBuffer> apply_watermark(ProfiledOutputBuffer input,
                                             const WatermarkParams &params,
                                             const AssetDescriptor &asset,
                                             const CancellationToken &cancellation)
{
    return detail::apply_watermark_controlled(std::move(input), params, asset, cancellation);
}

Result<ProfiledOutputBuffer> apply_watermark(ProfiledOutputBuffer input,
                                             const OperationInstance &operation,
                                             const AssetDescriptor &asset,
                                             const CancellationToken &cancellation)
{
    if (operation.id != kWatermarkOperationId ||
        operation.schema_version != kWatermarkOperationSchemaVersion)
        return make_error(ErrorCode::kValidation, "Operation is not canonical Watermark");
    if (operation.mask_id.has_value())
        return make_error(ErrorCode::kUnsupported, "Watermark masks are unsupported",
                          {{"reason", "unsupported_watermark_mask"}});
    auto params = watermark_from_parameters(operation.parameters);
    return params ? apply_watermark(std::move(input), params.value(), asset, cancellation) :
                    params.error();
}

Result<RenderedExportImage> apply_watermark_to_export_image(RenderedExportImage image,
                                                            const WatermarkParams &params,
                                                            const AssetDescriptor &asset,
                                                            const CancellationToken &cancellation)
{
    auto cancelled = cancellation.check();
    if (!cancelled)
        return cancelled.error();
    auto canonical = watermark_to_parameters(params);
    if (!canonical)
        return canonical.error();
    if (image.width == 0 || image.height == 0)
        return image;

    const std::size_t expected =
        static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 3U;
    ProfiledOutputBuffer profiled;
    profiled.width = image.width;
    profiled.height = image.height;
    profiled.color_profile = image.color_profile;
    profiled.channels.resize(expected);

    auto to_unit = [&](auto &samples) -> Result<void>
    {
        using Sample = std::decay_t<decltype(samples)>;
        if (samples.size() != expected)
        {
            return make_error(ErrorCode::kValidation, "Export watermark buffer size is invalid",
                              {{"reason", "invalid_export_watermark_buffer"}});
        }
        if constexpr (std::is_same_v<typename Sample::value_type, float>)
        {
            for (std::size_t i = 0; i < samples.size(); ++i)
                profiled.channels[i] = std::clamp(samples[i], 0.0f, 1.0f);
        }
        else if constexpr (std::is_same_v<typename Sample::value_type, std::uint16_t>)
        {
            constexpr float scale = 1.0f / 65535.0f;
            for (std::size_t i = 0; i < samples.size(); ++i)
                profiled.channels[i] = static_cast<float>(samples[i]) * scale;
        }
        else
        {
            constexpr float scale = 1.0f / 255.0f;
            for (std::size_t i = 0; i < samples.size(); ++i)
                profiled.channels[i] = static_cast<float>(samples[i]) * scale;
        }
        return {};
    };

    if (std::holds_alternative<std::vector<std::uint8_t>>(image.samples))
    {
        auto converted = to_unit(std::get<std::vector<std::uint8_t>>(image.samples));
        if (!converted)
            return converted.error();
    }
    else if (std::holds_alternative<std::vector<std::uint16_t>>(image.samples))
    {
        auto converted = to_unit(std::get<std::vector<std::uint16_t>>(image.samples));
        if (!converted)
            return converted.error();
    }
    else
    {
        auto converted = to_unit(std::get<std::vector<float>>(image.samples));
        if (!converted)
            return converted.error();
    }

    auto marked = apply_watermark(std::move(profiled), params, asset, cancellation);
    if (!marked)
        return marked.error();

    auto from_unit = [&](auto &samples) -> Result<RenderedExportImage>
    {
        using Sample = std::decay_t<decltype(samples)>;
        samples.resize(expected);
        if constexpr (std::is_same_v<typename Sample::value_type, float>)
        {
            for (std::size_t i = 0; i < expected; ++i)
                samples[i] = marked.value().channels[i];
        }
        else if constexpr (std::is_same_v<typename Sample::value_type, std::uint16_t>)
        {
            for (std::size_t i = 0; i < expected; ++i)
                samples[i] = static_cast<std::uint16_t>(
                    std::lround(std::clamp(marked.value().channels[i], 0.0f, 1.0f) * 65535.0f));
        }
        else
        {
            for (std::size_t i = 0; i < expected; ++i)
                samples[i] = static_cast<std::uint8_t>(
                    std::lround(std::clamp(marked.value().channels[i], 0.0f, 1.0f) * 255.0f));
        }
        image.color_profile = marked.value().color_profile;
        return image;
    };

    if (std::holds_alternative<std::vector<std::uint8_t>>(image.samples))
        return from_unit(std::get<std::vector<std::uint8_t>>(image.samples));
    if (std::holds_alternative<std::vector<std::uint16_t>>(image.samples))
        return from_unit(std::get<std::vector<std::uint16_t>>(image.samples));
    return from_unit(std::get<std::vector<float>>(image.samples));
}

} // namespace ravo
