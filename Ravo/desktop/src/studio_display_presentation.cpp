#include "ravo/desktop/studio_display_presentation.h"

#include <array>
#include <string>
#include <utility>

#include <QGuiApplication>
#include <QScreen>
#include <QWindow>

namespace ravo
{
namespace
{

[[nodiscard]] QVariantMap state_to_map(const DisplayPresentationState &state)
{
    return QVariantMap{
        {QStringLiteral("contractVersion"), QString::fromStdString(state.contract_version)},
        {QStringLiteral("screenToken"), QString::fromStdString(state.screen_token)},
        {QStringLiteral("source"),
         QString::fromUtf8(display_profile_source_name(state.source).data(),
                           static_cast<int>(display_profile_source_name(state.source).size()))},
        {QStringLiteral("reason"), QString::fromStdString(state.reason)},
        {QStringLiteral("profileIdentifier"),
         QString::fromStdString(state.monitor_profile.identifier)},
        {QStringLiteral("profileFingerprint"), QString::fromStdString(state.profile_fingerprint)},
        {QStringLiteral("iccByteCount"),
         QVariant::fromValue(static_cast<qulonglong>(state.monitor_profile.icc_bytes.size()))},
        {QStringLiteral("valid"), state.valid},
    };
}

} // namespace

StudioDisplayPresentation::StudioDisplayPresentation(QObject *parent)
    : QObject(parent)
{
    auto discovered = discover_monitor_presentation("primary");
    if (discovered)
    {
        publish(std::move(discovered).value());
    }
    else
    {
        presentation_ = {};
        presentation_.source = DisplayProfileSource::kFallbackSrgb;
        presentation_.reason = discovered.error().message;
        presentation_.valid = false;
        state_ = state_to_map(presentation_);
        state_.insert(
            QStringLiteral("contractVersion"),
            QString::fromUtf8(kDisplayPresentationContractVersion.data(),
                              static_cast<int>(kDisplayPresentationContractVersion.size())));
        state_.insert(QStringLiteral("screenToken"), QStringLiteral("primary"));
        state_.insert(QStringLiteral("source"), QStringLiteral("fallback_srgb"));
        state_.insert(QStringLiteral("valid"), false);
    }
}

StudioDisplayPresentation::~StudioDisplayPresentation()
{
    detach();
}

QVariantMap StudioDisplayPresentation::state() const
{
    return state_;
}

QString StudioDisplayPresentation::screenToken() const
{
    return state_.value(QStringLiteral("screenToken")).toString();
}

QString StudioDisplayPresentation::source() const
{
    return state_.value(QStringLiteral("source")).toString();
}

QString StudioDisplayPresentation::reason() const
{
    return state_.value(QStringLiteral("reason")).toString();
}

bool StudioDisplayPresentation::valid() const noexcept
{
    return state_.value(QStringLiteral("valid")).toBool();
}

QVariantList StudioDisplayPresentation::viewContracts() const
{
    QVariantList list;
    for (const auto &entry : display_presentation_view_contracts())
    {
        list.push_back(QVariantMap{
            {QStringLiteral("viewId"), QString::fromStdString(entry.view_id)},
            {QStringLiteral("pixelKind"),
             QString::fromUtf8(
                 display_view_pixel_kind_name(entry.pixel_kind).data(),
                 static_cast<int>(display_view_pixel_kind_name(entry.pixel_kind).size()))},
            {QStringLiteral("softProofInteraction"),
             QString::fromStdString(entry.soft_proof_interaction)},
            {QStringLiteral("notes"), QString::fromStdString(entry.notes)},
        });
    }
    return list;
}

const DisplayPresentationState &StudioDisplayPresentation::presentationState() const noexcept
{
    return presentation_;
}

void StudioDisplayPresentation::bindWindow(QObject *window_object)
{
    attach(qobject_cast<QWindow *>(window_object));
}

void StudioDisplayPresentation::refresh()
{
    if (window_ != nullptr)
    {
        handleScreenChanged(window_->screen());
        return;
    }
    applyToken(QStringLiteral("primary"), true);
}

bool StudioDisplayPresentation::injectSyntheticMatrixForTesting()
{
    const std::array<float, 9> boost{1.05F, 0.0F, 0.0F, 0.0F, 0.95F, 0.0F, 0.0F, 0.0F, 1.0F};
    auto presentation = make_synthetic_matrix_monitor_presentation(boost, "synthetic-test");
    if (!presentation)
        return false;
    synthetic_lock_ = true;
    publish(std::move(presentation).value());
    return true;
}

bool StudioDisplayPresentation::applyScreenTokenForTesting(const QString &screen_token)
{
    if (screen_token.isEmpty())
        return false;
    if (synthetic_lock_)
    {
        auto refreshed = refresh_monitor_presentation(presentation_, screen_token.toStdString());
        if (!refreshed)
            return false;
        publish(std::move(refreshed).value());
        return true;
    }
    applyToken(screen_token, true);
    return valid();
}

void StudioDisplayPresentation::attach(QWindow *window)
{
    if (window_ == window)
        return;
    detach();
    window_ = window;
    if (window_ == nullptr)
        return;
    connect(window_, &QWindow::screenChanged, this,
            &StudioDisplayPresentation::handleScreenChanged);
    handleScreenChanged(window_->screen());
}

void StudioDisplayPresentation::detach()
{
    if (window_ == nullptr)
        return;
    disconnect(window_, nullptr, this, nullptr);
    window_.clear();
}

void StudioDisplayPresentation::handleScreenChanged(QScreen *screen)
{
    if (synthetic_lock_)
        return;
    applyToken(tokenForScreen(screen), true);
}

void StudioDisplayPresentation::applyToken(const QString &token, const bool force_rediscover)
{
    static_cast<void>(force_rediscover);
    auto discovered = discover_monitor_presentation(token.toStdString());
    if (!discovered)
    {
        presentation_.valid = false;
        presentation_.reason = discovered.error().message;
        state_.insert(QStringLiteral("valid"), false);
        state_.insert(QStringLiteral("reason"), QString::fromStdString(discovered.error().message));
        emit stateChanged();
        return;
    }
    publish(std::move(discovered).value());
}

void StudioDisplayPresentation::publish(DisplayPresentationState state)
{
    const auto next = state_to_map(state);
    const bool unchanged =
        next == state_ && state.profile_fingerprint == presentation_.profile_fingerprint &&
        state.screen_token == presentation_.screen_token && state.source == presentation_.source;
    presentation_ = std::move(state);
    if (unchanged)
        return;
    state_ = next;
    emit stateChanged();
}

QString StudioDisplayPresentation::tokenForScreen(const QScreen *screen)
{
    if (screen == nullptr)
        return QStringLiteral("primary");
    const QRect geo = screen->geometry();
    const QPoint center = geo.center();
    return QString::fromStdString(macos_display_screen_token_for_point(
        static_cast<double>(center.x()), static_cast<double>(center.y())));
}

} // namespace ravo
