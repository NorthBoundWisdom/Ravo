#pragma once

#include <cstdint>

#include <QImage>

#include "ravo/foundation/error.h"

namespace ravo::studio_metal
{

// Copies the already-published display-sRGB IOSurface into owned RGB8 pixels.
// The caller must consume the snapshot immediately because Engine owns and
// double-buffers the surface token.
[[nodiscard]] Result<QImage> snapshot_iosurface_rgb8(std::uint64_t surface, std::uint32_t width,
                                                     std::uint32_t height);

// DISPLAY-01: Studio-owned RGBA8 IOSurface for QML after CPU monitor presentation.
// Create/release/write keep presentation pixels off the Engine double-buffer token.
[[nodiscard]] Result<std::uint64_t> create_iosurface_rgba8(std::uint32_t width,
                                                           std::uint32_t height);
void release_iosurface(std::uint64_t surface);

// Writes RGB888 (or converted) rows as opaque RGBA8 into an owned IOSurface.
[[nodiscard]] Result<void> write_rgb8_to_iosurface(std::uint64_t surface, const QImage &rgb8);

} // namespace ravo::studio_metal
