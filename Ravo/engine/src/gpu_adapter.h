#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/error.h"

namespace ravo
{

struct GpuAffineRgbParams
{
    float scale = 1.0F;
    float black = 0.0F;
};

struct GpuSigmoidRgbParams
{
    std::uint32_t mode = 0;
    float white_target = 1.0F;
    float black_target = 0.000152F;
    float paper_exposure = 1.0F;
    float film_fog = 0.0F;
    float film_power = 1.0F;
    float paper_power = 1.0F;
    float hue_preservation = 1.0F;
};

struct GpuRgbPass
{
    enum class Kind : std::uint8_t
    {
        kAffine = 0,
        kSigmoid = 1,
    };
    Kind kind = Kind::kAffine;
    GpuAffineRgbParams affine;
    GpuSigmoidRgbParams sigmoid;
};

class GpuAdapter
{
public:
    [[nodiscard]] static Result<std::shared_ptr<GpuAdapter>> try_create();
    ~GpuAdapter();
    GpuAdapter(const GpuAdapter &) = delete;
    GpuAdapter &operator=(const GpuAdapter &) = delete;
    GpuAdapter(GpuAdapter &&) noexcept;
    GpuAdapter &operator=(GpuAdapter &&) noexcept;

    [[nodiscard]] std::string_view backend_id() const noexcept;
    [[nodiscard]] Result<void> copy_rgb(std::span<const float> input, std::span<float> output,
                                        const CancellationToken &cancellation) const;
    [[nodiscard]] Result<void> apply_affine_rgb(std::span<const float> input, std::span<float> output,
                                                float scale, float black,
                                                const CancellationToken &cancellation) const;
    [[nodiscard]] Result<void> apply_rgb_passes(std::span<const float> input, std::span<float> output,
                                                std::span<const GpuRgbPass> passes,
                                                const CancellationToken &cancellation) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit GpuAdapter(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace ravo
