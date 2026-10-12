#include "studio_photo_merge_layout_smoke.h"
#include "ravo/desktop/studio_command_controller.h"
#include "ravo/foundation/log.h"

#include <QCoreApplication>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QVariantMap>

namespace ravo
{
bool smoke_photo_merge_dialog(QQmlApplicationEngine &engine, StudioCommandController &commands)
{
    if (engine.rootObjects().isEmpty())
        return false;
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().front());
    auto *dialog =
        engine.rootObjects().front()->findChild<QObject *>(QStringLiteral("PhotoMergeDialog"));
    if (!window || !dialog)
        return false;
    const auto original_width = window->width(), original_height = window->height();
    QStringList sources;
    for (int i = 0; i < 16; ++i)
        sources.push_back(QStringLiteral("photo-%1.CR2").arg(i));
    const auto context = QVariantMap{{QStringLiteral("kind"), QStringLiteral("hdr")},
                                     {QStringLiteral("token"), QStringLiteral("smoke-only")},
                                     {QStringLiteral("count"), 16},
                                     {QStringLiteral("sources"), sources}};
    bool valid = true;
    for (const auto size : {QSize(960, 640), QSize(1280, 800)})
    {
        window->resize(size);
        if (!QMetaObject::invokeMethod(dialog, "openForContext",
                                       Q_ARG(QVariant, QVariant(context))))
        {
            valid = false;
            break;
        }
        QCoreApplication::processEvents();
        const auto *body = dialog->property("bodyItem").value<QQuickItem *>();
        const auto *footer = dialog->property("footerItem").value<QQuickItem *>();
        valid = dialog->property("visible").toBool() && commands.modalOpen() && body && footer &&
                body->height() > 0 && footer->height() > 0 &&
                dialog->property("height").toDouble() <= window->height();
        QMetaObject::invokeMethod(dialog, "close");
        QCoreApplication::processEvents();
        valid = valid && !commands.modalOpen();
        if (!valid)
            break;
    }
    window->resize(original_width, original_height);
    if (!valid)
        LOG_ERROR(ravo::logger(),
                  "Photo merge dialog smoke failed parent/scroll/footer/modal contract");
    return valid;
}
} // namespace ravo
