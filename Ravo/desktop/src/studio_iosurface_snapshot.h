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

} // namespace ravo::studio_metal
