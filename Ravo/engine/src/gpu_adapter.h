#pragma once

#include <memory>
#include <span>
#include <string_view>

#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/error.h"

namespace ravo
{

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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit GpuAdapter(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace ravo
