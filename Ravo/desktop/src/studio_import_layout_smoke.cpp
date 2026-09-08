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
#include <QImage>
#include "ravo/desktop/import_candidate_list_model.h"
#include "ravo/desktop/studio_command_controller.h"
#include <QMetaObject>
#include <QKeySequence>
#include <QCoreApplication>
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

            // Select All through production StudioCommandShortcuts / StudioActions wiring.
            // openImportPage() requires an open catalog; workspace.visible alone is insufficient.
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
                if (presenter->importPageOpen())
                    presenter->closeImportPage();
                presenter->importFilePaths({seed_path});
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
            presenter->openImportPage();
            QEventLoop open_page;
            QTimer::singleShot(30, &open_page, &QEventLoop::quit);
            open_page.exec();
            if (!presenter->importPageOpen())
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
            // Filename template lives in copy/move mode and starts collapsed.
            // Use the presenter API so the field text is a non-empty production template.
            presenter->setImportMode(QStringLiteral("copy"));
            presenter->setImportDestination(select_all_dir.path());
            presenter->setImportFilenameTemplate(QStringLiteral("RavoSelectAll_{date}_{seq}"));
            for (QObject *parent = field->parent(); parent; parent = parent->parent())
            {
                if (parent->property("expanded").isValid())
                {
                    parent->setProperty("expanded", true);
                    break;
                }
            }
            QEventLoop expand_loop;
            QTimer::singleShot(30, &expand_loop, &QEventLoop::quit);
            expand_loop.exec();
            presenter->setImportSourceRoot(source);
            QElapsedTimer scan_timer;
            scan_timer.start();
            while (presenter->importScanActive() || presenter->importCandidates()->rowCount() < 3 ||
                   presenter->importScanTotal() < 3)
            {
                if (scan_timer.elapsed() > 30000)
                {
                    LOG_ERROR(
                        logger(), "Import scan for Select All smoke timed out rows={} total={}",
                        presenter->importCandidates()->rowCount(), presenter->importScanTotal());
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
            presenter->importCandidates()->highlightExclusive(0);
            QEventLoop candidate_ready;
            QTimer::singleShot(30, &candidate_ready, &QEventLoop::quit);
            candidate_ready.exec();
            const int eligible = [&]()
            {
                int count = 0;
                auto *model = presenter->importCandidates();
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
            for (int row = 0; row < presenter->importCandidates()->rowCount(); ++row)
            {
                if (presenter->importCandidates()
                        ->data(presenter->importCandidates()->index(row, 0),
                               ImportCandidateListModel::DuplicateRole)
                        .toBool())
                    ++duplicate_rows;
            }
            if (duplicate_rows < 1)
            {
                LOG_ERROR(logger(), "Select All smoke needs an ineligible duplicate candidate");
                return false;
            }
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
                auto *model = presenter->importCandidates();
                for (int row = 0; row < model->rowCount(); ++row)
                    count += model->highlighted(row) ? 1 : 0;
                return count;
            };
            const auto exact_eligible_highlighted = [&]()
            {
                auto *model = presenter->importCandidates();
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
            presenter->importCandidates()->highlightExclusive(0);
            int highlight_batches = 0;
            QMetaObject::Connection highlight_watch =
                QObject::connect(presenter->importCandidates(), &QAbstractItemModel::dataChanged,
                                 presenter->importCandidates(),
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
            presenter->importCandidates()->highlightExclusive(0);
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
            presenter->importCandidates()->highlightExclusive(0);
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
            for (auto *model :
                 {presenter->importSourceFolders(), presenter->importDestinationFolders()})
                model->resetWithRoots({{directory.path(), "Pictures", true}});
            presenter->setImportDestination(directory.path());
            presenter->importCandidates()->setCandidates({candidate});
            QEventLoop restore_layout;
            QTimer::singleShot(40, &restore_layout, &QEventLoop::quit);
            restore_layout.exec();
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
