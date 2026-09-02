#pragma once

#include <QObject>
#include <QPointer>
#include <QRect>
#include <QString>
#include <QTimer>

class QWindow;

namespace ravo
{

class StudioWindowGeometry final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int startupX READ startupX CONSTANT)
    Q_PROPERTY(int startupY READ startupY CONSTANT)
    Q_PROPERTY(int startupWidth READ startupWidth CONSTANT)
    Q_PROPERTY(int startupHeight READ startupHeight CONSTANT)
    Q_PROPERTY(bool hasStoredGeometry READ hasStoredGeometry CONSTANT)
    Q_PROPERTY(bool startupMaximized READ startupMaximized CONSTANT)
    Q_PROPERTY(QString lastError READ lastError NOTIFY errorChanged)

public:
    static constexpr int kDefaultWidth = 1440;
    static constexpr int kDefaultHeight = 900;
    static constexpr int kMinimumWidth = 640;
    static constexpr int kMinimumHeight = 480;

    explicit StudioWindowGeometry(QObject *parent = nullptr);
    ~StudioWindowGeometry() override;

    [[nodiscard]] bool initialize();
    [[nodiscard]] int startupX() const noexcept;
    [[nodiscard]] int startupY() const noexcept;
    [[nodiscard]] int startupWidth() const noexcept;
    [[nodiscard]] int startupHeight() const noexcept;
    [[nodiscard]] bool hasStoredGeometry() const noexcept;
    [[nodiscard]] bool startupMaximized() const noexcept;
    [[nodiscard]] QString lastError() const;
    [[nodiscard]] QRect fittedRect() const;

    Q_INVOKABLE void restore(QObject *window_object);
    bool rememberWindowed(int x, int y, int width, int height);
    bool setMaximized(bool maximized);

signals:
    void errorChanged();

private:
    void attach(QWindow *window);
    void detach();
    void handleWindowChanged();
    void flush();
    [[nodiscard]] bool persist();
    [[nodiscard]] bool loadStored();
    [[nodiscard]] bool clearStored();
    void setError(QString message);
    [[nodiscard]] static bool validWindowed(int x, int y, int width, int height) noexcept;
    [[nodiscard]] static QRect fitToScreens(QRect candidate);

    QPointer<QWindow> window_;
    QTimer persist_timer_;
    int x_ = 0;
    int y_ = 0;
    int width_ = kDefaultWidth;
    int height_ = kDefaultHeight;
    bool maximized_ = false;
    bool has_stored_ = false;
    bool applying_ = false;
    QString last_error_;
};

} // namespace ravo
