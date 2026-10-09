#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>
#include <optional>
#include <cstdint>
#include "ravo/foundation/json.h"

namespace ravo
{
class StudioPresenter;
class StudioCommandController;

// GUI-thread form state, owned by the command controller. Catalog policy and
// backup work remain in RecoveryService; neither reference outlives its owner.
class StudioBackupSettings final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool loaded READ loaded NOTIFY changed)
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY changed)
    Q_PROPERTY(QString directory READ directory WRITE setDirectory NOTIFY changed)
    Q_PROPERTY(int intervalMinutes READ intervalMinutes WRITE setIntervalMinutes NOTIFY changed)
    Q_PROPERTY(int retentionCount READ retentionCount WRITE setRetentionCount NOTIFY changed)
    Q_PROPERTY(bool dirty READ dirty NOTIFY changed)
    Q_PROPERTY(bool saving READ saving NOTIFY changed)
    Q_PROPERTY(bool canApply READ canApply NOTIFY changed)
    Q_PROPERTY(bool canEdit READ canEdit NOTIFY changed)
    Q_PROPERTY(bool canRun READ canRun NOTIFY changed)
    Q_PROPERTY(QString disabledReason READ disabledReason NOTIFY changed)
    Q_PROPERTY(QString lastError READ lastError NOTIFY changed)
    Q_PROPERTY(QVariantMap limits READ limits CONSTANT)

public:
    StudioBackupSettings(StudioPresenter &presenter, StudioCommandController &commands,
                         QObject *parent);
    bool loaded() const
    {
        return loaded_;
    }
    bool enabled() const;
    QString directory() const;
    int intervalMinutes() const;
    int retentionCount() const;
    bool dirty() const
    {
        return values_ != saved_;
    }
    bool saving() const
    {
        return saving_;
    }
    bool canApply() const;
    bool canEdit() const;
    bool canRun() const;
    QString disabledReason() const;
    QString lastError() const
    {
        return error_;
    }
    QVariantMap limits() const;
    void setEnabled(bool value);
    void setDirectory(const QString &value);
    void setIntervalMinutes(int value);
    void setRetentionCount(int value);
    Q_INVOKABLE bool apply();
    Q_INVOKABLE void reload();
    Q_INVOKABLE bool beginDirectorySelection();
    Q_INVOKABLE bool acceptDirectory(const QString &directory);
    Q_INVOKABLE bool runNow();
    [[nodiscard]] JsonValue jsonSnapshot() const;
signals:
    void changed();

private:
    void observe();
    void update(const QString &key, const QVariant &value);
    StudioPresenter &presenter_;
    StudioCommandController &commands_;
    QString catalog_;
    QVariantMap saved_;
    QVariantMap values_;
    QString error_;
    bool loaded_ = false;
    bool saving_ = false;
    bool conflict_ = false;
    std::uint64_t generation_ = 0;
    std::optional<std::uint64_t> folder_generation_;
};
} // namespace ravo
