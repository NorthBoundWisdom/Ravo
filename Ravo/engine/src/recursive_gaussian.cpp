#include "recursive_gaussian.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace ravo::detail
{
namespace
{

constexpr float kFrozenSignalMinimum = -1.0e9F;
constexpr float kFrozenSignalMaximum = 1.0e9F;

struct RecursiveGaussianCoefficients
{
    float a0 = 0.0F;
    float a1 = 0.0F;
    float a2 = 0.0F;
    float a3 = 0.0F;
    float b1 = 0.0F;
    float b2 = 0.0F;
    float coefp = 0.0F;
    float coefn = 0.0F;
};

[[nodiscard]] std::uint64_t saturating_multiply(const std::uint64_t left,
                                                const std::uint64_t right) noexcept
{
    if (left == 0U || right == 0U)
    {
        return 0U;
    }
    if (left > std::numeric_limits<std::uint64_t>::max() / right)
    {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return left * right;
}

void checkpoint(const RecursiveGaussianControl control, const RecursiveGaussianCheckpoint stage,
                const std::uint32_t progress) noexcept
{
    if (control.checkpoint_callback != nullptr)
    {
        control.checkpoint_callback(control.context, stage, progress);
    }
}

// This is the frozen CLAMPF conditional, not std::clamp: the legacy helper
// performs it at every recurrence read, including its +/- 1e9 mean-blur range.
[[nodiscard]] float frozen_clamp(const float value) noexcept
{
    return value >= kFrozenSignalMinimum ?
               (value <= kFrozenSignalMaximum ? value : kFrozenSignalMaximum) :
               kFrozenSignalMinimum;
}

[[nodiscard]] Result<RecursiveGaussianCoefficients> coefficients(const float sigma)
{
    if (!std::isfinite(sigma) || sigma <= 0.0F)
    {
        return make_error(ErrorCode::kValidation,
                          "Recursive Gaussian sigma must be finite and positive",
                          {{"reason", "invalid_recursive_gaussian_sigma"}});
    }

    // Keep the exact DT_IOP_GAUSSIAN_ZERO calculation and its float expression
    // order.  Ravo target flags disable contraction for both owner and oracle.
    const float alpha = 1.695F / sigma;
    const float ema = std::exp(-alpha);
    const float ema2 = std::exp(-2.0F * alpha);
    RecursiveGaussianCoefficients result;
    result.b1 = -2.0F * ema;
    result.b2 = ema2;
    const float k = (1.0F - ema) * (1.0F - ema) / (1.0F + (2.0F * alpha * ema) - ema2);
    result.a0 = k;
    result.a1 = k * (alpha - 1.0F) * ema;
    result.a2 = k * (alpha + 1.0F) * ema;
    result.a3 = -k * ema2;
    result.coefp = (result.a0 + result.a1) / (1.0F + result.b1 + result.b2);
    result.coefn = (result.a2 + result.a3) / (1.0F + result.b1 + result.b2);
    const auto finite = [](const float value) noexcept { return std::isfinite(value); };
    if (!finite(result.a0) || !finite(result.a1) || !finite(result.a2) || !finite(result.a3) ||
        !finite(result.b1) || !finite(result.b2) || !finite(result.coefp) || !finite(result.coefn))
    {
        return make_error(ErrorCode::kValidation,
                          "Recursive Gaussian coefficients are non-finite for sigma",
                          {{"reason", "invalid_recursive_gaussian_sigma"}});
    }
    return result;
}

[[nodiscard]] Result<std::size_t> element_count(const std::uint32_t width,
                                                const std::uint32_t height)
{
    if (width == 0U || height == 0U)
    {
        return make_error(ErrorCode::kValidation, "Recursive Gaussian dimensions must be non-zero",
                          {{"reason", "invalid_recursive_gaussian_dimensions"}});
    }
    const std::uint64_t pixels = static_cast<std::uint64_t>(width) * height;
    const std::size_t maximum = std::vector<float>{}.max_size();
    if (pixels > std::numeric_limits<std::size_t>::max() / 2U ||
        pixels > static_cast<std::uint64_t>(maximum / 2U))
    {
        return make_error(ErrorCode::kValidation,
                          "Recursive Gaussian dimensions exceed the two-channel buffer limit",
                          {{"reason", "recursive_gaussian_dimensions_overflow"}});
    }
    return static_cast<std::size_t>(pixels) * 2U;
}

class RecursiveGaussianZeroTwoChannel
{
public:
    RecursiveGaussianZeroTwoChannel(std::vector<float> signal, const std::uint32_t width,
                                    const std::uint32_t height,
                                    const RecursiveGaussianCoefficients coefficients)
        : signal_(std::move(signal))
        , width_(width)
        , height_(height)
        , coefficients_(coefficients)
    {
    }

    [[nodiscard]] Result<std::vector<float>> run(const CancellationToken &cancellation,
                                                 const RecursiveGaussianControl control)
    {
        scratch_.resize(signal_.size());

        // Vertical blur column by column.  The initialization and forward /
        // backward recurrence deliberately match legacy/src/common/gaussian.c.
        for (std::uint32_t column = 0U; column < width_; ++column)
        {
            checkpoint(control, RecursiveGaussianCheckpoint::kVerticalColumn, column);
            auto active = cancellation.check();
            if (!active)
            {
                return active.error();
            }

            float xp[2]{};
            float yb[2]{};
            float yp[2]{};
            for (std::size_t channel = 0U; channel < 2U; ++channel)
            {
                xp[channel] =
                    frozen_clamp(signal_[static_cast<std::size_t>(column) * 2U + channel]);
                yb[channel] = xp[channel] * coefficients_.coefp;
                yp[channel] = yb[channel];
            }

            float xc[2]{};
            float yc[2]{};
            float xn[2]{};
            float xa[2]{};
            float yn[2]{};
            float ya[2]{};
            for (std::uint32_t row = 0U; row < height_; ++row)
            {
                if ((row & 63U) == 0U)
                {
                    checkpoint(control, RecursiveGaussianCheckpoint::kVerticalRow, row);
                    active = cancellation.check();
                    if (!active)
                    {
                        return active.error();
                    }
                }
                const std::size_t offset = (static_cast<std::size_t>(row) * width_ + column) * 2U;
                for (std::size_t channel = 0U; channel < 2U; ++channel)
                {
                    xc[channel] = frozen_clamp(signal_[offset + channel]);
                    yc[channel] =
                        (coefficients_.a0 * xc[channel]) + (coefficients_.a1 * xp[channel]) -
                        (coefficients_.b1 * yp[channel]) - (coefficients_.b2 * yb[channel]);

                    scratch_[offset + channel] = yc[channel];

                    xp[channel] = xc[channel];
                    yb[channel] = yp[channel];
                    yp[channel] = yc[channel];
                }
            }

            const std::size_t last_offset =
                (static_cast<std::size_t>(height_ - 1U) * width_ + column) * 2U;
            for (std::size_t channel = 0U; channel < 2U; ++channel)
            {
                xn[channel] = frozen_clamp(signal_[last_offset + channel]);
                xa[channel] = xn[channel];
                yn[channel] = xn[channel] * coefficients_.coefn;
                ya[channel] = yn[channel];
            }

            for (std::uint32_t row = height_; row > 0U; --row)
            {
                if (((row - 1U) & 63U) == 0U)
                {
                    checkpoint(control, RecursiveGaussianCheckpoint::kVerticalRow, row - 1U);
                    active = cancellation.check();
                    if (!active)
                    {
                        return active.error();
                    }
                }
                const std::size_t offset =
                    (static_cast<std::size_t>(row - 1U) * width_ + column) * 2U;
                for (std::size_t channel = 0U; channel < 2U; ++channel)
                {
                    xc[channel] = frozen_clamp(signal_[offset + channel]);

                    yc[channel] =
                        (coefficients_.a2 * xn[channel]) + (coefficients_.a3 * xa[channel]) -
                        (coefficients_.b1 * yn[channel]) - (coefficients_.b2 * ya[channel]);

                    xa[channel] = xn[channel];
                    xn[channel] = xc[channel];
                    ya[channel] = yn[channel];
                    yn[channel] = yc[channel];

                    scratch_[offset + channel] += yc[channel];
                }
            }
        }

        // Horizontal blur line by line, publishing only into our owned signal.
        for (std::uint32_t row = 0U; row < height_; ++row)
        {
            checkpoint(control, RecursiveGaussianCheckpoint::kHorizontalRow, row);
            auto active = cancellation.check();
            if (!active)
            {
                return active.error();
            }

            float xp[2]{};
            float yb[2]{};
            float yp[2]{};
            const std::size_t first_offset = static_cast<std::size_t>(row) * width_ * 2U;
            for (std::size_t channel = 0U; channel < 2U; ++channel)
            {
                xp[channel] = frozen_clamp(scratch_[first_offset + channel]);
                yb[channel] = xp[channel] * coefficients_.coefp;
                yp[channel] = yb[channel];
            }

            float xc[2]{};
            float yc[2]{};
            float xn[2]{};
            float xa[2]{};
            float yn[2]{};
            float ya[2]{};
            for (std::uint32_t column = 0U; column < width_; ++column)
            {
                if ((column & 63U) == 0U)
                {
                    checkpoint(control, RecursiveGaussianCheckpoint::kHorizontalColumn, column);
                    active = cancellation.check();
                    if (!active)
                    {
                        return active.error();
                    }
                }
                const std::size_t offset = (static_cast<std::size_t>(row) * width_ + column) * 2U;
                for (std::size_t channel = 0U; channel < 2U; ++channel)
                {
                    xc[channel] = frozen_clamp(scratch_[offset + channel]);
                    yc[channel] =
                        (coefficients_.a0 * xc[channel]) + (coefficients_.a1 * xp[channel]) -
                        (coefficients_.b1 * yp[channel]) - (coefficients_.b2 * yb[channel]);

                    signal_[offset + channel] = yc[channel];

                    xp[channel] = xc[channel];
                    yb[channel] = yp[channel];
                    yp[channel] = yc[channel];
                }
            }

            const std::size_t last_offset = (static_cast<std::size_t>(row + 1U) * width_ - 1U) * 2U;
            for (std::size_t channel = 0U; channel < 2U; ++channel)
            {
                xn[channel] = frozen_clamp(scratch_[last_offset + channel]);
                xa[channel] = xn[channel];
                yn[channel] = xn[channel] * coefficients_.coefn;
                ya[channel] = yn[channel];
            }

            for (std::uint32_t column = width_; column > 0U; --column)
            {
                if (((column - 1U) & 63U) == 0U)
                {
                    checkpoint(control, RecursiveGaussianCheckpoint::kHorizontalColumn,
                               column - 1U);
                    active = cancellation.check();
                    if (!active)
                    {
                        return active.error();
                    }
                }
                const std::size_t offset =
                    (static_cast<std::size_t>(row) * width_ + (column - 1U)) * 2U;
                for (std::size_t channel = 0U; channel < 2U; ++channel)
                {
                    xc[channel] = frozen_clamp(scratch_[offset + channel]);

                    yc[channel] =
                        (coefficients_.a2 * xn[channel]) + (coefficients_.a3 * xa[channel]) -
                        (coefficients_.b1 * yn[channel]) - (coefficients_.b2 * ya[channel]);

                    xa[channel] = xn[channel];
                    xn[channel] = xc[channel];
                    ya[channel] = yn[channel];
                    yn[channel] = yc[channel];

                    signal_[offset + channel] += yc[channel];
                }
            }
        }

        for (std::uint32_t row = 0U; row < height_; ++row)
        {
            auto active = cancellation.check();
            if (!active)
            {
                return active.error();
            }
            const std::size_t begin = static_cast<std::size_t>(row) * width_ * 2U;
            const std::size_t end = begin + static_cast<std::size_t>(width_) * 2U;
            for (std::size_t index = begin; index < end; ++index)
            {
                if (!std::isfinite(signal_[index]))
                {
                    return make_error(ErrorCode::kValidation,
                                      "Recursive Gaussian produced a non-finite correction",
                                      {{"reason", "nonfinite_recursive_gaussian_output"},
                                       {"sample_index", std::to_string(index)}});
                }
            }
        }
        checkpoint(control, RecursiveGaussianCheckpoint::kBeforePublication, height_);
        auto active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
        return std::move(signal_);
    }

private:
    std::vector<float> signal_;
    std::vector<float> scratch_;
    std::uint32_t width_ = 0U;
    std::uint32_t height_ = 0U;
    RecursiveGaussianCoefficients coefficients_;
};

} // namespace

std::uint64_t recursive_gaussian_zero_2c_bytes(const std::uint32_t width,
                                               const std::uint32_t height) noexcept
{
    const std::uint64_t pixels = static_cast<std::uint64_t>(width) * height;
    return saturating_multiply(saturating_multiply(pixels, 2U * sizeof(float)), 2U);
}

Result<std::vector<float>> recursive_gaussian_zero_2c(std::vector<float> signal,
                                                      const std::uint32_t width,
                                                      const std::uint32_t height, const float sigma,
                                                      const CancellationToken &cancellation,
                                                      const RecursiveGaussianControl control)
try
{
    checkpoint(control, RecursiveGaussianCheckpoint::kBeforeValidation, 0U);
    auto active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    auto expected = element_count(width, height);
    if (!expected)
    {
        return expected.error();
    }
    if (signal.size() != expected.value())
    {
        return make_error(ErrorCode::kValidation,
                          "Recursive Gaussian signal does not match its dimensions",
                          {{"reason", "invalid_recursive_gaussian_buffer"}});
    }
    for (std::uint32_t row = 0U; row < height; ++row)
    {
        active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
        const std::size_t begin = static_cast<std::size_t>(row) * width * 2U;
        const std::size_t end = begin + static_cast<std::size_t>(width) * 2U;
        for (std::size_t index = begin; index < end; ++index)
        {
            if (!std::isfinite(signal[index]))
            {
                return make_error(ErrorCode::kValidation,
                                  "Recursive Gaussian signal contains a non-finite correction",
                                  {{"reason", "nonfinite_recursive_gaussian_signal"},
                                   {"sample_index", std::to_string(index)}});
            }
        }
    }
    auto params = coefficients(sigma);
    if (!params)
    {
        return params.error();
    }
    checkpoint(control, RecursiveGaussianCheckpoint::kBeforeAllocation, 0U);
    active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    RecursiveGaussianZeroTwoChannel owner(std::move(signal), width, height, params.value());
    return owner.run(cancellation, control);
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Recursive Gaussian allocation failed",
                      {{"reason", "allocation_failed"}});
}

} // namespace ravo::detail
