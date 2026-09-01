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

    QImage requestImage(const QString &id, QSize *size, const QSize &) override
    {
        const QImage image = id.startsWith(QLatin1String("before")) ?
                                 studio_->comparisonBeforeImage() :
                                 studio_->previewImage();
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

    QImage requestImage(const QString &id, QSize *size, const QSize &) override
    {
        const QImage image =
            id.startsWith(QLatin1String("waveform"))    ? studio_->scopeWaveformImage() :
            id.startsWith(QLatin1String("vectorscope")) ? studio_->scopeVectorscopeImage() :
            id.startsWith(QLatin1String("split"))       ? studio_->scopeSplitImage() :
                                                          studio_->scopeParadeImage();
        if (size != nullptr)
        {
            *size = image.size();
        }
        return image;
    }

private:
    StudioPresenter *studio_ = nullptr;
};

class ImportCandidateImageProvider final : public QQuickImageProvider
{
public:
    explicit ImportCandidateImageProvider(ImportCandidateListModel &model)
        : QQuickImageProvider(QQuickImageProvider::Image)
        , model_(&model)
    {
    }

    QImage requestImage(const QString &id, QSize *size, const QSize &) override
    {
        bool ok = false;
        const int row = id.section(QLatin1Char('?'), 0, 0).toInt(&ok);
        const QImage image = ok ? model_->thumbnail(row) : QImage{};
        if (size != nullptr)
            *size = image.size();
        return image;
    }

private:
    ImportCandidateListModel *model_ = nullptr;
};

} // namespace ravo
