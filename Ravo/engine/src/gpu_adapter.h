#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
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

struct GpuLightControlsParams
{
    float highlight_ev = 0.0F;
    float shadow_ev = 0.0F;
    float white_ev = 0.0F;
    float black_ev = 0.0F;
};

struct GpuSharpenRgbParams
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t radius = 0;
    float amount = 0.5F;
    float threshold = 0.5F;
    std::array<float, 25> kernel{};
};

struct GpuRgbPass
{
    enum class Kind : std::uint8_t
    {
        kAffine = 0,
        kSigmoid = 1,
        kLightControls = 2,
        kSharpen = 3,
        kRapidRawBasicTone = 4,
    };
    Kind kind = Kind::kAffine;
    GpuAffineRgbParams affine;
    GpuSigmoidRgbParams sigmoid;
    GpuLightControlsParams light;
    GpuSharpenRgbParams sharpen;
};

struct GpuRgbApplyOptions
{
    bool from_retained_source = false;
    bool download = true;
    bool publish_display = false;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t display_slot = 0;
    std::string retained_key;
};

struct GpuDisplayFrame
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t generation = 0;
    std::string_view backend = {};
    std::uint64_t native_surface = 0;
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
    [[nodiscard]] Result<void> apply_affine_rgb(std::span<const float> input,
                                                std::span<float> output, float scale, float black,
                                                const CancellationToken &cancellation) const;
    [[nodiscard]] Result<void> apply_rgb_passes(std::span<const float> input,
                                                std::span<float> output,
                                                std::span<const GpuRgbPass> passes,
                                                const CancellationToken &cancellation) const;
    [[nodiscard]] Result<void> apply_rgb_passes(std::span<const float> input,
                                                std::span<float> output,
                                                std::span<const GpuRgbPass> passes,
                                                GpuRgbApplyOptions options,
                                                const CancellationToken &cancellation) const;
    [[nodiscard]] Result<void> demosaic_rcd(std::span<const float> cfa, std::span<float> rgb,
                                            std::uint32_t width, std::uint32_t height,
                                            std::array<std::uint8_t, 4> pattern,
                                            const CancellationToken &cancellation) const;
    [[nodiscard]] Result<void> demosaic_rcd(std::span<const float> cfa, std::span<float> rgb,
                                            std::uint32_t width, std::uint32_t height,
                                            std::array<std::uint8_t, 4> pattern,
                                            std::uint32_t crop_x, std::uint32_t crop_y,
                                            std::uint32_t crop_width, std::uint32_t crop_height,
                                            const CancellationToken &cancellation) const;
    [[nodiscard]] bool has_retained_source(std::uint32_t width,
                                           std::uint32_t height) const noexcept;
    [[nodiscard]] std::string_view retained_source_key() const noexcept;
    [[nodiscard]] Result<void> retain_source_rgb(std::span<const float> rgb, std::uint32_t width,
                                                 std::uint32_t height,
                                                 const CancellationToken &cancellation,
                                                 std::string_view key = {}) const;
    [[nodiscard]] GpuDisplayFrame display_frame(std::uint32_t slot = 0) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit GpuAdapter(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace ravo
