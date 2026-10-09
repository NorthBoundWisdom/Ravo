#include "ravo/desktop/studio_command_controller.h"
#include "ravo/foundation/log.h"
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <QDir>
#include <QUrl>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>

namespace ravo
{
bool smoke_settings(QQmlApplicationEngine &engine, StudioCommandController &commands)
{
    auto *window = engine.rootObjects().isEmpty() ?
                       nullptr :
                       qobject_cast<QQuickWindow *>(engine.rootObjects().front());
    auto *page = window ? window->findChild<QQuickItem *>("settingsWorkspace") : nullptr;
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
            const bool section_valid = page->isVisible() && scroll && content &&
                                       scroll->width() > 0 && scroll->height() > 0 &&
                                       content->width() > 0 && content->height() > 0;
            valid = valid && section_valid;
            if (!section_valid)
                LOG_ERROR(
                    ravo::logger(),
                    "Settings smoke failed section={} width={} visible={} scroll={}x{} content={}x{}",
                    section.toStdString(), dimensions.width(), page->isVisible(),
                    scroll ? scroll->width() : -1, scroll ? scroll->height() : -1,
                    content ? content->width() : -1, content ? content->height() : -1);
        }
    }
    commands.setSettingsOpen(false);
    commands.selectSettingsSection("general");
    window->resize(size);
    if (!was_visible)
        window->hide();
    return valid;
}
} // namespace ravo
