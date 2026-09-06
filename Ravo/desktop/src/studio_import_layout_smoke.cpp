#include "studio_import_layout_smoke.h"

#include <array>
#include <QEventLoop>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>
#include <QTemporaryDir>
#include <QDir>
#include <QElapsedTimer>
#include "ravo/foundation/log.h"
#include "ravo/desktop/studio_presenter.h"

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
    auto *presenter = qobject_cast<StudioPresenter *>(
        engine.rootContext()->contextProperty(QStringLiteral("studio")).value<QObject *>());
    if (!presenter)
        return false;
    QTemporaryDir directory;
    if (!directory.isValid() || !QDir().mkpath(directory.filePath(QStringLiteral("2026"))))
        return false;
    for (auto *model : {presenter->importSourceFolders(), presenter->importDestinationFolders()})
        model->resetWithRoots({{directory.path(), "Pictures", true}});
    if (!QQmlProperty::write(workspace, QStringLiteral("visible"), true))
        return false;
    auto *preview_section =
        workspace->findChild<QQuickItem *>(QStringLiteral("importDestinationPreviewSection"));
    auto *preview_tree =
        workspace->findChild<QQuickItem *>(QStringLiteral("importDestinationPreviewTree"));
    if (!preview_section || !preview_tree ||
        !QQmlProperty::write(preview_section, QStringLiteral("visible"), true) ||
        !QQmlProperty::write(
            preview_tree, QStringLiteral("model"),
            QVariantList{
                QVariantMap{{QStringLiteral("name"), QStringLiteral("2026")},
                            {QStringLiteral("path"), directory.filePath(QStringLiteral("2026"))},
                            {QStringLiteral("depth"), 1},
                            {QStringLiteral("photoCount"), 123},
                            {QStringLiteral("willCreate"), true},
                            {QStringLiteral("secondCopy"), false}}}))
        return false;
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
        QList<QQuickItem *> visual_items{workspace};
        for (qsizetype index = 0; index < visual_items.size(); ++index)
            visual_items.append(visual_items[index]->childItems());
        int disclosures = 0;
        for (auto *item : visual_items)
        {
            if (item->isVisible() && item->objectName() == QStringLiteral("importFolderExpand"))
                ++disclosures;
            if (item->isVisible() && item->objectName() == QStringLiteral("importFolderExpand") &&
                (item->width() < 16 || item->height() < 16))
            {
                LOG_ERROR(logger(), "Import disclosure hit area is unusable: {}x{}", item->width(),
                          item->height());
                return false;
            }
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
        if (size.width() >= 1000 && disclosures < 2)
        {
            for (auto *item : visual_items)
                if (item->objectName().startsWith(QStringLiteral("import")))
                    LOG_ERROR(logger(), "Import fixture item {} visible={} size={}x{} count={}",
                              item->objectName().toStdString(), item->isVisible(), item->width(),
                              item->height(), item->property("count").toInt());
            LOG_ERROR(logger(), "Import layout is missing disclosure controls: {}", disclosures);
            return false;
        }
        if (size.width() >= 1000)
        {
            const auto activate_control = [&](const QString &name)
            {
                auto *tree = workspace->findChild<QQuickItem *>(
                    QStringLiteral("importDestinationFolderTree"));
                if (!tree)
                    return false;
                QList<QQuickItem *> items{tree};
                for (qsizetype index = 0; index < items.size(); ++index)
                {
                    auto *item = items[index];
                    if (item->objectName() == name && item->isVisible())
                    {
                        if (name == QLatin1String("importFolderExpand"))
                            return QMetaObject::invokeMethod(item, "clicked", Qt::DirectConnection);
                        // Exercise the real row handler without operating a user's window.
                        void *event = nullptr;
                        return QMetaObject::invokeMethod(
                            item, "clicked", Qt::DirectConnection,
                            QGenericArgument("QQuickMouseEvent*", &event));
                    }
                    items.append(item->childItems());
                }
                return false;
            };
            auto *model = presenter->importDestinationFolders();
            const auto previous_selection = model->selectedPath();
            if (!activate_control(QStringLiteral("importFolderExpand")))
                return false;
            QElapsedTimer deadline;
            deadline.start();
            while (model->rowCount() < 2 && deadline.elapsed() < 3000)
            {
                QEventLoop listing;
                QTimer::singleShot(10, &listing, &QEventLoop::quit);
                listing.exec();
            }
            if (model->rowCount() != 2 || model->selectedPath() != previous_selection)
            {
                LOG_ERROR(logger(), "Destination disclosure did not expand its fixture folder");
                return false;
            }
            if (!activate_control(QStringLiteral("importFolderExpand")) || model->rowCount() != 1)
                return false;
            // Activating a loaded, collapsed row resets its model synchronously. Its
            // folderChosen intent must still reach the presenter after the delegate dies.
            if (!activate_control(QStringLiteral("importFolderChoose")) || model->rowCount() != 2 ||
                presenter->importDestination() != directory.path())
            {
                LOG_ERROR(logger(), "Folder choice was lost during delegate replacement");
                return false;
            }
            model->toggleCollapsed(directory.path());
            if (model->rowCount() != 1)
                return false;
        }
    }
    return true;
}
} // namespace ravo
