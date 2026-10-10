#include <gtest/gtest.h>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QCryptographicHash>
#include <QColorSpace>
#include <QColorTransform>
#include <QColor>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <limits>
#include <algorithm>
#include <cmath>
#include <future>
#include "ravo/adapters/ffmpeg_video_decoder.h"
#include "ravo/adapters/qt_raster_decoder.h"
#include "ravo/adapters/sqlite_catalog.h"
#include "ravo/adapters/filesystem_preview_cache.h"
#include "ravo/adapters/filesystem_recovery_store.h"
#include "ravo/services/catalog_service.h"
#include "ravo/services/video.h"
#include "ravo/services/artifact_publication.h"
#include "ravo/engine/video_color.h"
#include "catalog_service_test_support.h"
#include "catalog_repository_test_control.h"
namespace ravo
{
namespace
{
std::string clip(const char *name)
{
    return std::string(RAVO_REPOSITORY_ROOT "/Ravo/tests/fixtures/video/") + name;
}
void qt()
{
    if (!QCoreApplication::instance())
    {
        static int argc = 1;
        static char name[] = "ravo-video-tests";
        static char *argv[]{name, nullptr};
        static QCoreApplication application(argc, argv);
    }
}
QByteArray hash(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
}
std::unique_ptr<CatalogService> service(const EngineFacade &engine, const QString &path,
                                        bool create)
{
    auto repository = create ? SqliteCatalogRepository::create(path.toStdString()) :
                               SqliteCatalogRepository::open(path.toStdString());
    auto cache = FilesystemPreviewCache::create(path.toStdString() + ".preview");
    auto recovery = FilesystemRecoveryStore::create_for_catalog(path.toStdString());
    if (!repository || !cache || !recovery)
        return {};
    return std::make_unique<CatalogService>(
        engine, std::move(repository).value(), std::make_unique<QtRasterDecoder>(),
        std::move(cache).value(), std::move(recovery).value(), std::shared_ptr<std::mutex>{},
        std::make_unique<FfmpegVideoDecoder>());
}
} // namespace
TEST(VideoDecoderTest, ReadsSdrAudioRotatedAndHdrWithoutChangingSources)
{
    qt();
    FfmpegVideoDecoder decoder;
    for (const auto *name : {"sdr_audio.mp4", "rotated.mov", "hlg.mp4", "pq.mp4"})
    {
        SCOPED_TRACE(name);
        const auto before = hash(QString::fromStdString(clip(name)));
        ASSERT_FALSE(before.isEmpty());
        auto info = decoder.probe(clip(name));
        ASSERT_TRUE(info) << info.error().message;
        EXPECT_EQ(info.value().width, 96U);
        EXPECT_EQ(info.value().height, 64U);
        ASSERT_TRUE(info.value().duration_us);
        EXPECT_GT(*info.value().duration_us, 0);
        auto value = decoder.frame(clip(name), 500000, 48);
        ASSERT_TRUE(value) << value.error().message;
        EXPECT_GE(value.value().timestamp_us, 500000);
        EXPECT_LE(std::max(value.value().width, value.value().height), 48U);
        auto rendered = render_video_frame(value.value());
        ASSERT_TRUE(rendered) << rendered.error().message;
        EXPECT_EQ(rendered.value().color_profile.identifier, "srgb");
        EXPECT_EQ(rendered.value().rgb.size(), value.value().width * value.value().height * 3U);
        if (std::string_view(name) == "rotated.mov")
        {
            EXPECT_EQ(info.value().rotation, 270);
            EXPECT_LT(value.value().width, value.value().height);
        }
        EXPECT_EQ(hash(QString::fromStdString(clip(name))), before);
        auto roundtrip = parse_video_info(video_info_json(info.value()));
        ASSERT_TRUE(roundtrip);
        EXPECT_EQ(roundtrip.value(), info.value());
    }
}
TEST(VideoDecoderTest, AuxiliaryMetadataKeepsPlayableAudioAndStructuredDiagnostics)
{
    qt();
    FfmpegVideoDecoder decoder;
    const auto source = clip("auxiliary_audio.mov");
    const auto before = hash(QString::fromStdString(source));
    ::testing::internal::CaptureStderr();
    auto info = decoder.probe(source);
    auto frame = decoder.frame(source, 500000, 48);
    const auto diagnostics = ::testing::internal::GetCapturedStderr();
    ASSERT_TRUE(info) << info.error().message;
    ASSERT_TRUE(frame) << frame.error().message;
    EXPECT_TRUE(info.value().has_audio);
    EXPECT_EQ(info.value().audio_codec, "aac");
    EXPECT_EQ(info.value().warnings, (std::vector<std::string>{"unknown_cover_ignored",
                                                               "extra_channel_descriptions_capped",
                                                               "unsupported_auxiliary_audio"}));
    EXPECT_EQ(frame.value().info.warnings, info.value().warnings);
    EXPECT_EQ(diagnostics.find("Unknown cover type"), std::string::npos) << diagnostics;
    EXPECT_EQ(diagnostics.find("channel descriptions"), std::string::npos) << diagnostics;
    EXPECT_EQ(diagnostics.find("Could not find codec parameters"), std::string::npos)
        << diagnostics;
    auto roundtrip = parse_video_info(video_info_json(info.value()));
    ASSERT_TRUE(roundtrip);
    EXPECT_EQ(roundtrip.value(), info.value());
    EXPECT_EQ(hash(QString::fromStdString(source)), before);
    auto metadata = *video_info_json(info.value()).object_if();
    metadata.erase("warnings");
    ASSERT_TRUE(parse_video_info(JsonValue{metadata})); // Optional additive v1 field.
    metadata["warnings"] = JsonValue::Array{"unrecognized_diagnostic"};
    EXPECT_FALSE(parse_video_info(JsonValue{metadata}));
    auto unsupported = decoder.probe(clip("unsupported_audio.mov"));
    ASSERT_FALSE(unsupported);
    EXPECT_EQ(unsupported.error().context.at("reason"), "video_audio_codec_unsupported");
    // Genuine demux errors still reach stderr as well as the structured result.
    QTemporaryDir directory;
    QFile broken(directory.filePath("broken.mov"));
    ASSERT_TRUE(broken.open(QIODevice::WriteOnly));
    broken.write("not a movie");
    broken.close();
    ::testing::internal::CaptureStderr();
    auto corrupt = decoder.probe(broken.fileName().toStdString());
    const auto errors = ::testing::internal::GetCapturedStderr();
    ASSERT_FALSE(corrupt);
    EXPECT_EQ(corrupt.error().context.at("reason"), "video_open_failed");
    EXPECT_NE(errors.find("moov atom not found"), std::string::npos) << errors;
}

TEST(VideoDecoderTest, P3Bt601MatricesAndBothRangesPreserveCodedRgb)
{
    qt();
    FfmpegVideoDecoder decoder;
    for (const auto *name :
         {"p3_bt601_limited.mov", "p3_bt601_full.mov", "p3_bt470bg.mov", "prores_p3.mov"})
    {
        SCOPED_TRACE(name);
        auto info = decoder.probe(clip(name));
        ASSERT_TRUE(info) << info.error().message;
        ASSERT_TRUE(info.value().matrix);
        EXPECT_EQ(*info.value().matrix, "bt601");
        EXPECT_EQ(info.value().primaries, "p3_d65");
        EXPECT_EQ(info.value().transfer, "bt709");
        ASSERT_TRUE(info.value().full_range.has_value());
        EXPECT_EQ(*info.value().full_range, std::string_view(name) == "p3_bt601_full.mov");
        if (std::string_view(name) == "prores_p3.mov")
            EXPECT_EQ(info.value().codec, "prores");
        auto frame = decoder.frame(clip(name), 500000, 48);
        ASSERT_TRUE(frame) << frame.error().message;
        const float expected[]{144.F / 255.F, 64.F / 255.F, 128.F / 255.F};
        for (std::size_t index = 0; index < frame.value().rgb.size(); ++index)
            EXPECT_NEAR(frame.value().rgb[index], expected[index % 3], .015F);
        auto parsed = parse_video_info(video_info_json(info.value()));
        ASSERT_TRUE(parsed);
        EXPECT_EQ(parsed.value(), info.value());
        auto old = *video_info_json(info.value()).object_if();
        old.erase("matrix");
        old.erase("full_range");
        auto legacy = parse_video_info(JsonValue{old});
        ASSERT_TRUE(legacy);
        EXPECT_FALSE(legacy.value().matrix);
        EXPECT_FALSE(legacy.value().full_range.has_value());
        old["matrix"] = "guess";
        EXPECT_FALSE(parse_video_info(JsonValue{old}));
        old["matrix"] = "bt601";
        old["full_range"] = "full";
        EXPECT_FALSE(parse_video_info(JsonValue{old}));
    }
    auto unknown = decoder.probe(clip("unsupported_matrix.mov"));
    ASSERT_FALSE(unknown);
    EXPECT_EQ(unknown.error().context.at("reason"), "video_matrix_unsupported");
    EXPECT_EQ(unknown.error().context.at("matrix"), "ycgco");
    EXPECT_EQ(unknown.error().context.at("primaries"), "smpte432");
}

TEST(VideoDecoderTest, NativeBt601PlanesUseTheSameMatrixAndRejectUnknownTags)
{
    FfmpegVideoDecoder decoder;
    VideoInfo info;
    info.matrix = "bt601";
    for (const bool full : {false, true})
    {
        // ITU BT.601 code values for nonlinear RGB (144, 64, 128).
        std::vector<std::uint8_t> y(4, full ? 95 : 98);
        const std::vector<std::uint8_t> u{static_cast<std::uint8_t>(full ? 147 : 144)};
        const std::vector<std::uint8_t> v{static_cast<std::uint8_t>(full ? 163 : 159)};
        VideoImageView image;
        image.width = image.height = 2;
        image.planes[0] = y;
        image.planes[1] = u;
        image.planes[2] = v;
        image.strides[0] = 2;
        image.strides[1] = image.strides[2] = 1;
        image.matrix = "bt601";
        image.full_range = full;
        auto frame = decoder.convert_frame(image, info, 2);
        ASSERT_TRUE(frame) << frame.error().message;
        const float expected[]{144.F / 255.F, 64.F / 255.F, 128.F / 255.F};
        for (std::size_t index = 0; index < frame.value().rgb.size(); ++index)
            EXPECT_NEAR(frame.value().rgb[index], expected[index % 3], .015F);
        image.matrix = "unknown";
        auto unknown = decoder.convert_frame(image, info, 2);
        ASSERT_FALSE(unknown);
        EXPECT_EQ(unknown.error().context.at("reason"), "video_matrix_unsupported");
        image.matrix = "bt709";
        auto mismatch = decoder.convert_frame(image, info, 2);
        ASSERT_FALSE(mismatch);
        EXPECT_EQ(mismatch.error().context.at("reason"), "video_frame_colour_mismatch");
    }
}

TEST(VideoDecoderTest, Native422PlanesRetainFullChromaHeightAndRejectShortStorage)
{
    FfmpegVideoDecoder decoder;
    std::vector<std::uint8_t> y(8, 128), u{16, 16, 240, 240}, v(4, 128);
    VideoImageView image;
    image.format = VideoPixelFormat::kYuv422p;
    image.width = 2;
    image.height = 4;
    image.planes[0] = y;
    image.planes[1] = u;
    image.planes[2] = v;
    image.strides[0] = 2;
    image.strides[1] = image.strides[2] = 1;
    image.matrix = "bt601";
    auto converted = decoder.convert_frame(image, {}, 4);
    ASSERT_TRUE(converted) << converted.error().message;
    ASSERT_EQ(converted.value().rgb.size(), 24U);
    EXPECT_GT(converted.value().rgb[23] - converted.value().rgb[2], .5F);
    image.planes[1] = std::span(u).first(2);
    auto short_plane = decoder.convert_frame(image, {}, 4);
    ASSERT_FALSE(short_plane);
    EXPECT_EQ(short_plane.error().context.at("reason"), "video_plane_invalid");
}

TEST(VideoDecoderTest, NativeP016PlanesRetainPrecisionAndRequireCompleteUvStorage)
{
    FfmpegVideoDecoder decoder;
    std::vector<std::uint8_t> y{0, 98, 0, 98, 0, 98, 0, 98}, uv{0, 144, 0, 159};
    VideoImageView image;
    image.format = VideoPixelFormat::kP016;
    image.width = image.height = 2;
    image.planes[0] = y;
    image.planes[1] = uv;
    image.strides[0] = image.strides[1] = 4;
    image.matrix = "bt601";
    auto converted = decoder.convert_frame(image, {}, 2);
    ASSERT_TRUE(converted) << converted.error().message;
    const float expected[]{144.F / 255.F, 64.F / 255.F, 128.F / 255.F};
    for (std::size_t index = 0; index < converted.value().rgb.size(); ++index)
        EXPECT_NEAR(converted.value().rgb[index], expected[index % 3], .015F);
    image.planes[1] = std::span(uv).first(3);
    auto incomplete = decoder.convert_frame(image, {}, 2);
    ASSERT_FALSE(incomplete);
    EXPECT_EQ(incomplete.error().context.at("reason"), "video_plane_invalid");
}

TEST(VideoColorTest, P3D65ConversionMatchesIndependentQtTransform)
{
    const QColorSpace p3(QColorSpace::DisplayP3), srgb(QColorSpace::SRgb);
    const auto transform = p3.transformationToColorSpace(srgb);
    const std::vector<float> input{.3F, .4F, .5F, .6F, .3F, .4F, .5F, .5F, .5F, 1.F, 1.F, 1.F};
    auto converted = video_rgb_to_sdr(input, "srgb", "p3_d65", 1000);
    ASSERT_TRUE(converted);
    auto unchanged = video_rgb_to_sdr(input, "srgb", "bt709", 1000);
    ASSERT_TRUE(unchanged);
    EXPECT_NE(converted.value(), unchanged.value());
    for (std::size_t index = 0; index < input.size(); index += 3)
    {
        const auto reference =
            transform.map(QColor::fromRgbF(input[index], input[index + 1], input[index + 2]));
        const double channels[]{reference.redF(), reference.greenF(), reference.blueF()};
        for (std::size_t channel = 0; channel < 3; ++channel)
            EXPECT_NEAR(converted.value()[index + channel],
                        std::round(std::clamp(channels[channel], 0., 1.) * 255.), 1);
    }
}

TEST(VideoDecoderTest, ConcurrentDiagnosticsDoNotHideOtherThreadsErrors)
{
    qt();
    FfmpegVideoDecoder decoder;
    QTemporaryDir directory;
    QFile file(directory.filePath("corrupt.mov"));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("not a movie");
    file.close();
    const auto path = file.fileName().toStdString();
    ::testing::internal::CaptureStderr();
    auto good = std::async(std::launch::async,
                           [&]
                           {
                               for (int index = 0; index < 12; ++index)
                               {
                                   auto info = decoder.probe(clip("auxiliary_audio.mov"));
                                   if (!info || info.value().warnings.size() != 3)
                                       return false;
                               }
                               return true;
                           });
    auto bad =
        std::async(std::launch::async,
                   [&]
                   {
                       for (int index = 0; index < 12; ++index)
                       {
                           auto info = decoder.probe(path);
                           if (info || info.error().context.at("reason") != "video_open_failed")
                               return false;
                       }
                       return true;
                   });
    const bool good_result = good.get();
    const bool bad_result = bad.get();
    const auto diagnostics = ::testing::internal::GetCapturedStderr();
    EXPECT_TRUE(good_result);
    EXPECT_TRUE(bad_result);
    EXPECT_NE(diagnostics.find("moov atom not found"), std::string::npos) << diagnostics;
    EXPECT_EQ(diagnostics.find("Unknown cover type"), std::string::npos) << diagnostics;
    EXPECT_EQ(diagnostics.find("Could not find codec parameters"), std::string::npos)
        << diagnostics;
}

TEST(VideoDecoderTest, FullRangeFramesUseExplicitRangeAndKeepMidtones)
{
    qt();
    FfmpegVideoDecoder decoder;
    ::testing::internal::CaptureStderr();
    auto frame = decoder.frame(clip("full_range.mov"), 500000, 48);
    const auto diagnostics = ::testing::internal::GetCapturedStderr();
    ASSERT_TRUE(frame) << frame.error().message;
    EXPECT_EQ(diagnostics.find("deprecated pixel format"), std::string::npos) << diagnostics;
    ASSERT_FALSE(frame.value().rgb.empty());
    for (const auto value : frame.value().rgb)
        EXPECT_NEAR(value, 64.F / 255.F, 0.012F);
}

TEST(VideoDecoderTest, CancellationCorruptionMissingBoundsAndInvalidPlanesAreExplicit)
{
    qt();
    FfmpegVideoDecoder decoder;
    CancellationSource stop;
    ASSERT_TRUE(stop.cancel("test"));
    auto cancelled = decoder.probe(clip("hlg.mp4"), stop.token());
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, ErrorCode::kCancelled);
    EXPECT_FALSE(decoder.probe(clip("missing.mp4")));
    EXPECT_FALSE(decoder.frame(clip("hlg.mp4"), -1, 320));
    EXPECT_FALSE(decoder.frame(clip("hlg.mp4"), 5000000, 320));
    EXPECT_FALSE(decoder.frame(clip("hlg.mp4"), 0, 10000));
    VideoImageView invalid;
    invalid.width = 96;
    invalid.height = 64;
    auto planes = decoder.convert_frame(invalid, {}, 320);
    ASSERT_FALSE(planes);
    EXPECT_EQ(planes.error().context.at("reason"), "video_plane_invalid");
    QTemporaryDir dir;
    QFile corrupt(dir.filePath("broken.mov"));
    ASSERT_TRUE(corrupt.open(QIODevice::WriteOnly));
    corrupt.write("not a movie");
    corrupt.close();
    EXPECT_FALSE(decoder.probe(corrupt.fileName().toStdString()));
}
TEST(VideoColorTest, HdrMappingIsBoundedMonotonicAndRejectsNonfinitePixels)
{
    for (const auto *transfer : {"pq", "hlg"})
    {
        std::vector<float> ramp;
        for (int i = 0; i <= 100; ++i)
            for (int c = 0; c < 3; ++c)
                ramp.push_back(static_cast<float>(i) / 100.F);
        auto result = video_rgb_to_sdr(ramp, transfer, "bt2020", 1000);
        ASSERT_TRUE(result);
        for (std::size_t i = 3; i < result.value().size(); i += 3)
            EXPECT_GE(result.value()[i], result.value()[i - 3]);
        EXPECT_EQ(result.value().front(), 0);
        EXPECT_GE(result.value().back(), 254);
    }
    std::vector<float> invalid{0, std::numeric_limits<float>::quiet_NaN(), 0};
    EXPECT_FALSE(video_rgb_to_sdr(invalid, "hlg", "bt2020", 1000));
    EXPECT_FALSE(video_rgb_to_sdr(std::span<const float>{}, "srgb", "bt709", 1000));
}
TEST(VideoCatalogTest, MoveCopyReopenRecoveryAndOriginalHashes)
{
    qt();
    QTemporaryDir dir;
    auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine);
    const auto database = dir.filePath("library.sqlite");
    auto catalog = service(engine.value(), database, true);
    ASSERT_TRUE(catalog);
    const auto source = dir.filePath("input.mp4");
    ASSERT_TRUE(QFile::copy(QString::fromStdString(clip("hlg.mp4")), source));
    const auto before = hash(source);
    ASSERT_FALSE(before.isEmpty());
    const auto destination = dir.filePath("destination");
    ASSERT_TRUE(QDir().mkpath(destination));
    ImportRequest request;
    request.inputs = {source.toStdString()};
    request.mode = ImportTransferMode::kMove;
    request.destination_directory = destination.toStdString();
    request.preview = ImportPreviewPolicy::kMinimal;
    auto imported = catalog->import().execute_import(request);
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_EQ(imported.value().imported, 1U);
    const auto &item = imported.value().items.front();
    ASSERT_TRUE(item.asset);
    ASSERT_TRUE(item.asset->video);
    EXPECT_FALSE(item.error);
    EXPECT_FALSE(item.source_cleanup_error);
    EXPECT_FALSE(QFile::exists(source));
    EXPECT_EQ(hash(destination + "/input.mp4"), before);
    const auto id = item.asset->id;
    auto frame = catalog->video_frame(id, 500000, 64);
    ASSERT_TRUE(frame) << frame.error().message;
    EXPECT_FALSE(catalog->develop().load_recipe(id));
    auto recovery = catalog->recovery().sync_recovery(std::nullopt);
    ASSERT_TRUE(recovery) << recovery.error().message;
    const auto backup = dir.filePath("backup");
    auto saved = catalog->recovery().create_backup(backup.toStdString(), {});
    ASSERT_TRUE(saved) << saved.error().message;
    const SqliteCatalogBackupVerifier verifier;
    auto sidecars = FilesystemRecoveryStore::open_existing((backup + "/sidecars").toStdString());
    ASSERT_TRUE(sidecars);
    CatalogRestoreRequest restore;
    restore.backup_directory = backup.toStdString();
    restore.destination_catalog = dir.filePath("restored.sqlite").toStdString();
    auto restored = restore_catalog_backup(verifier, verifier, *sidecars.value(), restore);
    ASSERT_TRUE(restored) << restored.error().message;
    auto restored_catalog =
        service(engine.value(), QString::fromStdString(restore.destination_catalog), false);
    ASSERT_TRUE(restored_catalog);
    EXPECT_TRUE(restored_catalog->video_info(id));
    ExportRequest original_copy;
    original_copy.asset_id = id;
    original_copy.output_path = dir.filePath("export.mp4").toStdString();
    original_copy.format = ExportFormat::kOriginalCopy;
    ASSERT_TRUE(catalog->exports().export_asset(original_copy));
    EXPECT_EQ(hash(QString::fromStdString(original_copy.output_path)), before);
    catalog.reset();
    catalog = service(engine.value(), database, false);
    ASSERT_TRUE(catalog);
    auto assets = catalog->library().list_assets();
    ASSERT_TRUE(assets);
    ASSERT_EQ(assets.value().size(), 1U);
    ASSERT_TRUE(assets.value().front().video);
    EXPECT_EQ(assets.value().front().video->transfer, "hlg");
    ImportRequest repeated;
    repeated.inputs = {(destination + "/input.mp4").toStdString()};
    auto duplicated = catalog->import().execute_import(repeated);
    ASSERT_TRUE(duplicated);
    EXPECT_EQ(duplicated.value().duplicates, 1U);
    EXPECT_EQ(hash(destination + "/input.mp4"), before);
}
TEST(VideoCatalogTest, P3MatrixMetadataAndPreviewsSurviveReopen)
{
    qt();
    QTemporaryDir directory;
    auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine);
    const auto database = directory.filePath("library.sqlite");
    auto catalog = service(engine.value(), database, true);
    ASSERT_TRUE(catalog);
    const auto path = clip("p3_bt601_full.mov");
    const auto before = hash(QString::fromStdString(path));
    auto imported = catalog->import().import_one(path, {});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto id = imported.value().asset->id;
    auto preview = catalog->video_frame(id, 500000, 48);
    ASSERT_TRUE(preview) << preview.error().message;
    catalog.reset();
    catalog = service(engine.value(), database, false);
    ASSERT_TRUE(catalog);
    auto info = catalog->video_info(id);
    ASSERT_TRUE(info);
    EXPECT_EQ(info.value().matrix, std::optional<std::string>{"bt601"});
    EXPECT_EQ(info.value().primaries, "p3_d65");
    auto reopened = catalog->video_frame(id, 500000, 48);
    ASSERT_TRUE(reopened) << reopened.error().message;
    EXPECT_EQ(preview.value().image.rgb, reopened.value().image.rgb);
    EXPECT_EQ(hash(QString::fromStdString(path)), before);
}

TEST(VideoCatalogTest, ConflictAndCancelledImportPublishNoVideo)
{
    qt();
    QTemporaryDir dir;
    auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine);
    auto catalog = service(engine.value(), dir.filePath("library.sqlite"), true);
    ASSERT_TRUE(catalog);
    const auto source = dir.filePath("input.mp4"), destination = dir.filePath("dest");
    ASSERT_TRUE(QFile::copy(QString::fromStdString(clip("pq.mp4")), source));
    ASSERT_TRUE(QDir().mkpath(destination));
    ASSERT_TRUE(QFile::copy(source, destination + "/input.mp4"));
    const auto before = hash(source);
    ImportRequest request;
    request.inputs = {source.toStdString()};
    request.mode = ImportTransferMode::kMove;
    request.destination_directory = destination.toStdString();
    EXPECT_FALSE(catalog->import().execute_import(request));
    CancellationSource stop;
    ASSERT_TRUE(stop.cancel("test"));
    request.cancellation = stop.token();
    EXPECT_FALSE(catalog->import().execute_import(request));
    EXPECT_TRUE(catalog->library().list_assets().value().empty());
    EXPECT_EQ(hash(source), before);
}
TEST(VideoCatalogTest, MigrationTransactionFailureAndSourceChangeAreExplicit)
{
    qt();
    QTemporaryDir dir;
    auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine);
    const auto database = dir.filePath("library.sqlite");
    auto initial = service(engine.value(), database, true);
    ASSERT_TRUE(initial);
    initial.reset();
    {
        auto connection = QSqlDatabase::addDatabase("QSQLITE", "video-migration-fixture");
        connection.setDatabaseName(database);
        ASSERT_TRUE(connection.open());
        QSqlQuery query(connection);
        ASSERT_TRUE(query.exec("DROP TABLE asset_video"));
        ASSERT_TRUE(query.exec("DROP TABLE foreign_conversion_record"));
        ASSERT_TRUE(query.exec("DROP TABLE foreign_conversion"));
        ASSERT_TRUE(query.exec("DROP TABLE foreign_catalog_chunk"));
        ASSERT_TRUE(query.exec("DROP TABLE foreign_catalog_source"));
        ASSERT_TRUE(query.exec("UPDATE schema_info SET schema_version = 17"));
        connection.close();
    }
    QSqlDatabase::removeDatabase("video-migration-fixture");
    auto repository = SqliteCatalogRepository::open(database.toStdString());
    ASSERT_TRUE(repository);
    EXPECT_EQ(repository.value()->snapshot().value().schema_version, kCatalogSchemaVersion);
    auto *faults = repository.value().get();
    auto cache = FilesystemPreviewCache::create(database.toStdString() + ".preview");
    ASSERT_TRUE(cache);
    auto recovery = FilesystemRecoveryStore::create_for_catalog(database.toStdString());
    ASSERT_TRUE(recovery);
    CatalogService catalog(engine.value(), std::move(repository).value(),
                           std::make_unique<QtRasterDecoder>(), std::move(cache).value(),
                           std::move(recovery).value(), {}, std::make_unique<FfmpegVideoDecoder>());
    const auto source = dir.filePath("input.mp4");
    ASSERT_TRUE(QFile::copy(QString::fromStdString(clip("pq.mp4")), source));
    const auto before = hash(source);
    testing::SqliteCatalogTestControl::inject(*faults, testing::SqliteImportFailure::kCommit);
    ImportRequest request;
    request.inputs = {source.toStdString()};
    auto failed = catalog.import().execute_import(request);
    ASSERT_TRUE(failed);
    EXPECT_EQ(failed.value().imported, 0U);
    EXPECT_EQ(failed.value().failed, 1U);
    EXPECT_TRUE(catalog.library().list_assets().value().empty());
    EXPECT_EQ(hash(source), before);
    auto imported = catalog.import().execute_import(request);
    ASSERT_TRUE(imported);
    ASSERT_EQ(imported.value().imported, 1U);
    const auto id = imported.value().items.front().asset->id;
    QFile changed(source);
    ASSERT_TRUE(changed.open(QIODevice::Append));
    ASSERT_GT(changed.write("changed"), 0);
    changed.close();
    auto stale = catalog.video_frame(id, 0, 64);
    ASSERT_FALSE(stale);
    EXPECT_EQ(stale.error().context.at("reason"), "video_source_changed");
}
TEST(VideoCatalogTest, SourceChangeDuringDecodeCannotPublishPreview)
{
    qt();
    QTemporaryDir directory;
    auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine);
    const auto database = directory.filePath("library.sqlite");
    class ChangingDecoder final : public VideoDecoder
    {
    public:
        FfmpegVideoDecoder decoder;
        mutable std::function<void()> changed;
        Result<VideoInfo> probe(std::string_view path,
                                const CancellationToken &token) const override
        {
            return decoder.probe(path, token);
        }
        Result<VideoFrame> frame(std::string_view path, std::int64_t time, std::uint32_t edge,
                                 const CancellationToken &token) const override
        {
            auto result = decoder.frame(path, time, edge, token);
            if (result && changed)
            {
                auto action = std::exchange(changed, {});
                action();
            }
            return result;
        }
        Result<VideoFrame> convert_frame(const VideoImageView &image, const VideoInfo &info,
                                         std::uint32_t edge,
                                         const CancellationToken &token) const override
        {
            return decoder.convert_frame(image, info, edge, token);
        }
    };
    auto decoder = std::make_unique<ChangingDecoder>();
    auto *change = decoder.get();
    auto repository = SqliteCatalogRepository::create(database.toStdString());
    ASSERT_TRUE(repository);
    auto cache = FilesystemPreviewCache::create(database.toStdString() + ".preview");
    ASSERT_TRUE(cache);
    auto recovery = FilesystemRecoveryStore::create_for_catalog(database.toStdString());
    ASSERT_TRUE(recovery);
    CatalogService catalog(engine.value(), std::move(repository).value(),
                           std::make_unique<QtRasterDecoder>(), std::move(cache).value(),
                           std::move(recovery).value(), {}, std::move(decoder));
    const auto source = directory.filePath("source.mp4");
    ASSERT_TRUE(QFile::copy(QString::fromStdString(clip("hlg.mp4")), source));
    auto imported = catalog.import().import_one(source.toStdString(), {});
    ASSERT_TRUE(imported);
    ASSERT_TRUE(imported.value().asset);
    const auto records = catalog.library().list_previews();
    ASSERT_TRUE(records);
    change->changed = [&]
    {
        QFile altered(source);
        ASSERT_TRUE(altered.open(QIODevice::Append));
        ASSERT_GT(altered.write("changed-during-decode"), 0);
    };
    PreviewRequest request;
    request.asset_id = imported.value().asset->id;
    request.max_edge = 64;
    auto rejected = catalog.preview().request_preview(request);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "video_source_changed");
    auto after = catalog.library().list_previews();
    ASSERT_TRUE(after);
    EXPECT_EQ(after.value(), records.value());
    EXPECT_EQ(QDir(database + ".preview").entryList({"*64x*"}, QDir::Files).size(), 0);
}
} // namespace ravo
