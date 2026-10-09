#pragma once
#include <functional>
#include <memory>
#include <QObject>
#include <QString>
#include <QVariantMap>
#include "ravo/domain/types.h"
namespace ravo
{
class StudioVideoPresenter final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QString warningsText READ warningsText NOTIFY changed)
    Q_PROPERTY(qint64 duration READ duration NOTIFY changed)
    Q_PROPERTY(qint64 position READ position NOTIFY changed)
    Q_PROPERTY(double volume READ volume NOTIFY changed)
    Q_PROPERTY(bool muted READ muted NOTIFY changed)

public:
    using PublishFrame = std::function<void(PreviewResult)>;
    explicit StudioVideoPresenter(PublishFrame publish, QObject *parent = nullptr);
    ~StudioVideoPresenter() override;
    void observeAsset(std::optional<AssetRecord> asset);
    void leaveView();
    void requestPoster();
    void shutdown();
    [[nodiscard]] bool available() const;
    [[nodiscard]] QString state() const;
    [[nodiscard]] QString error() const;
    [[nodiscard]] QString warningsText() const;
    [[nodiscard]] qint64 duration() const;
    [[nodiscard]] qint64 position() const;
    [[nodiscard]] double volume() const;
    [[nodiscard]] bool muted() const;
    [[nodiscard]] QVariantMap snapshot() const;
    [[nodiscard]] JsonValue jsonSnapshot() const;
    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void seek(qint64 milliseconds);
    Q_INVOKABLE void setVolume(double volume);
    Q_INVOKABLE void setMuted(bool muted);
signals:
    void changed();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace ravo
