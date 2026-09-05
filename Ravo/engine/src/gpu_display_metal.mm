#include "gpu_display_metal.h"

#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>

#include <CoreFoundation/CoreFoundation.h>
#include <cstdint>

namespace ravo::gpu_metal
{

void *create_iosurface(const std::uint32_t width, const std::uint32_t height)
{
    if (width == 0U || height == 0U)
    {
        return nullptr;
    }
    // Metal rejects IOSurface textures whose bytesPerRow is not 16-byte aligned
    // (odd preview widths such as 639 hit this on CI).
    constexpr std::uint32_t kRowAlign = 16U;
    const std::uint64_t raw_row = static_cast<std::uint64_t>(width) * 4U;
    const std::uint32_t bytes_per_row = static_cast<std::uint32_t>(
        ((raw_row + kRowAlign - 1U) / kRowAlign) * kRowAlign);
    const NSDictionary *properties = @{
        (id)kIOSurfaceWidth : @(width),
        (id)kIOSurfaceHeight : @(height),
        (id)kIOSurfaceBytesPerElement : @4,
        (id)kIOSurfaceBytesPerRow : @(bytes_per_row),
        (id)kIOSurfacePixelFormat : @(static_cast<unsigned int>('RGBA')),
    };
    return IOSurfaceCreate((__bridge CFDictionaryRef)properties);
}

void release_iosurface(void *surface)
{
    if (surface != nullptr)
    {
        CFRelease(surface);
    }
}

void *texture_from_iosurface(void *metal_device, void *surface, const std::uint32_t width,
                             const std::uint32_t height)
{
    id<MTLDevice> device = (__bridge id<MTLDevice>)metal_device;
    IOSurfaceRef io_surface = static_cast<IOSurfaceRef>(surface);
    if (device == nil || io_surface == nullptr || width == 0U || height == 0U)
    {
        return nullptr;
    }
    MTLTextureDescriptor *descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:width
                                                          height:height
                                                       mipmapped:NO];
    descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    descriptor.storageMode = MTLStorageModeShared;
    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor
                                                    iosurface:io_surface
                                                        plane:0];
    return (__bridge void *)texture;
}

void release_texture(void *texture)
{
    if (texture != nullptr)
    {
        CFRelease(texture);
    }
}

bool blit_texture(void *metal_device, void *metal_queue, void *source, void *destination,
                  const std::uint32_t width, const std::uint32_t height)
{
    return blit_texture_crop(metal_device, metal_queue, source, destination, 0U, 0U, width, height);
}

bool blit_texture_crop(void *metal_device, void *metal_queue, void *source, void *destination,
                       const std::uint32_t src_origin_x, const std::uint32_t src_origin_y,
                       const std::uint32_t width, const std::uint32_t height)
{
    id<MTLDevice> device = (__bridge id<MTLDevice>)metal_device;
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)metal_queue;
    id<MTLTexture> src = (__bridge id<MTLTexture>)source;
    id<MTLTexture> dst = (__bridge id<MTLTexture>)destination;
    if (device == nil || queue == nil || src == nil || dst == nil || width == 0U || height == 0U)
    {
        return false;
    }
    if (static_cast<std::uint32_t>(src.width) < src_origin_x + width ||
        static_cast<std::uint32_t>(src.height) < src_origin_y + height ||
        static_cast<std::uint32_t>(dst.width) < width ||
        static_cast<std::uint32_t>(dst.height) < height)
    {
        return false;
    }
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
    if (command == nil || blit == nil)
    {
        return false;
    }
    const MTLOrigin src_origin = MTLOriginMake(src_origin_x, src_origin_y, 0);
    const MTLOrigin dst_origin = MTLOriginMake(0, 0, 0);
    const MTLSize size = MTLSizeMake(width, height, 1);
    [blit copyFromTexture:src
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:src_origin
               sourceSize:size
                toTexture:dst
         destinationSlice:0
         destinationLevel:0
        destinationOrigin:dst_origin];
    [blit endEncoding];
    [command commit];
    [command waitUntilCompleted];
    return command.error == nil;
}

} // namespace ravo::gpu_metal
