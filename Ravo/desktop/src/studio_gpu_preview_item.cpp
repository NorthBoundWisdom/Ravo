#include "studio_gpu_preview_item.h"

#include <QSGSimpleTextureNode>
#include <QSGTexture>
#include <QQuickWindow>

#if defined(Q_OS_MACOS)
#include "studio_gpu_preview_metal.h"
#endif

namespace ravo
{

StudioGpuPreviewItem::StudioGpuPreviewItem(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
}

StudioGpuPreviewItem::~StudioGpuPreviewItem()
{
    release_owned_texture();
}

void StudioGpuPreviewItem::release_owned_texture()
{
    void *native = owned_native_texture_;
    owned_native_texture_ = nullptr;
#if defined(Q_OS_MACOS)
    studio_metal::release_texture(native);
#else
    static_cast<void>(native);
#endif
}

void StudioGpuPreviewItem::releaseResources()
{
    release_owned_texture();
    uploaded_surface_ = 0;
    uploaded_generation_ = 0;
    uploaded_width_ = 0;
    uploaded_height_ = 0;
    QQuickItem::releaseResources();
}

quint64 StudioGpuPreviewItem::generation() const noexcept
{
    return generation_;
}

void StudioGpuPreviewItem::setGeneration(const quint64 generation)
{
    if (generation_ == generation)
    {
        return;
    }
    generation_ = generation;
    emit generationChanged();
    update();
}

quint64 StudioGpuPreviewItem::nativeSurface() const noexcept
{
    return native_surface_;
}

void StudioGpuPreviewItem::setNativeSurface(const quint64 surface)
{
    if (native_surface_ == surface)
    {
        return;
    }
    native_surface_ = surface;
    uploaded_surface_ = 0;
    emit nativeSurfaceChanged();
    update();
}

int StudioGpuPreviewItem::sourceWidth() const noexcept
{
    return source_width_;
}

void StudioGpuPreviewItem::setSourceWidth(const int width)
{
    if (source_width_ == width)
    {
        return;
    }
    source_width_ = width;
    uploaded_surface_ = 0;
    emit sourceSizeChanged();
    update();
}

int StudioGpuPreviewItem::sourceHeight() const noexcept
{
    return source_height_;
}

void StudioGpuPreviewItem::setSourceHeight(const int height)
{
    if (source_height_ == height)
    {
        return;
    }
    source_height_ = height;
    uploaded_surface_ = 0;
    emit sourceSizeChanged();
    update();
}

bool StudioGpuPreviewItem::smooth() const noexcept
{
    return smooth_;
}

void StudioGpuPreviewItem::setSmooth(const bool smooth)
{
    if (smooth_ == smooth)
    {
        return;
    }
    smooth_ = smooth;
    emit smoothChanged();
    update();
}

QSGNode *StudioGpuPreviewItem::updatePaintNode(QSGNode *old_node, UpdatePaintNodeData *)
{
#if defined(Q_OS_MACOS)
    auto *node = static_cast<QSGSimpleTextureNode *>(old_node);
    if (native_surface_ == 0 || source_width_ <= 0 || source_height_ <= 0 || generation_ == 0)
    {
        delete node;
        release_owned_texture();
        uploaded_surface_ = 0;
        uploaded_generation_ = 0;
        uploaded_width_ = 0;
        uploaded_height_ = 0;
        return nullptr;
    }
    if (node == nullptr)
    {
        node = new QSGSimpleTextureNode();
        node->setOwnsTexture(true);
    }
    node->setRect(boundingRect());
    node->setFiltering(smooth_ ? QSGTexture::Linear : QSGTexture::Nearest);
    if (node->texture() != nullptr && owned_native_texture_ != nullptr &&
        uploaded_surface_ == native_surface_ && uploaded_generation_ == generation_ &&
        uploaded_width_ == source_width_ && uploaded_height_ == source_height_)
    {
        node->markDirty(QSGNode::DirtyMaterial);
        return node;
    }
    void *owned = nullptr;
    QSGTexture *sg_texture = studio_metal::scene_texture_from_iosurface(
        window(), native_surface_, source_width_, source_height_, &owned);
    if (sg_texture == nullptr)
    {
        delete node;
        release_owned_texture();
        uploaded_surface_ = 0;
        uploaded_generation_ = 0;
        return nullptr;
    }
    node->setTexture(sg_texture);
    release_owned_texture();
    owned_native_texture_ = owned;
    uploaded_surface_ = native_surface_;
    uploaded_generation_ = generation_;
    uploaded_width_ = source_width_;
    uploaded_height_ = source_height_;
    return node;
#else
    static_cast<void>(old_node);
    return nullptr;
#endif
}

} // namespace ravo
