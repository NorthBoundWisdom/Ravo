#include <QColorSpace>
#include <QColor>
#include <QTemporaryDir>
#include <array>
#include <gtest/gtest.h>

#include "ravo/desktop/studio_display_presentation.h"
#include "ravo/desktop/studio_inspect_presenter.h"
#include "ravo/desktop/studio_presenter.h"
#include "ravo/desktop/studio_command_controller.h"
#include "ravo/foundation/log.h"
#include "studio_test_support.h"

namespace ravo
{
namespace testing
{
class StudioDisplayPresentationTestControl
{
public:
    static void publishPresentation(StudioDisplayPresentation &owner,
                                    DisplayPresentationState state)
    {
        owner.publish(std::move(state));
    }
    static void publishCorruptProfile(StudioDisplayPresentation &owner)
    {
        auto state = owner.presentationState();
        state.source = DisplayProfileSource::kInjectedPath;
        state.monitor_profile.icc_bytes = {1, 2, 3};
        state.profile_fingerprint = "invalid-monitor-profile";
        owner.publish(std::move(state));
    }
};
} // namespace testing
namespace
{
// Frame tests share only construction and immutable input data.
// Each test owns publication, fault injection and its independent state oracle.
struct FrameOwner
{
    const QString asset = QStringLiteral("ast-frame");
    const QString catalog = QStringLiteral("isolated-frame-test");
    const std::optional<EngineFacade> engine;
    SerialExecutor executor;
    StudioInspectPresenter inspect{{asset, catalog, engine, executor},
                                   {[]() -> PreviewService * { return nullptr; },
                                    [] { return DevelopParams{}; }, [] { return false; }, [] {}}};

    FrameOwner()
    {
        inspect.observeSelection(asset);
    }
};
PreviewResult frame_preview()
{
    PreviewResult preview;
    preview.asset_id = "ast-frame";
    preview.width = 2;
    preview.height = 2;
    preview.rgb = {40, 80, 120, 160, 200, 240, 20, 60, 100, 140, 180, 220};
    preview.color_profile.kind = ColorProfileKind::kIcc;
    preview.color_profile.identifier = "srgb";
    const auto icc = QColorSpace(QColorSpace::SRgb).iccProfile();
    preview.color_profile.icc_bytes.assign(icc.begin(), icc.end());
    return preview;
}
} // namespace
TEST(StudioInspectFrame, FailedPreparationRetainsPublishedPixelsAndResourceIdentity)
{
    studio_test_support::ensure_qt_core();
    FrameOwner owner;
    auto &inspect = owner.inspect;
    auto preview = frame_preview();
    ASSERT_FALSE(preview.color_profile.icc_bytes.empty());
    ASSERT_TRUE(inspect.show_preview_result(preview, 8, true));
    DevelopParams displayed;
    displayed.exposure_ev = 0.5;
    inspect.observeDisplayedDevelop(displayed);
    ASSERT_TRUE(studio_test_support::wait_until([&] { return !inspect.identityPending(); }));
    const auto image = inspect.previewImage();
    const auto url = inspect.previewUrl();
    const auto digest = inspect.pixelSha256();
    const auto width = inspect.previewViewportWidth();
    const auto height = inspect.previewViewportHeight();
    QString error;
    QObject::connect(&inspect, &StudioInspectPresenter::errorOccurred, &inspect,
                     [&](QString value) { error = std::move(value); });

    auto invalid = preview;
    invalid.color_profile.icc_bytes = {1, 2, 3};
    invalid.gpu_display_generation = 17;
    invalid.gpu_display_native_surface = 99;
    invalid.gpu_display_width = 2;
    invalid.gpu_display_height = 2;
    EXPECT_FALSE(inspect.show_preview_result(invalid, 9, true));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_EQ(inspect.previewImage(), image);
    EXPECT_EQ(inspect.previewUrl(), url);
    EXPECT_EQ(inspect.frameRevision(), 8U);
    EXPECT_EQ(inspect.previewViewportWidth(), width);
    EXPECT_EQ(inspect.previewViewportHeight(), height);
    EXPECT_EQ(inspect.gpuPreviewGeneration(), 0U);
    EXPECT_EQ(inspect.gpuPreviewNativeSurface(), 0U);
    EXPECT_EQ(inspect.pixelSha256(), digest);
    EXPECT_FALSE(inspect.identityPending());
    ASSERT_TRUE(inspect.displayedDevelop());
    EXPECT_EQ(*inspect.displayedDevelop(), displayed);
    EXPECT_EQ(preview.rgb,
              (std::vector<std::uint8_t>{40, 80, 120, 160, 200, 240, 20, 60, 100, 140, 180, 220}));
}

TEST(StudioInspectFrame, NavigatorExtentIgnoresPreviewRoundingAndResetsForGeometry)
{
    studio_test_support::ensure_qt_core();
    FrameOwner owner;
    auto &inspect = owner.inspect;
    inspect.seedViewport(6000, 4000);
    std::uint64_t revision = 20;
    for (const auto size : {QSize{320, 213}, QSize{1600, 1067}, QSize{960, 640}})
    {
        auto preview = frame_preview();
        preview.width = static_cast<std::uint32_t>(size.width());
        preview.height = static_cast<std::uint32_t>(size.height());
        preview.rgb.assign(static_cast<std::size_t>(preview.width) * preview.height * 3, 120);
        ASSERT_TRUE(inspect.show_preview_result(preview, revision++, false));
        EXPECT_EQ(inspect.navigatorViewportWidth(), 6000);
        EXPECT_EQ(inspect.navigatorViewportHeight(), 4000);
        EXPECT_EQ(inspect.previewViewportWidth(), size.width());
        EXPECT_EQ(inspect.previewViewportHeight(), size.height());
    }
    auto square = frame_preview();
    // A previously cropped Gallery photo gets its aspect from the browse
    // thumbnail before the first exposure edit requests a full preview.
    inspect.clear_displayed_preview();
    inspect.seedViewport(6000, 4000);
    const QImage thumbnail(320, 320, QImage::Format_RGB888);
    inspect.observeNavigatorThumbnail(thumbnail);
    EXPECT_EQ(inspect.navigatorViewportWidth(), inspect.navigatorViewportHeight());
    square.width = 640;
    square.height = 640;
    square.rgb.assign(640U * 640U * 3U, 120);
    ASSERT_TRUE(inspect.show_preview_result(square, revision++, false));
    EXPECT_EQ(inspect.navigatorViewportWidth(), inspect.navigatorViewportHeight());
    inspect.seedViewport(4000, 6000);
    EXPECT_EQ(inspect.navigatorViewportWidth(), 4000);
    EXPECT_EQ(inspect.navigatorViewportHeight(), 6000);
    inspect.clear_displayed_preview();
    EXPECT_EQ(inspect.navigatorViewportWidth(), 0);
    EXPECT_EQ(inspect.navigatorViewportHeight(), 0);
}

TEST(StudioInspectFrame, PendingThumbnailCorrectsBothViewportsWithoutReplacingPublishedFrame)
{
    studio_test_support::ensure_qt_core();
    FrameOwner owner;
    auto &inspect = owner.inspect;
    for (const auto size : {QSize{200, 320}, QSize{320, 200}, QSize{320, 320}})
    {
        inspect.clear_displayed_preview();
        inspect.seedViewport(1600, 1067);
        inspect.setPreviewLoading(true);
        const QImage thumbnail(size, QImage::Format_RGB888);
        inspect.observeNavigatorThumbnail(thumbnail);
        const double expected_aspect = static_cast<double>(size.width()) / size.height();
        EXPECT_NEAR(static_cast<double>(inspect.previewViewportWidth()) /
                        inspect.previewViewportHeight(),
                    expected_aspect, 0.002);
        EXPECT_NEAR(static_cast<double>(inspect.navigatorViewportWidth()) /
                        inspect.navigatorViewportHeight(),
                    expected_aspect, 0.002);
        EXPECT_TRUE(inspect.previewImage().isNull());
        EXPECT_TRUE(inspect.previewUrl().isEmpty());
        EXPECT_TRUE(inspect.previewLoading());
    }
    auto preview = frame_preview();
    ASSERT_TRUE(inspect.show_preview_result(preview, 42, false));
    const auto image = inspect.previewImage();
    const auto url = inspect.previewUrl();
    inspect.observeNavigatorThumbnail(QImage(200, 320, QImage::Format_RGB888));
    EXPECT_EQ(inspect.previewViewportWidth(), 2);
    EXPECT_EQ(inspect.previewViewportHeight(), 2);
    EXPECT_EQ(inspect.navigatorViewportWidth(), inspect.navigatorViewportHeight());
    EXPECT_EQ(inspect.previewImage(), image);
    EXPECT_EQ(inspect.previewUrl(), url);
    EXPECT_EQ(inspect.frameRevision(), 42U);
}

TEST(StudioInspectFrame, MonitorConversionFailureRetainsFrameAndReportsError)
{
    studio_test_support::ensure_qt_core();
    StudioDisplayPresentation display;
    ASSERT_TRUE(display.injectSyntheticMatrixForTesting());
    FrameOwner owner;
    auto &inspect = owner.inspect;
    inspect.bindDisplayPresentation(&display);
    const auto preview = frame_preview();
    ASSERT_TRUE(inspect.show_preview_result(preview, 8, true));
    ASSERT_TRUE(studio_test_support::wait_until([&] { return !inspect.identityPending(); }));
    const auto image = inspect.previewImage();
    const auto url = inspect.previewUrl();
    const auto digest = inspect.pixelSha256();
    QString error;
    QObject::connect(&inspect, &StudioInspectPresenter::errorOccurred, &inspect,
                     [&](QString value) { error = std::move(value); });

    testing::StudioDisplayPresentationTestControl::publishCorruptProfile(display);
    EXPECT_FALSE(error.isEmpty());
    EXPECT_EQ(inspect.previewImage(), image);
    EXPECT_EQ(inspect.previewUrl(), url);
    EXPECT_EQ(inspect.frameRevision(), 8U);
    EXPECT_EQ(inspect.pixelSha256(), digest);
    EXPECT_FALSE(inspect.show_preview_result(preview, 9, true));
    EXPECT_EQ(inspect.previewImage(), image);
    EXPECT_EQ(inspect.previewUrl(), url);
    EXPECT_EQ(inspect.frameRevision(), 8U);
    EXPECT_EQ(inspect.pixelSha256(), digest);
    inspect.restorePreviewBase();
    EXPECT_EQ(inspect.previewImage(), image);
    EXPECT_FALSE(inspect.identityPending());
    ASSERT_TRUE(display.injectSyntheticMatrixForTesting());
    ASSERT_TRUE(inspect.show_preview_result(preview, 9, true));
    EXPECT_EQ(inspect.frameRevision(), 9U);
}
TEST(StudioInspectFrame, RestoredPreviewRetainsMonitorPresentation)
{
    studio_test_support::ensure_qt_core();
    StudioDisplayPresentation display;
    ASSERT_TRUE(display.injectSyntheticMatrixForTesting());
    FrameOwner owner;
    auto &inspect = owner.inspect;
    inspect.bindDisplayPresentation(&display);
    ASSERT_TRUE(inspect.show_preview_result(frame_preview(), 8, true));
    const auto presented = inspect.previewImage();
    ASSERT_NE(presented, inspect.previewBaseImage());
    const auto url = inspect.previewUrl();
    inspect.restorePreviewBase();
    EXPECT_EQ(inspect.previewImage(), presented);
    EXPECT_EQ(inspect.previewUrl(), url);
    EXPECT_EQ(inspect.frameRevision(), 8U);
}

TEST(StudioInspectFrame, AdoptedComparisonReappliesItsOwnSourceAfterDisplayChange)
{
    studio_test_support::ensure_qt_core();
    StudioDisplayPresentation display;
    ASSERT_TRUE(display.injectSyntheticMatrixForTesting());
    FrameOwner owner;
    auto &inspect = owner.inspect;
    inspect.bindDisplayPresentation(&display);
    const auto before = frame_preview();
    ASSERT_TRUE(inspect.show_preview_result(before, 8, true));
    ASSERT_TRUE(inspect.adoptPreviewAsComparison());
    const auto comparison_url = inspect.comparisonBeforeUrl();
    const auto original_comparison = inspect.comparisonBeforeImage();
    ASSERT_FALSE(original_comparison.isNull());

    auto after = before;
    after.rgb = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120};
    ASSERT_TRUE(inspect.show_preview_result(after, 9, true));
    EXPECT_EQ(inspect.comparisonBeforeImage(), original_comparison);
    const std::array<float, 9> swap_red_green{0, 1, 0, 1, 0, 0, 0, 0, 1};
    auto presentation =
        make_synthetic_matrix_monitor_presentation(swap_red_green, "comparison-channel-swap");
    ASSERT_TRUE(presentation) << presentation.error().message;
    testing::StudioDisplayPresentationTestControl::publishPresentation(
        display, std::move(presentation).value());

    // Independent pixel oracle: the display matrix swaps encoded red and green.
    EXPECT_EQ(inspect.comparisonBeforeImage().pixel(0, 0), qRgb(80, 40, 120));
    EXPECT_EQ(inspect.previewImage().pixel(0, 0), qRgb(20, 10, 30));
    EXPECT_EQ(inspect.comparisonBeforeUrl(), comparison_url);
    EXPECT_EQ(inspect.frameRevision(), 9U);
    EXPECT_EQ(before.rgb,
              (std::vector<std::uint8_t>{40, 80, 120, 160, 200, 240, 20, 60, 100, 140, 180, 220}));
}

TEST(StudioInspectFrame, RejectedRoiDispatchReportsFailureWithoutPublishing)
{
    studio_test_support::ensure_qt_core();
    FrameOwner owner;
    auto &inspect = owner.inspect;
    inspect.setZoomMode(QStringLiteral("actual"));
    owner.executor.request_stop();
    owner.executor.wait();
    QString error;
    QObject::connect(&inspect, &StudioInspectPresenter::errorOccurred, &inspect,
                     [&](QString value) { error = std::move(value); });
    inspect.requestInspectRoi(0.1, 0.1, 0.2, 0.2);
    EXPECT_FALSE(error.isEmpty());
    EXPECT_TRUE(inspect.inspectRoiUrl().isEmpty());
    EXPECT_TRUE(inspect.inspectRoiImage().isNull());
    EXPECT_EQ(inspect.gpuRoiNativeSurface(), 0U);
}
TEST(StudioInspectFrame, ShutdownRejectsLateFramePublication)
{
    studio_test_support::ensure_qt_core();
    FrameOwner owner;
    auto &inspect = owner.inspect;
    const auto preview = frame_preview();
    ASSERT_TRUE(inspect.show_preview_result(preview, 8, true));
    const auto image = inspect.previewImage();
    const auto url = inspect.previewUrl();
    inspect.shutdown();
    QString error;
    QObject::connect(&inspect, &StudioInspectPresenter::errorOccurred, &inspect,
                     [&](QString value) { error = std::move(value); });
    EXPECT_FALSE(inspect.show_preview_result(preview, 9, true));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_EQ(inspect.previewImage(), image);
    EXPECT_EQ(inspect.previewUrl(), url);
    EXPECT_EQ(inspect.frameRevision(), 8U);
    EXPECT_FALSE(inspect.adoptPreviewAsComparison());
    EXPECT_TRUE(inspect.comparisonBeforeUrl().isEmpty());
    inspect.requestInspectRoi(0.1, 0.1, 0.2, 0.2);
    EXPECT_TRUE(inspect.inspectRoiUrl().isEmpty());
    EXPECT_EQ(inspect.gpuPreviewNativeSurface(), 0U);
    EXPECT_EQ(inspect.gpuRoiNativeSurface(), 0U);
}
TEST(StudioPresenterTest, PendingBeforeFrameIsNotAdoptedAsComparison)
{
    using namespace studio_test_support;
    ensure_qt_core();
    ravo::init_logging("ravo-desktop-command-tests");
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString photo = directory.filePath(QStringLiteral("pending-comparison.png"));
    QImage image(96, 64, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(70, 105, 150));
    ASSERT_TRUE(image.save(photo, "PNG"));

    StudioPresenter presenter;
    StudioCommandController commands(presenter);
    presenter.createCatalogFromPath(directory.filePath(QStringLiteral("library.sqlite")));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
    presenter.imports()->importFilePaths({photo});
    ASSERT_TRUE(wait_until(
        [&]
        {
            return presenter.visibleCount() == 1 && !presenter.selectedAssetId().isEmpty() &&
                   !presenter.busy();
        }));
    presenter.openDevelop();
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.inspect()->previewLoading() &&
                   !presenter.inspect()->previewImage().isNull();
        }));
    const QImage unedited = presenter.inspect()->previewImage();
    const QPoint center(unedited.width() / 2, unedited.height() / 2);
    const QColor original_pixel = unedited.pixelColor(center);
    presenter.develop()->setDevelopNumber(QStringLiteral("exposure"), 1.0);
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.inspect()->previewLoading() &&
                   presenter.inspect()->previewUrl().isLocalFile();
        }));
    const QColor edited_pixel = presenter.inspect()->previewImage().pixelColor(center);
    ASSERT_NE(original_pixel, edited_pixel);

    const auto before_action = commands.ids().value(QStringLiteral("editBeforeAfter")).toString();
    const auto comparison_action =
        commands.ids().value(QStringLiteral("editComparison")).toString();
    ASSERT_TRUE(commands.executeAction(before_action, QStringLiteral("control"))
                    .value(QStringLiteral("accepted"))
                    .toBool());
    ASSERT_TRUE(presenter.inspect()->previewLoading());
    // No event-loop turn can publish the requested Before frame between these intents.
    ASSERT_TRUE(commands.executeAction(comparison_action, QStringLiteral("control"))
                    .value(QStringLiteral("accepted"))
                    .toBool());
    ASSERT_TRUE(wait_until(
        [&]
        {
            return presenter.develop()->comparisonActive() &&
                   !presenter.inspect()->previewLoading() &&
                   !presenter.inspect()->comparisonBeforeImage().isNull();
        }));
    EXPECT_EQ(presenter.inspect()->comparisonBeforeImage().pixelColor(center), original_pixel);
    EXPECT_EQ(presenter.inspect()->previewImage().pixelColor(center), edited_pixel);
    EXPECT_EQ(presenter.develop()->editExposure(), 1.0);
}
} // namespace ravo
