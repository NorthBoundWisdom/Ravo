#include "studio_import_layout_smoke.h"

#include <array>
#include <QEventLoop>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>
#include "ravo/foundation/log.h"

namespace ravo
{
bool smoke_import_layout(QQmlApplicationEngine &engine)
{
    if (engine.rootObjects().isEmpty())
        return false;
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().front());
    auto *workspace =
        window ? window->findChild<QQuickItem *>(QStringLiteral("importWorkspace")) : nullptr;
    if (!workspace)
        return false;
    workspace->setVisible(true);
    for (const auto &size :
         std::array<QSize, 3>{QSize{1440, 900}, QSize{1024, 640}, QSize{640, 480}})
    {
        window->resize(size);
        window->show();
        QEventLoop settle;
        QTimer::singleShot(80, &settle, &QEventLoop::quit);
        settle.exec();
        const auto *button =
            workspace->findChild<QQuickItem *>(QStringLiteral("importConfirmButton"));
        const auto *grid = workspace->findChild<QQuickItem *>(QStringLiteral("importPhotoGrid"));
        if (!button || !grid || button->width() <= 0 || grid->width() < 120 || grid->height() <= 0)
        {
            LOG_ERROR(logger(), "Import layout has no usable grid or action at {}x{}", size.width(),
                      size.height());
            return false;
        }
        for (auto *item : workspace->findChildren<QQuickItem *>())
        {
            if (!item->isVisible() ||
                (item->objectName() != QStringLiteral("importConfirmButton") &&
                 item->objectName() != QStringLiteral("importPhotoGrid") &&
                 item->objectName() != QStringLiteral("importSourcePanel") &&
                 item->objectName() != QStringLiteral("importDestinationPanel")))
                continue;
            const auto rect =
                item->mapRectToItem(workspace, QRectF(0, 0, item->width(), item->height()));
            if (rect.left() < -1 || rect.top() < -1 || rect.right() > workspace->width() + 1 ||
                rect.bottom() > workspace->height() + 1)
            {
                LOG_ERROR(logger(), "Import layout overflow: {} at {}x{}",
                          item->objectName().toStdString(), size.width(), size.height());
                return false;
            }
        }
    }
    return true;
}
} // namespace ravo
