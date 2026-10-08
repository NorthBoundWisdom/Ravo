#include "studio_curve_gesture_smoke.h"

#include <memory>
#include <QCoreApplication>
#include <QEventLoop>
#include <QMouseEvent>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>
#include <QVariantMap>
#include <QWheelEvent>

#include "ravo/foundation/log.h"

namespace ravo
{
bool smoke_curve_gesture(QQmlApplicationEngine &engine)
{
    const auto fail = [](const char *message)
    {
        LOG_ERROR(logger(), "Curve gesture smoke: {}", message);
        return false;
    };
    const auto pump = []
    {
        QEventLoop loop;
        QTimer::singleShot(20, &loop, &QEventLoop::quit);
        loop.exec();
    };
    QQuickWindow window;
    window.resize(420, 360);
    QQmlComponent viewport_component(&engine);
    viewport_component.setData(R"(
        import QtQuick
        Flickable {
            id: viewport
            width: 420; height: 360
            contentWidth: width; contentHeight: 900
            boundsBehavior: Flickable.StopAtBounds
            property var editor: null
            property int edits: 0
            property int commits: 0
            property var committedPoints: []
            Connections {
                target: viewport.editor
                function onCurveEdited(points) { viewport.edits += 1; }
                function onCurveCommitted(points) {
                    viewport.commits += 1;
                    viewport.committedPoints = points;
                }
            }
        }
    )",
                               QUrl(QStringLiteral("qrc:/curve-gesture-smoke.qml")));
    std::unique_ptr<QObject> viewport_owner(viewport_component.create());
    auto *viewport = qobject_cast<QQuickItem *>(viewport_owner.get());
    if (!viewport)
        return fail("unable to create Flickable");
    viewport->setParentItem(window.contentItem());
    auto *content = viewport->property("contentItem").value<QQuickItem *>();
    QQmlComponent curve_component(
        &engine, QUrl(QStringLiteral("qrc:/qt/qml/Ravo/Studio/qml/inspect/ToneCurveEditor.qml")));
    const QVariantList points{QVariantMap{{QStringLiteral("x"), 0.0}, {QStringLiteral("y"), 0.0}},
                              QVariantMap{{QStringLiteral("x"), 0.5}, {QStringLiteral("y"), 0.5}},
                              QVariantMap{{QStringLiteral("x"), 1.0}, {QStringLiteral("y"), 1.0}}};
    std::unique_ptr<QObject> curve_owner(curve_component.createWithInitialProperties(
        {{QStringLiteral("scrollViewport"), QVariant::fromValue(viewport)},
         {QStringLiteral("width"), 400},
         {QStringLiteral("height"), 220},
         {QStringLiteral("x"), 10},
         {QStringLiteral("y"), 100},
         {QStringLiteral("points"), points}}));
    auto *curve = qobject_cast<QQuickItem *>(curve_owner.get());
    if (!content || !curve)
        return fail("unable to create production curve editor");
    curve->setParentItem(content);
    viewport->setProperty("editor", QVariant::fromValue(curve));
    viewport->setProperty("contentY", 80.0);
    window.show();
    pump();
    unsigned long timestamp = 0;
    const auto mouse =
        [&](QEvent::Type type, QPointF point, Qt::MouseButton button, Qt::MouseButtons buttons)
    {
        QMouseEvent event(type, point, window.mapToGlobal(point.toPoint()), button, buttons,
                          Qt::NoModifier);
        event.setTimestamp(timestamp += type == QEvent::MouseButtonPress ? 1000UL : 16UL);
        QCoreApplication::sendEvent(&window, &event);
        pump();
    };
    const auto center = [&]
    { return curve->mapToScene(QPointF(curve->width() / 2, curve->height() / 2)); };
    const auto wheel = [&](QPointF point, Qt::MouseButtons buttons)
    {
        QWheelEvent event(point, window.mapToGlobal(point.toPoint()), QPoint(0, -60),
                          QPoint(0, -120), buttons, Qt::NoModifier, Qt::NoScrollPhase, false);
        event.setTimestamp(timestamp += 16);
        QCoreApplication::sendEvent(&window, &event);
        pump();
    };
    const auto begin = center();
    const auto initial_y = viewport->property("contentY").toDouble();
    mouse(QEvent::MouseButtonPress, begin, Qt::LeftButton, Qt::LeftButton);
    if (viewport->property("interactive").toBool())
        return fail("press did not suspend scrolling");
    mouse(QEvent::MouseMove, begin + QPointF(45, -55), Qt::NoButton, Qt::LeftButton);
    wheel(begin, Qt::LeftButton);
    if (viewport->property("contentY").toDouble() != initial_y ||
        viewport->property("edits").toInt() == 0)
        return fail("drag or wheel moved the parent instead of only the curve");
    mouse(QEvent::MouseButtonRelease, QPointF(415, 350), Qt::LeftButton, Qt::NoButton);
    const auto committed = viewport->property("committedPoints").toList();
    if (!viewport->property("interactive").toBool() || viewport->property("commits").toInt() != 1 ||
        committed.size() != 3 || committed[1].toMap().value(QStringLiteral("x")).toDouble() <= 0.5)
        return fail("outside release did not commit once and restore scrolling");
    wheel(center(), Qt::NoButton);
    if (viewport->property("contentY").toDouble() == initial_y)
        return fail("ordinary wheel scrolling did not resume");
    QMetaObject::invokeMethod(viewport, "cancelFlick");
    viewport->setProperty("contentY", initial_y);
    mouse(QEvent::MouseButtonPress, center(), Qt::LeftButton, Qt::LeftButton);
    curve->setProperty("editorEnabled", false);
    pump();
    if (!viewport->property("interactive").toBool() || viewport->property("commits").toInt() != 1)
    {
        LOG_ERROR(logger(), "Curve cancel state: interactive={} commits={}",
                  viewport->property("interactive").toBool(),
                  viewport->property("commits").toInt());
        return fail("cancel did not restore scrolling without a commit");
    }
    mouse(QEvent::MouseButtonRelease, center(), Qt::LeftButton, Qt::NoButton);
    curve->setProperty("editorEnabled", true);
    viewport->setProperty("interactive", false);
    mouse(QEvent::MouseButtonPress, center(), Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, center(), Qt::LeftButton, Qt::NoButton);
    if (viewport->property("interactive").toBool())
        return fail("release overwrote an already-disabled scroll owner");
    viewport->setProperty("interactive", true);
    mouse(QEvent::MouseButtonPress, center(), Qt::LeftButton, Qt::LeftButton);
    curve_owner.reset();
    pump();
    if (!viewport->property("interactive").toBool())
        return fail("editor destruction left the parent locked");
    mouse(QEvent::MouseButtonRelease, QPointF(415, 350), Qt::LeftButton, Qt::NoButton);
    return true;
}
} // namespace ravo
