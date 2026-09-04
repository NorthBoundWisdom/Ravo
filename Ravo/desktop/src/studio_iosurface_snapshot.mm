#include "studio_iosurface_snapshot.h"

#import <IOSurface/IOSurface.h>

#include <mach/kern_return.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <string>

#include <QColorSpace>

namespace ravo::studio_metal
{
namespace
{

class SurfaceReadLock
{
public:
    explicit SurfaceReadLock(IOSurfaceRef surface) noexcept
        : surface_(surface)
        , locked_(IOSurfaceLock(surface_, kIOSurfaceLockReadOnly, nullptr) == KERN_SUCCESS)
    {
    }

    ~SurfaceReadLock()
    {
        if (locked_)
        {
            static_cast<void>(IOSurfaceUnlock(surface_, kIOSurfaceLockReadOnly, nullptr));
        }
    }

    SurfaceReadLock(const SurfaceReadLock &) = delete;
    SurfaceReadLock &operator=(const SurfaceReadLock &) = delete;

    [[nodiscard]] bool locked() const noexcept { return locked_; }

private:
    IOSurfaceRef surface_ = nullptr;
    bool locked_ = false;
};

} // namespace

Result<QImage> snapshot_iosurface_rgb8(const std::uint64_t surface, const std::uint32_t width,
                                       const std::uint32_t height)
try
{
    IOSurfaceRef io_surface = reinterpret_cast<IOSurfaceRef>(surface);
    if (io_surface == nullptr || width == 0U || height == 0U ||
        width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        height > static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
    {
        return make_error(ErrorCode::kValidation, "GPU preview surface dimensions are invalid",
                          {{"reason", "invalid_gpu_display_surface"},
                           {"width", std::to_string(width)},
                           {"height", std::to_string(height)}});
    }
    if (IOSurfaceGetWidth(io_surface) != width || IOSurfaceGetHeight(io_surface) != height ||
        IOSurfaceGetBytesPerElement(io_surface) < 4U ||
        IOSurfaceGetBytesPerRow(io_surface) < static_cast<std::size_t>(width) * 4U)
    {
        return make_error(ErrorCode::kValidation, "GPU preview surface layout does not match",
                          {{"reason", "invalid_gpu_display_surface"}});
    }

    SurfaceReadLock lock(io_surface);
    if (!lock.locked())
    {
        return make_error(ErrorCode::kIo, "GPU preview surface could not be locked",
                          {{"reason", "gpu_display_snapshot_failed"}});
    }
    const auto *base = static_cast<const std::uint8_t *>(IOSurfaceGetBaseAddress(io_surface));
    if (base == nullptr)
    {
        return make_error(ErrorCode::kIo, "GPU preview surface has no readable storage",
                          {{"reason", "gpu_display_snapshot_failed"}});
    }

    QImage image(static_cast<int>(width), static_cast<int>(height), QImage::Format_RGB888);
    if (image.isNull())
    {
        return make_error(ErrorCode::kIo, "GPU preview snapshot allocation failed",
                          {{"reason", "allocation_failed"}});
    }
    const std::size_t source_stride = IOSurfaceGetBytesPerRow(io_surface);
    for (std::uint32_t row = 0U; row < height; ++row)
    {
        const auto *source = base + static_cast<std::size_t>(row) * source_stride;
        auto *destination = image.scanLine(static_cast<int>(row));
        for (std::uint32_t column = 0U; column < width; ++column)
        {
            const std::size_t source_index = static_cast<std::size_t>(column) * 4U;
            const std::size_t destination_index = static_cast<std::size_t>(column) * 3U;
            destination[destination_index] = source[source_index];
            destination[destination_index + 1U] = source[source_index + 1U];
            destination[destination_index + 2U] = source[source_index + 2U];
        }
    }
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    return image;
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "GPU preview snapshot allocation failed",
                      {{"reason", "allocation_failed"}});
}

} // namespace ravo::studio_metal
