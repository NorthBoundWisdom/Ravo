#include "gpu_adapter.h"

namespace ravo
{

struct GpuAdapter::Impl
{
};

GpuAdapter::GpuAdapter(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
GpuAdapter::~GpuAdapter() = default;
GpuAdapter::GpuAdapter(GpuAdapter &&) noexcept = default;
GpuAdapter &GpuAdapter::operator=(GpuAdapter &&) noexcept = default;

Result<std::shared_ptr<GpuAdapter>> GpuAdapter::try_create()
{
    return make_error(ErrorCode::kUnsupported, "GPU adapter is unavailable on this host",
                      {{"reason", "gpu_unavailable"}});
}

std::string_view GpuAdapter::backend_id() const noexcept
{
    return "unavailable";
}

Result<void> GpuAdapter::copy_rgb(std::span<const float>, std::span<float>,
                                  const CancellationToken &cancellation) const
{
    auto cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    return make_error(ErrorCode::kUnsupported, "GPU adapter is unavailable on this host",
                      {{"reason", "gpu_unavailable"}});
}

Result<void> GpuAdapter::apply_affine_rgb(std::span<const float>, std::span<float>, float, float,
                                          const CancellationToken &cancellation) const
{
    auto cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    return make_error(ErrorCode::kUnsupported, "GPU adapter is unavailable on this host",
                      {{"reason", "gpu_unavailable"}});
}

} // namespace ravo
