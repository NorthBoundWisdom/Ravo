#include <QCoreApplication>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QResource>

#include "ravo/desktop/studio_presenter.h"
#include "ravo/foundation/log.h"

void qml_register_types_GeoControls();
void qml_register_types_GeoControls_AppShell();

int main(int argc, char *argv[])
{
    Q_INIT_RESOURCE(icons);
    qml_register_types_GeoControls();
    qml_register_types_GeoControls_AppShell();

    QGuiApplication application(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("Ravo Studio"));
    QGuiApplication::setOrganizationName(QStringLiteral("Ravo"));
    QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/ravo/studio/icons/AppIcon.png")));
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    ravo::init_logging("RavoStudio");
    RFLOG_INFO("Ravo Studio starting");

    ravo::StudioPresenter presenter;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("studio"), &presenter);
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &application,
        []() { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.loadFromModule("Ravo.Studio", "Main");
    const int exit_code = QGuiApplication::exec();
    ravo::shutdown_logging();
    return exit_code;
}
