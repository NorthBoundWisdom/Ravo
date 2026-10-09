#include "studio_import_layout_smoke.h"

#include <array>
#include <memory>
#include <QEventLoop>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QImage>
#include <QLocale>
#include <QColorSpace>
#include "ravo/desktop/import_candidate_list_model.h"
#include "ravo/desktop/studio_command_controller.h"
#include <QMetaObject>
#include <QKeySequence>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QClipboard>
#include <QTemporaryDir>
#include <QDir>
#include <QElapsedTimer>
#include <QAbstractItemModel>
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
    const auto *planning = workspace->findChild<QObject *>(QStringLiteral("importPlanningDialog"));
    if (!planning || !planning->property("modal").toBool() ||
        !workspace->findChild<QObject *>(QStringLiteral("importPlanningCancel")))
    {
        LOG_ERROR(logger(),
                  "Import planning must provide modal progress with explicit cancellation");
        return false;
    }
    auto *presenter = qobject_cast<StudioPresenter *>(
        engine.rootContext()->contextProperty(QStringLiteral("studio")).value<QObject *>());
    if (!presenter)
        return false;
    QTemporaryDir directory;
    if (!directory.isValid() || !QDir().mkpath(directory.filePath(QStringLiteral("2026"))))
        return false;
    for (auto *model : {presenter->imports()->importSourceFolders(),
                        presenter->imports()->importDestinationFolders()})
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
    if (workspace->findChild<QQuickItem *>(QStringLiteral("importDestinationPreviewSection")) ||
        workspace->findChild<QQuickItem *>(QStringLiteral("importDestinationPreviewTree")))
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
        presenter->imports()->importCandidates()->setCandidates({candidate});
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
            auto photo = candidate;
            photo.size_bytes = 1024;
            auto video = candidate;
            video.source_path = directory.filePath(QStringLiteral("video.mov")).toStdString();
            video.media_type = "video/quicktime";
            video.size_bytes = 2ULL * 1024 * 1024 * 1024;
            auto *summary_model = presenter->imports()->importCandidates();
            summary_model->setCandidates({photo, video});
            QCoreApplication::processEvents();
            const auto *summary =
                workspace->findChild<QQuickItem *>(QStringLiteral("importCandidateSummary"));
            const auto *checked =
                workspace->findChild<QQuickItem *>(QStringLiteral("importSelectedSummary"));
            const auto total =
                QCoreApplication::translate("ImportPage", "Total: %1 photos · %2 videos · %3")
                    .arg(1)
                    .arg(1)
                    .arg(QLocale().toString(2., 'f', 1) + " GiB");
            if (!summary || !checked || !summary->property("text").toString().contains(total))
            {
                LOG_ERROR(logger(), "Import summary does not display mixed media and GiB totals");
                return false;
            }
            summary_model->toggleSelected(1);
            QCoreApplication::processEvents();
            const auto selected =
                QCoreApplication::translate("ImportPage", "Selected: %1 photos · %2 videos · %3")
                    .arg(1)
                    .arg(0)
                    .arg(QLocale().toString(1., 'f', 1) + " KiB");
            if (checked->property("text").toString() != selected ||
                checked->property("truncated").toBool() ||
                !summary->property("text").toString().contains(total))
            {
                LOG_ERROR(logger(),
                          "Import checked totals are stale, clipped or change complete totals");
                return false;
            }
            summary_model->setCandidates({candidate});
            const auto *context_menu =
                workspace->findChild<QObject *>(QStringLiteral("importPhotoContextMenu"));
            const auto *reveal_item =
                workspace->findChild<QObject *>(QStringLiteral("importRevealMenuItem"));
            const auto *copy_item =
                workspace->findChild<QObject *>(QStringLiteral("importCopyInfoMenuItem"));
            if (!context_menu || !reveal_item || !copy_item ||
                !reveal_item->property("action").value<QObject *>() ||
                !copy_item->property("action").value<QObject *>())
            {
                LOG_ERROR(logger(), "Import context menu must bind its registered source commands");
                return false;
            }
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
                presenter->imports()->importCandidates()->rowCount() > 1)
            {
                LOG_ERROR(logger(), "Import production window did not route Right key");
                return false;
            }

            // Select All through production StudioCommandShortcuts / StudioActions wiring.
            // import_workspace_->openImportPage() requires an open catalog; workspace.visible alone is insufficient.
            // Keep Select All artifacts out of the layout destination fixture directory.
            QTemporaryDir select_all_dir;
            if (!select_all_dir.isValid())
            {
                LOG_ERROR(logger(), "Select All smoke temp directory unavailable");
                return false;
            }
            if (!presenter->catalogOpen())
            {
                presenter->createCatalogFromPath(
                    select_all_dir.filePath(QStringLiteral("select-all-library.sqlite")));
                QElapsedTimer catalog_timer;
                catalog_timer.start();
                while (!presenter->catalogOpen() || presenter->busy())
                {
                    if (catalog_timer.elapsed() > 30000)
                    {
                        LOG_ERROR(logger(), "Catalog open for Select All smoke timed out: {}",
                                  presenter->errorText().toStdString());
                        return false;
                    }
                    QEventLoop wait_catalog;
                    QTimer::singleShot(20, &wait_catalog, &QEventLoop::quit);
                    wait_catalog.exec();
                }
            }
            const QString source = select_all_dir.filePath(QStringLiteral("select-all-source"));
            if (!QDir().mkpath(source))
            {
                LOG_ERROR(logger(), "Failed to create Select All smoke source directory");
                return false;
            }
            {
                QImage seed(8, 8, QImage::Format_RGB888);
                seed.fill(Qt::darkCyan);
                const QString seed_path = select_all_dir.filePath(QStringLiteral("seed.png"));
                if (!seed.save(seed_path, "PNG"))
                {
                    LOG_ERROR(logger(), "Failed to write Select All smoke seed PNG");
                    return false;
                }
                // Seed the catalog first so the later source scan has an ineligible duplicate.
                if (presenter->imports()->importPageOpen())
                    presenter->imports()->closeImportPage();
                presenter->imports()->importFilePaths({seed_path});
                QElapsedTimer import_timer;
                import_timer.start();
                while (presenter->visibleCount() < 1 || presenter->busy())
                {
                    if (import_timer.elapsed() > 30000)
                    {
                        LOG_ERROR(logger(), "Seed import for Select All smoke timed out");
                        return false;
                    }
                    QEventLoop wait_import;
                    QTimer::singleShot(20, &wait_import, &QEventLoop::quit);
                    wait_import.exec();
                }
                auto *gallery_grid =
                    window->findChild<QQuickItem *>(QStringLiteral("galleryPhotoGrid"));
                QElapsedTimer thumbnail_timer;
                thumbnail_timer.start();
                QQuickItem *gallery_cell = nullptr;
                QQuickItem *gallery_photo = nullptr;
                while (thumbnail_timer.elapsed() < 10000)
                {
                    QList<QQuickItem *> items{gallery_grid};
                    while (!items.isEmpty())
                    {
                        auto *item = items.takeLast();
                        if (!item)
                            continue;
                        if (item->objectName() == QLatin1String("galleryThumbnailCell"))
                        {
                            gallery_cell = item;
                            gallery_photo =
                                item->findChild<QQuickItem *>(QStringLiteral("thumbnailPhoto"));
                        }
                        items.append(item->childItems());
                    }
                    if (gallery_photo && gallery_photo->property("status").toInt() == 1)
                        break;
                    QEventLoop wait_thumbnail;
                    QTimer::singleShot(20, &wait_thumbnail, &QEventLoop::quit);
                    wait_thumbnail.exec();
                }
                if (!gallery_cell || !gallery_photo ||
                    gallery_cell->property("displayName").toString() != QLatin1String("seed.png") ||
                    gallery_photo->property("source").toUrl().isEmpty() ||
                    gallery_photo->property("status").toInt() != 1)
                {
                    LOG_ERROR(
                        logger(),
                        "Gallery thumbnail binding failed: cell={} name={} source={} status={}",
                        gallery_cell != nullptr,
                        gallery_cell ?
                            gallery_cell->property("displayName").toString().toStdString() :
                            "",
                        gallery_photo ?
                            gallery_photo->property("source").toUrl().toString().toStdString() :
                            "",
                        gallery_photo ? gallery_photo->property("status").toInt() : -1);
                    return false;
                }
                QImage image(8, 8, QImage::Format_RGB888);
                image.fill(Qt::darkCyan);
                if (!image.save(source + QStringLiteral("/dup.png"), "PNG"))
                {
                    LOG_ERROR(logger(), "Failed to write Select All smoke duplicate PNG");
                    return false;
                }
                image.fill(Qt::red);
                if (!image.save(source + QStringLiteral("/a.png"), "PNG"))
                {
                    LOG_ERROR(logger(), "Failed to write Select All smoke a.png");
                    return false;
                }
                image.fill(Qt::blue);
                if (!image.save(source + QStringLiteral("/b.png"), "PNG"))
                {
                    LOG_ERROR(logger(), "Failed to write Select All smoke b.png");
                    return false;
                }
            }
            presenter->imports()->openImportPage();
            QEventLoop open_page;
            QTimer::singleShot(30, &open_page, &QEventLoop::quit);
            open_page.exec();
            if (!presenter->imports()->importPageOpen())
            {
                LOG_ERROR(logger(), "Import page must be open for production Select All");
                return false;
            }
            auto *commands = qobject_cast<StudioCommandController *>(
                engine.rootContext()
                    ->contextProperty(QStringLiteral("studioCommands"))
                    .value<QObject *>());
            auto *field =
                workspace->findChild<QQuickItem *>(QStringLiteral("importFilenameTemplate"));
            if (!commands || !field)
            {
                LOG_ERROR(logger(), "Production StudioCommands or filename template missing");
                return false;
            }
            // Rename is an opt-in component builder; its example is selectable but read-only.
            presenter->imports()->setImportMode(QStringLiteral("copy"));
            presenter->imports()->setImportDestination(select_all_dir.path());
            presenter->imports()->setImportSourceRoot(source);
            auto *planning_cancel =
                workspace->findChild<QObject *>(QStringLiteral("importPlanningCancel"));
            if (!presenter->imports()->importInteractionBlocked() || !planning_cancel ||
                !QMetaObject::invokeMethod(planning_cancel, "clicked", Qt::DirectConnection) ||
                !presenter->imports()->importPageOpen() ||
                !presenter->imports()->importSourceRoot().isEmpty() ||
                presenter->imports()->importScanActive() ||
                presenter->imports()->importInteractionBlocked() ||
                presenter->imports()->importCandidates()->rowCount() != 0)
            {
                LOG_ERROR(
                    logger(),
                    "Production planning Cancel must abandon the source and keep Import open");
                return false;
            }
            {
                auto *source_tree =
                    workspace->findChild<QObject *>(QStringLiteral("importSourceFolderTree"));
                auto *source_menu = workspace->findChild<QObject *>(
                    QStringLiteral("importSourceFolderContextMenu"));
                auto *copy_path =
                    workspace->findChild<QObject *>(QStringLiteral("importSourceCopyPath"));
                auto *reveal_folder =
                    workspace->findChild<QObject *>(QStringLiteral("importSourceRevealFolder"));
                if (!source_tree || !source_menu || !copy_path || !reveal_folder ||
                    !QMetaObject::invokeMethod(source_tree, "folderContextRequested",
                                               Q_ARG(QString, source),
                                               Q_ARG(QPointF, QPointF(20, 20))) ||
                    source_menu->property("folderPath").toString() != source ||
                    !copy_path->property("enabled").toBool() ||
                    !reveal_folder->property("enabled").toBool() ||
                    !QMetaObject::invokeMethod(copy_path, "triggered", Qt::DirectConnection) ||
                    QGuiApplication::clipboard()->text() != source ||
                    !presenter->imports()->importSourceRoot().isEmpty() ||
                    presenter->imports()->importScanActive())
                {
                    LOG_ERROR(
                        logger(),
                        "Source folder context actions must target the clicked path without scanning");
                    return false;
                }
                QMetaObject::invokeMethod(source_menu, "close", Qt::DirectConnection);
            }
            auto *preview =
                workspace->findChild<QQuickItem *>(QStringLiteral("importPreviewSettings"));
            auto *rename =
                workspace->findChild<QQuickItem *>(QStringLiteral("importRenameSettings"));
            auto *destination =
                workspace->findChild<QQuickItem *>(QStringLiteral("importDestinationSection"));
            auto *second_copy =
                workspace->findChild<QQuickItem *>(QStringLiteral("importSecondCopySettings"));
            auto *rename_check =
                workspace->findChild<QObject *>(QStringLiteral("importRenameEnabled"));
            if (!preview || !rename || !destination || !second_copy || !rename_check ||
                preview->property("expanded").isValid() || preview->y() >= rename->y() ||
                rename->y() >= second_copy->y() || second_copy->y() >= destination->y() ||
                destination->property("expanded").isValid() ||
                !field->property("readOnly").toBool())
            {
                LOG_ERROR(logger(),
                          "Import preview/rename ordering or read-only example is invalid");
                return false;
            }
            rename_check->setProperty("checked", true);
            QMetaObject::invokeMethod(rename_check, "clicked", Qt::DirectConnection);
            if (!presenter->imports()->importRenameEnabled())
                return false;
            QEventLoop expand_loop;
            QTimer::singleShot(30, &expand_loop, &QEventLoop::quit);
            expand_loop.exec();
            presenter->imports()->setImportSourceRoot(source);
            QElapsedTimer scan_timer;
            scan_timer.start();
            while (presenter->imports()->importScanActive() ||
                   presenter->imports()->importCandidates()->rowCount() < 3 ||
                   presenter->imports()->importScanTotal() < 3)
            {
                if (scan_timer.elapsed() > 30000)
                {
                    LOG_ERROR(logger(),
                              "Import scan for Select All smoke timed out rows={} total={}",
                              presenter->imports()->importCandidates()->rowCount(),
                              presenter->imports()->importScanTotal());
                    return false;
                }
                QEventLoop wait_scan;
                QTimer::singleShot(20, &wait_scan, &QEventLoop::quit);
                wait_scan.exec();
            }
            // Production Import workspace must not be treated as a dialog modal.
            // Re-applying importPageOpen into modalOpen must remain detectable here.
            QEventLoop modal_bind;
            QTimer::singleShot(20, &modal_bind, &QEventLoop::quit);
            modal_bind.exec();
            if (commands->modalOpen())
            {
                LOG_ERROR(
                    logger(),
                    "Import workspace must not set modalOpen; production shortcuts stay gated");
                return false;
            }
            presenter->imports()->importCandidates()->highlightExclusive(0);
            QEventLoop candidate_ready;
            QTimer::singleShot(30, &candidate_ready, &QEventLoop::quit);
            candidate_ready.exec();
            const int eligible = [&]()
            {
                int count = 0;
                auto *model = presenter->imports()->importCandidates();
                for (int row = 0; row < model->rowCount(); ++row)
                    count +=
                        model->data(model->index(row, 0), ImportCandidateListModel::EligibleRole)
                                .toBool() ?
                            1 :
                            0;
                return count;
            }();
            if (eligible <= 1)
            {
                LOG_ERROR(logger(), "Select All smoke needs more than one eligible candidate");
                return false;
            }
            int duplicate_rows = 0;
            int duplicate_row = -1;
            for (int row = 0; row < presenter->imports()->importCandidates()->rowCount(); ++row)
            {
                if (presenter->imports()
                        ->importCandidates()
                        ->data(presenter->imports()->importCandidates()->index(row, 0),
                               ImportCandidateListModel::DuplicateRole)
                        .toBool())
                {
                    ++duplicate_rows;
                    duplicate_row = row;
                }
            }
            if (duplicate_rows < 1)
            {
                LOG_ERROR(logger(), "Select All smoke needs an ineligible duplicate candidate");
                return false;
            }
            keyboard_grid->setProperty("currentIndex", duplicate_row);
            QCoreApplication::processEvents();
            auto *context_cell = qobject_cast<QQuickItem *>(
                keyboard_grid->property("currentItem").value<QObject *>());
            auto *source_menu =
                workspace->findChild<QObject *>(QStringLiteral("importPhotoContextMenu"));
            if (!context_cell || !source_menu)
                return false;
            const QPointF point = context_cell->mapToScene(
                QPointF(context_cell->width() / 2, context_cell->height() / 2));
            const QPointF global = window->mapToGlobal(point.toPoint());
            QMouseEvent context_press(QEvent::MouseButtonPress, point, global, Qt::RightButton,
                                      Qt::RightButton, Qt::NoModifier);
            QMouseEvent context_release(QEvent::MouseButtonRelease, point, global, Qt::RightButton,
                                        Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(window, &context_press);
            QCoreApplication::sendEvent(window, &context_release);
            QCoreApplication::processEvents();
            if (!source_menu->property("visible").toBool() ||
                presenter->imports()->importContextPath() !=
                    presenter->imports()->importCandidates()->sourcePath(duplicate_row))
            {
                LOG_ERROR(logger(), "Import right click must open source commands for a duplicate");
                return false;
            }
            QMetaObject::invokeMethod(source_menu, "close");
            QCoreApplication::processEvents();

            // Gallery mutation negatives: Import must not rate/flag/navigate the restored
            // Gallery selection through production Shortcuts while the Import page is open.
            if (presenter->visibleCount() < 1 || presenter->assets()->assetIdAt(0).isEmpty())
            {
                LOG_ERROR(logger(), "Gallery mutation smoke needs a seeded Gallery asset");
                return false;
            }
            const QString gallery_asset = presenter->assets()->assetIdAt(0);
            presenter->selectAsset(gallery_asset);
            QEventLoop select_gallery;
            QTimer::singleShot(30, &select_gallery, &QEventLoop::quit);
            select_gallery.exec();
            if (presenter->selectedAssetId() != gallery_asset)
            {
                LOG_ERROR(logger(), "Failed to select Gallery asset before Import mutation smoke");
                return false;
            }
            const auto snapshot_gallery = [&]()
            {
                const auto asset = presenter->assets()->assetById(gallery_asset);
                QVariantMap snap;
                snap.insert(QStringLiteral("selected"), presenter->selectedAssetId());
                snap.insert(QStringLiteral("count"), presenter->selectedCount());
                if (asset)
                {
                    snap.insert(QStringLiteral("rating"), asset->review.rating);
                    snap.insert(QStringLiteral("picked"), asset->review.picked);
                    snap.insert(QStringLiteral("rejected"), asset->review.rejected);
                }
                return snap;
            };
            const auto baseline = snapshot_gallery();
            if (!baseline.contains(QStringLiteral("rating")))
            {
                LOG_ERROR(logger(), "Gallery asset missing from model for mutation smoke");
                return false;
            }
            const auto wait_idle = [&]()
            {
                QElapsedTimer idle_timer;
                idle_timer.start();
                while (presenter->busy() || presenter->catalogOperationActive() ||
                       presenter->imports()->importWorkActive())
                {
                    if (idle_timer.elapsed() > 30000)
                        return false;
                    QEventLoop wait_idle_loop;
                    QTimer::singleShot(20, &wait_idle_loop, &QEventLoop::quit);
                    wait_idle_loop.exec();
                }
                return true;
            };
            const auto send_window_key =
                [&](const int key, const Qt::KeyboardModifiers mods = Qt::NoModifier)
            {
                QKeyEvent press(QEvent::KeyPress, key, mods);
                QKeyEvent release(QEvent::KeyRelease, key, mods);
                QCoreApplication::sendEvent(window, &press);
                QCoreApplication::sendEvent(window, &release);
                QEventLoop key_loop;
                QTimer::singleShot(30, &key_loop, &QEventLoop::quit);
                key_loop.exec();
            };
            const auto assert_gallery_unchanged = [&](const char *label) -> bool
            {
                if (!wait_idle())
                {
                    LOG_ERROR(logger(), "{}: timed out waiting for idle after Import key", label);
                    return false;
                }
                const auto now = snapshot_gallery();
                if (now.value(QStringLiteral("selected")) !=
                        baseline.value(QStringLiteral("selected")) ||
                    now.value(QStringLiteral("count")) != baseline.value(QStringLiteral("count")) ||
                    now.value(QStringLiteral("rating")) !=
                        baseline.value(QStringLiteral("rating")) ||
                    now.value(QStringLiteral("picked")) !=
                        baseline.value(QStringLiteral("picked")) ||
                    now.value(QStringLiteral("rejected")) !=
                        baseline.value(QStringLiteral("rejected")))
                {
                    LOG_ERROR(logger(),
                              "{} mutated Gallery asset rating/flag/selection under Import", label);
                    return false;
                }
                return true;
            };
            const auto rating_shortcut_enabled = [&]()
            {
                for (const auto &value : commands->shortcutEntries())
                {
                    const auto entry = value.toMap();
                    if (entry.value(QStringLiteral("actionId")).toString() ==
                        QStringLiteral("studio.photo.rating_5"))
                        return entry.value(QStringLiteral("enabled")).toBool();
                }
                return false;
            };
            if (rating_shortcut_enabled())
            {
                LOG_ERROR(logger(),
                          "Import open must disable Gallery rating_5 shortcut for stale selection");
                return false;
            }
            keyboard_grid->forceActiveFocus();
            QEventLoop focus_mut_grid;
            QTimer::singleShot(20, &focus_mut_grid, &QEventLoop::quit);
            focus_mut_grid.exec();
            if (commands->textInputActive() || commands->modalOpen())
            {
                LOG_ERROR(logger(), "Mutation grid focus must leave text/modal gates inactive");
                return false;
            }
            // Separate keys so auto-advance cannot confuse which asset would have changed.
            send_window_key(Qt::Key_5);
            if (!assert_gallery_unchanged("Key_5"))
                return false;
            send_window_key(Qt::Key_X);
            if (!assert_gallery_unchanged("Key_X"))
                return false;
            send_window_key(Qt::Key_P);
            if (!assert_gallery_unchanged("Key_P"))
                return false;
            send_window_key(Qt::Key_U);
            if (!assert_gallery_unchanged("Key_U"))
                return false;
            // Gallery nav shortcuts are blocked; Import grid may still consume Right locally.
            const auto gallery_selected_before_nav = presenter->selectedAssetId();
            send_window_key(Qt::Key_Right);
            if (!assert_gallery_unchanged("Key_Right"))
                return false;
            if (presenter->selectedAssetId() != gallery_selected_before_nav)
            {
                LOG_ERROR(logger(), "Import Right must not change Gallery selectedAssetId");
                return false;
            }
            auto *mutation_tree =
                workspace->findChild<QQuickItem *>(QStringLiteral("importSourceFolderTree"));
            if (!mutation_tree)
            {
                LOG_ERROR(logger(), "Import source tree missing for Gallery mutation smoke");
                return false;
            }
            mutation_tree->forceActiveFocus();
            QEventLoop focus_mut_tree;
            QTimer::singleShot(20, &focus_mut_tree, &QEventLoop::quit);
            focus_mut_tree.exec();
            send_window_key(Qt::Key_5);
            if (!assert_gallery_unchanged("source-tree Key_5"))
                return false;
            send_window_key(Qt::Key_X);
            if (!assert_gallery_unchanged("source-tree Key_X"))
                return false;
            send_window_key(Qt::Key_P);
            if (!assert_gallery_unchanged("source-tree Key_P"))
                return false;
            send_window_key(Qt::Key_U);
            if (!assert_gallery_unchanged("source-tree Key_U"))
                return false;

            const auto select_all_shortcut_gate = [&](int *entries_out, bool *enabled_out)
            {
                int entries = 0;
                bool enabled = false;
                for (const auto &value : commands->shortcutEntries())
                {
                    const auto entry = value.toMap();
                    if (entry.value(QStringLiteral("actionId")).toString() !=
                        QStringLiteral("studio.photo.select_all"))
                        continue;
                    ++entries;
                    enabled = entry.value(QStringLiteral("enabled")).toBool();
                }
                if (entries_out)
                    *entries_out = entries;
                if (enabled_out)
                    *enabled_out = enabled;
            };
            const auto send_select_all = [&]()
            {
                const QKeySequence select_all(QKeySequence::SelectAll);
                for (int index = 0; index < select_all.count(); ++index)
                {
                    const QKeyCombination combo = select_all[static_cast<uint>(index)];
                    QKeyEvent press(QEvent::KeyPress, combo.key(), combo.keyboardModifiers());
                    QKeyEvent release(QEvent::KeyRelease, combo.key(), combo.keyboardModifiers());
                    QCoreApplication::sendEvent(window, &press);
                    QCoreApplication::sendEvent(window, &release);
                }
                QEventLoop key_loop;
                QTimer::singleShot(30, &key_loop, &QEventLoop::quit);
                key_loop.exec();
            };
            const auto highlighted_count = [&]()
            {
                int count = 0;
                auto *model = presenter->imports()->importCandidates();
                for (int row = 0; row < model->rowCount(); ++row)
                    count += model->highlighted(row) ? 1 : 0;
                return count;
            };
            const auto exact_eligible_highlighted = [&]()
            {
                auto *model = presenter->imports()->importCandidates();
                for (int row = 0; row < model->rowCount(); ++row)
                {
                    const bool is_eligible =
                        model->data(model->index(row, 0), ImportCandidateListModel::EligibleRole)
                            .toBool();
                    if (model->highlighted(row) != is_eligible)
                        return false;
                }
                return highlighted_count() == eligible;
            };

            // Text context (neg for candidates): production textInputActive Binding owns the gate.
            const QString template_text = field->property("text").toString();
            if (template_text.isEmpty())
            {
                LOG_ERROR(logger(),
                          "Presenter filename template must be non-empty before Select All");
                return false;
            }
            field->forceActiveFocus();
            if (field->property("cursorPosition").isValid())
                field->setProperty("cursorPosition", template_text.size());
            QMetaObject::invokeMethod(field, "deselect", Qt::DirectConnection);
            QEventLoop focus_text;
            QTimer::singleShot(20, &focus_text, &QEventLoop::quit);
            focus_text.exec();
            if (!commands->textInputActive() ||
                !field->property("selectedText").toString().isEmpty())
            {
                LOG_ERROR(
                    logger(),
                    "Filename template must start unselected with production textInputActive");
                return false;
            }
            {
                int entries = 0;
                bool enabled = true;
                select_all_shortcut_gate(&entries, &enabled);
                if (entries != 1 || enabled)
                {
                    LOG_ERROR(logger(),
                              "Text focus must keep exactly one disabled Select All shortcut");
                    return false;
                }
            }
            send_select_all();
            if (field->property("selectedText").toString() != template_text ||
                highlighted_count() != 1)
            {
                LOG_ERROR(logger(),
                          "Text Select All must select template text without mutating candidates");
                return false;
            }

            // Grid context (pos): production bindings only — no modalOpen/textInputActive overrides.
            {
                const auto state = commands->action(QStringLiteral("studio.photo.select_all"));
                if (!state.value(QStringLiteral("enabled")).toBool())
                {
                    LOG_ERROR(
                        logger(), "Production Select All unavailable: {}",
                        state.value(QStringLiteral("disabledReason")).toString().toStdString());
                    return false;
                }
            }
            keyboard_grid->forceActiveFocus();
            QEventLoop focus_grid;
            QTimer::singleShot(20, &focus_grid, &QEventLoop::quit);
            focus_grid.exec();
            if (commands->textInputActive() || commands->modalOpen())
            {
                LOG_ERROR(logger(), "Grid focus must clear textInputActive with modalOpen false");
                return false;
            }
            {
                int entries = 0;
                bool enabled = false;
                select_all_shortcut_gate(&entries, &enabled);
                if (entries != 1 || !enabled)
                {
                    LOG_ERROR(logger(),
                              "Import grid must expose exactly one enabled Select All shortcut");
                    return false;
                }
            }
            const int gallery_selected = presenter->selectedCount();
            presenter->imports()->importCandidates()->highlightExclusive(0);
            int highlight_batches = 0;
            QMetaObject::Connection highlight_watch = QObject::connect(
                presenter->imports()->importCandidates(), &QAbstractItemModel::dataChanged,
                presenter->imports()->importCandidates(),
                [&highlight_batches](const QModelIndex &, const QModelIndex &,
                                     const QList<int> &roles)
                {
                    if (roles.isEmpty() ||
                        roles.contains(ImportCandidateListModel::HighlightedRole))
                        ++highlight_batches;
                });
            send_select_all();
            QObject::disconnect(highlight_watch);
            if (highlight_batches < 1)
            {
                LOG_ERROR(logger(),
                          "One Select All key must drive the candidate highlight command (got {})",
                          highlight_batches);
                return false;
            }
            if (!exact_eligible_highlighted())
            {
                LOG_ERROR(logger(), "Grid Select All must highlight exact eligible set ({} vs {})",
                          highlighted_count(), eligible);
                return false;
            }
            if (presenter->selectedCount() != gallery_selected)
            {
                LOG_ERROR(logger(), "Import Select All must not mutate Gallery selection");
                return false;
            }
            // Idempotent second Select All — still one owner, no growth beyond eligible.
            send_select_all();
            if (!exact_eligible_highlighted())
            {
                LOG_ERROR(logger(), "Second Select All must remain idempotent on eligible set");
                return false;
            }

            // Source tree focus (pos): non-text focus keeps Select All eligible for candidates.
            auto *source_tree =
                workspace->findChild<QQuickItem *>(QStringLiteral("importSourceFolderTree"));
            if (!source_tree)
            {
                LOG_ERROR(logger(), "Import source folder tree missing for Select All context");
                return false;
            }
            presenter->imports()->importCandidates()->highlightExclusive(0);
            source_tree->forceActiveFocus();
            QEventLoop focus_tree;
            QTimer::singleShot(20, &focus_tree, &QEventLoop::quit);
            focus_tree.exec();
            if (commands->textInputActive() || commands->modalOpen())
            {
                LOG_ERROR(logger(), "Source tree focus must leave text/modal gates inactive");
                return false;
            }
            {
                int entries = 0;
                bool enabled = false;
                select_all_shortcut_gate(&entries, &enabled);
                if (entries != 1 || !enabled)
                {
                    LOG_ERROR(logger(), "Source tree focus must keep Select All shortcut enabled");
                    return false;
                }
            }
            send_select_all();
            if (!exact_eligible_highlighted())
            {
                LOG_ERROR(logger(), "Source tree Select All must highlight exact eligible set");
                return false;
            }

            // Real dialog modal (neg): production aboutDialog must block the shortcut path.
            presenter->imports()->importCandidates()->highlightExclusive(0);
            if (!QMetaObject::invokeMethod(window, "openAboutDialog", Qt::DirectConnection))
            {
                LOG_ERROR(logger(), "Unable to open production About dialog for modal gating");
                return false;
            }
            QEventLoop about_open;
            QTimer::singleShot(40, &about_open, &QEventLoop::quit);
            about_open.exec();
            if (!commands->modalOpen())
            {
                LOG_ERROR(logger(), "About dialog must set production modalOpen");
                return false;
            }
            {
                int entries = 0;
                bool enabled = true;
                select_all_shortcut_gate(&entries, &enabled);
                if (entries != 1 || enabled)
                {
                    LOG_ERROR(logger(), "Real modal must disable the Select All shortcut entry");
                    return false;
                }
            }
            send_select_all();
            if (highlighted_count() != 1)
            {
                LOG_ERROR(logger(), "Select All must not mutate candidates while a dialog is open");
                return false;
            }
            auto *about = window->findChild<QObject *>(QStringLiteral("aboutDialog"));
            if (!about || !about->property("visible").toBool())
            {
                LOG_ERROR(logger(), "Production aboutDialog is not visible for modal gating");
                return false;
            }
            if (!QMetaObject::invokeMethod(about, "close", Qt::DirectConnection))
            {
                LOG_ERROR(logger(), "Unable to close production aboutDialog");
                return false;
            }
            QEventLoop about_close;
            QTimer::singleShot(40, &about_close, &QEventLoop::quit);
            about_close.exec();
            if (commands->modalOpen())
            {
                LOG_ERROR(logger(), "Closing About must clear production modalOpen");
                return false;
            }

            // Restore layout fixtures after catalog/source/destination mutations above.
            for (auto *model : {presenter->imports()->importSourceFolders(),
                                presenter->imports()->importDestinationFolders()})
                model->resetWithRoots({{directory.path(), "Pictures", true}});
            presenter->imports()->setImportDestination(directory.path());
            presenter->imports()->importCandidates()->setCandidates({candidate});
            QEventLoop restore_layout;
            QTimer::singleShot(40, &restore_layout, &QEventLoop::quit);
            restore_layout.exec();
            auto *header = window->findChild<QQuickItem *>(QStringLiteral("libraryHeader"));
            auto *progress =
                window->findChild<QQuickItem *>(QStringLiteral("libraryPreviewProgress"));
            auto *backup = window->findChild<QQuickItem *>(QStringLiteral("libraryBackupStatus"));
            if (!header || !progress || !backup)
                return false;
            const auto header_height = header->height();
            const auto backup_y = backup->y();
            const auto check_progress = [&](int total, int completed, bool active, bool expected)
            {
                QQmlProperty::write(progress, "total", total);
                QQmlProperty::write(progress, "completed", completed);
                QQmlProperty::write(progress, "workActive", active);
                QEventLoop progress_layout;
                QTimer::singleShot(650, &progress_layout, &QEventLoop::quit);
                progress_layout.exec();
                return progress->property("revealed").toBool() == expected &&
                       header->height() == header_height && backup->y() == backup_y;
            };
            if (!check_progress(1, 0, true, false) || !check_progress(8, 0, true, true) ||
                !check_progress(8, 8, false, false))
            {
                LOG_ERROR(logger(),
                          "Library preview progress must debounce without moving the rail");
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
                // Cancel the preceding import fixture's planner before supplying
                // a controlled overlay; its late result must not replace this plan.
                presenter->imports()->closeImportPage();
                auto *model = presenter->imports()->importDestinationFolders();
                QTemporaryDir preview_directory;
                if (!preview_directory.isValid())
                    return false;
                const auto home = preview_directory.filePath("Home");
                const auto pictures = home + "/Pictures";
                if (!QDir().mkpath(pictures + "/2026/03"))
                    return false;
                // Many Home siblings put Pictures outside the initial viewport.
                // Deferred real listings must not reset the final preview scroll.
                for (int sibling = 0; sibling < 60; ++sibling)
                    if (!QDir().mkpath(
                            home +
                            QStringLiteral("/Folder%1").arg(sibling, 2, 10, QLatin1Char('0'))))
                        return false;
                model->resetWithRoots({{home, "Home", true}});
                const auto month = pictures + "/2026/10";
                bool preview_revealed = false;
                const auto reveal_connection = QObject::connect(
                    model, &FilesystemBrowserModel::folderRevealed, model,
                    [&](const int row)
                    {
                        preview_revealed =
                            model->data(model->index(row, 0), FilesystemBrowserModel::PathRole)
                                .toString() == month;
                    });
                model->setPreviewFolders(
                    {{pictures.toStdString(), "Pictures", 0, 1, false, false},
                     {(pictures + "/2026").toStdString(), "2026", 1, 1, false, false},
                     {(pictures + "/2026/10").toStdString(), "10", 2, 1, true, false}},
                    pictures);
                // Three real directory listings run on the filesystem worker.
                // Observe their final reveal instead of sampling an intermediate
                // reset after a fixed delay on a busy CI host.
                QElapsedTimer preview_deadline;
                preview_deadline.start();
                while (!preview_revealed && preview_deadline.elapsed() < 10000)
                {
                    QEventLoop listing;
                    QTimer::singleShot(20, &listing, &QEventLoop::quit);
                    listing.exec();
                }
                QObject::disconnect(reveal_connection);
                if (!preview_revealed)
                {
                    LOG_ERROR(logger(), "Destination tree preview did not finish its reveal");
                    return false;
                }
                QEventLoop preview_layout;
                QTimer::singleShot(30, &preview_layout, &QEventLoop::quit);
                preview_layout.exec();
                QQuickItem *year_label = nullptr;
                QQuickItem *month_label = nullptr;
                int planned_counts = 0;
                const auto find_labels = [&](auto &&visit, QQuickItem *item) -> void
                {
                    if (item->objectName() == QLatin1String("importFolderName"))
                    {
                        if (item->property("text").toString() == QLatin1String("2026"))
                            year_label = item;
                        if (item->property("text").toString() == QLatin1String("10"))
                            month_label = item;
                    }
                    if (item->objectName() == QLatin1String("importFolderPlannedCount") &&
                        item->isVisible() &&
                        item->property("text").toString() == QLatin1String("1"))
                        ++planned_counts;
                    for (auto *child : item->childItems())
                        visit(visit, child);
                };
                find_labels(find_labels, tree);
                if (!year_label || !month_label || planned_counts != 3 ||
                    year_label->property("color") == month_label->property("color") ||
                    QQmlProperty::read(year_label, "font.italic").toBool() ||
                    !QQmlProperty::read(month_label, "font.italic").toBool() ||
                    !tree->boundingRect().contains(
                        month_label->mapRectToItem(tree, month_label->boundingRect())) ||
                    QDir(pictures + "/2026/10").exists())
                {
                    LOG_ERROR(
                        logger(),
                        "Destination tree preview style failed: rows={} year={} month={} yearItalic={} monthItalic={}",
                        model->rowCount(), year_label != nullptr, month_label != nullptr,
                        year_label && QQmlProperty::read(year_label, "font.italic").toBool(),
                        month_label && QQmlProperty::read(month_label, "font.italic").toBool());
                    return false;
                }
                model->setPreviewFolders({});
                model->resetWithRoots({{directory.path(), "Pictures", true}});
                model->selectFolder(directory.path());
                presenter->imports()->importCandidates()->setCandidates({candidate});
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
                settle_layout();
                const bool destination_fixed = !section->property("expanded").isValid() &&
                                               section->isVisible() && tree->isVisible();
                presenter->imports()->setImportMode(QStringLiteral("add"));
                settle_layout();
                const bool add_fits = qAbs(section->parentItem()->height() -
                                           section->parentItem()->implicitHeight()) < 1;
                presenter->imports()->setImportMode(QStringLiteral("copy"));
                settle_layout();
                if (!destination_fixed || !add_fits)
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
                auto *segment = item->parentItem();
                auto *row = segment ? segment->parentItem() : nullptr;
                const bool move =
                    item->objectName() == QStringLiteral("importTransferModeSegment2");
                if (!row || row->objectName() != QStringLiteral("importTransferMode") ||
                    qAbs(item->width() * 3 - row->width()) > 0.1 ||
                    item->height() != row->height() ||
                    item->isEnabled() !=
                        (!move || presenter->imports()->importMoveUnavailableReason().isEmpty()))
                {
                    LOG_ERROR(
                        logger(),
                        "Import mode segments must share equal width and follow transfer availability");
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
                      size.width(), size.height(),
                      presenter->imports()->importCandidates()->rowCount());
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
            auto *model = presenter->imports()->importDestinationFolders();
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
                presenter->imports()->importDestination() != directory.path())
            {
                LOG_ERROR(logger(), "Folder choice was lost during delegate replacement");
                return false;
            }
            model->toggleCollapsed(directory.path());
            if (model->rowCount() != 1)
                return false;
        }
    }
    // Exercise the production photo surface, including the first click and a
    // quick second click. Static QML checks cannot detect a self-bound presenter.
    presenter->imports()->closeImportPage();
    const auto inspect_catalog = directory.filePath(QStringLiteral("inspect-click.sqlite"));
    const auto inspect_photo = directory.filePath(QStringLiteral("inspect-click.png"));
    QImage inspect_image(1600, 1000, QImage::Format_RGB888);
    inspect_image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    inspect_image.fill(Qt::cyan);
    if (!inspect_image.save(inspect_photo))
        return false;
    const auto wait_ready = [](const auto &ready)
    {
        QElapsedTimer timer;
        timer.start();
        while (!ready())
        {
            if (timer.elapsed() > 30000)
                return false;
            QEventLoop loop;
            QTimer::singleShot(10, &loop, &QEventLoop::quit);
            loop.exec();
        }
        return true;
    };
    presenter->createCatalogFromPath(inspect_catalog);
    if (!wait_ready([&]
                    { return presenter->catalogPath() == inspect_catalog && !presenter->busy(); }))
        return false;
    presenter->imports()->importFilePaths({inspect_photo});
    if (!wait_ready(
            [&]
            {
                return !presenter->busy() && !presenter->imports()->importWorkActive() &&
                       presenter->visibleCount() == 1 &&
                       presenter->selectedUri().endsWith(QStringLiteral("inspect-click.png"));
            }))
        return false;
    window->resize(1440, 900);
    presenter->setBrowseMode(QStringLiteral("loupe"));
    presenter->inspect()->setZoomMode(QStringLiteral("fit"));
    auto *zoom = window->findChild<QObject *>(QStringLiteral("photoInspectZoomController"));
    auto *scroller = window->findChild<QQuickItem *>(QStringLiteral("photoInspectScroller"));
    if (!zoom || !scroller ||
        !wait_ready([&] { return zoom->property("photoInspectEnabled").toBool(); }))
    {
        LOG_ERROR(logger(),
                  "Photo click zoom did not become ready: mode={} preview={} error={} bound={}",
                  presenter->browseMode().toStdString(),
                  presenter->inspect()->previewUrl().toString().toStdString(),
                  presenter->errorText().toStdString(),
                  zoom && zoom->property("studio").value<QObject *>() == presenter);
        return false;
    }
    const auto click_photo = [&]
    {
        const auto point =
            scroller->mapToScene(QPointF(scroller->width() / 2, scroller->height() / 2));
        const QPointF global = window->mapToGlobal(point.toPoint());
        QMouseEvent press(QEvent::MouseButtonPress, point, global, Qt::LeftButton, Qt::LeftButton,
                          Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, point, global, Qt::LeftButton, Qt::NoButton,
                            Qt::NoModifier);
        QCoreApplication::sendEvent(window, &press);
        QCoreApplication::sendEvent(window, &release);
    };
    click_photo();
    if (presenter->inspect()->zoomMode() != QStringLiteral("actual"))
    {
        LOG_ERROR(logger(), "Single photo click must immediately select 1:1");
        return false;
    }
    click_photo();
    if (presenter->inspect()->zoomMode() != QStringLiteral("fit") ||
        presenter->browseMode() != QStringLiteral("loupe"))
    {
        LOG_ERROR(logger(), "Second photo click must restore Fit without leaving Loupe");
        return false;
    }
    auto *zoom_plane = window->findChild<QQuickItem *>(QStringLiteral("photoInspectPlane"));
    if (!zoom_plane)
        return false;
    for (const auto size : {QSize{1440, 900}, QSize{900, 1200}})
    {
        window->resize(size);
        presenter->inspect()->setZoomMode(QStringLiteral("30percent"));
        if (!wait_ready(
                [&]
                {
                    const double visible_fraction =
                        std::max(scroller->width(), scroller->height()) /
                        std::max(zoom_plane->width(), zoom_plane->height());
                    return std::abs(visible_fraction - 0.3) < 0.001;
                }))
        {
            LOG_ERROR(logger(), "30% zoom must preserve the long-edge viewport fraction");
            return false;
        }
        presenter->inspect()->toggleActualSize();
        presenter->inspect()->toggleActualSize();
        if (presenter->inspect()->zoomMode() != QStringLiteral("30percent"))
            return false;
    }
    window->resize(1440, 900);
    presenter->inspect()->setZoomMode(QStringLiteral("fit"));
    presenter->setBrowseMode(QStringLiteral("develop"));
    // Verify real slider bindings, including value changes after construction.
    // A QML source check cannot catch an undefined Repeater property lookup.
    const std::array tone_controls{std::pair{"exposure", "lightExposureSlider"},
                                   std::pair{"highlights", "lightHighlightsSlider"},
                                   std::pair{"shadows", "lightShadowsSlider"},
                                   std::pair{"whites", "lightWhitesSlider"},
                                   std::pair{"blacks", "lightBlacksSlider"}};
    const auto find_visual = [](const auto &self, QQuickItem *item,
                                const QString &name) -> QQuickItem *
    {
        if (item->objectName() == name)
            return item;
        for (auto *child : item->childItems())
            if (auto *found = self(self, child, name))
                return found;
        return nullptr;
    };
    for (const auto &[field, name] : tone_controls)
    {
        QObject *slider = nullptr;
        if (!wait_ready(
                [&]
                {
                    slider =
                        find_visual(find_visual, window->contentItem(), QString::fromLatin1(name));
                    return slider && slider->property("value").isValid();
                }))
        {
            LOG_ERROR(logger(), "Light slider was not constructed: {}", name);
            return false;
        }
        presenter->develop()->setDevelopNumber(QString::fromLatin1(field), 0.017);
        if (!wait_ready([&] { return slider->property("value").toDouble() == 0.017; }))
        {
            LOG_ERROR(logger(), "Light slider failed to follow its presenter: {}", name);
            return false;
        }
        presenter->develop()->setDevelopNumber(QString::fromLatin1(field), 0.0);
        if (!wait_ready([&] { return slider->property("value").toDouble() == 0.0; }))
            return false;
    }
    auto *tool_bar = window->findChild<QQuickItem *>(QStringLiteral("developToolBar"));
    auto *edit_tool =
        find_visual(find_visual, window->contentItem(), QStringLiteral("developTool_edit"));
    auto *crop_tool =
        find_visual(find_visual, window->contentItem(), QStringLiteral("developTool_crop"));
    auto *local_tool =
        find_visual(find_visual, window->contentItem(), QStringLiteral("developTool_local"));
    auto *tool_scroll = window->findChild<QQuickItem *>(QStringLiteral("developPanelScroller"));
    if (!tool_bar || !edit_tool || !crop_tool || !local_tool || !tool_scroll ||
        !edit_tool->property("selected").toBool())
    {
        LOG_ERROR(logger(), "Develop toolbar or initial edit selection is missing");
        return false;
    }
    QCoreApplication::processEvents();
    for (auto *tool : {edit_tool, crop_tool, local_tool})
    {
        if (tool->width() + .1 < tool->implicitWidth())
        {
            LOG_ERROR(logger(), "Develop tool label does not fit its standard button");
            return false;
        }
    }
    if (tool_bar->x() + tool_bar->width() > tool_bar->parentItem()->width() + .1)
    {
        LOG_ERROR(logger(), "Develop toolbar exceeds the inspector width");
        return false;
    }
    const auto toolbar_position = tool_bar->mapToScene(QPointF{});
    tool_scroll->setProperty("contentY", 200.0);
    QCoreApplication::processEvents();
    if (tool_bar->mapToScene(QPointF{}) != toolbar_position ||
        !QMetaObject::invokeMethod(local_tool, "clicked") ||
        !wait_ready([&] { return local_tool->property("selected").toBool(); }))
    {
        LOG_ERROR(logger(), "Develop toolbar moved during scrolling or local navigation failed");
        return false;
    }
    auto *new_mask =
        find_visual(find_visual, window->contentItem(), QStringLiteral("createLocalMask"));
    auto *tool_commands = qobject_cast<StudioCommandController *>(
        engine.rootContext()->contextProperty(QStringLiteral("studioCommands")).value<QObject *>());
    auto *create_menu = window->findChild<QObject *>(QStringLiteral("localCreateMenu"));
    if (!new_mask || !create_menu || !QMetaObject::invokeMethod(new_mask, "clicked") ||
        !wait_ready([&] { return create_menu->property("opened").toBool(); }) ||
        create_menu->property("count").toInt() != 5 ||
        !QMetaObject::invokeMethod(create_menu, "close"))
    {
        LOG_ERROR(logger(), "Standard mask creation menu failed to open with its five actions");
        return false;
    }
    if (!new_mask || !new_mask->isVisible() || !tool_commands ||
        !tool_commands->localAdjustment(QStringLiteral("create"), {{QStringLiteral("kind"), 2}})
             .value(QStringLiteral("ok"))
             .toBool() ||
        !presenter->develop()->localEditing())
        return false;
    auto *local_panel = window->findChild<QQuickItem *>(QStringLiteral("pinnedLocalPanel"));
    auto *mask_settings = window->findChild<QQuickItem *>(QStringLiteral("localMaskSettings"));
    if (!local_panel || !mask_settings || !local_panel->isVisible())
        return false;
    QVariantMap mask_gesture{{"id", presenter->develop()->activeLocalId()},
                             {"asset", presenter->selectedAssetId()},
                             {"x", 0.3},
                             {"y", 0.3},
                             {"handle", "draw"}};
    const auto mask_begin =
        tool_commands->localAdjustment(QStringLiteral("gesture_begin"), mask_gesture);
    if (!mask_begin.value("ok").toBool())
        return false;
    mask_gesture.remove("handle");
    mask_gesture.insert("token", mask_begin.value("token"));
    mask_gesture.insert("x", 0.7);
    mask_gesture.insert("y", 0.7);
    if (!tool_commands->localAdjustment(QStringLiteral("gesture_end"), mask_gesture)
             .value("ok")
             .toBool() ||
        !wait_ready(
            [&]
            {
                return !presenter->inspect()->previewLoading() &&
                       presenter->develop()->localMaskGeometry().size() == 3;
            }))
        return false;
    auto *mask_geometry =
        find_visual(find_visual, window->contentItem(), QStringLiteral("localMaskGeometry"));
    if (!mask_geometry || !mask_geometry->isVisible())
        return false;
    mask_settings->setProperty("expanded", true);
    if (!wait_ready(
            [&]
            {
                return local_panel->property("contentHeight").toDouble() >
                           local_panel->height() + 40 &&
                       tool_scroll->property("contentHeight").toDouble() >
                           tool_scroll->height() + 80;
            }))
    {
        LOG_ERROR(logger(), "Local tools and adjustments must have independent bounded viewports");
        return false;
    }
    const auto local_position = local_panel->mapToScene(QPointF{});
    const auto adjustment_position = tool_scroll->mapToScene(QPointF{});
    tool_scroll->setProperty("contentY", 80.0);
    local_panel->setProperty("contentY", 40.0);
    QCoreApplication::processEvents();
    if (local_panel->mapToScene(QPointF{}) != local_position ||
        tool_scroll->mapToScene(QPointF{}) != adjustment_position ||
        tool_bar->mapToScene(QPointF{}) != toolbar_position ||
        std::abs(local_panel->property("contentY").toDouble() - 40.0) > .1 ||
        std::abs(tool_scroll->property("contentY").toDouble() - 80.0) > .1)
    {
        LOG_ERROR(logger(), "Scrolling local tools moved the pinned workspace or adjustments");
        return false;
    }
    mask_settings->setProperty("expanded", false);
    if (!QMetaObject::invokeMethod(crop_tool, "clicked") ||
        !wait_ready(
            [&]
            {
                return !presenter->develop()->localEditing() &&
                       presenter->develop()->cropToolActive() &&
                       crop_tool->property("selected").toBool();
            }))
    {
        LOG_ERROR(logger(), "Develop local workspace or crop navigation failed");
        return false;
    }
    auto *pinned = window->findChild<QQuickItem *>(QStringLiteral("pinnedCropPanel"));
    auto *develop_scroll = window->findChild<QQuickItem *>(QStringLiteral("developPanelScroller"));
    auto *plane = window->findChild<QQuickItem *>(QStringLiteral("photoInspectPlane"));
    if (!pinned || !develop_scroll || !plane ||
        !wait_ready(
            [&]
            {
                return !presenter->inspect()->previewLoading() &&
                       !presenter->inspect()->cropPreviewLayout().isEmpty() && pinned->isVisible();
            }))
        return false;
    for (const auto *name : {"cropAspectRatio", "cropAutoLevel", "cropFineRotation"})
    {
        auto *control = pinned->findChild<QQuickItem *>(QString::fromLatin1(name));
        if (!control || !control->isVisible())
        {
            LOG_ERROR(logger(), "Pinned crop control is missing: {}", name);
            return false;
        }
    }
    const auto *stage = plane->parentItem();
    const auto geometry = presenter->inspect()->cropPreviewLayout();
    const double old_width =
        std::min(stage->width(), stage->height()) * geometry.value("widthScale").toDouble();
    const double old_height =
        std::min(stage->width(), stage->height()) * geometry.value("heightScale").toDouble();
    const double contain_scale = std::min(stage->width() / old_width, stage->height() / old_height);
    if (std::abs(plane->width() - (old_width + old_width * contain_scale) / 2) > 1.0 ||
        std::abs(plane->height() - (old_height + old_height * contain_scale) / 2) > 1.0 ||
        develop_scroll->y() < pinned->y() + pinned->height() - 1.0)
    {
        LOG_ERROR(logger(),
                  "Crop workspace did not halve its surround or pin controls above scrolling");
        return false;
    }
    const auto pinned_y = pinned->y();
    develop_scroll->setProperty("contentY", 200.0);
    if (std::abs(pinned->y() - pinned_y) > .1)
        return false;
    presenter->inspect()->setZoomMode(QStringLiteral("30percent"));
    if (!wait_ready(
            [&]
            {
                return std::abs(std::max(scroller->width(), scroller->height()) /
                                    std::max(plane->width(), plane->height()) -
                                0.3) < 0.001;
            }))
        return false;
    presenter->inspect()->setZoomMode(QStringLiteral("fit"));
    if (!QMetaObject::invokeMethod(edit_tool, "clicked") ||
        !wait_ready(
            [&]
            {
                return !presenter->develop()->cropToolActive() &&
                       edit_tool->property("selected").toBool();
            }))
        return false;
    if (pinned->isVisible() || !wait_ready([&] { return !presenter->inspect()->previewLoading(); }))
        return false;
    presenter->setBrowseMode(QStringLiteral("loupe"));
    auto *metadata_button =
        find_visual(find_visual, window->contentItem(), QStringLiteral("editMetadataButton"));
    if (!metadata_button || !QMetaObject::invokeMethod(metadata_button, "clicked"))
        return false;
    QQuickItem *metadata_value = nullptr;
    QQuickItem *metadata_save = nullptr;
    if (!wait_ready(
            [&]
            {
                metadata_value = find_visual(find_visual, window->contentItem(),
                                             QStringLiteral("metadataEditValue_title"));
                metadata_save = find_visual(find_visual, window->contentItem(),
                                            QStringLiteral("metadataEditSave"));
                return metadata_value && metadata_save && metadata_value->isVisible();
            }))
        return false;
    const auto original_title = presenter->selectedTitle();
    metadata_value->setProperty("text", QStringLiteral("Cancelled metadata"));
    QMetaObject::invokeMethod(metadata_value, "textEdited");
    auto *metadata_cancel =
        find_visual(find_visual, window->contentItem(), QStringLiteral("metadataEditCancel"));
    if (!metadata_cancel || !QMetaObject::invokeMethod(metadata_cancel, "clicked") ||
        !wait_ready([&] { return !metadata_value->isVisible(); }) ||
        presenter->selectedTitle() != original_title)
        return false;
    if (!QMetaObject::invokeMethod(metadata_button, "clicked") ||
        !wait_ready(
            [&]
            {
                metadata_value = find_visual(find_visual, window->contentItem(),
                                             QStringLiteral("metadataEditValue_title"));
                return metadata_value && metadata_value->isVisible();
            }))
        return false;
    metadata_value->setProperty("text", QStringLiteral("Metadata smoke"));
    QMetaObject::invokeMethod(metadata_value, "textEdited");
    auto *description_value = find_visual(find_visual, window->contentItem(),
                                          QStringLiteral("metadataEditValue_description"));
    if (!description_value)
        return false;
    description_value->setProperty("text", QStringLiteral("List editor description"));
    QMetaObject::invokeMethod(description_value, "textEdited");
    if (!QMetaObject::invokeMethod(metadata_save, "clicked") ||
        !wait_ready(
            [&]
            {
                return presenter->selectedTitle() == QStringLiteral("Metadata smoke") &&
                       presenter->selectedDescription() ==
                           QStringLiteral("List editor description");
            }))
    {
        LOG_ERROR(logger(), "Metadata dialog save failed: {}",
                  presenter->errorText().toStdString());
        return false;
    }
    // A real ThumbnailCell must keep its bands/labels while an edited image
    // is decoded asynchronously. No screenshots or separate renderer are used.
    const auto first_thumbnail = directory.filePath(QStringLiteral("thumbnail-first.png"));
    const auto next_thumbnail = directory.filePath(QStringLiteral("thumbnail-next.png"));
    QImage thumbnail_image(320, 200, QImage::Format_RGB888);
    thumbnail_image.fill(Qt::darkRed);
    if (!thumbnail_image.save(first_thumbnail))
        return false;
    thumbnail_image.fill(Qt::darkGreen);
    if (!thumbnail_image.save(next_thumbnail))
        return false;
    QQmlComponent cell_component(
        &engine, QUrl(QStringLiteral("qrc:/qt/qml/Ravo/Studio/qml/gallery/ThumbnailCell.qml")));
    std::unique_ptr<QObject> cell_object(cell_component.createWithInitialProperties(
        {{QStringLiteral("width"), 260},
         {QStringLiteral("height"), 240},
         {QStringLiteral("displayName"), QStringLiteral("reload.png")},
         {QStringLiteral("thumbnailUrl"), QUrl::fromLocalFile(first_thumbnail)}}));
    auto *cell = qobject_cast<QQuickItem *>(cell_object.get());
    if (!cell)
        return false;
    cell->setParentItem(window->contentItem());
    auto *cell_photo = cell->findChild<QQuickItem *>(QStringLiteral("thumbnailPhoto"));
    auto *cell_chrome = cell->findChild<QQuickItem *>(QStringLiteral("thumbnailChrome"));
    if (!cell_photo || !cell_chrome ||
        !wait_ready(
            [&]
            { return cell_photo->property("status").toInt() == 1 && cell_chrome->isVisible(); }))
        return false;
    int chrome_hides = 0;
    QObject reload_observer;
    const auto chrome_connection =
        QObject::connect(cell_chrome, &QQuickItem::visibleChanged, &reload_observer,
                         [&]
                         {
                             if (!cell_chrome->isVisible())
                                 ++chrome_hides;
                         });
    const auto gutter = cell_chrome->property("gutterY").toDouble();
    cell->setProperty("thumbnailUrl", QUrl::fromLocalFile(next_thumbnail));
    if (cell_photo->property("status").toInt() != 2 ||
        !cell_photo->property("retainWhileLoading").toBool() || !cell_chrome->isVisible() ||
        cell_chrome->property("gutterY").toDouble() != gutter ||
        !wait_ready([&] { return cell_photo->property("status").toInt() == 1; }) ||
        chrome_hides != 0)
    {
        LOG_ERROR(logger(), "Thumbnail reload hid its chrome or changed its retained geometry");
        return false;
    }
    QObject::disconnect(chrome_connection);
    cell->setProperty("thumbnailUrl", QUrl{});
    if (!wait_ready(
            [&]
            {
                return !cell_photo->property("hasReadyImage").toBool() && !cell_chrome->isVisible();
            }))
        return false;
    presenter->setBrowseMode(QStringLiteral("grid"));
    if (!wait_ready(
            [&]
            { return !presenter->inspect()->previewLoading() && !presenter->previewWorkActive(); }))
        return false;
    QEventLoop navigator_layout;
    QTimer::singleShot(30, &navigator_layout, &QEventLoop::quit);
    navigator_layout.exec();
    auto *navigator = window->findChild<QQuickItem *>(QStringLiteral("libraryNavigator"));
    auto *view_box = window->findChild<QQuickItem *>(QStringLiteral("navigatorViewBox"));
    auto *exposure_commands = qobject_cast<StudioCommandController *>(
        engine.rootContext()->contextProperty(QStringLiteral("studioCommands")).value<QObject *>());
    if (!navigator || !view_box || !exposure_commands || !view_box->isVisible())
        return false;
    const auto scene_rect = [](QQuickItem *item)
    { return QRectF(item->mapToScene(QPointF{}), QSizeF(item->width(), item->height())); };
    const auto navigator_rect = scene_rect(navigator);
    const auto viewport_rect = scene_rect(view_box);
    bool moved = false;
    bool wrong_navigator_frame = false;
    QObject geometry_observer;
    auto *held_navigation =
        navigator->findChild<QQuickItem *>(QStringLiteral("navigatorHeldImage"));
    if (!held_navigation)
        return false;
    QObject::connect(held_navigation, &QQuickItem::visibleChanged, &geometry_observer,
                     [&] { wrong_navigator_frame |= held_navigation->isVisible(); });
    const auto check_navigator_frame = [&]
    {
        wrong_navigator_frame |=
            held_navigation->isVisible() || navigator->property("gpuLive").toBool() ||
            navigator->property("liveSource").toUrl() != presenter->selectedThumbnailUrl();
    };
    QObject::connect(presenter->inspect(), &StudioInspectPresenter::previewChanged,
                     &geometry_observer,
                     [check_navigator_frame, &geometry_observer]
                     {
                         QMetaObject::invokeMethod(&geometry_observer, check_navigator_frame,
                                                   Qt::QueuedConnection);
                     });
    check_navigator_frame();
    const auto check_geometry = [&]
    { moved |= scene_rect(navigator) != navigator_rect || scene_rect(view_box) != viewport_rect; };
    for (auto *item : {navigator, view_box})
    {
        QObject::connect(item, &QQuickItem::xChanged, &geometry_observer, check_geometry);
        QObject::connect(item, &QQuickItem::yChanged, &geometry_observer, check_geometry);
        QObject::connect(item, &QQuickItem::widthChanged, &geometry_observer, check_geometry);
        QObject::connect(item, &QQuickItem::heightChanged, &geometry_observer, check_geometry);
    }
    for (const double delta : {1.0, -1.0, 1.0 / 3.0, -1.0 / 3.0})
    {
        const auto result = exposure_commands->executeCommand(
            exposure_commands->ids().value(QStringLiteral("photoAdjustExposure")).toString(), delta,
            QStringLiteral("control"));
        if (!result.value(QStringLiteral("accepted")).toBool() ||
            !wait_ready(
                [&]
                {
                    return !presenter->inspect()->previewLoading() &&
                           !presenter->previewWorkActive();
                }))
            return false;
        check_geometry();
        check_navigator_frame();
    }
    if (moved)
    {
        LOG_ERROR(logger(), "Quick exposure moved the navigator frame or viewport border");
        return false;
    }
    if (wrong_navigator_frame)
    {
        LOG_ERROR(logger(), "Gallery navigator displayed a frame outside the Grid thumbnail path");
        return false;
    }
    // Exercise the production Images with an original-size rectangle that does
    // not match the cached photo. Retained pixels and loading placeholders must
    // keep their own aspect while selection geometry is being replaced.
    auto *navigation_image =
        navigator->findChild<QQuickItem *>(QStringLiteral("navigatorLiveImage"));
    auto *placeholder = window->findChild<QQuickItem *>(QStringLiteral("previewPlaceholderImage"));
    const auto thumbnail_url = presenter->selectedThumbnailUrl();
    const QImage selected_thumbnail(thumbnail_url.toLocalFile());
    if (!navigation_image || !placeholder || selected_thumbnail.isNull())
        return false;
    // The placeholder is normally inactive in Grid. Supply the real selected
    // resource explicitly so this geometry test need not race a full render.
    placeholder->setProperty("source", thumbnail_url);
    if (!wait_ready(
            [&]
            {
                return placeholder->property("status").toInt() == 1 &&
                       navigation_image->property("status").toInt() == 1 &&
                       held_navigation->property("status").toInt() == 1;
            }))
        return false;
    presenter->inspect()->clear_displayed_preview();
    const double pixel_aspect =
        static_cast<double>(selected_thumbnail.width()) / selected_thumbnail.height();
    for (const auto size : {QSize{1600, 1000}, QSize{1000, 1600}, QSize{1000, 1000}})
    {
        presenter->inspect()->seedViewport(size.width(), size.height());
        presenter->inspect()->notifyPreviewChanged();
        if (!wait_ready(
                [&]
                {
                    for (auto *item : {navigation_image, held_navigation, placeholder})
                    {
                        const auto width = item->property("paintedWidth").toDouble();
                        const auto height = item->property("paintedHeight").toDouble();
                        if (height <= 0 || std::abs(width / height - pixel_aspect) > 0.001)
                            return false;
                    }
                    return true;
                }))
        {
            LOG_ERROR(logger(), "Selection geometry stretched retained or placeholder pixels");
            return false;
        }
    }
    placeholder->setProperty("source", QUrl{});
    return true;
}
} // namespace ravo
