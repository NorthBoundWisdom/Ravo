#include <QColor>
#include <QColorSpace>
#include <QCoreApplication>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QIcon>
#include <QPalette>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QString>
#include <QStyleHints>
#include <QSurfaceFormat>
#include <QUrl>

#include "ravo/desktop/studio_assistant_controller.h"
#include "ravo/desktop/studio_command_controller.h"
#include "ravo/desktop/studio_presenter.h"
#include "ravo/foundation/log.h"
#include "studio_image_providers.h"
#include "studio_language_manager.h"

void qml_register_types_GeoControls();
void qml_register_types_GeoControls_AppShell();

namespace
{

bool generic_font_family(const QString &family)
{
    const QString name = family.trimmed().toLower();
    return name.isEmpty() || name == QLatin1String("sans serif") ||
           name == QLatin1String("sans-serif") || name == QLatin1String("serif") ||
           name == QLatin1String("monospace") || name == QLatin1String("cursive") ||
           name == QLatin1String("fantasy") || name == QLatin1String("system-ui") ||
           name == QLatin1String("ui-sans-serif") || name == QLatin1String("ui-serif") ||
           name == QLatin1String("ui-monospace") || name == QLatin1String("ui-rounded") ||
           name == QLatin1String("sans");
}

QString installed_family(const QStringList &installed, const QString &family)
{
    if (generic_font_family(family))
    {
        return {};
    }
    for (const QString &have : installed)
    {
        if (have.compare(family, Qt::CaseInsensitive) == 0)
        {
            return have;
        }
    }
    return {};
}

void append_family(QStringList &families, const QString &family)
{
    if (!family.isEmpty() && !families.contains(family))
    {
        families.push_back(family);
    }
}

QString first_public_family(QFontDatabase::WritingSystem writing_system)
{
    for (const QString &family : QFontDatabase::families(writing_system))
    {
        if (!generic_font_family(family) && !QFontDatabase::isPrivateFamily(family))
        {
            return family;
        }
    }
    return {};
}

// Offscreen QPA and Qt generic families request "Sans Serif", which is not a
// CoreText face. Never look up missing names; keep the platform system font
// when it is real, then match fallbacks against QFontDatabase::families().
QStringList studio_ui_font_families(const QFont &system_font)
{
    const QStringList installed = QFontDatabase::families();
    QStringList families;

    auto append_installed = [&](const QString &family)
    {
        append_family(families, installed_family(installed, family));
    };

    for (const QString &family : system_font.families())
    {
        if (generic_font_family(family))
        {
            continue;
        }
        const QString canonical = installed_family(installed, family);
        append_family(families, canonical.isEmpty() ? family : canonical);
    }
    if (families.isEmpty() && !generic_font_family(system_font.family()))
    {
        const QString canonical = installed_family(installed, system_font.family());
        append_family(families, canonical.isEmpty() ? system_font.family() : canonical);
    }

    if (families.isEmpty())
    {
        for (const auto &family :
             {QStringLiteral("Segoe UI"), QStringLiteral("Noto Sans"),
              QStringLiteral("DejaVu Sans"), QStringLiteral("Liberation Sans"),
              QStringLiteral("Ubuntu"), QStringLiteral("Cantarell"),
              QStringLiteral("FreeSans"), QStringLiteral("Lucida Grande"),
              QStringLiteral("Helvetica Neue"), QStringLiteral("Helvetica"),
              QStringLiteral("Arial")})
        {
            append_installed(family);
            if (!families.isEmpty())
            {
                break;
            }
        }
    }
    if (families.isEmpty())
    {
        append_family(families, first_public_family(QFontDatabase::Latin));
    }

    const qsizetype latin_count = families.size();
    for (const auto &family :
         {QStringLiteral("PingFang SC"), QStringLiteral("Hiragino Sans GB"),
          QStringLiteral("Songti SC"), QStringLiteral("Noto Sans CJK SC"),
          QStringLiteral("Noto Sans SC"), QStringLiteral("Source Han Sans SC"),
          QStringLiteral("Microsoft YaHei UI"), QStringLiteral("Microsoft YaHei"),
          QStringLiteral("WenQuanYi Zen Hei"), QStringLiteral("WenQuanYi Micro Hei"),
          QStringLiteral("Droid Sans Fallback")})
    {
        append_installed(family);
    }
    if (families.size() == latin_count)
    {
        append_family(families, first_public_family(QFontDatabase::SimplifiedChinese));
    }
    return families;
}

} // namespace

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

    // DarkThemePalette tokens are authored sRGB. Request an sRGB default
    // framebuffer so Linux display-profile color management does not remap
    // those hex colors (or preview pixels tagged sRGB) through a mis-read ICC.
    QSurfaceFormat surface_format = QSurfaceFormat::defaultFormat();
    surface_format.setColorSpace(QColorSpace(QColorSpace::SRgb));
    QSurfaceFormat::setDefaultFormat(surface_format);

#if defined(Q_OS_LINUX)
    // xcb/wayland otherwise auto-select the gtk3 platform theme, which paints
    // a desktop light QPalette over ApplicationWindow.palette.
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORMTHEME"))
        qputenv("QT_QPA_PLATFORMTHEME", "xdgdesktopportal");
#endif

    QGuiApplication application(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("Ravo Studio"));
    QGuiApplication::setOrganizationName(QStringLiteral("Ravo"));
#if defined(Q_OS_LINUX)
    QPalette linux_palette;
    linux_palette.setColor(QPalette::Window, QColor(0x1c, 0x1c, 0x1c));
    linux_palette.setColor(QPalette::WindowText, QColor(0xe6, 0xe6, 0xe6));
    linux_palette.setColor(QPalette::Base, QColor(0x2b, 0x2b, 0x2b));
    linux_palette.setColor(QPalette::AlternateBase, QColor(0x32, 0x32, 0x32));
    linux_palette.setColor(QPalette::Text, QColor(0xe6, 0xe6, 0xe6));
    linux_palette.setColor(QPalette::Button, QColor(0x3a, 0x3a, 0x3a));
    linux_palette.setColor(QPalette::ButtonText, QColor(0xe8, 0xe8, 0xe8));
    linux_palette.setColor(QPalette::Light, QColor(0x5a, 0x5a, 0x5a));
    linux_palette.setColor(QPalette::Midlight, QColor(0x40, 0x40, 0x40));
    linux_palette.setColor(QPalette::Mid, QColor(0x5c, 0x5c, 0x5c));
    linux_palette.setColor(QPalette::Dark, QColor(0x12, 0x12, 0x12));
    linux_palette.setColor(QPalette::Shadow, QColor(0, 0, 0, 0x99));
    linux_palette.setColor(QPalette::Highlight, QColor(0xc8, 0xc8, 0xc8));
    linux_palette.setColor(QPalette::HighlightedText, QColor(0x1a, 0x1a, 0x1a));
    linux_palette.setColor(QPalette::PlaceholderText, QColor(0x9a, 0x9a, 0x9a));
    linux_palette.setColor(QPalette::Link, QColor(0xc8, 0xc8, 0xc8));
    linux_palette.setColor(QPalette::Accent, QColor(0xc8, 0xc8, 0xc8));
    QGuiApplication::setPalette(linux_palette);
    QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Dark);
#endif
#ifndef Q_OS_MACOS
    // macOS Dock/Finder use the bundle ICNS. A single 1024 PNG window icon
    // replaces those sized representations and reads one stop too large.
    QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/ravo/studio/icons/AppIcon.ico")));
#endif

    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QFont ui_font = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    const QStringList families = studio_ui_font_families(ui_font);
    if (!families.isEmpty())
    {
        ui_font.setFamilies(families);
        ui_font.setStyleHint(QFont::AnyStyle);
    }
    QGuiApplication::setFont(ui_font);
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
    ravo::StudioAssistantController assistant_controller;
    if (!assistant_controller.initialize())
    {
        LOG_ERROR(ravo::logger(), "assistant settings failed to initialize");
        ravo::shutdown_logging();
        return 1;
    }
    QObject::connect(&language_manager, &ravo::StudioLanguageManager::languageChanged,
                     &command_controller, &ravo::StudioCommandController::retranslate);
    QObject::connect(&language_manager, &ravo::StudioLanguageManager::languageChanged, &presenter,
                     &ravo::StudioPresenter::retranslate);
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
    engine.rootContext()->setContextProperty(QStringLiteral("studioAssistant"),
                                             &assistant_controller);
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &application,
        []() { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreated, &application,
        [smoke](QObject *object, const QUrl &)
        {
            if (smoke)
            {
                QCoreApplication::exit(object == nullptr ? 1 : 0);
            }
        },
        Qt::QueuedConnection);
    engine.loadFromModule("Ravo.Studio", "Main");
    const int exit_code = QGuiApplication::exec();
    ravo::shutdown_logging();
    return exit_code;
}
