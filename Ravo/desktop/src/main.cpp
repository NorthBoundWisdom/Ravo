#include <QCoreApplication>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QIcon>
#include <QImage>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickImageProvider>
#include <QQuickStyle>
#include <QResource>
#include <QSize>
#include <QUrl>

#include "ravo/desktop/studio_presenter.h"
#include "ravo/foundation/log.h"

void qml_register_types_GeoControls();
void qml_register_types_GeoControls_AppShell();

namespace
{

class StudioPreviewImageProvider final : public QQuickImageProvider
{
public:
    explicit StudioPreviewImageProvider(ravo::StudioPresenter &studio)
        : QQuickImageProvider(QQuickImageProvider::Image)
        , studio_(&studio)
    {
    }

    QImage requestImage(const QString &, QSize *size, const QSize &) override
    {
        const QImage image = studio_->previewImage();
        if (size != nullptr)
        {
            *size = image.size();
        }
        return image;
    }

private:
    ravo::StudioPresenter *studio_ = nullptr;
};

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
    const bool smoke = QCoreApplication::arguments().contains(QStringLiteral("--smoke"));
    LOG_INFO(ravo::logger(), "Ravo Studio starting");

    ravo::StudioPresenter presenter;
    QQmlApplicationEngine engine;
    engine.addImportPath(QStringLiteral("qrc:/"));
    engine.addImageProvider(QStringLiteral("studioPreview"),
                            new StudioPreviewImageProvider(presenter));
    engine.rootContext()->setContextProperty(QStringLiteral("studio"), &presenter);
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
