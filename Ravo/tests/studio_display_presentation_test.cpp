#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include <QColor>
#include <QColorSpace>
#include <QCoreApplication>
#include <QImage>
#include <QTemporaryDir>
#include <QDir>
#include <QFileInfo>
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
#if defined(Q_OS_MACOS)
#include "studio_iosurface_snapshot.h"
#include "ravo/services/display_presentation.h"
#endif

namespace ravo
{
namespace
{
using studio_test_support::wait_until;

void ensure_qt_core()
{
    if (QCoreApplication::instance() != nullptr)
        return;
    static int argc = 1;
    static char executable[] = "ravo-desktop-display-presentation-tests";
    static char *argv[] = {executable, nullptr};
    static auto *application = new QCoreApplication(argc, argv);
    static_cast<void>(application);
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
        record.cache_relpath = "full.png";
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
    QObject::connect(&presenter, &StudioPresenter::filterChanged, [&]
    {
        if (presenter.visibleCount() == 200)
            observed_pending_presentation |= presenter.assets()->thumbnailState("ast_folder_0") == "presenting";
    });
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
    const auto thumbnail = [&]
    {
        const int row = presenter.assets()->indexOf("ast_folder_0");
        return presenter.assets()->data(presenter.assets()->index(row, 0), AssetListModel::ThumbnailUrlRole).toUrl();
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
    EXPECT_FALSE(QFileInfo::exists(cached.toLocalFile()));
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
