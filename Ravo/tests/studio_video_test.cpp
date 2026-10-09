#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <QTemporaryDir>
#include <QCoreApplication>
#include <QPointer>
#include <QFileInfo>
#include "ravo/domain/uri.h"
#include "ravo/desktop/studio_presenter.h"
#include "ravo/desktop/studio_command_controller.h"
#include "ravo/desktop/studio_live_session_controller.h"
#include "ravo/adapters/ffmpeg_video_decoder.h"
#include "ravo/services/video.h"
#include "ravo/foundation/log.h"
#include "studio_test_support.h"
namespace ravo
{
using namespace studio_test_support;
TEST(StudioVideoTest, PlaybackSeekMuteAndSelectionReleaseOwnedFrames)
{
    ensure_qt_core();
    init_logging("ravo-studio-video-tests");
    QTemporaryDir directory;
    StudioPresenter presenter;
    StudioCommandController commands(presenter);
    presenter.createCatalogFromPath(directory.filePath("library.sqlite"));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
    presenter.imports()->importFilePaths(
        {QStringLiteral(RAVO_REPOSITORY_ROOT "/Ravo/tests/fixtures/video/hlg.mp4")});
    ASSERT_TRUE(wait_until(
        [&]
        {
            return presenter.visibleCount() == 1 && !presenter.imports()->importWorkActive() &&
                   !presenter.selectedAssetId().isEmpty();
        }));
    EXPECT_TRUE(presenter.errorText().isEmpty()) << presenter.errorText().toStdString();
    EXPECT_FALSE(presenter.video()->available());
    presenter.setBrowseMode(QStringLiteral("loupe"));
    ASSERT_TRUE(wait_until(
        [&]
        {
            return presenter.video()->state() == QStringLiteral("paused") ||
                   !presenter.video()->error().isEmpty();
        },
        15000));
    ASSERT_TRUE(presenter.video()->error().isEmpty()) << presenter.video()->error().toStdString();
    ASSERT_TRUE(wait_until([&] { return !presenter.inspect()->previewBaseImage().isNull(); }))
        << presenter.errorText().toStdString() << ";mode=" << presenter.browseMode().toStdString()
        << ";video=" << serialize_json(presenter.video()->jsonSnapshot());
    EXPECT_FALSE(
        commands.action(QStringLiteral("studio.view.show_develop")).value("enabled").toBool());
    EXPECT_FALSE(
        commands.action(QStringLiteral("studio.photo.adjust_exposure")).value("enabled").toBool());
    EXPECT_FALSE(
        commands.action(QStringLiteral("studio.edit.rotate_left")).value("enabled").toBool());
    const auto formats = presenter.exports()->exportFormatChoices();
    ASSERT_EQ(formats.size(), 1);
    EXPECT_EQ(formats.front().toMap().value("id").toString(), QStringLiteral("original"));
    EXPECT_EQ(presenter.exports()->exportDefaultOptions().value("format").toString(),
              QStringLiteral("original"));
    ASSERT_TRUE(commands.executeCommand(QStringLiteral("studio.video.mute"), true)
                    .value("accepted")
                    .toBool());
    ASSERT_TRUE(commands.executeCommand(QStringLiteral("studio.video.volume"), .25)
                    .value("accepted")
                    .toBool());
    EXPECT_TRUE(presenter.video()->muted());
    EXPECT_NEAR(presenter.video()->volume(), .25, 1e-5);
    EXPECT_FALSE(commands.executeCommand(QStringLiteral("studio.video.volume"), 2.)
                     .value("accepted")
                     .toBool());
    auto live = StudioLiveSessionController::create(presenter, commands);
    ASSERT_TRUE(live) << live.error().message;
    auto state = live.value()->snapshot();
    const auto revision = QString::fromStdString(state.find("revision")->number_if()->text);
    const auto selection =
        QString::fromStdString(state.find("selection")->find("revision")->number_if()->text);
    const auto generation =
        QString::fromStdString(state.find("video")->find("generation")->number_if()->text);
    const QStringList control{"studio",
                              "video",
                              "--session-id",
                              QString::fromStdString(live.value()->descriptor().session_id),
                              "--asset-id",
                              presenter.selectedAssetId(),
                              "--expect-session-revision",
                              revision,
                              "--expect-selection-revision",
                              selection,
                              "--expect-video-generation",
                              generation,
                              "--action",
                              "volume",
                              "--value",
                              "0.3",
                              "--json"};
    auto changed = run_cli_process(control);
    ASSERT_EQ(changed.exit_code, 0) << changed.standard_output.toStdString();
    EXPECT_NEAR(presenter.video()->volume(), .3, 1e-5);
    auto stale = run_cli_process(control);
    EXPECT_NE(stale.exit_code, 0);
    EXPECT_TRUE(stale.standard_output.contains("stale_video_request"));
    const auto poster_revision = presenter.inspect()->frameRevision();
    ASSERT_TRUE(
        commands.executeCommand(QStringLiteral("studio.video.play")).value("accepted").toBool());
    ASSERT_TRUE(wait_until(
        [&]
        {
            return (presenter.video()->position() > 100 &&
                    presenter.inspect()->frameRevision() > poster_revision) ||
                   !presenter.video()->error().isEmpty();
        }));
    ASSERT_TRUE(presenter.video()->error().isEmpty()) << presenter.video()->error().toStdString();
    ASSERT_TRUE(
        commands.executeCommand(QStringLiteral("studio.video.pause")).value("accepted").toBool());
    EXPECT_EQ(presenter.video()->state(), QStringLiteral("paused"));
    EXPECT_TRUE(commands.executeCommand(QStringLiteral("studio.video.seek"), 500)
                    .value("accepted")
                    .toBool());
    ASSERT_TRUE(wait_until([&] { return presenter.video()->position() >= 500; }));
    FfmpegVideoDecoder decoder;
    auto reference = decode_video_preview(
        decoder, RAVO_REPOSITORY_ROOT "/Ravo/tests/fixtures/video/hlg.mp4", 500000, 1600);
    ASSERT_TRUE(reference);
    const auto image = presenter.inspect()->previewBaseImage();
    ASSERT_EQ(image.width(), static_cast<int>(reference.value().image.width));
    const auto actual = image.pixelColor(image.width() / 2, image.height() / 2);
    const auto offset = (reference.value().image.height / 2 * reference.value().image.width +
                         reference.value().image.width / 2) *
                        3U;
    EXPECT_NEAR(actual.red(), reference.value().image.rgb[offset], 2);
    EXPECT_NEAR(actual.green(), reference.value().image.rgb[offset + 1], 2);
    EXPECT_NEAR(actual.blue(), reference.value().image.rgb[offset + 2], 2);
    presenter.setBrowseMode(QStringLiteral("grid"));
    EXPECT_FALSE(presenter.video()->available());
    EXPECT_EQ(presenter.video()->state(), QStringLiteral("empty"));
    EXPECT_FALSE(
        commands.executeCommand(QStringLiteral("studio.video.play")).value("accepted").toBool());
    presenter.setBrowseMode(QStringLiteral("loupe"));
    ASSERT_TRUE(wait_until([&] { return presenter.video()->state() == QStringLiteral("paused"); }));
    presenter.video()->shutdown();
    EXPECT_FALSE(presenter.video()->available());
}
TEST(StudioVideoTest, AuxiliaryAudioDiagnosticsPersistAndSupportedTrackPlays)
{
    ensure_qt_core();
    init_logging("ravo-studio-video-tests");
    QTemporaryDir directory;
    const auto catalog = directory.filePath("library.sqlite");
    StudioPresenter presenter;
    presenter.createCatalogFromPath(catalog);
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
    presenter.imports()->importFilePaths(
        {QStringLiteral(RAVO_REPOSITORY_ROOT "/Ravo/tests/fixtures/video/auxiliary_audio.mov")});
    ASSERT_TRUE(wait_until(
        [&]
        {
            return presenter.visibleCount() == 1 && !presenter.imports()->importWorkActive() &&
                   !presenter.selectedAssetId().isEmpty();
        }));
    const auto info = run_cli_process({"catalog", "video-info", "--catalog", catalog, "--asset-id",
                                       presenter.selectedAssetId(), "--json"});
    ASSERT_EQ(info.exit_code, 0) << info.standard_error.toStdString();
    auto metadata = cli_data(info.standard_output);
    ASSERT_TRUE(metadata);
    const auto *warnings = metadata.value().find("warnings");
    ASSERT_NE(warnings, nullptr);
    ASSERT_NE(warnings->array_if(), nullptr);
    EXPECT_EQ(warnings->array_if()->size(), 3U);
    presenter.video()->setMuted(true);
    presenter.setBrowseMode(QStringLiteral("loupe"));
    ASSERT_TRUE(wait_until(
        [&]
        {
            return (presenter.video()->state() == "paused" && presenter.video()->duration() > 0) ||
                   !presenter.video()->error().isEmpty();
        },
        15000));
    ASSERT_TRUE(presenter.video()->error().isEmpty()) << presenter.video()->error().toStdString();
    EXPECT_FALSE(presenter.video()->warningsText().isEmpty());
    presenter.video()->play();
    ASSERT_TRUE(wait_until(
        [&] { return presenter.video()->position() > 0 || !presenter.video()->error().isEmpty(); },
        15000));
    EXPECT_TRUE(presenter.video()->error().isEmpty()) << presenter.video()->error().toStdString();
    EXPECT_GT(presenter.video()->position(), 0);
    presenter.video()->leaveView();
    EXPECT_TRUE(presenter.video()->warningsText().isEmpty());
}

TEST(StudioVideoTest, RealCliMetadataAndNoReplaceFrameContract)
{
    ensure_qt_core();
    QTemporaryDir directory;
    const auto catalog = directory.filePath("library.sqlite");
    auto run = [&](QStringList args)
    {
        args.append({"--catalog", catalog, "--json"});
        return run_cli_process(args);
    };
    auto created = run({"catalog", "create"});
    ASSERT_EQ(created.exit_code, 0) << created.standard_error.toStdString();
    auto imported = run({"catalog", "import", "--input",
                         QStringLiteral(RAVO_REPOSITORY_ROOT "/Ravo/tests/fixtures/video/pq.mp4")});
    ASSERT_EQ(imported.exit_code, 0) << imported.standard_error.toStdString();
    auto listed = run({"catalog", "list"});
    ASSERT_EQ(listed.exit_code, 0);
    auto data = cli_data(listed.standard_output);
    ASSERT_TRUE(data);
    const auto *assets = data.value().find("assets");
    ASSERT_NE(assets, nullptr);
    ASSERT_NE(assets->array_if(), nullptr);
    ASSERT_EQ(assets->array_if()->size(), 1U);
    const auto *id = assets->array_if()->front().find("id");
    ASSERT_NE(id, nullptr);
    ASSERT_NE(id->string_if(), nullptr);
    const auto asset_id = QString::fromStdString(*id->string_if());
    auto info = run({"catalog", "video-info", "--asset-id", asset_id});
    ASSERT_EQ(info.exit_code, 0) << info.standard_output.toStdString();
    auto parsed = cli_data(info.standard_output);
    ASSERT_TRUE(parsed);
    ASSERT_NE(parsed.value().find("transfer"), nullptr);
    EXPECT_EQ(*parsed.value().find("transfer")->string_if(), "pq");
    const auto output = directory.filePath("frame.png");
    auto frame = run({"catalog", "video-frame", "--asset-id", asset_id, "--time-us", "500000",
                      "--output", output});
    ASSERT_EQ(frame.exit_code, 0) << frame.standard_output.toStdString();
    auto artifact = cli_data(frame.standard_output);
    ASSERT_TRUE(artifact);
    ASSERT_NE(artifact.value().find("artifact"), nullptr);
    EXPECT_TRUE(QFileInfo::exists(output));
    auto conflict = run({"catalog", "video-frame", "--asset-id", asset_id, "--output", output});
    EXPECT_NE(conflict.exit_code, 0);
    auto invalid = run({"catalog", "video-frame", "--asset-id", asset_id, "--time-us", "-1",
                        "--output", directory.filePath("invalid.png")});
    EXPECT_NE(invalid.exit_code, 0);
    EXPECT_FALSE(QFileInfo::exists(directory.filePath("invalid.png")));
}
TEST(StudioVideoTest, P3Bt601PlaybackMatchesPosterForBothRanges)
{
    ensure_qt_core();
    init_logging("ravo-video-matrix-tests");
    for (const auto *name :
         {"p3_bt601_limited.mov", "p3_bt601_full.mov", "prores_p3.mov", "hlg.mp4"})
    {
        SCOPED_TRACE(name);
        const std::string path =
            std::string(RAVO_REPOSITORY_ROOT "/Ravo/tests/fixtures/video/") + name;
        FfmpegVideoDecoder decoder;
        auto info = decoder.probe(path);
        ASSERT_TRUE(info);
        auto identity = read_file_identity(path);
        ASSERT_TRUE(identity);
        AssetRecord asset;
        asset.id = "matrix-playback";
        asset.normalized_uri = normalize_local_input(path).value().uri;
        asset.size_bytes = identity.value().size_bytes;
        asset.mtime_unix_ms = identity.value().mtime_unix_ms;
        asset.video = info.value();
        asset.media_type = "video/quicktime";
        if (std::string_view(name) == "hlg.mp4")
        {
            asset.video->matrix.reset(); // Older v1 catalog metadata must be freshly probed.
            asset.video->full_range.reset();
        }
        auto decoded = decoder.frame(path, 0, 1600);
        ASSERT_TRUE(decoded);
        auto poster = render_video_frame(decoded.value());
        ASSERT_TRUE(poster);
        std::optional<PreviewResult> live;
        StudioVideoPresenter video([&](PreviewResult value) { live = std::move(value); });
        video.setMuted(true);
        video.observeAsset(asset);
        ASSERT_TRUE(wait_until(
            [&]
            {
                return (video.state() == "paused" && video.duration() > 0) ||
                       !video.error().isEmpty();
            },
            15000));
        ASSERT_TRUE(video.error().isEmpty()) << video.error().toStdString();
        video.play();
        ASSERT_TRUE(
            wait_until([&] { return live.has_value() || !video.error().isEmpty(); }, 15000));
        ASSERT_TRUE(video.error().isEmpty()) << video.error().toStdString();
        ASSERT_TRUE(live);
        EXPECT_EQ(live->width, poster.value().width);
        EXPECT_EQ(live->height, poster.value().height);
        ASSERT_EQ(live->rgb.size(), poster.value().rgb.size());
        int largest = 0;
        for (std::size_t index = 0; index < live->rgb.size(); ++index)
            largest = std::max(largest, std::abs(static_cast<int>(live->rgb[index]) -
                                                 static_cast<int>(poster.value().rgb[index])));
        EXPECT_LE(largest, 3);
        video.leaveView();
    }
}

TEST(StudioVideoTest, ReplacingSelectionAndClosingDuringPrepareRejectsLatePublication)
{
    init_logging("ravo-studio-video-tests");
    ensure_qt_core();
    FfmpegVideoDecoder decoder;
    const std::string path = RAVO_REPOSITORY_ROOT "/Ravo/tests/fixtures/video/pq.mp4";
    auto info = decoder.probe(path);
    ASSERT_TRUE(info);
    auto identity = read_file_identity(path);
    ASSERT_TRUE(identity);
    AssetRecord asset;
    asset.id = "video-test";
    asset.normalized_uri = normalize_local_input(path).value().uri;
    asset.size_bytes = identity.value().size_bytes;
    asset.mtime_unix_ms = identity.value().mtime_unix_ms;
    asset.video = info.value();
    asset.media_type = "video/mp4";
    int publications = 0;
    {
        StudioVideoPresenter video([&](PreviewResult) { ++publications; });
        video.observeAsset(asset);
        video.requestPoster();
        video.observeAsset(std::nullopt);
        EXPECT_FALSE(video.available());
        video.observeAsset(asset);
        video.requestPoster();
        video.shutdown();
    }
    QCoreApplication::processEvents();
    EXPECT_EQ(publications, 0);
}
} // namespace ravo
