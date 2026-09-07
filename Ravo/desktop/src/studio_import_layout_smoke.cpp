#include "studio_import_layout_smoke.h"

#include <array>
#include <QEventLoop>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>
#include <QKeyEvent>
#include <QCoreApplication>
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
    ImportCandidate candidate;
    candidate.source_path = directory.filePath(QStringLiteral("photo.png")).toStdString();
    candidate.display_name = "photo.png";
    if (!QQmlProperty::write(workspace, QStringLiteral("visible"), true))
        return false;
    auto *menu_bar = window->findChild<QQuickItem *>(QStringLiteral("studioMenuBar"));
    if (!menu_bar)
        return false;
    const auto menu_height = menu_bar->height();
    // A native menu occupies no client area. Exercise that geometry transition
    // offscreen without hiding the menu object or dropping its commands.
    for (const qreal height : {qreal{0}, menu_height})
    {
        menu_bar->setHeight(height);
        QEventLoop layout;
        QTimer::singleShot(30, &layout, &QEventLoop::quit);
        layout.exec();
        const auto top = workspace->mapToItem(window->contentItem(), QPointF{}).y();
        if (qAbs(top - height) > 1)
        {
            LOG_ERROR(logger(), "Import content retains a menu inset: top={} menu={}", top, height);
            return false;
        }
    }
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
    qreal standard_tree_height = 0;
    for (const auto &size : std::array<QSize, 4>{QSize{1440, 900}, QSize{1440, 1100},
                                                 QSize{1024, 640}, QSize{640, 480}})
    {
        window->resize(size);
        window->show();
        QEventLoop settle;
        QTimer::singleShot(80, &settle, &QEventLoop::quit);
        settle.exec();
        // Seed after startup's queued catalog-close intent has drained.
        presenter->importCandidates()->setCandidates({candidate});
        QEventLoop candidate_layout;
        QTimer::singleShot(30, &candidate_layout, &QEventLoop::quit);
        candidate_layout.exec();
        const auto *button =
            workspace->findChild<QQuickItem *>(QStringLiteral("importConfirmButton"));
        const auto *grid = workspace->findChild<QQuickItem *>(QStringLiteral("importPhotoGrid"));
        if (!button || !grid || button->width() <= 0 || grid->width() < 120 || grid->height() <= 0)
        {
            LOG_ERROR(logger(), "Import layout has no usable grid or action at {}x{}", size.width(),
                      size.height());
            return false;
        }
        if (size.width() == 1440 && size.height() == 900)
        {
            auto *keyboard_grid =
                workspace->findChild<QQuickItem *>(QStringLiteral("importCandidateKeyboardGrid"));
            if (!keyboard_grid)
            {
                LOG_ERROR(logger(), "Import production keyboard grid missing");
                return false;
            }
            keyboard_grid->forceActiveFocus();
            QEventLoop focus_loop;
            QTimer::singleShot(20, &focus_loop, &QEventLoop::quit);
            focus_loop.exec();
            if (!keyboard_grid->hasActiveFocus())
            {
                LOG_ERROR(logger(), "Import production keyboard grid could not take focus");
                return false;
            }
            const int before = keyboard_grid->property("currentIndex").toInt();
            QKeyEvent press(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
            QKeyEvent release(QEvent::KeyRelease, Qt::Key_Right, Qt::NoModifier);
            auto *target = window->focusObject() ? window->focusObject() :
                                                   static_cast<QObject *>(keyboard_grid);
            QCoreApplication::sendEvent(target, &press);
            QCoreApplication::sendEvent(target, &release);
            QEventLoop key_loop;
            QTimer::singleShot(20, &key_loop, &QEventLoop::quit);
            key_loop.exec();
            if (keyboard_grid->property("currentIndex").toInt() == before &&
                presenter->importCandidates()->rowCount() > 1)
            {
                LOG_ERROR(logger(), "Import production window did not route Right key");
                return false;
            }
        }
        if (size.width() == 1440)
        {
            auto *surface =
                workspace->findChild<QQuickItem *>(QStringLiteral("importDestinationTreeSurface"));
            auto *tree =
                workspace->findChild<QQuickItem *>(QStringLiteral("importDestinationFolderTree"));
            if (!surface || !tree || surface->height() < surface->implicitHeight() ||
                !surface->boundingRect().contains(
                    tree->mapRectToItem(surface, tree->boundingRect())))
            {
                LOG_ERROR(logger(), "Destination tree must fit its adaptive background");
                return false;
            }
            if (size.height() == 900)
                standard_tree_height = surface->height();
            else if (surface->height() <= standard_tree_height + 50)
            {
                LOG_ERROR(logger(), "Destination tree did not grow with its panel: {} -> {}",
                          standard_tree_height, surface->height());
                return false;
            }
            if (size.height() == 1100)
            {
                auto *section =
                    workspace->findChild<QQuickItem *>(QStringLiteral("importDestinationSection"));
                if (!section || !section->parentItem())
                    return false;
                const auto settle_layout = []
                {
                    QEventLoop layout;
                    QTimer::singleShot(30, &layout, &QEventLoop::quit);
                    layout.exec();
                };
                QQmlProperty::write(section, QStringLiteral("expanded"), false);
                settle_layout();
                const bool collapsed_fits = qAbs(section->parentItem()->height() -
                                                 section->parentItem()->implicitHeight()) < 1;
                QQmlProperty::write(section, QStringLiteral("expanded"), true);
                presenter->setImportMode(QStringLiteral("add"));
                settle_layout();
                const bool add_fits = qAbs(section->parentItem()->height() -
                                           section->parentItem()->implicitHeight()) < 1;
                presenter->setImportMode(QStringLiteral("copy"));
                settle_layout();
                if (!collapsed_fits || !add_fits)
                {
                    LOG_ERROR(logger(),
                              "Hidden destination tree must not stretch the other settings");
                    return false;
                }
            }
        }
        QList<QQuickItem *> visual_items{workspace};
        for (qsizetype index = 0; index < visual_items.size(); ++index)
            visual_items.append(visual_items[index]->childItems());
        int disclosures = 0;
        int checkboxes = 0;
        int transfer_modes = 0;
        int transfer_segments = 0;
        for (auto *item : visual_items)
        {
            if (item->isVisible() &&
                item->objectName() == QStringLiteral("importSourceTreeSurface"))
            {
                auto *destination = workspace->findChild<QQuickItem *>(
                    QStringLiteral("importDestinationTreeSurface"));
                auto *tree =
                    workspace->findChild<QQuickItem *>(QStringLiteral("importSourceFolderTree"));
                if (!destination || !tree || item->height() <= 0 ||
                    !item->boundingRect().contains(tree->mapRectToItem(item, tree->boundingRect())))
                    return false;
                for (const auto *property : {"color", "border.color", "border.width", "radius"})
                    if (QQmlProperty::read(item, QString::fromLatin1(property)) !=
                        QQmlProperty::read(destination, QString::fromLatin1(property)))
                    {
                        LOG_ERROR(
                            logger(),
                            "Import folder trees must share their background and border style");
                        return false;
                    }
            }
            if (item->isVisible() &&
                item->objectName().startsWith(QStringLiteral("importTransferModeSegment")))
            {
                ++transfer_segments;
                auto *row = item->parentItem();
                const bool move =
                    item->objectName() == QStringLiteral("importTransferModeSegment2");
                if (!row || row->objectName() != QStringLiteral("importTransferMode") ||
                    qAbs(item->width() * 3 - row->width()) > 0.1 ||
                    item->height() != row->height() || item->isEnabled() == move)
                {
                    LOG_ERROR(logger(),
                              "Import mode segments must share equal width and keep Move disabled");
                    return false;
                }
            }
            if (item->isVisible() && item->objectName() == QStringLiteral("importTransferModes"))
            {
                ++transfer_modes;
                auto *panel = item->parentItem();
                if (!panel || panel->objectName() != QStringLiteral("importDestinationPanel") ||
                    !panel->boundingRect().contains(
                        item->mapRectToItem(panel, item->boundingRect())))
                {
                    LOG_ERROR(logger(),
                              "Import transfer modes must fit inside the destination panel");
                    return false;
                }
            }
            if (item->isVisible() &&
                item->objectName() == QStringLiteral("importCandidateCheckBox"))
            {
                ++checkboxes;
                auto *indicator = item->property("indicator").value<QQuickItem *>();
                if (item->width() < 32 || item->height() < 32 || !indicator ||
                    indicator->width() < 24 || indicator->height() < 24 ||
                    !item->boundingRect().contains(
                        indicator->mapRectToItem(item, indicator->boundingRect())))
                {
                    LOG_ERROR(logger(), "Import checkbox or its hit area is too small or clipped");
                    return false;
                }
            }
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
        if (size.width() >= 1000 && (transfer_modes != 1 || transfer_segments != 3))
        {
            LOG_ERROR(logger(), "Import destination panel is missing its transfer modes");
            return false;
        }
        if (checkboxes == 0)
        {
            LOG_ERROR(logger(), "Import checkbox fixture was not instantiated at {}x{}; rows={}",
                      size.width(), size.height(), presenter->importCandidates()->rowCount());
            return false;
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
                    auto *row = item;
                    while (row && !row->property("path").isValid())
                        row = row->parentItem();
                    // Model resets may leave a child delegate in the visual tree
                    // until the next polish. Always target the fixture root, not
                    // whichever disclosure happens to be visited first.
                    if (item->objectName() == name && item->isVisible() && item->isEnabled() &&
                        row && row->property("path").toString() == directory.path())
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
