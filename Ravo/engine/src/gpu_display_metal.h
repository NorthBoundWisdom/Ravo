#pragma once

#include <cstdint>

namespace ravo::gpu_metal
{

[[nodiscard]] void *create_iosurface(std::uint32_t width, std::uint32_t height);
void release_iosurface(void *surface);
[[nodiscard]] void *texture_from_iosurface(void *metal_device, void *surface, std::uint32_t width,
                                           std::uint32_t height);
void release_texture(void *texture);
[[nodiscard]] bool blit_texture(void *metal_device, void *metal_queue, void *source,
                                void *destination, std::uint32_t width, std::uint32_t height);
[[nodiscard]] bool blit_texture_crop(void *metal_device, void *metal_queue, void *source,
                                     void *destination, std::uint32_t src_origin_x,
                                     std::uint32_t src_origin_y, std::uint32_t width,
                                     std::uint32_t height);

} // namespace ravo::gpu_metal
