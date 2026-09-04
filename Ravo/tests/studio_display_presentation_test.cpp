#include <QColor>
#include <QColorSpace>
#include <QCoreApplication>
#include <QImage>
#include <QTemporaryDir>
#include <QThread>
#include <gtest/gtest.h>

#include "ravo/adapters/sqlite_catalog.h"
#include "ravo/desktop/studio_display_presentation.h"
#include "ravo/desktop/studio_presenter.h"
#include "ravo/domain/types.h"
#include "ravo/domain/uri.h"
#include "ravo/foundation/log.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/recipe.h"
#include "studio_test_support.h"

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

} // namespace
} // namespace ravo
