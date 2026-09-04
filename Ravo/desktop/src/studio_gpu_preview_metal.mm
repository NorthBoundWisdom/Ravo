#include "studio_gpu_preview_metal.h"

#include <QQuickWindow>
#include <QSGTexture>
#include <QSize>
#include <QtQuick/qsgtexture_platform.h>
#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>

#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>

#include <CoreFoundation/CoreFoundation.h>

namespace ravo::studio_metal
{
namespace
{

[[nodiscard]] void *quick_metal_device(QQuickWindow *window)
{
    if (window == nullptr)
    {
        return nullptr;
    }
    QRhi *rhi = window->rhi();
    if (rhi == nullptr || rhi->backend() != QRhi::Metal)
    {
        return nullptr;
    }
    const auto *metal = static_cast<const QRhiMetalNativeHandles *>(rhi->nativeHandles());
    return metal != nullptr ? metal->dev : nullptr;
}

} // namespace

QSGTexture *scene_texture_from_iosurface(QQuickWindow *window, const std::uint64_t surface,
                                         const int width, const int height, void **owned_native)
{
    if (owned_native != nullptr)
    {
        *owned_native = nullptr;
    }
    id<MTLDevice> device = (__bridge id<MTLDevice>)quick_metal_device(window);
    IOSurfaceRef io_surface = reinterpret_cast<IOSurfaceRef>(surface);
    if (window == nullptr || device == nil || io_surface == nullptr || width <= 0 || height <= 0)
    {
        return nullptr;
    }
    MTLTextureDescriptor *descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:static_cast<NSUInteger>(width)
                                                          height:static_cast<NSUInteger>(height)
                                                       mipmapped:NO];
    descriptor.usage = MTLTextureUsageShaderRead;
    descriptor.storageMode = MTLStorageModeShared;
    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor
                                                    iosurface:io_surface
                                                        plane:0];
    if (texture == nil)
    {
        return nullptr;
    }
    QSGTexture *sg_texture =
        QNativeInterface::QSGMetalTexture::fromNative(texture, window, QSize(width, height));
    if (sg_texture == nullptr)
    {
        [texture release];
        return nullptr;
    }
    if (owned_native != nullptr)
    {
        *owned_native = (__bridge void *)texture;
    }
    else
    {
        [texture release];
    }
    return sg_texture;
}

void release_texture(void *texture)
{
    if (texture != nullptr)
    {
        CFRelease(texture);
    }
}

} // namespace ravo::studio_metal
