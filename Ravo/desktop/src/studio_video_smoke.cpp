#include "ravo/desktop/studio_video_presenter.h"
#include "ravo/adapters/ffmpeg_video_decoder.h"
#include "ravo/domain/uri.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QCryptographicHash>
#include <cstdio>
namespace ravo
{
bool smoke_video_playback(const QString &input)
{
    const auto path = input.toStdString();
    FfmpegVideoDecoder decoder;
    auto info = decoder.probe(path);
    auto location = normalize_local_input(path);
    auto identity = read_file_identity(path);
    if (!info || !location || !identity)
    {
        const auto message = !info     ? info.error().message :
                             !location ? location.error().message :
                                         identity.error().message;
        fprintf(stderr, "Video smoke preparation failed: %s\n", message.c_str());
        return false;
    }
    AssetRecord asset;
    asset.id = "video-smoke";
    asset.normalized_uri = location.value().uri;
    asset.media_type = info.value().container == "mov" ? "video/quicktime" : "video/mp4";
    asset.video = info.value();
    asset.size_bytes = identity.value().size_bytes;
    asset.mtime_unix_ms = identity.value().mtime_unix_ms;
    std::size_t frames = 0;
    std::string hash;
    bool valid_pixels = true;
    StudioVideoPresenter video(
        [&](PreviewResult result)
        {
            valid_pixels =
                valid_pixels && result.width > 0 && result.height > 0 &&
                result.rgb.size() == static_cast<std::size_t>(result.width) * result.height * 3U &&
                result.color_profile.identifier == "srgb";
            hash = QCryptographicHash::hash(
                       QByteArrayView(reinterpret_cast<const char *>(result.rgb.data()),
                                      static_cast<qsizetype>(result.rgb.size())),
                       QCryptographicHash::Sha256)
                       .toHex()
                       .toStdString();
            ++frames;
        });
    video.observeAsset(asset);
    QElapsedTimer timer;
    timer.start();
    bool started = false;
    while (timer.elapsed() < 15000 && video.error().isEmpty())
    {
        QCoreApplication::processEvents();
        if (!started && video.state() == QLatin1String("paused"))
        {
            video.setMuted(true);
            video.play();
            started = true;
        }
        if (frames >= 2 || (frames > 0 && video.state() == QLatin1String("ended")))
            break;
        QThread::msleep(10);
    }
    const auto error = video.error();
    video.shutdown();
    if (!started || frames == 0 || !valid_pixels || !error.isEmpty())
    {
        fprintf(stderr, "Video playback smoke failed: %s; frames=%zu\n", error.toUtf8().constData(),
                frames);
        return false;
    }
    const auto report =
        serialize_json(JsonValue::Object{{"schema", "ravo.video_playback_smoke/v1"},
                                         {"frames", JsonValue::number(std::to_string(frames))},
                                         {"pixel_sha256", hash},
                                         {"color_profile", "srgb"},
                                         {"audio_output_tested", false}});
    printf("%s\n", report.c_str());
    return true;
}
} // namespace ravo
