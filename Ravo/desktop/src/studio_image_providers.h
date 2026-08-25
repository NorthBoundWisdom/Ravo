#pragma once

#include <QImage>
#include <QQuickImageProvider>
#include <QSize>
#include <QString>

#include "ravo/desktop/studio_presenter.h"

namespace ravo
{

class StudioPreviewImageProvider final : public QQuickImageProvider
{
public:
    explicit StudioPreviewImageProvider(StudioPresenter &studio)
        : QQuickImageProvider(QQuickImageProvider::Image)
        , studio_(&studio)
    {
    }

    QImage requestImage(const QString &, QSize *size, const QSize &) override
    {
        const QImage image = studio_->previewImage();
        if (size != nullptr)
        {
            *size = image.size();
        }
        return image;
    }

private:
    StudioPresenter *studio_ = nullptr;
};

class StudioScopeImageProvider final : public QQuickImageProvider
{
public:
    explicit StudioScopeImageProvider(StudioPresenter &studio)
        : QQuickImageProvider(QQuickImageProvider::Image)
        , studio_(&studio)
    {
    }

    QImage requestImage(const QString &, QSize *size, const QSize &) override
    {
        const QImage image = studio_->scopeParadeImage();
        if (size != nullptr)
        {
            *size = image.size();
        }
        return image;
    }

private:
    StudioPresenter *studio_ = nullptr;
};

} // namespace ravo
