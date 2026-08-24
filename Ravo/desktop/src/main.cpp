#include <QCoreApplication>
#include <QFont>
#include <QFontDatabase>
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
    // GeoControls is a static NO_PLUGIN QML module under qrc:/GeoControls, not
    // qrc:/qt/qml. Force-init those resources so dead-stripped static constructors
    // cannot leave the AppShell URI registered without MainStatusBar.qml.
    Q_INIT_RESOURCE(icons);
    Q_INIT_RESOURCE(qmake_GeoControls);
    Q_INIT_RESOURCE(GeoControlsControls_raw_qml_0);
    Q_INIT_RESOURCE(qmake_GeoControls_AppShell);
    Q_INIT_RESOURCE(GeoControlsAppShell_raw_qml_0);
    qml_register_types_GeoControls();
    qml_register_types_GeoControls_AppShell();

    QGuiApplication application(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("Ravo Studio"));
    QGuiApplication::setOrganizationName(QStringLiteral("Ravo"));
#ifndef Q_OS_MACOS
    // macOS Dock/Finder use the bundle ICNS. A single 1024 PNG window icon
    // replaces those sized representations and reads one stop too large.
    QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/ravo/studio/icons/AppIcon.ico")));
#endif

    QFont ui_font = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    QStringList families = ui_font.families();
    if (families.isEmpty() && !ui_font.family().isEmpty())
    {
        families.push_back(ui_font.family());
    }
    for (const auto &family :
         {QStringLiteral("PingFang SC"), QStringLiteral("Hiragino Sans GB"),
          QStringLiteral("Songti SC"), QStringLiteral("Noto Sans CJK SC"),
          QStringLiteral("Noto Sans SC"), QStringLiteral("Microsoft YaHei UI"),
          QStringLiteral("Microsoft YaHei"), QStringLiteral("Source Han Sans SC")})
    {
        if (!families.contains(family))
        {
            families.push_back(family);
        }
    }
    ui_font.setFamilies(families);
    QGuiApplication::setFont(ui_font);
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    ravo::init_logging("RavoStudio");
    LOG_INFO(ravo::logger(), "Ravo Studio starting");

    ravo::StudioPresenter presenter;
    QQmlApplicationEngine engine;
    engine.addImportPath(QStringLiteral("qrc:/"));
    engine.rootContext()->setContextProperty(QStringLiteral("studio"), &presenter);
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &application,
        []() { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.loadFromModule("Ravo.Studio", "Main");
    const int exit_code = QGuiApplication::exec();
    ravo::shutdown_logging();
    return exit_code;
}
