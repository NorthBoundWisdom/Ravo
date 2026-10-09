#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

#include <QByteArray>
#include <QColor>
#include <QColorSpace>
#include <QCoreApplication>
#include <QEventLoop>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QIcon>
#include <QJSValue>
#include <QPalette>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScopeGuard>
#include "studio_import_layout_smoke.h"
#include "studio_curve_gesture_smoke.h"
#include "studio_photo_merge_layout_smoke.h"
#include <QQuickStyle>
#include <QString>
#include <QStyleHints>
#include <QSurfaceFormat>
#include <QSettings>
#include <QQmlProperty>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QtLogging>

#include "ravo/desktop/studio_assistant_controller.h"
#include "ravo/desktop/studio_command_controller.h"
#include "ravo/desktop/studio_live_session_controller.h"
#include "ravo/desktop/studio_presenter.h"
#include "ravo/desktop/export_option_conversion.h"
#include "ravo/desktop/studio_window_geometry.h"
#include "ravo/desktop/studio_panel_layout.h"
#include "ravo/desktop/studio_display_presentation.h"
#include "ravo/foundation/log.h"
#include "studio_image_providers.h"
#include "studio_language_manager.h"
#include "studio_startup_controller.h"

void qml_register_types_GeoControls();
void qml_register_types_GeoControls_AppShell();

namespace
{

struct PreviewPresentationTrace
{
    std::optional<std::uint64_t> pending_revision;
    std::chrono::steady_clock::time_point intent_started_at{};
    std::int64_t intent_to_image_us = 0;
};

void smoke_message_handler(const QtMsgType type, const QMessageLogContext &, const QString &message)
{
    const QByteArray utf8 = message.toUtf8();
    std::fwrite(utf8.constData(), 1U, static_cast<std::size_t>(utf8.size()), stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
    if (type == QtFatalMsg)
        std::_Exit(EXIT_FAILURE);
}

bool smoke_startup_splash(QQmlApplicationEngine &engine)
{
    if (engine.rootObjects().isEmpty())
        return false;
    auto *main = qobject_cast<QQuickWindow *>(engine.rootObjects().front());
    auto *splash =
        main ? main->findChild<QQuickWindow *>(QStringLiteral("startupSplash")) : nullptr;
    auto *logo = splash ? splash->findChild<QQuickItem *>(QStringLiteral("startupLogo")) : nullptr;
    if (!main || !splash || !logo || main->isVisible() || splash->isVisible() ||
        splash->transientParent() != nullptr ||
        (splash->flags() & Qt::WindowType_Mask) != Qt::SplashScreen || splash->width() <= 0 ||
        splash->height() <= 0 || splash->width() >= main->width() ||
        splash->height() >= main->height() || logo->width() <= 0 || logo->height() <= 0 ||
        logo->opacity() <= 0 || logo->property("status").toInt() != 1) // Image.Ready
    {
        LOG_ERROR(ravo::logger(),
                  "Startup splash must be compact, independent and have a ready logo");
        return false;
    }
    splash->show();
    const bool independent = splash->isVisible() && !main->isVisible();
    splash->hide();
    if (!independent)
        LOG_ERROR(ravo::logger(), "Showing startup splash must leave the main window hidden");
    return independent;
}

bool smoke_panel_layout(QQmlApplicationEngine &engine, ravo::StudioPanelLayout &layout)
{
    auto *root = engine.rootObjects().isEmpty() ? nullptr : engine.rootObjects().front();
    auto *strip = root ? root->findChild<QObject *>(QStringLiteral("studioFilmstrip")) : nullptr;
    auto *left = root ? root->findChild<QObject *>(QStringLiteral("librarySidePanel")) : nullptr;
    auto *right = root ? root->findChild<QObject *>(QStringLiteral("inspectorSidePanel")) : nullptr;
    auto *split = root ? root->findChild<QObject *>(QStringLiteral("studioSidePanels")) : nullptr;
    if (!strip || !left || !right || !split)
    {
        LOG_ERROR(ravo::logger(), "Panel layout smoke missing production controls");
        return false;
    }
    const auto preferred = [](QObject *panel)
    {
        return QQmlProperty::read(panel, QStringLiteral("SplitView.preferredWidth"),
                                  qmlContext(panel))
            .toInt();
    };
    const auto set_preferred = [](QObject *panel, int width)
    {
        return QQmlProperty::write(panel, QStringLiteral("SplitView.preferredWidth"), width,
                                   qmlContext(panel));
    };
    if (!layout.setSideWidths(280, 390) || preferred(left) != 280 || preferred(right) != 390 ||
        !set_preferred(left, 305) || !set_preferred(right, 420) ||
        !QMetaObject::invokeMethod(split, "resizingChanged") || layout.leftWidth() != 305 ||
        layout.rightWidth() != 420 ||
        !QMetaObject::invokeMethod(strip, "panelHeightRequested", Q_ARG(double, 225.0)) ||
        layout.filmstripHeight() != 225 || !layout.flush())
    {
        LOG_ERROR(ravo::logger(), "Panel layout smoke intent failed left={} right={} bottom={}",
                  preferred(left), preferred(right), layout.filmstripHeight());
        return false;
    }
    ravo::StudioPanelLayout reopened;
    if (!reopened.initialize() || reopened.leftWidth() != 305 || reopened.rightWidth() != 420 ||
        reopened.filmstripHeight() != 225)
    {
        LOG_ERROR(ravo::logger(), "Panel layout smoke reload failed");
        return false;
    }
    // View constraints must not rewrite the preferred height during a small-window layout.
    strip->setProperty("maximumPanelHeight", 120);
    QCoreApplication::processEvents();
    const auto strip_preferred_height = [strip]()
    {
        return QQmlProperty::read(strip, QStringLiteral("Layout.preferredHeight"),
                                  qmlContext(strip))
            .toInt();
    };
    const bool retained = strip_preferred_height() == 120 && layout.filmstripHeight() == 225;
    strip->setProperty("maximumPanelHeight", 400);
    QCoreApplication::processEvents();
    const bool restored = strip_preferred_height() == 225;
    if (!retained || !restored)
        LOG_ERROR(ravo::logger(), "Panel layout smoke constraint failed retained={} restored={}",
                  retained, restored);
    return retained && restored;
}

bool smoke_export_options(QQmlApplicationEngine &engine)
{
    auto *dialog =
        engine.rootObjects().front()->findChild<QObject *>(QStringLiteral("ExportOptionsDialog"));
    if (!dialog || !QMetaObject::invokeMethod(dialog, "resetFromPresenter"))
        return false;
    const auto reset =
        qScopeGuard([dialog] { QMetaObject::invokeMethod(dialog, "resetFromPresenter"); });
    if (dialog->property("sizingMode").toString() != QLatin1String("original") ||
        dialog->property("maxEdge").toInt() != 2560 ||
        dialog->property("maxWidth").toInt() != 2560 ||
        dialog->property("maxHeight").toInt() != 1440)
        return false;
    for (const auto &format :
         {QStringLiteral("jpeg"), QStringLiteral("png"), QStringLiteral("tiff")})
    {
        dialog->setProperty("formatId", format);
        for (const auto &mode : {QStringLiteral("original"), QStringLiteral("long_edge"),
                                 QStringLiteral("dimensions")})
        {
            dialog->setProperty("sizingMode", mode);
            QVariant returned;
            if (!QMetaObject::invokeMethod(dialog, "selectedOptions",
                                           Q_RETURN_ARG(QVariant, returned)))
                return false;
            const auto options = returned.metaType().id() == qMetaTypeId<QJSValue>() ?
                                     returned.value<QJSValue>().toVariant().toMap() :
                                     returned.toMap();
            auto parsed = ravo::studio_export_options_from_presentation(format, options);
            if (!parsed || options.value("maxEdge").toInt() != (mode == "long_edge" ? 2560 : 0) ||
                options.value("maxWidth").toInt() != (mode == "dimensions" ? 2560 : 0) ||
                options.value("maxHeight").toInt() != (mode == "dimensions" ? 1440 : 0))
            {
                LOG_ERROR(ravo::logger(), "Export sizing smoke failed: format={} mode={}",
                          format.toStdString(), mode.toStdString());
                return false;
            }
        }
    }
    dialog->setProperty("formatId", QStringLiteral("jpeg"));
    dialog->setProperty("jpegSizeLimitEnabled", true);
    QVariant limited_result;
    if (!QMetaObject::invokeMethod(dialog, "selectedOptions",
                                   Q_RETURN_ARG(QVariant, limited_result)))
        return false;
    const auto limited_options = limited_result.value<QJSValue>().toVariant().toMap();
    auto limited = ravo::make_studio_export_options(QStringLiteral("jpeg"), limited_options);
    if (!limited || limited.value().jpeg_max_bytes != 2'000'000U)
        return false;
    auto *missing = engine.rootObjects().front()->findChild<QObject *>(
        QStringLiteral("CompanionMissingDialog"));
    if (!missing)
        return false;
    QMetaObject::invokeMethod(missing, "finished",
                              Q_ARG(QString, QCoreApplication::translate("Main", "Cancel")));
    if (dialog->property("visible").toBool())
        return false;
    QMetaObject::invokeMethod(
        missing, "finished",
        Q_ARG(QString, QCoreApplication::translate("Main", "Export from RAW")));
    if (dialog->property("formatId").toString() != QLatin1String("jpeg"))
        return false;
    QMetaObject::invokeMethod(dialog, "close");
    return true;
}

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
QStringList studio_ui_font_families(const QFont &system_font, const QString &language)
{
    const QStringList installed = QFontDatabase::families();
    QStringList families;

    auto append_installed = [&](const QString &family)
    { append_family(families, installed_family(installed, family)); };

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
              QStringLiteral("Ubuntu"), QStringLiteral("Cantarell"), QStringLiteral("FreeSans"),
              QStringLiteral("Lucida Grande"), QStringLiteral("Helvetica Neue"),
              QStringLiteral("Helvetica"), QStringLiteral("Arial")})
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

    auto append_script =
        [&](const QStringList &candidates, const QFontDatabase::WritingSystem writing_system)
    {
        const qsizetype before = families.size();
        for (const auto &family : candidates)
            append_installed(family);
        if (families.size() == before)
            append_family(families, first_public_family(writing_system));
    };
    auto append_simplified = [&]()
    {
        append_script({QStringLiteral("PingFang SC"), QStringLiteral("Hiragino Sans GB"),
                       QStringLiteral("Noto Sans CJK SC"), QStringLiteral("Noto Sans SC"),
                       QStringLiteral("Source Han Sans SC"), QStringLiteral("Microsoft YaHei UI"),
                       QStringLiteral("Microsoft YaHei")},
                      QFontDatabase::SimplifiedChinese);
    };
    auto append_traditional = [&]()
    {
        append_script({QStringLiteral("PingFang TC"), QStringLiteral("Hiragino Sans CNS"),
                       QStringLiteral("Noto Sans CJK TC"), QStringLiteral("Noto Sans TC"),
                       QStringLiteral("Microsoft JhengHei UI"),
                       QStringLiteral("Microsoft JhengHei")},
                      QFontDatabase::TraditionalChinese);
    };
    auto append_japanese = [&]()
    {
        append_script({QStringLiteral("Hiragino Sans"), QStringLiteral("Yu Gothic UI"),
                       QStringLiteral("Yu Gothic"), QStringLiteral("Meiryo"),
                       QStringLiteral("Noto Sans CJK JP"), QStringLiteral("Noto Sans JP")},
                      QFontDatabase::Japanese);
    };
    auto append_korean = [&]()
    {
        append_script({QStringLiteral("Apple SD Gothic Neo"), QStringLiteral("Malgun Gothic"),
                       QStringLiteral("Noto Sans CJK KR"), QStringLiteral("Noto Sans KR")},
                      QFontDatabase::Korean);
    };

    if (language == QLatin1String("zh_TW"))
    {
        append_traditional();
        append_simplified();
        append_japanese();
        append_korean();
    }
    else if (language == QLatin1String("ja_JP"))
    {
        append_japanese();
        append_simplified();
        append_traditional();
        append_korean();
    }
    else if (language == QLatin1String("ko_KR"))
    {
        append_korean();
        append_simplified();
        append_traditional();
        append_japanese();
    }
    else
    {
        append_simplified();
        append_traditional();
        append_japanese();
        append_korean();
    }
    return families;
}

} // namespace

int main(int argc, char *argv[])
{
    bool requested_smoke = false;
    bool requested_startup_smoke = false;
    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument(argv[index]);
        if (argument == "--smoke" || argument == "--startup-smoke")
        {
            requested_smoke = true;
            requested_startup_smoke = requested_startup_smoke || argument == "--startup-smoke";
        }
    }
    if (requested_smoke)
    {
        if (!requested_startup_smoke)
            qputenv("QT_QPA_PLATFORM", "offscreen");
        qputenv("QSG_RHI_BACKEND", "software");
        qputenv("QT_QUICK_BACKEND", "software");
        // Windows can otherwise surface a Qt fatal through an interactive crash
        // dialog, turning a real QML/registry failure into an opaque timeout.
        qInstallMessageHandler(smoke_message_handler);
    }

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

    // Logging outlives Qt, the presenter and every owner-managed worker. In
    // particular, cancellation can still log while presenter destruction joins
    // a thumbnail task after the event loop has stopped.
    ravo::init_logging("RavoStudio");
    const auto logging_lifetime = qScopeGuard([] { ravo::shutdown_logging(); });
    QGuiApplication application(argc, argv);
    if (requested_startup_smoke && QGuiApplication::platformName() != QStringLiteral("cocoa") &&
        QGuiApplication::platformName() != QStringLiteral("windows") &&
        QGuiApplication::platformName() != QStringLiteral("xcb"))
    {
        LOG_ERROR(ravo::logger(), "Startup smoke requires a native cocoa/windows/xcb platform");
        return EXIT_FAILURE;
    }
    QGuiApplication::setApplicationName(QStringLiteral("Ravo Studio"));
    QGuiApplication::setOrganizationName(QStringLiteral("Ravo"));
    // Every smoke entry point (including POST_BUILD and direct --smoke) must
    // isolate all desktop preferences before constructing any settings owner.
    // The directory outlives the presenter and its joined workers.
    std::optional<QTemporaryDir> smoke_settings;
    if (requested_smoke)
    {
        smoke_settings.emplace();
        if (!smoke_settings->isValid())
            return EXIT_FAILURE;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, smoke_settings->path());
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, smoke_settings->path());
    }
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
    const QStringList arguments = QCoreApplication::arguments();
    const bool smoke = requested_smoke;
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
                LOG_ERROR(ravo::logger(), "--language requires a locale code");
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
                LOG_ERROR(ravo::logger(), "--language requires a locale code");
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
        return 1;
    }
    auto apply_ui_font = [&language_manager]()
    {
        QFont ui_font = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
        const QStringList families = studio_ui_font_families(ui_font, language_manager.language());
        if (!families.isEmpty())
        {
            ui_font.setFamilies(families);
            ui_font.setStyleHint(QFont::AnyStyle);
        }
        QGuiApplication::setFont(ui_font);
    };
    apply_ui_font();
    QObject::connect(&language_manager, &ravo::StudioLanguageManager::languageChanged, &application,
                     apply_ui_font);
    ravo::StudioPresenter presenter;
    if (smoke)
    {
        // QObject destruction follows the presenter's worker cancellation/join.
        // Exercise logging at that boundary, not just while main() is running.
        QObject::connect(
            &presenter, &QObject::destroyed,
            [] { LOG_INFO(ravo::logger(), "Ravo Studio smoke presenter teardown complete"); });
    }
    ravo::StudioStartupController startup_controller(presenter,
                                                     presenter.defaultCatalogFile().toLocalFile());
    ravo::StudioCommandController command_controller(presenter);
    ravo::StudioAssistantController assistant_controller;
    if (!assistant_controller.initialize())
    {
        LOG_ERROR(ravo::logger(), "assistant settings failed to initialize");
        return 1;
    }
    ravo::StudioWindowGeometry window_geometry;
    ravo::StudioPanelLayout panel_layout;
    if (!panel_layout.initialize())
    {
        LOG_ERROR(ravo::logger(), "panel layout failed to initialize: {}",
                  panel_layout.lastError().toStdString());
        return 1;
    }
    ravo::StudioDisplayPresentation display_presentation;
    if (!window_geometry.initialize())
    {
        LOG_ERROR(ravo::logger(), "window geometry failed to initialize: {}",
                  window_geometry.lastError().toStdString());
        return 1;
    }
    auto live_session = ravo::StudioLiveSessionController::create(presenter, command_controller);
    if (!live_session)
    {
        LOG_ERROR(ravo::logger(), "live Studio control failed to initialize: {}",
                  live_session.error().message);
        return 1;
    }
    if (smoke)
        LOG_INFO(ravo::logger(), "Ravo Studio smoke C++ owners ready");
    QObject::connect(&language_manager, &ravo::StudioLanguageManager::languageChanged,
                     &command_controller, &ravo::StudioCommandController::retranslate);
    QObject::connect(&language_manager, &ravo::StudioLanguageManager::languageChanged,
                     presenter.develop(), &ravo::StudioDevelopPresenter::retranslate);
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
                            new ravo::StudioPreviewImageProvider(*presenter.inspect()));
    engine.addImageProvider(QStringLiteral("studioScope"),
                            new ravo::StudioScopeImageProvider(*presenter.inspect()));
    engine.addImageProvider(
        QStringLiteral("importCandidate"),
        new ravo::ImportCandidateImageProvider(*presenter.imports()->importCandidates()));
    engine.rootContext()->setContextProperty(QStringLiteral("studio"), &presenter);
    engine.rootContext()->setContextProperty(QStringLiteral("studioStartup"), &startup_controller);
    engine.rootContext()->setContextProperty(QStringLiteral("studioCommands"), &command_controller);
    engine.rootContext()->setContextProperty(QStringLiteral("studioLanguage"), &language_manager);
    engine.rootContext()->setContextProperty(QStringLiteral("studioAssistant"),
                                             &assistant_controller);
    engine.rootContext()->setContextProperty(QStringLiteral("studioWindow"), &window_geometry);
    engine.rootContext()->setContextProperty(QStringLiteral("studioLayout"), &panel_layout);
    engine.rootContext()->setContextProperty(QStringLiteral("studioDisplayPresentation"),
                                             &display_presentation);
    presenter.bindDisplayPresentation(&display_presentation);
    engine.rootContext()->setContextProperty(QStringLiteral("studioSmoke"), smoke);
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &application,
        []() { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreated, &application,
        [smoke, &presenter](QObject *object, const QUrl &)
        {
            if (smoke || object == nullptr ||
                !qEnvironmentVariableIntValue("RAVO_TRACE_PREVIEW_PRESENTATION"))
            {
                return;
            }
            auto *window = qobject_cast<QQuickWindow *>(object);
            if (window == nullptr)
            {
                LOG_ERROR(ravo::logger(),
                          "interactive preview presentation trace requires a QQuickWindow root");
                return;
            }
            auto trace = std::make_shared<PreviewPresentationTrace>();
            QObject::connect(presenter.develop(),
                             &ravo::StudioDevelopPresenter::interactivePreviewPublished, window,
                             [trace](const qulonglong revision, const qlonglong intent_to_image_us)
                             {
                                 trace->pending_revision = static_cast<std::uint64_t>(revision);
                                 trace->intent_to_image_us =
                                     std::max<std::int64_t>(intent_to_image_us, 0);
                                 trace->intent_started_at =
                                     std::chrono::steady_clock::now() -
                                     std::chrono::microseconds(trace->intent_to_image_us);
                             });
            QObject::connect(
                window, &QQuickWindow::frameSwapped, window,
                [trace]
                {
                    if (!trace->pending_revision)
                    {
                        return;
                    }
                    const auto intent_to_frame_swap_us =
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - trace->intent_started_at)
                            .count();
                    LOG_INFO(ravo::logger(),
                             "interactive preview presented revision={} intent_to_image={}us "
                             "intent_to_frame_swap={}us",
                             *trace->pending_revision, trace->intent_to_image_us,
                             intent_to_frame_swap_us);
                    trace->pending_revision.reset();
                });
        },
        Qt::QueuedConnection);
    if (smoke)
        LOG_INFO(ravo::logger(), "Ravo Studio smoke loading root QML");
    engine.loadFromModule("Ravo.Studio", "Main");
    if (requested_startup_smoke)
    {
        auto *window = engine.rootObjects().isEmpty() ?
                           nullptr :
                           qobject_cast<QQuickWindow *>(engine.rootObjects().front());
        if (!window)
            return EXIT_FAILURE;
        // Exercise the real native window and renderer without the offscreen
        // interaction suite's focus assumptions. All owners retain normal RAII
        // teardown, including worker cancellation after this bounded event loop.
        QEventLoop startup_loop;
        QTimer deadline;
        deadline.setSingleShot(true);
        bool presented = false;
        QObject::connect(
            window, &QQuickWindow::frameSwapped, &startup_loop,
            [&]
            {
                presented = true;
                startup_loop.quit();
            },
            Qt::QueuedConnection);
        QObject::connect(&deadline, &QTimer::timeout, &startup_loop, &QEventLoop::quit);
        deadline.start(15000);
        window->show();
        window->requestUpdate();
        startup_loop.exec();
        window->hide();
        if (!presented)
        {
            LOG_ERROR(ravo::logger(), "Startup smoke timed out before the first native frame");
            return EXIT_FAILURE;
        }
        std::puts("{\"type\":\"ravo.native_startup\",\"version\":1,\"first_frame\":true}");
        return EXIT_SUCCESS;
    }
    if (smoke)
    {
        const bool loaded = smoke_startup_splash(engine) &&
                            smoke_panel_layout(engine, panel_layout) &&
                            smoke_export_options(engine) &&
                            ravo::smoke_photo_merge_dialog(engine, command_controller) &&
                            ravo::smoke_import_layout(engine) && ravo::smoke_curve_gesture(engine);
        if (!loaded)
            LOG_ERROR(ravo::logger(), "Ravo Studio smoke failed to instantiate QML");
        else
            LOG_INFO(ravo::logger(), "Ravo Studio smoke loaded");
        return loaded ? 0 : 1;
    }
    const int exit_code = QGuiApplication::exec();
    LOG_INFO(ravo::logger(), "Ravo Studio event loop stopped exit_code={}", exit_code);
    return exit_code;
}
