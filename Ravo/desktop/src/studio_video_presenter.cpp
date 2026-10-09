#include "ravo/desktop/studio_video_presenter.h"
#include <algorithm>
#include <cmath>
#include <utility>
#include <QAudioOutput>
#include <QAudioDevice>
#include <QMediaDevices>
#include <QMediaPlayer>
#include <QMediaFormat>
#include <QMediaMetaData>
#include <QPointer>
#include <QVideoFrame>
#include <QVideoSink>
#include "ravo/adapters/ffmpeg_video_decoder.h"
#include "ravo/domain/uri.h"
#include "ravo/foundation/executor.h"
#include "ravo/services/video.h"

namespace ravo
{
struct StudioVideoPresenter::Impl
{
    StudioVideoPresenter &owner;
    PublishFrame publish;
    QMediaPlayer player;
    QAudioOutput audio;
    QVideoSink sink;
    QMediaDevices devices;
    SerialExecutor executor;
    CancellationSource operation;
    std::optional<AssetRecord> asset;
    QString state{"empty"};
    QString error;
    std::uint64_t generation = 0;
    std::uint64_t frame_revision = 0;
    bool stopped = false;
    bool audio_unavailable = false;
    bool converting = false;
    QVideoFrame pending;
    Impl(StudioVideoPresenter &parent, PublishFrame callback)
        : owner(parent)
        , publish(std::move(callback))
    {
        audio.setVolume(.5);
        player.setAudioOutput(&audio);
        player.setVideoSink(&sink);
    }
    void fail(QString message)
    {
        audio_unavailable = false;
        error = std::move(message);
        state = "error";
        player.pause();
        emit owner.changed();
    }
    void present(DecodedRaster image, const std::string &id)
    {
        PreviewResult result;
        result.asset_id = id;
        result.request_revision = ++frame_revision;
        result.width = image.width;
        result.height = image.height;
        result.rgb = std::move(image.rgb);
        result.color_profile = image.color_profile;
        result.preview_apply_mode = "video_sdr";
        result.pixel_provenance = "video_frame_sdr_srgb8";
        publish(std::move(result));
    }
    void dispatch()
    {
        if (converting || !pending.isValid() || !asset || !asset->video || stopped)
            return;
        QVideoFrame frame = std::exchange(pending, {});
        const auto info = *asset->video;
        const auto source = *asset;
        const auto id = asset->id;
        const auto expected = generation;
        const auto token = operation.token();
        converting = true;
        QPointer<StudioVideoPresenter> self(&owner);
        if (!executor.post(
                [this, self, frame = std::move(frame), info, source, id, expected, token]() mutable
                {
                    const auto decode = [&]() -> Result<DecodedRaster>
                    {
                        if (auto active = token.check(); !active)
                            return active.error();
                        auto original = video_source_path(source, token);
                        if (!original)
                            return original.error();
                        if (!frame.map(QVideoFrame::ReadOnly))
                            return make_error(ErrorCode::kUnsupported,
                                              "Video frame cannot be mapped",
                                              {{"reason", "video_frame_map_failed"}});
                        struct Unmap
                        {
                            QVideoFrame &frame;
                            ~Unmap()
                            {
                                frame.unmap();
                            }
                        } unmap{frame};
                        VideoImageView image;
                        const auto transfer = frame.surfaceFormat().colorTransfer();
                        if ((info.transfer == "hlg" &&
                             transfer != QVideoFrameFormat::ColorTransfer_Unknown &&
                             transfer != QVideoFrameFormat::ColorTransfer_STD_B67) ||
                            (info.transfer == "pq" &&
                             transfer != QVideoFrameFormat::ColorTransfer_Unknown &&
                             transfer != QVideoFrameFormat::ColorTransfer_ST2084))
                            return make_error(
                                ErrorCode::kUnsupported,
                                "Playback backend changed HDR transfer before presentation",
                                {{"reason", "video_frame_colour_mismatch"}});
                        image.width = static_cast<std::uint32_t>(frame.width());
                        image.height = static_cast<std::uint32_t>(frame.height());
                        switch (frame.pixelFormat())
                        {
                        case QVideoFrameFormat::Format_YUV420P:
                            image.format = VideoPixelFormat::kYuv420p;
                            break;
                        case QVideoFrameFormat::Format_YUV422P:
                            image.format = VideoPixelFormat::kYuv422p;
                            break;
                        case QVideoFrameFormat::Format_YUV420P10:
                            image.format = VideoPixelFormat::kYuv420p10;
                            break;
                        case QVideoFrameFormat::Format_NV12:
                            image.format = VideoPixelFormat::kNv12;
                            break;
                        case QVideoFrameFormat::Format_P010:
                            image.format = VideoPixelFormat::kP010;
                            break;
                        case QVideoFrameFormat::Format_P016:
                            image.format = VideoPixelFormat::kP016;
                            break;
                        case QVideoFrameFormat::Format_RGBA8888:
                        case QVideoFrameFormat::Format_RGBX8888:
                            image.format = VideoPixelFormat::kRgba;
                            break;
                        case QVideoFrameFormat::Format_BGRA8888:
                        case QVideoFrameFormat::Format_BGRX8888:
                            image.format = VideoPixelFormat::kBgra;
                            break;
                        default:
                            return make_error(
                                ErrorCode::kUnsupported,
                                "Playback pixel format is unsupported: " +
                                    QVideoFrameFormat::pixelFormatToString(frame.pixelFormat())
                                        .toStdString(),
                                {{"reason", "video_pixel_format_unsupported"}});
                        }
                        if (frame.planeCount() > 3)
                            return make_error(ErrorCode::kUnsupported, "Video has too many planes");
                        for (int p = 0; p < frame.planeCount(); ++p)
                        {
                            image.planes[p] = {frame.bits(p),
                                               static_cast<std::size_t>(frame.mappedBytes(p))};
                            image.strides[p] = frame.bytesPerLine(p);
                        }
                        // Qt 6.11's software buffer converts YUVJ420P into limited-range
                        // YUV420P and drops the AVFrame colour tags. The mapped bytes no
                        // longer have the source's full range. Hardware NV12 and frames
                        // carrying explicit range retain their declared interpretation.
                        const bool normalized_software_yuv =
                            frame.pixelFormat() == QVideoFrameFormat::Format_YUV420P &&
                            frame.surfaceFormat().colorRange() ==
                                QVideoFrameFormat::ColorRange_Unknown &&
                            frame.surfaceFormat().colorSpace() ==
                                QVideoFrameFormat::ColorSpace_Undefined &&
                            transfer == QVideoFrameFormat::ColorTransfer_Unknown;
                        image.full_range = normalized_software_yuv ?
                                               false :
                                           frame.surfaceFormat().colorRange() ==
                                                   QVideoFrameFormat::ColorRange_Unknown ?
                                               info.full_range.value_or(false) :
                                               frame.surfaceFormat().colorRange() ==
                                                   QVideoFrameFormat::ColorRange_Full;
                        switch (frame.surfaceFormat().colorSpace())
                        {
                        case QVideoFrameFormat::ColorSpace_BT601:
                            image.matrix = "bt601";
                            break;
                        case QVideoFrameFormat::ColorSpace_BT709:
                            image.matrix = "bt709";
                            break;
                        case QVideoFrameFormat::ColorSpace_BT2020:
                            image.matrix = "bt2020";
                            break;
                        case QVideoFrameFormat::ColorSpace_Undefined:
                            if (!info.matrix)
                                return make_error(ErrorCode::kUnsupported,
                                                  "Video matrix metadata is unavailable",
                                                  {{"reason", "video_matrix_unavailable"}});
                            image.matrix = *info.matrix;
                            break;
                        default:
                            return make_error(ErrorCode::kUnsupported,
                                              "Playback colour matrix is unsupported",
                                              {{"reason", "video_matrix_unsupported"}});
                        }
                        FfmpegVideoDecoder decoder;
                        auto converted = decoder.convert_frame(image, info, 1600, token);
                        if (!converted)
                            return converted.error();
                        auto rendered = render_video_frame(converted.value(), token);
                        if (!rendered)
                            return rendered.error();
                        auto unchanged = video_source_path(source, token);
                        if (!unchanged)
                            return unchanged.error();
                        return rendered;
                    };
                    auto rendered = decode();
                    QMetaObject::invokeMethod(
                        &owner,
                        [this, self, expected, id, token, rendered = std::move(rendered)]() mutable
                        {
                            if (!self)
                                return;
                            converting = false;
                            if (stopped || expected != generation ||
                                token.is_cancellation_requested())
                            {
                                dispatch();
                                return;
                            }
                            if (!rendered)
                            {
                                fail(QString::fromStdString(rendered.error().message));
                                return;
                            }
                            present(std::move(rendered).value(), id);
                            dispatch();
                        },
                        Qt::QueuedConnection);
                }))
        {
            converting = false;
            fail("Video worker is closed");
        }
    }
};
StudioVideoPresenter::StudioVideoPresenter(PublishFrame publish, QObject *parent)
    : QObject(parent)
    , impl_(std::make_unique<Impl>(*this, std::move(publish)))
{
    auto &p = *impl_;
    initialize_ffmpeg_diagnostics();
    connect(&p.player, &QMediaPlayer::tracksChanged, this,
            [this]
            {
                auto &p = *impl_;
                if (!p.asset || !p.asset->video || !p.asset->video->has_audio || p.stopped)
                    return;
                const auto expected = p.asset->video->audio_codec == "aac" ?
                                          QMediaFormat::AudioCodec::AAC :
                                          QMediaFormat::AudioCodec::Wave;
                const auto tracks = p.player.audioTracks();
                if (tracks.empty())
                    return; // Source replacement clears tracks before the new source loads.
                const int active = p.player.activeAudioTrack();
                if (active >= 0 && active < tracks.size() &&
                    tracks[active]
                            .value(QMediaMetaData::AudioCodec)
                            .value<QMediaFormat::AudioCodec>() == expected)
                    return; // Preserve a supported backend default (for example, language).
                for (qsizetype index = 0; index < tracks.size(); ++index)
                    if (tracks[index]
                            .value(QMediaMetaData::AudioCodec)
                            .value<QMediaFormat::AudioCodec>() == expected)
                    {
                        p.player.setActiveAudioTrack(static_cast<int>(index));
                        return;
                    }
                p.player.setActiveAudioTrack(-1);
                p.fail(tr("Video has no playable audio track matching the imported codec."));
            });
    connect(&p.player, &QMediaPlayer::positionChanged, this, &StudioVideoPresenter::changed);
    connect(&p.player, &QMediaPlayer::durationChanged, this, &StudioVideoPresenter::changed);
    connect(&p.player, &QMediaPlayer::playbackStateChanged, this,
            [this]
            {
                auto &p = *impl_;
                if (!p.asset || p.state == "error")
                    return;
                p.state = p.player.mediaStatus() == QMediaPlayer::EndOfMedia     ? "ended" :
                          p.player.playbackState() == QMediaPlayer::PlayingState ? "playing" :
                                                                                   "paused";
                emit changed();
            });
    connect(&p.player, &QMediaPlayer::mediaStatusChanged, this,
            [this](QMediaPlayer::MediaStatus status)
            {
                auto &p = *impl_;
                if (!p.asset || p.state == "error")
                    return;
                if (status == QMediaPlayer::LoadedMedia)
                    p.state = "paused";
                if (status == QMediaPlayer::EndOfMedia)
                    p.state = "ended";
                emit changed();
            });
    connect(&p.player, &QMediaPlayer::errorOccurred, this,
            [this](QMediaPlayer::Error, const QString &error)
            {
                if (impl_->asset && !impl_->stopped)
                    impl_->fail(error);
            });
    connect(&p.devices, &QMediaDevices::audioOutputsChanged, this,
            [this]
            {
                auto &p = *impl_;
                if (!p.asset || !p.asset->video->has_audio || p.stopped || p.audio.isMuted())
                    return;
                const auto device = QMediaDevices::defaultAudioOutput();
                if (device.isNull())
                {
                    p.fail(
                        tr("Audio output is unavailable. Mute the video to play without sound."));
                    p.audio_unavailable = true;
                }
                else
                {
                    p.audio.setDevice(device);
                    if (p.audio_unavailable)
                    {
                        p.error.clear();
                        p.state = "paused";
                        p.audio_unavailable = false;
                        emit changed();
                    }
                }
            });
    connect(&p.sink, &QVideoSink::videoFrameChanged, this,
            [this](const QVideoFrame &frame)
            {
                auto &p = *impl_;
                if (!p.asset || !frame.isValid() || p.stopped || p.state == "error")
                    return;
                if (frame.startTime() >= 0 &&
                    std::abs(frame.startTime() / 1000 - p.player.position()) > 500)
                    return;
                p.pending = frame;
                p.dispatch();
            });
}
StudioVideoPresenter::~StudioVideoPresenter()
{
    shutdown();
}
void StudioVideoPresenter::observeAsset(std::optional<AssetRecord> asset)
{
    auto &p = *impl_;
    if (p.stopped)
        return;
    if (asset && p.asset && asset->id == p.asset->id &&
        asset->normalized_uri == p.asset->normalized_uri)
        return;
    p.asset.reset();
    p.player.stop();
    p.player.setSource({});
    p.pending = {};
    static_cast<void>(p.operation.cancel("video_selection_replaced"));
    p.operation = CancellationSource{};
    ++p.generation;
    p.error.clear();
    p.state = "empty";
    p.audio_unavailable = false;
    if (!asset || !asset->video)
    {
        emit changed();
        return;
    }
    p.asset = std::move(asset);
    p.state = "loading";
    emit changed();
    const auto selected = *p.asset;
    const auto generation = p.generation;
    const auto token = p.operation.token();
    QPointer<StudioVideoPresenter> self(this);
    if (!p.executor.post(
            [this, self, selected, generation, token]
            {
                const auto prepare = [&]() -> Result<std::pair<std::string, VideoInfo>>
                {
                    auto path = video_source_path(selected, token);
                    if (!path)
                        return path.error();
                    FfmpegVideoDecoder decoder;
                    auto info = decoder.probe(path.value(), token);
                    if (!info)
                        return info.error();
                    auto observed = info.value();
                    auto imported = *selected.video;
                    // Diagnostics are informational, not a source identity revision.
                    observed.warnings.clear();
                    imported.warnings.clear();
                    // Earlier v1 records did not store a matrix. Resolve it from
                    // the unchanged original, never guess it from the primaries.
                    if (!imported.matrix)
                        imported.matrix = observed.matrix;
                    if (!imported.full_range)
                        imported.full_range = observed.full_range;
                    if (observed != imported)
                        return make_error(ErrorCode::kConflict,
                                          "Video metadata changed since import");
                    auto unchanged = video_source_path(selected, token);
                    if (!unchanged)
                        return unchanged.error();
                    return std::pair{path.value(), info.value()};
                };
                auto prepared = prepare();
                QMetaObject::invokeMethod(
                    this,
                    [this, self, generation, token, prepared = std::move(prepared)]
                    {
                        if (!self || impl_->stopped || generation != impl_->generation ||
                            token.is_cancellation_requested())
                            return;
                        if (!prepared)
                        {
                            impl_->fail(QString::fromStdString(prepared.error().message));
                            return;
                        }
                        impl_->asset->video = prepared.value().second;
                        impl_->player.setSource(
                            QUrl::fromLocalFile(QString::fromStdString(prepared.value().first)));
                    },
                    Qt::QueuedConnection);
            }))
        p.fail("Video worker is closed");
}
void StudioVideoPresenter::leaveView()
{
    observeAsset(std::nullopt);
}
void StudioVideoPresenter::requestPoster()
{
    auto &p = *impl_;
    if (!p.asset || p.stopped || p.state == "playing")
        return;
    const auto asset = *p.asset;
    const auto generation = p.generation;
    const auto token = p.operation.token();
    const auto time = position() * 1000;
    QPointer<StudioVideoPresenter> self(this);
    if (!p.executor.post(
            [this, self, asset, generation, token, time]
            {
                FfmpegVideoDecoder decoder;
                auto frame = decode_video_asset_preview(decoder, asset, time, 1600, token);
                QMetaObject::invokeMethod(
                    this,
                    [this, self, asset, generation, token, frame = std::move(frame)]() mutable
                    {
                        if (!self || impl_->stopped || generation != impl_->generation ||
                            token.is_cancellation_requested() || impl_->state == "playing")
                            return;
                        if (!frame)
                        {
                            impl_->fail(QString::fromStdString(frame.error().message));
                            return;
                        }
                        impl_->present(std::move(frame.value().image), asset.id);
                    },
                    Qt::QueuedConnection);
            }))
        p.fail("Video worker is closed");
}
void StudioVideoPresenter::shutdown()
{
    auto &p = *impl_;
    if (p.stopped)
        return;
    p.stopped = true;
    ++p.generation;
    static_cast<void>(p.operation.cancel("video_window_closed"));
    p.player.stop();
    p.player.setSource({});
    p.player.setVideoSink(nullptr);
    p.player.setAudioOutput(nullptr);
    p.pending = {};
    p.asset.reset();
    p.state = "empty";
    p.error.clear();
    p.executor.request_stop();
    p.executor.wait();
}
bool StudioVideoPresenter::available() const
{
    return impl_->asset.has_value();
}
QString StudioVideoPresenter::state() const
{
    return impl_->state;
}
QString StudioVideoPresenter::error() const
{
    return impl_->error;
}
QString StudioVideoPresenter::warningsText() const
{
    QStringList messages;
    if (impl_->asset && impl_->asset->video)
        for (const auto &code : impl_->asset->video->warnings)
        {
            if (code == "unsupported_auxiliary_audio")
                messages.push_back(tr(
                    "Playing the supported audio track. Additional audio tracks are unsupported; originals are preserved."));
            else if (code == "unknown_cover_ignored")
                messages.push_back(
                    tr("Unrecognized embedded cover skipped; the preview uses a video frame."));
            else if (code == "extra_channel_descriptions_capped")
                messages.push_back(tr(
                    "Extra channel descriptions ignored; audio uses the declared channel count."));
        }
    return messages.join(QLatin1Char('\n'));
}
qint64 StudioVideoPresenter::duration() const
{
    return impl_->player.duration();
}
qint64 StudioVideoPresenter::position() const
{
    return impl_->player.position();
}
double StudioVideoPresenter::volume() const
{
    return impl_->audio.volume();
}
bool StudioVideoPresenter::muted() const
{
    return impl_->audio.isMuted();
}
QVariantMap StudioVideoPresenter::snapshot() const
{
    return {{"schema", "ravo.studio.video/v1"},
            {"asset_id", impl_->asset ? QString::fromStdString(impl_->asset->id) : QString{}},
            {"state", state()},
            {"error", error()},
            {"warnings_text", warningsText()},
            {"position_ms", position()},
            {"duration_ms", duration()},
            {"volume", volume()},
            {"muted", muted()},
            {"generation", QVariant::fromValue<qulonglong>(impl_->generation)}};
}
JsonValue StudioVideoPresenter::jsonSnapshot() const
{
    JsonValue::Array warnings;
    if (impl_->asset && impl_->asset->video)
        for (const auto &code : impl_->asset->video->warnings)
            warnings.emplace_back(code);
    return JsonValue::Object{
        {"schema", "ravo.studio.video/v1"},
        {"asset_id", impl_->asset ? impl_->asset->id : std::string{}},
        {"state", state().toStdString()},
        {"error", error().toStdString()},
        {"warnings", std::move(warnings)},
        {"position_ms", JsonValue::number(std::to_string(position()))},
        {"duration_ms", JsonValue::number(std::to_string(duration()))},
        {"volume", JsonValue::number(QString::number(volume(), 'g', 17).toStdString())},
        {"muted", muted()},
        {"generation", JsonValue::number(std::to_string(impl_->generation))}};
}
void StudioVideoPresenter::play()
{
    auto &p = *impl_;
    if (!available() || p.stopped || (p.state != "paused" && p.state != "ended"))
        return;
    if (p.asset->video->has_audio && !muted() && QMediaDevices::defaultAudioOutput().isNull())
    {
        p.fail(tr("Audio output is unavailable. Mute the video to play without sound."));
        p.audio_unavailable = true;
        return;
    }
    if (p.state == "ended")
        p.player.setPosition(0);
    p.player.play();
}
void StudioVideoPresenter::pause()
{
    if (!impl_->stopped)
        impl_->player.pause();
}
void StudioVideoPresenter::seek(qint64 milliseconds)
{
    auto &p = *impl_;
    if (!available() || milliseconds < 0 || milliseconds > duration() || p.stopped)
        return;
    static_cast<void>(p.operation.cancel("video_seek_replaced"));
    p.operation = CancellationSource{};
    ++p.generation;
    p.pending = {};
    if (p.state == "ended")
        p.state = "paused";
    p.player.setPosition(milliseconds);
    emit changed();
}
void StudioVideoPresenter::setVolume(double value)
{
    if (!std::isfinite(value) || value < 0 || value > 1)
        return;
    impl_->audio.setVolume(static_cast<float>(value));
    emit changed();
}
void StudioVideoPresenter::setMuted(bool value)
{
    impl_->audio.setMuted(value);
    if (value && impl_->audio_unavailable && impl_->player.error() == QMediaPlayer::NoError)
    {
        impl_->error.clear();
        impl_->state = "paused";
        impl_->audio_unavailable = false;
    }
    emit changed();
}
} // namespace ravo
