#include "studio_startup_controller.h"

#include <QFileInfo>
#include <utility>

#include "ravo/desktop/studio_presenter.h"

namespace ravo
{
StudioStartupController::StudioStartupController(StudioPresenter &presenter,
                                                 QString default_catalog_path, QObject *parent)
    : QObject(parent)
    , presenter_(presenter)
    , default_catalog_path_(std::move(default_catalog_path))
{
    // busyChanged(false) precedes the presenter's catalog/error publication.
    // Queue the handoff so the main window sees the complete result.
    connect(
        &presenter_, &StudioPresenter::busyChanged, this,
        [this]
        {
            if (started_ && !presenter_.busy())
                finish();
        },
        Qt::QueuedConnection);
}

void StudioStartupController::start()
{
    if (started_)
        return;
    started_ = true;
    if (presenter_.busy())
        return;
    if (presenter_.catalogOpen())
    {
        finish();
        return;
    }
    const QString explicit_path = presenter_.startupCatalogPath();
    if (!explicit_path.isEmpty())
        presenter_.openCatalogFromPath(explicit_path);
    else if (QFileInfo(default_catalog_path_).isFile())
        presenter_.openCatalogFromPath(default_catalog_path_);
    else
    {
        finish(true);
        return;
    }
    if (!presenter_.busy())
        finish();
}

void StudioStartupController::finish(const bool create_library)
{
    if (finished_)
        return;
    finished_ = true;
    emit finished(create_library);
}
} // namespace ravo
