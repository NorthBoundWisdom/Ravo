#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

namespace ravo
{

// Desktop preferences in logical pixels, independent of catalogs and recipes.
// Owned and used on the GUI thread; pending writes flush before destruction.
class StudioPanelLayout final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int leftWidth READ leftWidth NOTIFY layoutChanged)
    Q_PROPERTY(int rightWidth READ rightWidth NOTIFY layoutChanged)
    Q_PROPERTY(int filmstripHeight READ filmstripHeight NOTIFY layoutChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY errorChanged)

public:
    explicit StudioPanelLayout(QObject *parent = nullptr);
    ~StudioPanelLayout() override;
    [[nodiscard]] bool initialize();
    int leftWidth() const noexcept
    {
        return left_width_;
    }
    int rightWidth() const noexcept
    {
        return right_width_;
    }
    int filmstripHeight() const noexcept
    {
        return filmstrip_height_;
    }
    QString lastError() const
    {
        return last_error_;
    }
    Q_INVOKABLE bool setSideWidths(int left, int right);
    Q_INVOKABLE bool setFilmstripHeight(int height);
    Q_INVOKABLE bool flush();

signals:
    void layoutChanged();
    void errorChanged();

private:
    void changed();
    void setError(QString message);
    QTimer persist_timer_;
    bool dirty_ = false;
    int left_width_ = 240;
    int right_width_ = 320;
    int filmstrip_height_ = 108;
    QString last_error_;
};

} // namespace ravo
