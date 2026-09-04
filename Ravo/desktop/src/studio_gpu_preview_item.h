#pragma once

#include <QQuickItem>
#include <QtQml/qqmlregistration.h>

namespace ravo
{

class StudioGpuPreviewItem : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(quint64 generation READ generation WRITE setGeneration NOTIFY generationChanged)
    Q_PROPERTY(
        quint64 nativeSurface READ nativeSurface WRITE setNativeSurface NOTIFY nativeSurfaceChanged)
    Q_PROPERTY(int sourceWidth READ sourceWidth WRITE setSourceWidth NOTIFY sourceSizeChanged)
    Q_PROPERTY(int sourceHeight READ sourceHeight WRITE setSourceHeight NOTIFY sourceSizeChanged)
    Q_PROPERTY(bool smooth READ smooth WRITE setSmooth NOTIFY smoothChanged)

public:
    explicit StudioGpuPreviewItem(QQuickItem *parent = nullptr);
    ~StudioGpuPreviewItem() override;

    [[nodiscard]] quint64 generation() const noexcept;
    void setGeneration(quint64 generation);
    [[nodiscard]] quint64 nativeSurface() const noexcept;
    void setNativeSurface(quint64 surface);
    [[nodiscard]] int sourceWidth() const noexcept;
    void setSourceWidth(int width);
    [[nodiscard]] int sourceHeight() const noexcept;
    void setSourceHeight(int height);
    [[nodiscard]] bool smooth() const noexcept;
    void setSmooth(bool smooth);

signals:
    void generationChanged();
    void nativeSurfaceChanged();
    void sourceSizeChanged();
    void smoothChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *old_node, UpdatePaintNodeData *) override;
    void releaseResources() override;

private:
    void release_owned_texture();

    quint64 generation_ = 0;
    quint64 native_surface_ = 0;
    int source_width_ = 0;
    int source_height_ = 0;
    bool smooth_ = true;
    quint64 uploaded_surface_ = 0;
    quint64 uploaded_generation_ = 0;
    int uploaded_width_ = 0;
    int uploaded_height_ = 0;
    void *owned_native_texture_ = nullptr;
};

} // namespace ravo
