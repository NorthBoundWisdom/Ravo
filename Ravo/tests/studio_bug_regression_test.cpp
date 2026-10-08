#include <gtest/gtest.h>
#include <cmath>
#include <QColor>
#include <QColorSpace>
#include <QImage>
#include <QFile>
#include <QTemporaryDir>
#include "ravo/desktop/studio_command_controller.h"
#include "ravo/desktop/studio_presenter.h"
#include "ravo/desktop/studio_display_presentation.h"
#include "ravo/foundation/log.h"
#include "studio_language_manager.h"
#include "studio_test_support.h"

namespace ravo
{
namespace
{
using namespace studio_test_support;
TEST(StudioCommands, WritableMetadataAcceptsEveryDomainFieldAndRejectsInvalidInput)
{
    ensure_qt_core();
    ravo::init_logging("ravo-desktop-command-tests");
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto photo = directory.filePath(QStringLiteral("metadata.png"));
    QImage image(32, 24, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(120, 130, 140));
    ASSERT_TRUE(image.save(photo, "PNG"));
    QFile original(photo);
    ASSERT_TRUE(original.open(QIODevice::ReadOnly));
    const auto original_bytes = original.readAll();
    original.close();
    const auto catalog = directory.filePath(QStringLiteral("library.sqlite"));
    StudioPresenter presenter;
    StudioCommandController controller(presenter);
    presenter.createCatalogFromPath(catalog);
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
    presenter.imports()->importFilePaths({photo});
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.selectedAssetId().isEmpty() && !presenter.busy() &&
                   !presenter.imports()->importWorkActive();
        }));
    const auto command = controller.ids().value(QStringLiteral("photoSetMetadata")).toString();
    const QStringList fields{QStringLiteral("title"),       QStringLiteral("description"),
                             QStringLiteral("creator"),     QStringLiteral("copyright"),
                             QStringLiteral("country"),     QStringLiteral("province_state"),
                             QStringLiteral("city"),        QStringLiteral("sublocation"),
                             QStringLiteral("headline"),    QStringLiteral("credit"),
                             QStringLiteral("source"),      QStringLiteral("instructions"),
                             QStringLiteral("usage_terms"), QStringLiteral("job_id")};
    const QStringList properties{
        QStringLiteral("selectedTitle"),      QStringLiteral("selectedDescription"),
        QStringLiteral("selectedCreator"),    QStringLiteral("selectedCopyright"),
        QStringLiteral("selectedCountry"),    QStringLiteral("selectedProvinceState"),
        QStringLiteral("selectedCity"),       QStringLiteral("selectedSublocation"),
        QStringLiteral("selectedHeadline"),   QStringLiteral("selectedCredit"),
        QStringLiteral("selectedSource"),     QStringLiteral("selectedInstructions"),
        QStringLiteral("selectedUsageTerms"), QStringLiteral("selectedJobId")};
    for (qsizetype i = 0; i < fields.size(); ++i)
    {
        const auto value = QStringLiteral("测试 ") + fields[i];
        const auto result = controller.executeCommand(
            command,
            QVariantMap{{QStringLiteral("name"), fields[i]}, {QStringLiteral("value"), value}},
            QStringLiteral("control"));
        ASSERT_TRUE(result.value(QStringLiteral("accepted")).toBool()) << fields[i].toStdString();
        ASSERT_TRUE(wait_until(
            [&] { return presenter.property(properties[i].toUtf8()).toString() == value; }))
            << fields[i].toStdString() << ": " << presenter.errorText().toStdString();
    }
    for (const auto &argument : {QVariantMap{{QStringLiteral("name"), QStringLiteral("unknown")},
                                             {QStringLiteral("value"), QStringLiteral("x")}},
                                 QVariantMap{{QStringLiteral("name"), QStringLiteral("headline")},
                                             {QStringLiteral("value"), 42}}})
        EXPECT_FALSE(controller.executeCommand(command, argument, QStringLiteral("control"))
                         .value(QStringLiteral("accepted"))
                         .toBool());
    EXPECT_EQ(presenter.selectedHeadline(), QStringLiteral("测试 headline"));
    EXPECT_TRUE(
        controller
            .executeCommand(command,
                            QVariantMap{{QStringLiteral("name"), QStringLiteral("headline")},
                                        {QStringLiteral("value"), QString{}}},
                            QStringLiteral("control"))
            .value(QStringLiteral("accepted"))
            .toBool());
    ASSERT_TRUE(wait_until([&] { return presenter.selectedHeadline().isEmpty(); }));
    const auto context = presenter.metadataEditContext();
    const QVariantMap edit{{QStringLiteral("name"), QStringLiteral("headline")},
                           {QStringLiteral("value"), QStringLiteral("From dialog")},
                           {QStringLiteral("context"), context}};
    ASSERT_TRUE(controller.executeCommand(command, edit, QStringLiteral("control"))
                    .value(QStringLiteral("accepted"))
                    .toBool());
    ASSERT_TRUE(
        wait_until([&] { return presenter.selectedHeadline() == QStringLiteral("From dialog"); }));
    EXPECT_FALSE(controller.executeCommand(command, edit, QStringLiteral("control"))
                     .value(QStringLiteral("accepted"))
                     .toBool());
    EXPECT_EQ(presenter.selectedHeadline(), QStringLiteral("From dialog"));
    const auto list_context = presenter.metadataEditContext();
    const QVariantMap list_fields{
        {QStringLiteral("title"), QStringLiteral("List title")},
        {QStringLiteral("description"), QStringLiteral("List description")},
        {QStringLiteral("credit"), QString{}}};
    const QVariantMap list_edit{{QStringLiteral("fields"), list_fields},
                                {QStringLiteral("context"), list_context}};
    ASSERT_TRUE(controller.executeCommand(command, list_edit, QStringLiteral("control"))
                    .value(QStringLiteral("accepted"))
                    .toBool());
    ASSERT_TRUE(wait_until(
        [&]
        {
            return presenter.selectedTitle() == QStringLiteral("List title") &&
                   presenter.selectedDescription() == QStringLiteral("List description") &&
                   presenter.selectedCredit().isEmpty();
        }));
    EXPECT_EQ(presenter.selectedCreator(), QStringLiteral("测试 creator"));
    auto invalid_fields = list_fields;
    invalid_fields[QStringLiteral("title")] = QStringLiteral("Must not be saved");
    invalid_fields[QStringLiteral("unknown")] = QStringLiteral("invalid");
    EXPECT_FALSE(controller
                     .executeCommand(
                         command,
                         QVariantMap{{QStringLiteral("fields"), invalid_fields},
                                     {QStringLiteral("context"), presenter.metadataEditContext()}},
                         QStringLiteral("control"))
                     .value(QStringLiteral("accepted"))
                     .toBool());
    EXPECT_EQ(presenter.selectedTitle(), QStringLiteral("List title"));
    ASSERT_TRUE(wait_until([&] { return presenter.develop()->canAdjustExposure(); }));
    presenter.setBrowseMode(QStringLiteral("grid"));
    const auto exposure_command =
        controller.ids().value(QStringLiteral("photoAdjustExposure")).toString();
    const double initial_ev = presenter.develop()->editExposure();
    double expected_ev = initial_ev;
    for (const double delta : {-1.0, -1.0 / 3.0, 1.0 / 3.0, 1.0})
    {
        ASSERT_TRUE(controller.executeCommand(exposure_command, delta, QStringLiteral("control"))
                        .value(QStringLiteral("accepted"))
                        .toBool());
        expected_ev += delta;
        ASSERT_TRUE(wait_until(
            [&]
            {
                return std::abs(presenter.develop()->editExposure() - expected_ev) < 1e-9 &&
                       !presenter.inspect()->previewLoading();
            }))
            << presenter.errorText().toStdString() << " EV=" << presenter.develop()->editExposure()
            << " expected=" << expected_ev << " loading=" << presenter.inspect()->previewLoading();
        EXPECT_EQ(presenter.browseMode(), QStringLiteral("grid"));
    }
    EXPECT_FALSE(controller.executeCommand(exposure_command, 10.0, QStringLiteral("control"))
                     .value(QStringLiteral("accepted"))
                     .toBool());
    EXPECT_NEAR(presenter.develop()->editExposure(), initial_ev, 1e-9);
    image.fill(QColor(40, 50, 60));
    const auto new_photo = directory.filePath(QStringLiteral("new.png"));
    ASSERT_TRUE(image.save(new_photo, "PNG"));
    const auto sync = controller.ids().value(QStringLiteral("librarySyncFolder")).toString();
    ASSERT_TRUE(controller.executeCommand(sync, directory.path(), QStringLiteral("control"))
                    .value(QStringLiteral("accepted"))
                    .toBool());
    EXPECT_TRUE(presenter.imports()->importPageOpen());
    EXPECT_EQ(presenter.imports()->importMode(), QStringLiteral("add"));
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.imports()->importScanActive() &&
                   presenter.imports()->importCandidates()->rowCount() == 2;
        }));
    presenter.imports()->startPlannedImport();
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.imports()->importWorkActive() && presenter.visibleCount() == 1 &&
                   presenter.selectedUri().endsWith(QStringLiteral("new.png"));
        }));
    presenter.imports()->closeImportPage();
    presenter.selectFolder(QString{});
    ASSERT_TRUE(wait_until([&] { return presenter.visibleCount() == 2; }));
    ASSERT_TRUE(original.open(QIODevice::ReadOnly));
    EXPECT_EQ(original.readAll(), original_bytes);
}

TEST(StudioLocalization, SettingsCommandSurvivesLanguageChanges)
{
    ensure_qt_core();
    StudioLanguageManager manager(QStringList{QStringLiteral(RAVO_STUDIO_TRANSLATION_DIR)});
    ASSERT_TRUE(manager.initialize(QStringLiteral("en_US")));
    StudioPresenter presenter;
    StudioCommandController controller(presenter);
    const auto id = controller.ids().value(QStringLiteral("windowSettings")).toString();
    QString requested;
    QObject::connect(&controller, &StudioCommandController::presentationCommandRequested,
                     &controller,
                     [&](const QString &command, const QVariant &) { requested = command; });
    for (const auto &language : manager.supportedLanguages())
    {
        ASSERT_TRUE(manager.initialize(language)) << manager.lastError().toStdString();
        controller.retranslate();
        requested.clear();
        EXPECT_TRUE(controller.executeAction(id, QStringLiteral("menu"))
                        .value(QStringLiteral("accepted"))
                        .toBool());
        EXPECT_EQ(requested, id);
    }
}

TEST(StudioCommands, GalleryExposureRefreshesGridThumbnailPixels)
{
    ensure_qt_core();
    ravo::init_logging("ravo-desktop-command-tests");
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto photo = directory.filePath(QStringLiteral("grid.png"));
    QImage source(640, 480, QImage::Format_RGB888);
    source.setColorSpace(QColorSpace(QColorSpace::SRgb));
    source.fill(QColor(100, 120, 140));
    ASSERT_TRUE(source.save(photo, "PNG"));
    int thumbnail_notifications = 0;
    StudioDisplayPresentation display;
    ASSERT_TRUE(display.injectSyntheticMatrixForTesting());
    StudioPresenter presenter;
    presenter.bindDisplayPresentation(&display);
    StudioCommandController commands(presenter);
    presenter.createCatalogFromPath(directory.filePath(QStringLiteral("library.sqlite")));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
    presenter.imports()->importFilePaths({photo});
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.imports()->importWorkActive() &&
                   presenter.develop()->canAdjustExposure();
        }));
    presenter.setBrowseMode(QStringLiteral("grid"));
    const auto id = presenter.selectedAssetId();
    const auto thumbnail = [&]
    {
        return presenter.assets()
            ->data(presenter.assets()->index(presenter.assets()->indexOf(id), 0),
                   AssetListModel::ThumbnailUrlRole)
            .toUrl();
    };
    const auto ready = [&]
    { return presenter.assets()->thumbnailState(id.toStdString()) == QStringLiteral("ready"); };
    presenter.ensureThumbnail(id);
    ASSERT_TRUE(wait_until([&] { return ready() && thumbnail().isLocalFile(); }));
    const auto original_url = thumbnail();
    const QImage original(original_url.toLocalFile());
    ASSERT_FALSE(original.isNull());
    QObject::connect(presenter.assets(), &QAbstractItemModel::dataChanged, &presenter,
                     [&](const QModelIndex &, const QModelIndex &, const QList<int> &roles)
                     {
                         if (roles.contains(AssetListModel::ThumbnailUrlRole))
                             ++thumbnail_notifications;
                     });
    const auto action = commands.ids().value(QStringLiteral("photoAdjustExposure")).toString();
    ASSERT_TRUE(commands.executeCommand(action, -1.0, QStringLiteral("control"))
                    .value(QStringLiteral("accepted"))
                    .toBool());
    ASSERT_TRUE(wait_until(
        [&]
        {
            return ready() && thumbnail() != original_url && !presenter.inspect()->previewLoading();
        }))
        << presenter.errorText().toStdString();
    const auto dark_url = thumbnail();
    const QImage dark(dark_url.toLocalFile());
    ASSERT_FALSE(dark.isNull());
    EXPECT_LE(dark.width(), static_cast<int>(kThumbnailMaxEdge));
    EXPECT_LT(dark.pixelColor(10, 10).red(), original.pixelColor(10, 10).red());
    EXPECT_GT(thumbnail_notifications, 0);
    ASSERT_TRUE(commands.executeCommand(action, 1.0, QStringLiteral("control"))
                    .value(QStringLiteral("accepted"))
                    .toBool());
    ASSERT_TRUE(wait_until(
        [&]
        { return ready() && thumbnail() != dark_url && !presenter.inspect()->previewLoading(); }));
    const QImage restored(thumbnail().toLocalFile());
    EXPECT_EQ(restored.pixelColor(10, 10), original.pixelColor(10, 10));
    for (const double delta : {1.0, -1.0, 1.0})
        ASSERT_TRUE(commands.executeCommand(action, delta, QStringLiteral("control"))
                        .value(QStringLiteral("accepted"))
                        .toBool());
    ASSERT_TRUE(wait_until(
        [&]
        {
            return ready() && thumbnail() != original_url && !presenter.inspect()->previewLoading();
        }));
    const QImage latest(thumbnail().toLocalFile());
    ASSERT_FALSE(latest.isNull());
    EXPECT_GT(latest.pixelColor(10, 10).red(), original.pixelColor(10, 10).red());
    EXPECT_DOUBLE_EQ(presenter.develop()->editExposure(), 1.0);
    EXPECT_EQ(presenter.browseMode(), QStringLiteral("grid"));
}

} // namespace
} // namespace ravo
