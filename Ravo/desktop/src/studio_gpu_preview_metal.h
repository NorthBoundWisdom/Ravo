#pragma once

#include <cstdint>

class QQuickWindow;
class QSGTexture;

namespace ravo::studio_metal
{

[[nodiscard]] QSGTexture *scene_texture_from_iosurface(QQuickWindow *window, std::uint64_t surface,
                                                       int width, int height, void **owned_native);
void release_texture(void *texture);

} // namespace ravo::studio_metal
