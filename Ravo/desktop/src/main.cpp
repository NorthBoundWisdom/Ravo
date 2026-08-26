#include <QCoreApplication>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QString>
#include <QUrl>

#include "ravo/desktop/studio_command_controller.h"
#include "ravo/desktop/studio_presenter.h"
#include "ravo/foundation/log.h"
#include "studio_image_providers.h"
#include "studio_language_manager.h"

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
    const QStringList arguments = QCoreApplication::arguments();
    const bool smoke = arguments.contains(QStringLiteral("--smoke"));
    QString catalog_path;
    QString requested_language;
    for (int index = 1; index < arguments.size(); ++index)
    {
        const QString &argument = arguments.at(index);
        if (argument == QLatin1String("--catalog") && index + 1 < arguments.size())
        {
            catalog_path = arguments.at(++index);
            continue;
        }
        if (argument.startsWith(QLatin1String("--catalog=")))
        {
            catalog_path = argument.mid(QStringLiteral("--catalog=").size());
            continue;
        }
        if (argument == QLatin1String("--language"))
        {
            if (index + 1 >= arguments.size() || arguments.at(index + 1).trimmed().isEmpty())
            {
                LOG_ERROR(ravo::logger(), "--language requires en_US or zh_CN");
                ravo::shutdown_logging();
                return 1;
            }
            requested_language = arguments.at(++index);
            continue;
        }
        if (argument.startsWith(QLatin1String("--language=")))
        {
            requested_language = argument.mid(QStringLiteral("--language=").size());
            if (requested_language.trimmed().isEmpty())
            {
                LOG_ERROR(ravo::logger(), "--language requires en_US or zh_CN");
                ravo::shutdown_logging();
                return 1;
            }
        }
    }
    LOG_INFO(ravo::logger(), "Ravo Studio starting");

    ravo::StudioLanguageManager language_manager;
    if (!language_manager.initialize(requested_language) && !requested_language.isEmpty())
    {
        LOG_ERROR(ravo::logger(), "requested UI language failed: {}",
                  requested_language.toStdString());
        ravo::shutdown_logging();
        return 1;
    }
    ravo::StudioPresenter presenter;
    ravo::StudioCommandController command_controller(presenter);
    QObject::connect(&language_manager, &ravo::StudioLanguageManager::languageChanged,
                     &command_controller, &ravo::StudioCommandController::retranslate);
    if (!catalog_path.isEmpty())
    {
        presenter.setStartupCatalogPath(QFileInfo(catalog_path).absoluteFilePath());
        LOG_INFO(ravo::logger(), "startup catalog path={}",
                 presenter.startupCatalogPath().toStdString());
    }
    QQmlApplicationEngine engine;
    language_manager.setQmlEngine(&engine);
    engine.addImportPath(QStringLiteral("qrc:/"));
    engine.addImageProvider(QStringLiteral("studioPreview"),
                            new ravo::StudioPreviewImageProvider(presenter));
    engine.addImageProvider(QStringLiteral("studioScope"),
                            new ravo::StudioScopeImageProvider(presenter));
    engine.rootContext()->setContextProperty(QStringLiteral("studio"), &presenter);
    engine.rootContext()->setContextProperty(QStringLiteral("studioCommands"), &command_controller);
    engine.rootContext()->setContextProperty(QStringLiteral("studioLanguage"), &language_manager);
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &application,
        []() { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    if (smoke)
    {
        QObject::connect(
            &engine, &QQmlApplicationEngine::objectCreated, &application,
            [](QObject *object, const QUrl &)
            { QCoreApplication::exit(object == nullptr ? 1 : 0); }, Qt::QueuedConnection);
    }
    engine.loadFromModule("Ravo.Studio", "Main");
    const int exit_code = QGuiApplication::exec();
    ravo::shutdown_logging();
    return exit_code;
}
