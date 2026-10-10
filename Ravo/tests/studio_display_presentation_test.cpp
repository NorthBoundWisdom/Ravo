#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include <QColor>
#include <QColorSpace>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QImage>
#include <QBuffer>
#include <QQmlEngine>
#include <QQmlComponent>
#include <QTemporaryDir>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QLockFile>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QElapsedTimer>
#include <QThread>
#include <gtest/gtest.h>

#include "ravo/adapters/sqlite_catalog.h"
#include "ravo/desktop/studio_display_presentation.h"
#include "ravo/desktop/studio_presenter.h"
#include "ravo/domain/types.h"
#include "ravo/domain/uri.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/log.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/recipe.h"
#include "studio_test_support.h"
#include "studio_gallery_thumbnail_cache.h"
#if defined(Q_OS_MACOS)
#include "studio_iosurface_snapshot.h"
#include "ravo/services/display_presentation.h"
#endif

namespace ravo
{
namespace
{
using studio_test_support::ensure_qt_core;
using studio_test_support::wait_until;

TEST(StudioDisplayPresentationTest, ThumbnailCellReportsReaderFailureAndAcceptsRepairedUrl)
{
    ensure_qt_core();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QQmlEngine engine;
    engine.addImportPath(QStringLiteral("qrc:/"));
    engine.addImportPath(QStringLiteral(RAVO_GEOCONTROLS_QML_IMPORT_ROOT));
    engine.addImportPath(QStringLiteral(RAVO_GEOCONTROLS_APPSHELL_QML_IMPORT_ROOT));
    QQmlComponent component(&engine);
    component.setData("import QtQuick\nimport \".\"\nThumbnailCell { width: 100; height: 100; "
                      "property url failedUrl; property int failures: 0; "
                      "onLoadFailed: function(url) { failedUrl = url; failures += 1; } }",
                      QUrl::fromLocalFile(QStringLiteral(
                          RAVO_REPOSITORY_ROOT "/Ravo/desktop/qml/gallery/ReaderTest.qml")));
    ASSERT_TRUE(component.isReady()) << component.errorString().toStdString();
    std::unique_ptr<QObject> cell(component.create());
    ASSERT_TRUE(cell);
    const auto missing = QUrl::fromLocalFile(directory.filePath("thumbnail.png"));
    ASSERT_TRUE(cell->setProperty("thumbnailUrl", missing));
    ASSERT_TRUE(wait_until([&] { return cell->property("failedUrl").toUrl() == missing; }));
    const int failures = cell->property("failures").toInt();
    // A read can fail while presentation is pending. Ready publication must
    // report that failure again even when the URL itself did not change.
    ASSERT_TRUE(cell->setProperty("thumbnailState", "ready"));
    EXPECT_EQ(cell->property("failures").toInt(), failures + 1);
    QImage pixels(32, 24, QImage::Format_RGB888);
    pixels.fill(Qt::blue);
    ASSERT_TRUE(pixels.save(missing.toLocalFile()));
    auto repaired = missing;
    repaired.setFragment("repaired");
    ASSERT_TRUE(cell->setProperty("thumbnailUrl", repaired));
    auto *photo = cell->findChild<QObject *>("thumbnailPhoto");
    ASSERT_TRUE(photo);
    ASSERT_TRUE(wait_until([&] { return photo->property("status").toInt() == 1; }));
    EXPECT_TRUE(photo->property("hasReadyImage").toBool());
}

TEST(StudioDisplayPresentationTest, GalleryCacheReusesIndexAndObservesOtherPublishers)
{
    ensure_qt_core();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QImage image(32, 24, QImage::Format_RGB888);
    image.fill(Qt::blue);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    ASSERT_TRUE(buffer.open(QIODevice::WriteOnly));
    ASSERT_TRUE(image.save(&buffer, "PNG"));
    const auto budget = static_cast<std::uint64_t>(bytes.size()) * 2U;
    StudioGalleryThumbnailCache first(budget);
    StudioGalleryThumbnailCache second(budget);
    ASSERT_TRUE(first.publish(directory.path(), "one", bytes, false, {}));
    ASSERT_TRUE(first.publish(directory.path(), "two", bytes, false, {}));
    EXPECT_EQ(first.indexBuildCount(), 1U);
    // Another window must account for the first one's files before eviction.
    ASSERT_TRUE(second.publish(directory.path(), "three", bytes, false, {}));
    EXPECT_EQ(second.indexBuildCount(), 1U);
    EXPECT_FALSE(QFileInfo::exists(directory.filePath("one.png")));
    ASSERT_TRUE(first.publish(directory.path(), "four", bytes, false, {}));
    EXPECT_EQ(first.indexBuildCount(), 2U);
    EXPECT_EQ(QDir(directory.path()).entryList({"*.png"}, QDir::Files).size(), 2);
    ASSERT_TRUE(first.publish(directory.path(), "four", bytes, false, {}));
    EXPECT_EQ(first.indexBuildCount(), 2U);
    CancellationSource cancelled;
    ASSERT_TRUE(cancelled.cancel("test"));
    auto stopped = first.publish(directory.path(), "cancelled", bytes, false, cancelled.token());
    ASSERT_FALSE(stopped);
    EXPECT_EQ(stopped.error().code, ErrorCode::kCancelled);
    EXPECT_FALSE(QFileInfo::exists(directory.filePath("cancelled.png")));
    // Damaged generation metadata must be reported, never treated as permission
    // to use a potentially stale capacity index.
    QFile epoch(directory.filePath("index-epoch"));
    ASSERT_TRUE(epoch.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_EQ(epoch.write(QByteArray(65, 'x')), 65);
    epoch.close();
    auto invalid = first.publish(directory.path(), "five", bytes, false, {});
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code, ErrorCode::kIo);
}

enum class ThumbnailCacheLoss
{
    BeforeOpen,
    DuringPresentation,
    OriginalAlsoMissing,
    RepeatedEviction,
    ListingReplaced,
    PagedPresentation
};

void check_thumbnail_cache_recovery(const ThumbnailCacheLoss loss)
{
    ensure_qt_core();
    init_logging("ravo-thumbnail-cache-recovery-tests");
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalog = directory.filePath("library.sqlite");
    const QString photo = directory.filePath("photo.png");
    const QString cache = catalog + ".preview";
    const QString seeded_preview = cache + "/seed.png";
    ASSERT_TRUE(QDir().mkpath(cache));
    QImage image(48, 32, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(200, 40, 40));
    ASSERT_TRUE(image.save(photo, "PNG"));
    ASSERT_TRUE(image.save(seeded_preview, "PNG"));
    QFile original(photo);
    ASSERT_TRUE(original.open(QIODevice::ReadOnly));
    const auto original_bytes = original.readAll();
    const auto original_hash = QCryptographicHash::hash(original_bytes, QCryptographicHash::Sha256);
    original.close();
    auto location = normalize_local_input(photo.toStdString());
    ASSERT_TRUE(location);
    auto identity = read_file_identity(location.value().path);
    ASSERT_TRUE(identity);
    auto repository = SqliteCatalogRepository::create(catalog.toStdString());
    ASSERT_TRUE(repository);
    AssetRecord asset;
    asset.id = "ast_cache_recovery";
    asset.normalized_uri = location.value().uri;
    asset.media_type = std::string(kMediaTypePng);
    asset.size_bytes = identity.value().size_bytes;
    asset.mtime_unix_ms = identity.value().mtime_unix_ms;
    asset.content_fingerprint = make_content_fingerprint(identity.value());
    asset.width = 48U;
    asset.height = 32U;
    asset.created_unix_ms = 1000;
    ASSERT_TRUE(repository.value()->commit_imported_asset(asset));
    if (loss == ThumbnailCacheLoss::PagedPresentation)
    {
        for (int row = 0; row < 200; ++row)
        {
            const auto filler_path = directory.filePath(QString("filler-%1.png").arg(row));
            ASSERT_TRUE(image.save(filler_path, "PNG"));
            auto filler_location = normalize_local_input(filler_path.toStdString());
            ASSERT_TRUE(filler_location);
            auto filler_identity = read_file_identity(filler_location.value().path);
            ASSERT_TRUE(filler_identity);
            AssetRecord filler = asset;
            filler.id = "ast_filler_" + std::to_string(row);
            filler.normalized_uri = filler_location.value().uri;
            filler.size_bytes = filler_identity.value().size_bytes;
            filler.mtime_unix_ms = filler_identity.value().mtime_unix_ms;
            filler.content_fingerprint = make_content_fingerprint(filler_identity.value());
            filler.created_unix_ms = 2000 + row;
            ASSERT_TRUE(repository.value()->commit_imported_asset(filler));
        }
    }
    PreviewRecord record;
    record.asset_id = asset.id;
    record.cache_key = "seed";
    record.cache_relpath = "seed.png";
    record.width = 48U;
    record.height = 32U;
    record.state = std::string(kPreviewStateReady);
    ASSERT_TRUE(repository.value()->upsert_preview(record));
    ASSERT_TRUE(repository.value()->close());
    repository.value().reset();
    if (loss == ThumbnailCacheLoss::BeforeOpen)
        ASSERT_TRUE(QFile::remove(seeded_preview));

    StudioDisplayPresentation display;
    ASSERT_TRUE(display.injectSyntheticMatrixForTesting());
    StudioPresenter presenter;
    presenter.bindDisplayPresentation(&display);
    int evictions = 0;
    bool saw_error = false;
    QObject::connect(&presenter, &StudioPresenter::errorChanged, &presenter,
                     [&] { saw_error |= !presenter.errorText().isEmpty(); });
    QObject::connect(presenter.assets(), &QAbstractItemModel::dataChanged, &presenter,
                     [&]
                     {
                         if (loss == ThumbnailCacheLoss::ListingReplaced && evictions > 0 &&
                             presenter.assets()->thumbnailState(asset.id) == "pending")
                         {
                             presenter.selectFolder(
                                 QStringLiteral("file:///ravo-cache-recovery-empty"));
                             return;
                         }
                         if (loss == ThumbnailCacheLoss::BeforeOpen ||
                             presenter.assets()->thumbnailState(asset.id) != "presenting" ||
                             (evictions > 0 && loss != ThumbnailCacheLoss::RepeatedEviction))
                             return;
                         // setThumbnail emits synchronously before the display task is posted.
                         // Remove its input here to deterministically reproduce cache eviction.
                         const auto entries = QDir(cache).entryList({"*.png"}, QDir::Files);
                         if (entries.isEmpty())
                             return;
                         for (const auto &entry : entries)
                             EXPECT_TRUE(QFile::remove(QDir(cache).filePath(entry)));
                         ++evictions;
                         if (loss == ThumbnailCacheLoss::OriginalAlsoMissing)
                             EXPECT_TRUE(QFile::remove(photo));
                     });
    presenter.openCatalogFromPath(catalog);
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }))
        << presenter.errorText().toStdString();
    if (loss == ThumbnailCacheLoss::PagedPresentation)
    {
        ASSERT_FALSE(presenter.assets()->rowLoaded(200));
        presenter.selectLibraryRow(200);
        ASSERT_TRUE(wait_until([&] { return presenter.selectedAssetId() == asset.id.c_str(); }));
        ASSERT_TRUE(wait_until([&] { return evictions == 1; }))
            << "Sparse page bypassed thumbnail presentation and cache recovery";
    }
    if (loss == ThumbnailCacheLoss::BeforeOpen)
        presenter.ensureThumbnail(presenter.selectedAssetId());
    if (loss == ThumbnailCacheLoss::ListingReplaced)
    {
        ASSERT_TRUE(wait_until(
            [&] { return presenter.visibleCount() == 0 && !presenter.previewWorkActive(); }));
        EXPECT_EQ(evictions, 1);
        EXPECT_FALSE(saw_error) << presenter.errorText().toStdString();
        EXPECT_TRUE(presenter.selectedThumbnailUrl().isEmpty());
        ASSERT_TRUE(original.open(QIODevice::ReadOnly));
        EXPECT_EQ(QCryptographicHash::hash(original.readAll(), QCryptographicHash::Sha256),
                  original_hash);
        return;
    }
    const QString expected_state = loss == ThumbnailCacheLoss::OriginalAlsoMissing ? "missing" :
                                   loss == ThumbnailCacheLoss::RepeatedEviction    ? "failed" :
                                                                                     "ready";
    ASSERT_TRUE(wait_until(
        [&]
        {
            return presenter.assets()->thumbnailState(asset.id) == expected_state &&
                   !presenter.previewWorkActive();
        }))
        << presenter.errorText().toStdString();
    if (loss == ThumbnailCacheLoss::RepeatedEviction)
    {
        EXPECT_EQ(evictions, 2);
        EXPECT_TRUE(saw_error);
        EXPECT_TRUE(presenter.errorText().contains("Gallery thumbnail source is missing"));
    }
    else
    {
        EXPECT_FALSE(saw_error) << presenter.errorText().toStdString();
        EXPECT_TRUE(presenter.errorText().isEmpty());
    }
    if (loss == ThumbnailCacheLoss::OriginalAlsoMissing)
    {
        EXPECT_TRUE(presenter.selectedThumbnailUrl().isEmpty());
        EXPECT_EQ(presenter.selectedImportState(),
                  QString::fromUtf8(kImportStateMissing.data(), kImportStateMissing.size()));
    }
    else
    {
        ASSERT_TRUE(original.open(QIODevice::ReadOnly));
        EXPECT_EQ(QCryptographicHash::hash(original.readAll(), QCryptographicHash::Sha256),
                  original_hash);
        if (loss != ThumbnailCacheLoss::RepeatedEviction)
        {
            QImage presented(presenter.selectedThumbnailUrl().toLocalFile());
            ASSERT_FALSE(presented.isNull());
            EXPECT_EQ(presented.size(), image.size());
            EXPECT_NE(qRed(presented.pixel(24, 16)), 200);
        }
    }
}

TEST(StudioDisplayPresentationTest, MissingThumbnailCacheBeforeOpenRebuildsOnDemand)
{
    check_thumbnail_cache_recovery(ThumbnailCacheLoss::BeforeOpen);
}

TEST(StudioDisplayPresentationTest, EvictedThumbnailDuringOpenRebuildsWithoutError)
{
    check_thumbnail_cache_recovery(ThumbnailCacheLoss::DuringPresentation);
}

TEST(StudioDisplayPresentationTest, EvictedThumbnailAndMissingOriginalBecomeAssetState)
{
    check_thumbnail_cache_recovery(ThumbnailCacheLoss::OriginalAlsoMissing);
}

TEST(StudioDisplayPresentationTest, RepeatedThumbnailEvictionStopsAfterOneRepair)
{
    check_thumbnail_cache_recovery(ThumbnailCacheLoss::RepeatedEviction);
}

TEST(StudioDisplayPresentationTest, ThumbnailRepairDiscardsReplacedListing)
{
    check_thumbnail_cache_recovery(ThumbnailCacheLoss::ListingReplaced);
}

TEST(StudioDisplayPresentationTest, PagedThumbnailUsesPresentationAndEvictionRecovery)
{
    check_thumbnail_cache_recovery(ThumbnailCacheLoss::PagedPresentation);
}

TEST(StudioQmlContract, InspectZoomAdmitsGpuSurfaceWithoutHiddenImageReadiness)
{
    ensure_qt_core();
    QQmlEngine engine;
    QQmlComponent state(&engine);
    state.setData(R"(import QtQuick
QtObject {
    property string browseMode: "loupe"
    property QtObject develop: QtObject { property bool cropToolActive: false }
    property QtObject inspect: QtObject {
        property int gpuPreviewGeneration: 0
        property url previewUrl: "image://preview/photo"
    }
    property int status: 0
})",
                  QUrl{});
    std::unique_ptr<QObject> studio(state.create());
    ASSERT_NE(studio, nullptr) << state.errorString().toStdString();
    auto *inspect = studio->property("inspect").value<QObject *>();
    auto *develop = studio->property("develop").value<QObject *>();
    ASSERT_NE(inspect, nullptr);
    ASSERT_NE(develop, nullptr);
    QQmlComponent component(
        &engine, QUrl::fromLocalFile(QStringLiteral(
                     RAVO_REPOSITORY_ROOT "/Ravo/desktop/qml/inspect/InspectZoomController.qml")));
    std::unique_ptr<QObject> zoom(component.create());
    ASSERT_NE(zoom, nullptr) << component.errorString().toStdString();
    zoom->setProperty("studio", QVariant::fromValue(studio.get()));
    zoom->setProperty("previewImage", QVariant::fromValue(studio.get()));
    EXPECT_FALSE(zoom->property("photoInspectEnabled").toBool());
    inspect->setProperty("gpuPreviewGeneration", 1);
    EXPECT_TRUE(zoom->property("photoInspectEnabled").toBool());
    zoom->setProperty("comparisonReady", true);
    EXPECT_FALSE(zoom->property("photoInspectEnabled").toBool());
    studio->setProperty("status", 1); // Image.Ready
    EXPECT_TRUE(zoom->property("photoInspectEnabled").toBool());
    studio->setProperty("browseMode", "grid");
    EXPECT_FALSE(zoom->property("photoInspectEnabled").toBool());
    studio->setProperty("browseMode", "develop");
    develop->setProperty("cropToolActive", true);
    EXPECT_FALSE(zoom->property("photoInspectEnabled").toBool());
}

TEST(StudioDisplayPresentationTest, ScreenTokenRefreshLeavesRecipeUnchanged)
{
    ensure_qt_core();
    StudioDisplayPresentation owner;
    ASSERT_TRUE(owner.injectSyntheticMatrixForTesting());
    const auto before_token = owner.screenToken();
    const auto before_fingerprint =
        owner.state().value(QStringLiteral("profileFingerprint")).toString();

    DevelopParams develop;
    develop.exposure_ev = -0.25;
    develop.output_color.output_profile = "adobe_rgb";
    const AssetDescriptor asset{"asset-display-owner", "file:///fixture.raw", std::nullopt};
    auto before_recipe = recipe_from_develop(asset, develop);
    ASSERT_TRUE(before_recipe) << before_recipe.error().message;
    auto before_json = serialize_recipe(before_recipe.value());
    ASSERT_TRUE(before_json) << before_json.error().message;

    ASSERT_TRUE(owner.applyScreenTokenForTesting(QStringLiteral("screen-b")));
    EXPECT_EQ(owner.screenToken(), QStringLiteral("screen-b"));
    EXPECT_EQ(owner.state().value(QStringLiteral("profileFingerprint")).toString(),
              before_fingerprint);
    EXPECT_NE(before_token, owner.screenToken());

    auto after_recipe = recipe_from_develop(asset, develop);
    ASSERT_TRUE(after_recipe) << after_recipe.error().message;
    auto after_json = serialize_recipe(after_recipe.value());
    ASSERT_TRUE(after_json) << after_json.error().message;
    EXPECT_EQ(after_json.value(), before_json.value());
    EXPECT_EQ(develop.output_color.output_profile, "adobe_rgb");
    EXPECT_EQ(develop.exposure_ev, -0.25);
}

TEST(StudioDisplayPresentationTest, InitialStateIsMachineVisible)
{
    ensure_qt_core();
    StudioDisplayPresentation owner;
    EXPECT_FALSE(owner.screenToken().isEmpty());
    EXPECT_FALSE(owner.source().isEmpty());
    EXPECT_FALSE(owner.reason().isEmpty());
    EXPECT_TRUE(owner.state().contains(QStringLiteral("contractVersion")));
}

TEST(StudioDisplayPresentationTest, ViewContractsAreMachineVisible)
{
    ensure_qt_core();
    StudioDisplayPresentation owner;
    const auto contracts = owner.viewContracts();
    ASSERT_FALSE(contracts.isEmpty());
    bool saw_display_transformed = false;
    bool saw_scopes = false;
    for (const auto &entry : contracts)
    {
        const auto map = entry.toMap();
        EXPECT_FALSE(map.value(QStringLiteral("viewId")).toString().isEmpty());
        EXPECT_FALSE(map.value(QStringLiteral("pixelKind")).toString().isEmpty());
        EXPECT_EQ(map.value(QStringLiteral("softProofInteraction")).toString(),
                  QStringLiteral("after_soft_proof_display_only"));
        if (map.value(QStringLiteral("pixelKind")).toString() ==
            QStringLiteral("display_transformed"))
            saw_display_transformed = true;
        if (map.value(QStringLiteral("viewId")).toString() == QStringLiteral("scopes"))
            saw_scopes = true;
    }
    EXPECT_TRUE(saw_display_transformed);
    EXPECT_TRUE(saw_scopes);
}

TEST(StudioDisplayPresentationTest, FolderSwitchPublishesBeforeThumbnailWorkAndReusesDisplayCache)
{
    ensure_qt_core();
    init_logging("ravo-folder-presentation-tests");
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    // This case deliberately holds the publication lock and corrupts cache files.
    // Give it a private app-cache namespace so parallel CTest processes are safe.
    const QString previous_name = QCoreApplication::applicationName();
    QCoreApplication::setApplicationName("ravo-gallery-test-" +
                                         QFileInfo(directory.path()).fileName());
    const QString isolated_cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    const auto restore_cache_scope = qScopeGuard(
        [previous_name, isolated_cache]
        {
            QDir(isolated_cache).removeRecursively();
            QCoreApplication::setApplicationName(previous_name);
        });
    const auto catalog = directory.filePath("library.sqlite");
    const auto cache = catalog + ".preview";
    ASSERT_TRUE(QDir().mkpath(cache));
    ASSERT_TRUE(QDir().mkpath(directory.filePath("photos")));
    QImage preview(1600, 1066, QImage::Format_RGB888);
    preview.setColorSpace(QColorSpace(QColorSpace::SRgb));
    preview.fill(QColor(200, 40, 40));
    ASSERT_TRUE(preview.save(cache + "/full.png"));
    auto repository = SqliteCatalogRepository::create(catalog.toStdString());
    ASSERT_TRUE(repository);
    for (int row = 0; row < 200; ++row)
    {
        const auto photo = directory.filePath(QStringLiteral("photos/%1.png").arg(row));
        QImage source(16, 16, QImage::Format_RGB888);
        source.fill(Qt::red);
        ASSERT_TRUE(source.save(photo));
        auto location = normalize_local_input(photo.toStdString());
        ASSERT_TRUE(location);
        AssetRecord asset;
        asset.id = "ast_folder_" + std::to_string(row);
        asset.normalized_uri = location.value().uri;
        asset.media_type = std::string(kMediaTypePng);
        asset.width = asset.height = 16U;
        asset.created_unix_ms = row + 1;
        ASSERT_TRUE(repository.value()->commit_imported_asset(asset));
        PreviewRecord record;
        record.asset_id = asset.id;
        record.cache_key = asset.id;
        record.width = 1600U;
        record.height = 1066U;
        record.state = std::string(kPreviewStateReady);
        record.cache_relpath = "row-" + std::to_string(row) + ".png";
        ASSERT_TRUE(QFile::copy(cache + "/full.png",
                                cache + "/" + QString::fromStdString(*record.cache_relpath)));
        ASSERT_TRUE(repository.value()->upsert_preview(record));
    }
    ASSERT_TRUE(repository.value()->close());
    repository.value().reset();
    StudioDisplayPresentation display;
    ASSERT_TRUE(display.injectSyntheticMatrixForTesting());
    auto owned_presenter = std::make_unique<StudioPresenter>();
    auto &presenter = *owned_presenter;
    presenter.bindDisplayPresentation(&display);
    bool observed_pending_presentation = false;
    std::vector<std::string> first_completions;
    QObject::connect(presenter.assets(), &QAbstractItemModel::dataChanged,
                     [&](const QModelIndex &first, const QModelIndex &last, const QList<int> &)
                     {
                         for (int row = first.row(); row <= last.row(); ++row)
                         {
                             const auto id = presenter.assets()->assetIdAt(row).toStdString();
                             if (presenter.assets()->thumbnailState(id) == "ready")
                                 first_completions.push_back(id);
                         }
                     });
    QObject::connect(&presenter, &StudioPresenter::filterChanged,
                     [&]
                     {
                         if (presenter.visibleCount() == 200)
                         {
                             observed_pending_presentation |=
                                 presenter.assets()->thumbnailState("ast_folder_0") == "presenting";
                             presenter.ensureThumbnail("ast_folder_123");
                         }
                     });
    QElapsedTimer cold_timer;
    cold_timer.start();
    presenter.openCatalogFromPath(catalog);
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }, 30000));
    EXPECT_TRUE(observed_pending_presentation);
    const auto ready = [&]
    {
        if (presenter.visibleCount() != 200)
            return false;
        for (int row = 0; row < 200; ++row)
            if (presenter.assets()->thumbnailState("ast_folder_" + std::to_string(row)) != "ready")
                return false;
        return true;
    };
    ASSERT_TRUE(wait_until(ready, 30000)) << presenter.errorText().toStdString();
    const auto demanded =
        std::find(first_completions.begin(), first_completions.end(), "ast_folder_123");
    ASSERT_NE(demanded, first_completions.end());
    EXPECT_LE(std::distance(first_completions.begin(), demanded), 4)
        << "Visible demand waited behind the page's background presentations";
    RecordProperty("cold_open_all_thumbnails_ms", cold_timer.elapsed());
    const auto thumbnail = [&]
    {
        const int row = presenter.assets()->indexOf("ast_folder_0");
        return presenter.assets()
            ->data(presenter.assets()->index(row, 0), AssetListModel::ThumbnailUrlRole)
            .toUrl();
    };
    const auto cached = thumbnail();
    const auto timestamp = QFileInfo(cached.toLocalFile()).lastModified();
    EXPECT_EQ(QImage(cached.toLocalFile()).width(), 320);
    const auto folder_location = normalize_local_input(directory.filePath("photos").toStdString());
    const auto empty_location = normalize_local_input(directory.filePath("empty").toStdString());
    ASSERT_TRUE(folder_location);
    ASSERT_TRUE(empty_location);
    const QString folder = QString::fromStdString(folder_location.value().uri);
    const QString empty = QString::fromStdString(empty_location.value().uri);
    presenter.selectFolder(empty);
    ASSERT_TRUE(wait_until([&] { return presenter.visibleCount() == 0; }));
    QElapsedTimer timer;
    timer.start();
    presenter.selectFolder(folder);
    ASSERT_TRUE(wait_until([&] { return presenter.visibleCount() == 200; }));
    RecordProperty("folder_listing_ms", timer.elapsed());
    ASSERT_TRUE(wait_until(ready, 30000));
    EXPECT_EQ(thumbnail(), cached);
    EXPECT_EQ(QFileInfo(cached.toLocalFile()).lastModified(), timestamp);
    presenter.selectFolder(empty);
    presenter.selectFolder(folder);
    presenter.selectFolder(empty);
    ASSERT_TRUE(wait_until([&] { return presenter.visibleCount() == 0; }));
    EXPECT_EQ(presenter.selectedFolderUri(), empty);
    presenter.selectFolder(folder);
    ASSERT_TRUE(wait_until([&] { return presenter.visibleCount() == 200; }));
    owned_presenter.reset();
    ASSERT_TRUE(QFileInfo::exists(cached.toLocalFile()));
    // A new owner must reuse the exact published pixels without rewriting PNGs.
    StudioPresenter reopened;
    reopened.bindDisplayPresentation(&display);
    timer.restart();
    reopened.openCatalogFromPath(catalog);
    ASSERT_TRUE(wait_until(
        [&]
        {
            return reopened.visibleCount() == 200 &&
                   reopened.assets()->thumbnailState("ast_folder_0") == "ready";
        },
        30000));
    RecordProperty("warm_reopen_first_thumbnail_ms", timer.elapsed());
    const auto reopened_ready = [&]
    {
        for (int row = 0; row < 200; ++row)
        {
            if (reopened.assets()->thumbnailState("ast_folder_" + std::to_string(row)) != "ready")
                return false;
        }
        return true;
    };
    ASSERT_TRUE(wait_until(reopened_ready, 30000));
    RecordProperty("warm_reopen_all_thumbnails_ms", timer.elapsed());
    const auto reopened_url = [&]
    {
        const auto row = reopened.assets()->indexOf("ast_folder_0");
        return reopened.assets()
            ->data(reopened.assets()->index(row, 0), AssetListModel::ThumbnailUrlRole)
            .toUrl();
    };
    EXPECT_EQ(reopened_url(), cached);
    EXPECT_EQ(QFileInfo(cached.toLocalFile()).lastModified(), timestamp);
    // A monitor change must not reuse pixels transformed for the old monitor.
    StudioDisplayPresentation alternate;
    ASSERT_TRUE(alternate.valid());
    ASSERT_NE(alternate.presentationState().profile_fingerprint,
              display.presentationState().profile_fingerprint);
    reopened.bindDisplayPresentation(&alternate);
    ASSERT_TRUE(wait_until(
        [&]
        {
            return reopened.assets()->thumbnailState("ast_folder_0") == "ready" &&
                   reopened_url() != cached;
        },
        30000));
    const auto alternate_url = reopened_url();
    {
        QFile corrupt(alternate_url.toLocalFile());
        ASSERT_TRUE(corrupt.open(QIODevice::WriteOnly | QIODevice::Truncate));
        ASSERT_EQ(corrupt.write("broken"), 6);
    }
    const auto reload = [&]
    {
        reopened.selectFolder(empty);
        EXPECT_TRUE(wait_until([&] { return reopened.visibleCount() == 0; }));
        reopened.selectFolder(folder);
    };
    reload();
    ASSERT_TRUE(wait_until(
        [&]
        {
            return reopened.assets()->thumbnailState("ast_folder_0") == "ready" &&
                   !QImage(alternate_url.toLocalFile()).isNull();
        },
        30000));
    EXPECT_EQ(reopened_url(), alternate_url);

    // The repaired first row can finish before the remaining 199 publications.
    // Drain the full monitor-change batch before this fixture owns publish.lock;
    // otherwise it can race its own presenter rather than the intended conflict.
    ASSERT_TRUE(wait_until(reopened_ready, 30000));

    // A replaced source preview invalidates the output; a contended publisher
    // must report a conflict and never publish the old monitor-corrected image.
    QImage changed(64, 48, QImage::Format_RGB888);
    changed.setColorSpace(QColorSpace(QColorSpace::SRgb));
    changed.fill(Qt::blue);
    ASSERT_TRUE(changed.save(cache + "/row-0.png"));
    QLockFile lock(QFileInfo(alternate_url.toLocalFile()).dir().filePath("publish.lock"));
    ASSERT_TRUE(lock.tryLock());
    reload();
    ASSERT_TRUE(wait_until(
        [&] { return reopened.assets()->thumbnailState("ast_folder_0") == "failed"; }, 30000));
    EXPECT_TRUE(reopened.errorText().contains("Gallery display cache is busy"));
    lock.unlock();
    reload();
    ASSERT_TRUE(wait_until(
        [&]
        {
            return reopened.assets()->thumbnailState("ast_folder_0") == "ready" &&
                   reopened_url() != alternate_url;
        },
        30000));
    EXPECT_EQ(QImage(reopened_url().toLocalFile()).size(), QSize(64, 48));
    ASSERT_TRUE(wait_until(reopened_ready, 30000));
    const auto damaged_url = reopened_url();
    {
        QFile damaged(damaged_url.toLocalFile());
        ASSERT_TRUE(damaged.open(QIODevice::WriteOnly | QIODevice::Truncate));
        ASSERT_EQ(damaged.write("broken"), 6);
    }
    reopened.thumbnailLoadFailed("ast_folder_0", damaged_url);
    ASSERT_TRUE(wait_until(
        [&]
        {
            return reopened.assets()->thumbnailState("ast_folder_0") == "ready" &&
                   reopened_url() != damaged_url;
        },
        30000));
    EXPECT_FALSE(QImage(reopened_url().toLocalFile()).isNull());
    EXPECT_FALSE(reopened_url().fragment().isEmpty());
    // A stale QML failure must not invalidate replacement pixels.
    reopened.thumbnailLoadFailed("ast_folder_0", damaged_url);
    EXPECT_EQ(reopened.assets()->thumbnailState("ast_folder_0"), "ready");
    reopened.thumbnailLoadFailed("ast_folder_0", reopened_url());
    EXPECT_EQ(reopened.assets()->thumbnailState("ast_folder_0"), "failed");
    EXPECT_TRUE(reopened.errorText().contains("remains unreadable after repair"));
    const auto proxy_row = reopened.assets()->indexOf("ast_folder_1");
    const auto proxy_url =
        reopened.assets()
            ->data(reopened.assets()->index(proxy_row, 0), AssetListModel::ThumbnailUrlRole)
            .toUrl();
    reopened.assets()->setThumbnail("ast_folder_1", proxy_url, QStringLiteral("proxy"));
    const auto work_total = reopened.previewWorkTotal();
    for (int demand = 0; demand < 8; ++demand)
        reopened.ensureThumbnail("ast_folder_1");
    EXPECT_EQ(reopened.previewWorkTotal(), work_total);
    EXPECT_FALSE(reopened.previewWorkActive());
    EXPECT_EQ(reopened.assets()->thumbnailState("ast_folder_1"), "proxy");
}

TEST(StudioDisplayPresentationPerformanceProbe, MeasuresPrivateCatalogFolderSwitch)
{
    const char *catalog = std::getenv("RAVO_FOLDER_PERF_CATALOG");
    const char *folder = std::getenv("RAVO_FOLDER_PERF_URI");
    if (!catalog || !folder)
        GTEST_SKIP() << "requires a private catalog copy and explicit folder URI";
    ensure_qt_core();
    init_logging("ravo-folder-performance");
    StudioDisplayPresentation display;
    StudioPresenter presenter;
    presenter.bindDisplayPresentation(&display);
    presenter.openCatalogFromPath(QString::fromUtf8(catalog));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }, 30000));
    for (int run = 0; run < 3; ++run)
    {
        presenter.selectFolder(QStringLiteral("file:///ravo-folder-performance-empty"));
        ASSERT_TRUE(wait_until([&] { return presenter.visibleCount() == 0; }, 30000));
        QElapsedTimer timer;
        timer.start();
        presenter.selectFolder(QString::fromUtf8(folder));
        ASSERT_TRUE(wait_until([&] { return presenter.visibleCount() > 0; }, 30000))
            << presenter.errorText().toStdString();
        RecordProperty("folder_listing_ms_" + std::to_string(run), timer.elapsed());
        RecordProperty("folder_total", presenter.libraryTotal());
    }
}

TEST(StudioDisplayPresentationTest, GalleryThumbnailAppliesMonitorPresentation)
{
    ensure_qt_core();
    init_logging("ravo-desktop-display-presentation-tests");
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalog = directory.filePath(QStringLiteral("library.sqlite"));
    auto repository = SqliteCatalogRepository::create(catalog.toStdString());
    ASSERT_TRUE(repository) << repository.error().message;

    const QString photo = directory.filePath(QStringLiteral("photo.png"));
    QImage image(48, 32, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(200, 40, 40));
    ASSERT_TRUE(image.save(photo, "PNG"));
    auto location = normalize_local_input(photo.toStdString());
    ASSERT_TRUE(location) << location.error().message;
    auto identity = read_file_identity(location.value().path);
    ASSERT_TRUE(identity) << identity.error().message;

    AssetRecord asset;
    asset.id = "ast_display_thumb";
    asset.normalized_uri = location.value().uri;
    asset.media_type = std::string(kMediaTypePng);
    asset.size_bytes = identity.value().size_bytes;
    asset.mtime_unix_ms = identity.value().mtime_unix_ms;
    asset.content_fingerprint = make_content_fingerprint(identity.value());
    asset.width = 48U;
    asset.height = 32U;
    asset.created_unix_ms = 1000;
    ASSERT_TRUE(repository.value()->commit_imported_asset(asset));
    ASSERT_TRUE(repository.value()->close());
    repository.value().reset();

    StudioDisplayPresentation display;
    ASSERT_TRUE(display.injectSyntheticMatrixForTesting());

    StudioPresenter presenter;
    presenter.bindDisplayPresentation(&display);
    presenter.openCatalogFromPath(catalog);
    ASSERT_TRUE(wait_until(
        [&]
        {
            return presenter.catalogOpen() && !presenter.busy() && presenter.visibleCount() == 1 &&
                   !presenter.selectedAssetId().isEmpty();
        }))
        << presenter.errorText().toStdString();

    presenter.ensureThumbnail(presenter.selectedAssetId());
    ASSERT_TRUE(wait_until(
        [&]
        {
            return presenter.assets()->thumbnailState(presenter.selectedAssetId().toStdString()) ==
                       QLatin1String("ready") &&
                   !presenter.selectedThumbnailUrl().isEmpty() && !presenter.previewWorkActive();
        }))
        << presenter.errorText().toStdString();

    const QUrl presented_url = presenter.selectedThumbnailUrl();
    ASSERT_TRUE(presented_url.isLocalFile());
    QImage presented(presented_url.toLocalFile());
    ASSERT_FALSE(presented.isNull());
    presented = presented.convertToFormat(QImage::Format_RGB888);
    const QRgb presented_pixel = presented.pixel(presented.width() / 2, presented.height() / 2);

    // Synthetic matrix boosts R and attenuates G; presented pixels must differ from source fill.
    EXPECT_NE(qRed(presented_pixel), 200);
    EXPECT_NE(qGreen(presented_pixel), 40);

    DevelopParams develop;
    develop.exposure_ev = 0.5;
    const AssetDescriptor descriptor{asset.id, asset.normalized_uri, std::nullopt};
    auto before_recipe = recipe_from_develop(descriptor, develop);
    ASSERT_TRUE(before_recipe) << before_recipe.error().message;
    auto before_json = serialize_recipe(before_recipe.value());
    ASSERT_TRUE(before_json) << before_json.error().message;

    ASSERT_TRUE(display.applyScreenTokenForTesting(QStringLiteral("gallery-thumb-screen-b")));
    ASSERT_TRUE(wait_until(
        [&]
        {
            const QUrl url = presenter.selectedThumbnailUrl();
            return url.isLocalFile() && QImage(url.toLocalFile()).width() > 0;
        }))
        << presenter.errorText().toStdString();

    auto after_recipe = recipe_from_develop(descriptor, develop);
    ASSERT_TRUE(after_recipe) << after_recipe.error().message;
    auto after_json = serialize_recipe(after_recipe.value());
    ASSERT_TRUE(after_json) << after_json.error().message;
    EXPECT_EQ(after_json.value(), before_json.value());
    EXPECT_EQ(develop.exposure_ev, 0.5);

    bool saw_gallery = false;
    for (const auto &entry : display.viewContracts())
    {
        const auto map = entry.toMap();
        if (map.value(QStringLiteral("viewId")).toString() == QStringLiteral("gallery_thumbnail"))
        {
            EXPECT_EQ(map.value(QStringLiteral("pixelKind")).toString(),
                      QStringLiteral("display_transformed"));
            saw_gallery = true;
        }
    }
    EXPECT_TRUE(saw_gallery);
    const auto presented_timestamp = QFileInfo(presented_url.toLocalFile()).lastModified();
    StudioPresenter reopened;
    reopened.bindDisplayPresentation(&display);
    reopened.openCatalogFromPath(catalog);
    ASSERT_TRUE(wait_until(
        [&]
        {
            return reopened.catalogOpen() && !reopened.busy() &&
                   reopened.selectedThumbnailUrl() == presented_url;
        }));
    EXPECT_EQ(QFileInfo(presented_url.toLocalFile()).lastModified(), presented_timestamp);
}

#if defined(Q_OS_MACOS)
TEST(StudioDisplayPresentationTest, GpuNativeOwnedSurfaceAppliesMonitorPresentation)
{
    ensure_qt_core();
    auto created = studio_metal::create_iosurface_rgba8(32U, 24U);
    ASSERT_TRUE(created) << created.error().message;
    QImage source(32, 24, QImage::Format_RGB888);
    source.setColorSpace(QColorSpace(QColorSpace::SRgb));
    source.fill(QColor(200, 40, 40));
    ASSERT_TRUE(studio_metal::write_rgb8_to_iosurface(created.value(), source));

    auto snap = studio_metal::snapshot_iosurface_rgb8(created.value(), 32U, 24U);
    ASSERT_TRUE(snap) << snap.error().message;
    EXPECT_EQ(snap.value().pixel(16, 12), source.pixel(16, 12));

    auto presentation = make_synthetic_matrix_monitor_presentation(
        {1.25f, 0.0f, 0.0f, 0.0f, 0.5f, 0.0f, 0.0f, 0.0f, 1.0f}, "gpu-native-matrix");
    ASSERT_TRUE(presentation) << presentation.error().message;

    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(32 * 24 * 3));
    for (int y = 0; y < 24; ++y)
    {
        std::copy_n(source.constScanLine(y), 32U * 3U,
                    pixels.begin() + static_cast<std::ptrdiff_t>(y * 32 * 3));
    }
    ColorProfileState srgb;
    srgb.identifier = "srgb";
    auto converted = apply_display_presentation_rgb8(pixels, 32U, 24U, srgb, presentation.value(),
                                                     CancellationToken{});
    ASSERT_TRUE(converted) << converted.error().message;
    QImage presented(32, 24, QImage::Format_RGB888);
    for (int y = 0; y < 24; ++y)
    {
        std::copy_n(converted.value().rgb8.data() + static_cast<std::ptrdiff_t>(y * 32 * 3),
                    32U * 3U, presented.scanLine(y));
    }
    ASSERT_TRUE(studio_metal::write_rgb8_to_iosurface(created.value(), presented));
    auto presented_snap = studio_metal::snapshot_iosurface_rgb8(created.value(), 32U, 24U);
    ASSERT_TRUE(presented_snap) << presented_snap.error().message;
    const QRgb out = presented_snap.value().pixel(16, 12);
    EXPECT_NE(qRed(out), 200);
    EXPECT_NE(qGreen(out), 40);
    EXPECT_EQ(out, presented.pixel(16, 12));

    DevelopParams develop;
    develop.exposure_ev = 0.25;
    const AssetDescriptor asset{"asset-gpu-present", "file:///fixture.raw", std::nullopt};
    auto before_recipe = recipe_from_develop(asset, develop);
    ASSERT_TRUE(before_recipe) << before_recipe.error().message;
    auto before_json = serialize_recipe(before_recipe.value());
    ASSERT_TRUE(before_json) << before_json.error().message;

    StudioDisplayPresentation owner;
    ASSERT_TRUE(owner.injectSyntheticMatrixForTesting());
    ASSERT_TRUE(owner.applyScreenTokenForTesting(QStringLiteral("gpu-present-screen-b")));

    auto after_recipe = recipe_from_develop(asset, develop);
    ASSERT_TRUE(after_recipe) << after_recipe.error().message;
    auto after_json = serialize_recipe(after_recipe.value());
    ASSERT_TRUE(after_json) << after_json.error().message;
    EXPECT_EQ(after_json.value(), before_json.value());
    EXPECT_EQ(develop.exposure_ev, 0.25);

    bool saw_gpu = false;
    for (const auto &entry : owner.viewContracts())
    {
        const auto map = entry.toMap();
        if (map.value(QStringLiteral("viewId")).toString() == QStringLiteral("gpu_native_preview"))
        {
            EXPECT_EQ(map.value(QStringLiteral("pixelKind")).toString(),
                      QStringLiteral("display_transformed"));
            saw_gpu = true;
        }
    }
    EXPECT_TRUE(saw_gpu);
    studio_metal::release_iosurface(created.value());
}
#endif

} // namespace
} // namespace ravo
