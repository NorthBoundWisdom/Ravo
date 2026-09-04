#include "studio_iosurface_snapshot.h"

#import <IOSurface/IOSurface.h>
#import <Foundation/Foundation.h>

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


Result<std::uint64_t> create_iosurface_rgba8(const std::uint32_t width, const std::uint32_t height)
{
    if (width == 0U || height == 0U ||
        width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        height > static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
    {
        return make_error(ErrorCode::kValidation, "GPU presentation surface dimensions are invalid",
                          {{"reason", "invalid_gpu_presentation_surface"},
                           {"width", std::to_string(width)},
                           {"height", std::to_string(height)}});
    }
    constexpr std::uint32_t kRowAlign = 16U;
    const std::uint64_t raw_row = static_cast<std::uint64_t>(width) * 4U;
    const std::uint32_t bytes_per_row =
        static_cast<std::uint32_t>(((raw_row + kRowAlign - 1U) / kRowAlign) * kRowAlign);
    const NSDictionary *properties = @{
        (id)kIOSurfaceWidth : @(width),
        (id)kIOSurfaceHeight : @(height),
        (id)kIOSurfaceBytesPerElement : @4,
        (id)kIOSurfaceBytesPerRow : @(bytes_per_row),
        (id)kIOSurfacePixelFormat : @(static_cast<unsigned int>('RGBA')),
    };
    IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef)properties);
    if (surface == nullptr)
    {
        return make_error(ErrorCode::kIo, "GPU presentation surface allocation failed",
                          {{"reason", "allocation_failed"}});
    }
    return reinterpret_cast<std::uint64_t>(surface);
}

void release_iosurface(const std::uint64_t surface)
{
    if (surface == 0U)
    {
        return;
    }
    CFRelease(reinterpret_cast<IOSurfaceRef>(surface));
}

Result<void> write_rgb8_to_iosurface(const std::uint64_t surface, const QImage &rgb8_in)
try
{
    IOSurfaceRef io_surface = reinterpret_cast<IOSurfaceRef>(surface);
    if (io_surface == nullptr || rgb8_in.isNull() || rgb8_in.width() <= 0 || rgb8_in.height() <= 0)
    {
        return make_error(ErrorCode::kValidation, "GPU presentation write inputs are invalid",
                          {{"reason", "invalid_gpu_presentation_write"}});
    }
    QImage rgb8 = rgb8_in;
    if (rgb8.format() != QImage::Format_RGB888)
        rgb8 = rgb8.convertToFormat(QImage::Format_RGB888);
    const auto width = static_cast<std::uint32_t>(rgb8.width());
    const auto height = static_cast<std::uint32_t>(rgb8.height());
    if (IOSurfaceGetWidth(io_surface) != width || IOSurfaceGetHeight(io_surface) != height ||
        IOSurfaceGetBytesPerElement(io_surface) < 4U ||
        IOSurfaceGetBytesPerRow(io_surface) < static_cast<std::size_t>(width) * 4U)
    {
        return make_error(ErrorCode::kValidation, "GPU presentation surface layout does not match",
                          {{"reason", "invalid_gpu_presentation_surface"}});
    }
    if (IOSurfaceLock(io_surface, 0, nullptr) != KERN_SUCCESS)
    {
        return make_error(ErrorCode::kIo, "GPU presentation surface could not be locked",
                          {{"reason", "gpu_presentation_write_failed"}});
    }
    auto *base = static_cast<std::uint8_t *>(IOSurfaceGetBaseAddress(io_surface));
    if (base == nullptr)
    {
        static_cast<void>(IOSurfaceUnlock(io_surface, 0, nullptr));
        return make_error(ErrorCode::kIo, "GPU presentation surface has no writable storage",
                          {{"reason", "gpu_presentation_write_failed"}});
    }
    const std::size_t destination_stride = IOSurfaceGetBytesPerRow(io_surface);
    for (std::uint32_t row = 0U; row < height; ++row)
    {
        const auto *source = rgb8.constScanLine(static_cast<int>(row));
        auto *destination = base + static_cast<std::size_t>(row) * destination_stride;
        for (std::uint32_t column = 0U; column < width; ++column)
        {
            const std::size_t source_index = static_cast<std::size_t>(column) * 3U;
            const std::size_t destination_index = static_cast<std::size_t>(column) * 4U;
            destination[destination_index] = source[source_index];
            destination[destination_index + 1U] = source[source_index + 1U];
            destination[destination_index + 2U] = source[source_index + 2U];
            destination[destination_index + 3U] = 255U;
        }
    }
    static_cast<void>(IOSurfaceUnlock(io_surface, 0, nullptr));
    return {};
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "GPU presentation write allocation failed",
                      {{"reason", "allocation_failed"}});
}

} // namespace ravo::studio_metal
