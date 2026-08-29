#include "output_dither.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <string>

namespace ravo
{
namespace
{

constexpr float kRightWeight = 7.0F / 16.0F;
constexpr float kDownRightWeight = 1.0F / 16.0F;
constexpr float kDownWeight = 5.0F / 16.0F;
constexpr float kDownLeftWeight = 3.0F / 16.0F;

struct FloydParameters
{
    bool enabled = false;
    bool gray = false;
    std::uint32_t levels = 0U;
};

void checkpoint(const detail::OutputDitherControl &control,
                const detail::OutputDitherCheckpoint stage, const std::uint32_t progress) noexcept
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
        {
            return make_error(ErrorCode::kValidation,
                              "Output dither input contains NaN or infinity",
                              {{"sample_index", std::to_string(index)},
                               {"reason", "nonfinite_output_dither_input"}});
        }
    }
    return {};
}

[[nodiscard]] float quantize(const float value, const float factor, const float reciprocal) noexcept
{
    return reciprocal * std::ceil(value * factor - 0.5F);
}

void nearest_color(float *const pixel, std::array<float, 3> &error, const bool gray,
                   const float factor, const float reciprocal) noexcept
{
    if (gray)
    {
        const float luminance = 0.30F * pixel[0] + 0.59F * pixel[1] + 0.11F * pixel[2];
        const float mapped = quantize(luminance, factor, reciprocal);
        for (std::size_t channel = 0U; channel < 3U; ++channel)
        {
            error[channel] = pixel[channel] - mapped;
            pixel[channel] = mapped;
        }
        return;
    }
    for (std::size_t channel = 0U; channel < 3U; ++channel)
    {
        const float previous = pixel[channel];
        const float mapped = quantize(previous, factor, reciprocal);
        error[channel] = previous - mapped;
        pixel[channel] = mapped;
    }
}

void diffuse(float *const pixel, const std::array<float, 3> &error, const float weight) noexcept
{
    for (std::size_t channel = 0U; channel < 3U; ++channel)
        pixel[channel] += error[channel] * weight;
}

[[nodiscard]] FloydParameters floyd_parameters(const OutputDitherMethod method,
                                               const OutputDitherTarget target) noexcept
{
    switch (method)
    {
    case OutputDitherMethod::kFloydSteinberg1BitGray:
        return {true, true, 2U};
    case OutputDitherMethod::kFloydSteinberg1BitRgb:
        return {true, false, 2U};
    case OutputDitherMethod::kFloydSteinberg2BitGray:
        return {true, true, 4U};
    case OutputDitherMethod::kFloydSteinberg2BitRgb:
        return {true, false, 4U};
    case OutputDitherMethod::kFloydSteinberg4BitGray:
        return {true, true, 16U};
    case OutputDitherMethod::kFloydSteinberg4BitRgb:
        return {true, false, 16U};
    case OutputDitherMethod::kFloydSteinberg6BitGray:
        return {true, true, 64U};
    case OutputDitherMethod::kFloydSteinberg8BitRgb:
        return {true, false, 256U};
    case OutputDitherMethod::kFloydSteinberg16BitRgb:
        return {true, false, 65536U};
    case OutputDitherMethod::kFloydSteinbergAuto:
        if (target == OutputDitherTarget::kExportRgb8)
            return {true, false, 256U};
        if (target == OutputDitherTarget::kExportRgb16)
            return {true, false, 65536U};
        return {};
    default:
        return {};
    }
}

[[nodiscard]] int posterize_levels(const OutputDitherMethod method) noexcept
{
    switch (method)
    {
    case OutputDitherMethod::kPosterize2:
        return 2;
    case OutputDitherMethod::kPosterize3:
        return 3;
    case OutputDitherMethod::kPosterize4:
        return 4;
    case OutputDitherMethod::kPosterize5:
        return 5;
    case OutputDitherMethod::kPosterize6:
        return 6;
    case OutputDitherMethod::kPosterize7:
        return 7;
    case OutputDitherMethod::kPosterize8:
        return 8;
    default:
        return 0;
    }
}

void encrypt_tea(std::array<std::uint32_t, 2> &state) noexcept
{
    constexpr std::array<std::uint32_t, 4> key{0xa341316cU, 0xc8013ea4U, 0xad90777dU, 0x7e95761eU};
    std::uint32_t value0 = state[0];
    std::uint32_t value1 = state[1];
    std::uint32_t sum = 0U;
    constexpr std::uint32_t delta = 0x9e3779b9U;
    for (int round = 0; round < 8; ++round)
    {
        sum += delta;
        value0 += ((value1 << 4U) + key[0]) ^ (value1 + sum) ^ ((value1 >> 5U) + key[1]);
        value1 += ((value0 << 4U) + key[2]) ^ (value0 + sum) ^ ((value0 >> 5U) + key[3]);
    }
    state = {value0, value1};
}

[[nodiscard]] float triangular_noise(const std::uint32_t random) noexcept
{
    const float uniform =
        static_cast<float>(random) / static_cast<float>(std::numeric_limits<std::uint32_t>::max());
    return uniform < 0.5F ? std::sqrt(2.0F * uniform) - 1.0F :
                            1.0F - std::sqrt(2.0F * (1.0F - uniform));
}

[[nodiscard]] Result<void> clip_rows(ProfiledOutputBuffer &output,
                                     const CancellationToken &cancellation,
                                     const detail::OutputDitherControl &control)
{
    for (std::uint32_t row = 0U; row < output.height; ++row)
    {
        checkpoint(control, detail::OutputDitherCheckpoint::kProcessRow, row);
        auto active = cancellation.check();
        if (!active)
            return active.error();
        const std::size_t begin = static_cast<std::size_t>(row) * output.width * 3U;
        const std::size_t end = begin + static_cast<std::size_t>(output.width) * 3U;
        for (std::size_t index = begin; index < end; ++index)
            output.channels[index] = std::clamp(output.channels[index], 0.0F, 1.0F);
    }
    return {};
}

} // namespace

Result<ProfiledOutputBuffer> detail::apply_output_dither_controlled(
    ProfiledOutputBuffer input, const OutputDitherParams &params, const OutputDitherTarget target,
    const CancellationToken &cancellation, const OutputDitherControl control)
try
{
    checkpoint(control, OutputDitherCheckpoint::kBeforeValidation, 0U);
    auto active = cancellation.check();
    if (!active)
        return active.error();
    auto canonical = output_dither_to_parameters(params);
    if (!canonical)
        return canonical.error();
    switch (target)
    {
    case OutputDitherTarget::kPreviewRgb8:
    case OutputDitherTarget::kExportRgb8:
    case OutputDitherTarget::kExportRgb16:
    case OutputDitherTarget::kExportRgbFloat:
        break;
    default:
        return make_error(ErrorCode::kValidation, "Output dither target is unsupported",
                          {{"reason", "invalid_output_dither_target"}});
    }
    auto valid = validate_input(input);
    if (!valid)
        return valid.error();

    const int levels = posterize_levels(params.method);
    if (levels > 0)
    {
        const float factor = static_cast<float>(levels - 1);
        const float reciprocal = 1.0F / factor;
        for (std::uint32_t row = 0U; row < input.height; ++row)
        {
            checkpoint(control, OutputDitherCheckpoint::kProcessRow, row);
            active = cancellation.check();
            if (!active)
                return active.error();
            const std::size_t begin = static_cast<std::size_t>(row) * input.width * 3U;
            const std::size_t end = begin + static_cast<std::size_t>(input.width) * 3U;
            for (std::size_t index = begin; index < end; ++index)
                input.channels[index] = quantize(input.channels[index], factor, reciprocal);
        }
    }
    else if (params.method == OutputDitherMethod::kRandom)
    {
        const float amplitude =
            std::pow(2.0F, static_cast<float>(params.random_damping_db) / 10.0F);
        std::array<std::uint32_t, 2> state{};
        for (std::uint32_t row = 0U; row < input.height; ++row)
        {
            checkpoint(control, OutputDitherCheckpoint::kProcessRow, row);
            active = cancellation.check();
            if (!active)
                return active.error();
            state[0] = row * input.height;
            for (std::uint32_t column = 0U; column < input.width; ++column)
            {
                encrypt_tea(state);
                const float noise = amplitude * triangular_noise(state[0]);
                const std::size_t pixel =
                    (static_cast<std::size_t>(row) * input.width + column) * 3U;
                for (std::size_t channel = 0U; channel < 3U; ++channel)
                    input.channels[pixel + channel] =
                        std::clamp(input.channels[pixel + channel] + noise, 0.0F, 1.0F);
            }
        }
    }
    else
    {
        const FloydParameters floyd = floyd_parameters(params.method, target);
        auto clipped = clip_rows(input, cancellation, control);
        if (!clipped)
            return clipped.error();
        if (floyd.enabled)
        {
            const float factor = static_cast<float>(floyd.levels - 1U);
            const float reciprocal = 1.0F / factor;
            std::array<float, 3> error{};
            if (input.width < 3U || input.height < 3U)
            {
                for (std::uint32_t row = 0U; row < input.height; ++row)
                {
                    checkpoint(control, OutputDitherCheckpoint::kProcessRow, row);
                    active = cancellation.check();
                    if (!active)
                        return active.error();
                    for (std::uint32_t column = 0U; column < input.width; ++column)
                    {
                        const std::size_t pixel =
                            (static_cast<std::size_t>(row) * input.width + column) * 3U;
                        nearest_color(input.channels.data() + pixel, error, floyd.gray, factor,
                                      reciprocal);
                    }
                }
                checkpoint(control, OutputDitherCheckpoint::kBeforePublication, 0U);
                active = cancellation.check();
                return active ? Result<ProfiledOutputBuffer>{std::move(input)} : active.error();
            }
            for (std::uint32_t row = 0U; row < input.height; ++row)
            {
                checkpoint(control, OutputDitherCheckpoint::kProcessRow, row);
                active = cancellation.check();
                if (!active)
                    return active.error();
                for (std::uint32_t column = 0U; column < input.width; ++column)
                {
                    const std::size_t pixel =
                        (static_cast<std::size_t>(row) * input.width + column) * 3U;
                    nearest_color(input.channels.data() + pixel, error, floyd.gray, factor,
                                  reciprocal);
                    if (column + 1U < input.width)
                        diffuse(input.channels.data() + pixel + 3U, error, kRightWeight);
                    if (row + 1U >= input.height)
                        continue;
                    const std::size_t down = pixel + static_cast<std::size_t>(input.width) * 3U;
                    if (column > 0U)
                        diffuse(input.channels.data() + down - 3U, error, kDownLeftWeight);
                    diffuse(input.channels.data() + down, error, kDownWeight);
                    if (column + 1U < input.width)
                        diffuse(input.channels.data() + down + 3U, error, kDownRightWeight);
                }
            }
        }
    }

    for (std::size_t index = 0U; index < input.channels.size(); ++index)
    {
        if (!std::isfinite(input.channels[index]))
        {
            return make_error(ErrorCode::kValidation, "Output dither produced NaN or infinity",
                              {{"sample_index", std::to_string(index)},
                               {"reason", "nonfinite_output_dither_output"}});
        }
    }
    checkpoint(control, OutputDitherCheckpoint::kBeforePublication, 0U);
    active = cancellation.check();
    if (!active)
        return active.error();
    return input;
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Output dither allocation failed",
                      {{"reason", "allocation_failed"}});
}

Result<ProfiledOutputBuffer> apply_output_dither(ProfiledOutputBuffer input,
                                                 const OutputDitherParams &params,
                                                 const OutputDitherTarget target,
                                                 const CancellationToken &cancellation)
{
    return detail::apply_output_dither_controlled(std::move(input), params, target, cancellation);
}

Result<ProfiledOutputBuffer> apply_output_dither(ProfiledOutputBuffer input,
                                                 const OperationInstance &operation,
                                                 const OutputDitherTarget target,
                                                 const CancellationToken &cancellation)
{
    if (operation.id != kOutputDitherOperationId)
    {
        return make_error(ErrorCode::kValidation, "Operation is not Output Dither",
                          {{"operation_id", operation.id}});
    }
    if (operation.schema_version != kOutputDitherOperationSchemaVersion)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Output dither operation schema version is unsupported",
                          {{"schema_version", std::to_string(operation.schema_version)},
                           {"reason", "unsupported_output_dither_schema"}});
    }
    if (operation.mask_id.has_value())
    {
        return make_error(
            ErrorCode::kUnsupported, "Output dither masks are unsupported",
            {{"mask_id", *operation.mask_id}, {"reason", "unsupported_output_dither_mask"}});
    }
    auto params = output_dither_from_parameters(operation.parameters);
    if (!params)
        return params.error();
    return apply_output_dither(std::move(input), params.value(), target, cancellation);
}

} // namespace ravo
