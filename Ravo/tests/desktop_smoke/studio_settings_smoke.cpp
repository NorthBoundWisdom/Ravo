#include "ravo/desktop/studio_command_controller.h"
#include "ravo/desktop/studio_update_controller.h"
#include "ravo/foundation/log.h"
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <QDir>
#include <QKeyEvent>
#include <QUrl>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>

namespace ravo
{
bool smoke_settings(QQmlApplicationEngine &engine, StudioCommandController &commands,
                    StudioUpdateController &updates)
{
    auto *window = engine.rootObjects().isEmpty() ?
                       nullptr :
                       qobject_cast<QQuickWindow *>(engine.rootObjects().front());
    auto *page = window ? window->findChild<QObject *>("settingsWorkspace") : nullptr;
    if (!window || !page)
        return false;
    auto *folder = window->findChild<QObject *>("settingsBackupFolderDialog");
    const auto path = QDir::temp().filePath(QString::fromUtf8("Ravo backup 目录 #%"));
    const auto url = QUrl::fromLocalFile(path);
    for (const auto &input : {QVariant(url), QVariant(url.toString(QUrl::FullyEncoded))})
    {
        QVariant converted;
        if (!folder ||
            !QMetaObject::invokeMethod(folder, "toLocalFile", Q_RETURN_ARG(QVariant, converted),
                                       Q_ARG(QVariant, input)) ||
            converted.toString() != path)
        {
            LOG_ERROR(ravo::logger(),
                      "Settings folder selection must preserve native Unicode and escaped paths");
            return false;
        }
    }
    const auto size = window->size();
    const bool was_visible = window->isVisible();
    window->show();
    bool valid = true;
    commands.setSettingsOpen(true);
    for (const auto &section : {"general", "workspace", "backup", "assistant"})
    {
        auto *panel = page->findChild<QQuickItem *>(QStringLiteral("settingsSection_") + section);
        if (!panel || !panel->property("expanded").toBool())
        {
            LOG_ERROR(ravo::logger(), "Settings panel must be expanded initially: {}", section);
            valid = false;
        }
    }
    for (const auto dimensions : {QSize(640, 480), QSize(1280, 800)})
    {
        window->resize(dimensions);
        for (const auto &section : {QStringLiteral("general"), QStringLiteral("workspace"),
                                    QStringLiteral("backup"), QStringLiteral("assistant")})
        {
            commands.selectSettingsSection(section);
            QEventLoop layout;
            QTimer::singleShot(40, &layout, &QEventLoop::quit);
            layout.exec();
            auto *scroll = page->findChild<QQuickItem *>("settingsScroll");
            auto *content =
                page->findChild<QQuickItem *>(section == "general"   ? "settingsLanguage" :
                                              section == "workspace" ? "settingsResetLayout" :
                                              section == "backup"    ? "settingsBackupSection" :
                                                                       "settingsAssistantSection");
            const double dialog_width = page->property("width").toDouble();
            const double dialog_height = page->property("height").toDouble();
            auto *panel =
                page->findChild<QQuickItem *>(QStringLiteral("settingsSection_") + section);
            auto *flickable = scroll ? scroll->property("contentItem").value<QObject *>() : nullptr;
            const double content_y = flickable ? flickable->property("contentY").toDouble() : -1;
            const bool section_valid =
                page->property("visible").toBool() && page->property("modal").toBool() && scroll &&
                content && panel && dialog_width > 0 && dialog_width < window->width() &&
                dialog_height > 0 && dialog_height < window->height() && scroll->width() > 0 &&
                scroll->height() > 0 && content->width() > 0 && content->height() > 0 &&
                scroll->property("rightPadding").toDouble() > 0 &&
                panel->mapToItem(scroll, QPointF(panel->width(), 0)).x() <=
                    scroll->width() - scroll->property("rightPadding").toDouble() + 1 &&
                content->width() <= panel->width() + 1 && panel->property("expanded").toBool() &&
                content_y >= 0 && panel->y() >= content_y - 1 &&
                panel->y() < content_y + scroll->height();
            valid = valid && section_valid;
            if (section == QLatin1String("general") && section_valid)
            {
                auto *toggle = page->findChild<QObject *>("settingsAutomaticUpdates");
                const bool previous = updates.automatic();
                bool update_valid = toggle && toggle->property("checked").toBool() == previous &&
                                    toggle->setProperty("checked", !previous) &&
                                    QMetaObject::invokeMethod(toggle, "clicked") &&
                                    updates.automatic() == !previous;
                StudioUpdateController reopened;
                update_valid = update_valid && reopened.initialize() &&
                               reopened.automatic() == !previous &&
                               reopened.currentVersion() == QCoreApplication::applicationVersion();
                // Restore through the same production control intent.
                update_valid = update_valid && toggle->setProperty("checked", previous) &&
                               QMetaObject::invokeMethod(toggle, "clicked") &&
                               updates.automatic() == previous;
                valid = valid && update_valid;
                if (!update_valid)
                    LOG_ERROR(ravo::logger(), "Settings update preference smoke failed");
            }
            if (!section_valid)
                LOG_ERROR(
                    ravo::logger(),
                    "Settings smoke failed section={} width={} visible={} scroll={}x{} content={}x{} dialog={}x{} panel_y={} scroll_y={}",
                    section.toStdString(), dimensions.width(), page->property("visible").toBool(),
                    scroll ? scroll->width() : -1, scroll ? scroll->height() : -1,
                    content ? content->width() : -1, content ? content->height() : -1, dialog_width,
                    dialog_height, panel ? panel->y() : -1, content_y);
        }
    }
    auto *assistant_panel = page->findChild<QQuickItem *>("settingsSection_assistant");
    if (!assistant_panel || !assistant_panel->setProperty("expanded", false))
        valid = false;
    commands.setSettingsOpen(false);
    valid = valid && !page->property("visible").toBool();
    commands.setSettingsOpen(true);
    QEventLoop reopen;
    QTimer::singleShot(40, &reopen, &QEventLoop::quit);
    reopen.exec();
    valid = valid && assistant_panel && assistant_panel->property("expanded").toBool();
    // The actual close control must synchronize the C++ command state too.
    auto *close_button = page->findChild<QObject *>("settingsClose");
    valid = valid && close_button && QMetaObject::invokeMethod(close_button, "clicked") &&
            !commands.settingsOpen() && !page->property("visible").toBool();
    commands.setSettingsOpen(true);
    auto *dialog_content =
        qobject_cast<QQuickItem *>(page->property("contentItem").value<QObject *>());
    if (dialog_content)
        dialog_content->forceActiveFocus();
    QKeyEvent escape_press(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QKeyEvent escape_release(QEvent::KeyRelease, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &escape_press);
    QCoreApplication::sendEvent(window, &escape_release);
    valid = valid && !commands.settingsOpen() && !page->property("visible").toBool();
    if (!valid)
        LOG_ERROR(ravo::logger(), "Settings modal layout/close/reopen smoke failed");
    commands.setSettingsOpen(false);
    commands.selectSettingsSection("general");
    window->resize(size);
    if (!was_visible)
        window->hide();
    return valid;
}
} // namespace ravo
