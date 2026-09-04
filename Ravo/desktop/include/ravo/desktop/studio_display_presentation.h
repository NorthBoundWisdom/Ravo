#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include "ravo/services/display_presentation.h"

class QScreen;
class QWindow;

namespace ravo
{

// ADR-0144 DISPLAY-01: C++ owner for Studio window→screen monitor ICC refresh.
// Presentation-only; never mutates recipe, history, catalog revision, or export.
class StudioDisplayPresentation final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantMap state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString screenToken READ screenToken NOTIFY stateChanged)
    Q_PROPERTY(QString source READ source NOTIFY stateChanged)
    Q_PROPERTY(QString reason READ reason NOTIFY stateChanged)
    Q_PROPERTY(bool valid READ valid NOTIFY stateChanged)
    Q_PROPERTY(QVariantList viewContracts READ viewContracts CONSTANT)

public:
    explicit StudioDisplayPresentation(QObject *parent = nullptr);
    ~StudioDisplayPresentation() override;

    [[nodiscard]] QVariantMap state() const;
    [[nodiscard]] QString screenToken() const;
    [[nodiscard]] QString source() const;
    [[nodiscard]] QString reason() const;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] QVariantList viewContracts() const;
    [[nodiscard]] const DisplayPresentationState &presentationState() const noexcept;

    Q_INVOKABLE void bindWindow(QObject *window_object);
    Q_INVOKABLE void refresh();
    // Test/headless: inject synthetic presentation without OS discovery.
    Q_INVOKABLE bool injectSyntheticMatrixForTesting();
    Q_INVOKABLE bool applyScreenTokenForTesting(const QString &screen_token);

signals:
    void stateChanged();

private:
    void attach(QWindow *window);
    void detach();
    void handleScreenChanged(QScreen *screen);
    void applyToken(const QString &token, bool force_rediscover);
    void publish(DisplayPresentationState state);
    [[nodiscard]] static QString tokenForScreen(const QScreen *screen);

    QPointer<QWindow> window_;
    DisplayPresentationState presentation_;
    QVariantMap state_;
    bool synthetic_lock_ = false;
};

} // namespace ravo
