#include "gpu_display_metal.h"

#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>

#include <CoreFoundation/CoreFoundation.h>

namespace ravo::gpu_metal
{

void *create_iosurface(const std::uint32_t width, const std::uint32_t height)
{
    if (width == 0U || height == 0U)
    {
        return nullptr;
    }
    const NSDictionary *properties = @{
        (id)kIOSurfaceWidth : @(width),
        (id)kIOSurfaceHeight : @(height),
        (id)kIOSurfaceBytesPerElement : @4,
        (id)kIOSurfaceBytesPerRow : @(static_cast<NSUInteger>(width) * 4U),
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
    id<MTLDevice> device = (__bridge id<MTLDevice>)metal_device;
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)metal_queue;
    id<MTLTexture> src = (__bridge id<MTLTexture>)source;
    id<MTLTexture> dst = (__bridge id<MTLTexture>)destination;
    if (device == nil || queue == nil || src == nil || dst == nil || width == 0U || height == 0U)
    {
        return false;
    }
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
    if (command == nil || blit == nil)
    {
        return false;
    }
    const MTLOrigin origin = MTLOriginMake(0, 0, 0);
    const MTLSize size = MTLSizeMake(width, height, 1);
    [blit copyFromTexture:src
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:origin
               sourceSize:size
                toTexture:dst
         destinationSlice:0
         destinationLevel:0
        destinationOrigin:origin];
    [blit endEncoding];
    [command commit];
    [command waitUntilCompleted];
    return command.error == nil;
}

} // namespace ravo::gpu_metal
