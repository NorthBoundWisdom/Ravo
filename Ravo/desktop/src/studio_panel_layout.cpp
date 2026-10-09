#include "ravo/desktop/studio_panel_layout.h"

#include <utility>

#include <QCoreApplication>
#include <QSettings>
#include <QVariantMap>

namespace ravo
{
namespace
{
constexpr auto kLayoutKey = "desktop/panel-layout/v1";
bool valid_sizes(int left, int right, int bottom)
{
    return left >= 160 && left <= 640 && right >= 260 && right <= 800 && bottom >= 88 &&
           bottom <= 400;
}
} // namespace

StudioPanelLayout::StudioPanelLayout(QObject *parent)
    : QObject(parent)
{
    persist_timer_.setSingleShot(true);
    persist_timer_.setInterval(200);
    connect(&persist_timer_, &QTimer::timeout, this, [this]() { static_cast<void>(flush()); });
    if (QCoreApplication::instance())
        connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this,
                [this]() { static_cast<void>(flush()); });
}

StudioPanelLayout::~StudioPanelLayout()
{
    static_cast<void>(flush());
}

bool StudioPanelLayout::initialize()
{
    QSettings settings;
    const auto stored = settings.value(QLatin1String(kLayoutKey));
    if (settings.status() != QSettings::NoError)
    {
        setError(tr("Unable to read the panel layout."));
        return false;
    }
    if (!stored.isValid())
        return true;
    const auto sizes = stored.toMap();
    bool left_ok = false, right_ok = false, bottom_ok = false;
    const int left = sizes.value(QStringLiteral("leftWidth")).toInt(&left_ok);
    const int right = sizes.value(QStringLiteral("rightWidth")).toInt(&right_ok);
    const int bottom = sizes.value(QStringLiteral("filmstripHeight")).toInt(&bottom_ok);
    if (!left_ok || !right_ok || !bottom_ok || !valid_sizes(left, right, bottom))
    {
        setError(tr("The stored panel layout is invalid."));
        return false;
    }
    left_width_ = left;
    right_width_ = right;
    filmstrip_height_ = bottom;
    emit layoutChanged();
    return true;
}

bool StudioPanelLayout::setSideWidths(int left, int right)
{
    if (!valid_sizes(left, right, filmstrip_height_))
    {
        setError(tr("Panel size is invalid."));
        return false;
    }
    if (left_width_ == left && right_width_ == right)
        return true;
    left_width_ = left;
    right_width_ = right;
    changed();
    return true;
}

bool StudioPanelLayout::setFilmstripHeight(int height)
{
    if (!valid_sizes(left_width_, right_width_, height))
    {
        setError(tr("Panel size is invalid."));
        return false;
    }
    if (filmstrip_height_ == height)
        return true;
    filmstrip_height_ = height;
    changed();
    return true;
}

void StudioPanelLayout::changed()
{
    dirty_ = true;
    persist_timer_.start();
    emit layoutChanged();
}

bool StudioPanelLayout::flush()
{
    persist_timer_.stop();
    if (!dirty_)
        return true;
    QSettings settings;
    settings.setValue(QLatin1String(kLayoutKey),
                      QVariantMap{{QStringLiteral("leftWidth"), left_width_},
                                  {QStringLiteral("rightWidth"), right_width_},
                                  {QStringLiteral("filmstripHeight"), filmstrip_height_}});
    settings.sync();
    if (settings.status() != QSettings::NoError)
    {
        setError(tr("Unable to save the panel layout."));
        return false;
    }
    dirty_ = false;
    setError({});
    return true;
}

void StudioPanelLayout::setError(QString message)
{
    if (last_error_ == message)
        return;
    last_error_ = std::move(message);
    emit errorChanged();
}
} // namespace ravo
