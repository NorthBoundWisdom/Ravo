#include "ravo/desktop/studio_window_geometry.h"

#include <algorithm>
#include <tuple>
#include <utility>

#include <QCoreApplication>
#include <QGuiApplication>
#include <QScreen>
#include <QSettings>
#include <QWindow>

namespace ravo
{
namespace
{

constexpr auto kXKey = "desktop/window/x";
constexpr auto kYKey = "desktop/window/y";
constexpr auto kWidthKey = "desktop/window/width";
constexpr auto kHeightKey = "desktop/window/height";
constexpr auto kMaximizedKey = "desktop/window/maximized";
constexpr int kPersistDelayMs = 200;
constexpr int kMinimumVisibleEdge = 80;
constexpr int kMaximumExtent = 16384;
constexpr int kMaximumOrigin = 100000;

} // namespace

StudioWindowGeometry::StudioWindowGeometry(QObject *parent)
    : QObject(parent)
{
    persist_timer_.setSingleShot(true);
    persist_timer_.setInterval(kPersistDelayMs);
    connect(&persist_timer_, &QTimer::timeout, this, [this]() { static_cast<void>(persist()); });
}

StudioWindowGeometry::~StudioWindowGeometry()
{
    detach();
    flush();
}

bool StudioWindowGeometry::initialize()
{
    if (!loadStored())
        return false;
    return true;
}

int StudioWindowGeometry::startupX() const noexcept
{
    return fittedRect().x();
}

int StudioWindowGeometry::startupY() const noexcept
{
    return fittedRect().y();
}

int StudioWindowGeometry::startupWidth() const noexcept
{
    return fittedRect().width();
}

int StudioWindowGeometry::startupHeight() const noexcept
{
    return fittedRect().height();
}

bool StudioWindowGeometry::hasStoredGeometry() const noexcept
{
    return has_stored_;
}

bool StudioWindowGeometry::startupMaximized() const noexcept
{
    return maximized_;
}

QString StudioWindowGeometry::lastError() const
{
    return last_error_;
}

QRect StudioWindowGeometry::fittedRect() const
{
    return fitToScreens(QRect(x_, y_, width_, height_));
}

void StudioWindowGeometry::restore(QObject *window_object)
{
    attach(qobject_cast<QWindow *>(window_object));
}

bool StudioWindowGeometry::rememberWindowed(const int x, const int y, const int width,
                                            const int height)
{
    if (!validWindowed(x, y, width, height))
    {
        setError(QCoreApplication::translate("StudioWindowGeometry",
                                             "Window size or position is invalid."));
        return false;
    }
    const auto previous = std::tuple(x_, y_, width_, height_, maximized_, has_stored_);
    x_ = x;
    y_ = y;
    width_ = width;
    height_ = height;
    maximized_ = false;
    if (persist())
        return true;
    std::tie(x_, y_, width_, height_, maximized_, has_stored_) = previous;
    return false;
}

bool StudioWindowGeometry::setMaximized(const bool maximized)
{
    const bool previous = maximized_;
    const bool previous_stored = has_stored_;
    maximized_ = maximized;
    if (persist())
        return true;
    maximized_ = previous;
    has_stored_ = previous_stored;
    return false;
}

void StudioWindowGeometry::attach(QWindow *window)
{
    detach();
    window_ = window;
    if (window_ == nullptr)
        return;
    connect(window_, &QWindow::xChanged, this, &StudioWindowGeometry::handleWindowChanged);
    connect(window_, &QWindow::yChanged, this, &StudioWindowGeometry::handleWindowChanged);
    connect(window_, &QWindow::widthChanged, this, &StudioWindowGeometry::handleWindowChanged);
    connect(window_, &QWindow::heightChanged, this, &StudioWindowGeometry::handleWindowChanged);
    connect(window_, &QWindow::visibilityChanged, this, &StudioWindowGeometry::handleWindowChanged);
    if (QGuiApplication::instance() != nullptr)
    {
        connect(QGuiApplication::instance(), &QGuiApplication::aboutToQuit, this,
                &StudioWindowGeometry::flush);
    }
    if (!has_stored_)
        return;
    applying_ = true;
    window_->setGeometry(fittedRect());
    if (maximized_)
        window_->setVisibility(QWindow::Maximized);
    applying_ = false;
}

void StudioWindowGeometry::detach()
{
    if (window_ == nullptr)
        return;
    disconnect(window_, nullptr, this, nullptr);
    window_.clear();
}

void StudioWindowGeometry::handleWindowChanged()
{
    if (applying_ || window_ == nullptr)
        return;
    switch (window_->visibility())
    {
    case QWindow::Minimized:
    case QWindow::Hidden:
    case QWindow::AutomaticVisibility:
        return;
    case QWindow::Maximized:
        maximized_ = true;
        persist_timer_.start();
        return;
    case QWindow::FullScreen:
        return;
    case QWindow::Windowed:
        break;
    }
    if (!validWindowed(window_->x(), window_->y(), window_->width(), window_->height()))
        return;
    x_ = window_->x();
    y_ = window_->y();
    width_ = window_->width();
    height_ = window_->height();
    maximized_ = false;
    persist_timer_.start();
}

void StudioWindowGeometry::flush()
{
    if (!persist_timer_.isActive())
        return;
    persist_timer_.stop();
    static_cast<void>(persist());
}

bool StudioWindowGeometry::persist()
{
    QSettings settings;
    settings.setValue(QLatin1String(kXKey), x_);
    settings.setValue(QLatin1String(kYKey), y_);
    settings.setValue(QLatin1String(kWidthKey), width_);
    settings.setValue(QLatin1String(kHeightKey), height_);
    settings.setValue(QLatin1String(kMaximizedKey), maximized_);
    settings.sync();
    if (settings.status() != QSettings::NoError)
    {
        setError(QCoreApplication::translate("StudioWindowGeometry",
                                             "Unable to save the window position."));
        return false;
    }
    has_stored_ = true;
    setError({});
    return true;
}

bool StudioWindowGeometry::loadStored()
{
    QSettings settings;
    const bool has_x = settings.contains(QLatin1String(kXKey));
    const bool has_y = settings.contains(QLatin1String(kYKey));
    const bool has_width = settings.contains(QLatin1String(kWidthKey));
    const bool has_height = settings.contains(QLatin1String(kHeightKey));
    const bool has_maximized = settings.contains(QLatin1String(kMaximizedKey));
    if (!has_x && !has_y && !has_width && !has_height && !has_maximized)
    {
        has_stored_ = false;
        return true;
    }
    if (!(has_x && has_y && has_width && has_height && has_maximized))
    {
        if (!clearStored())
        {
            setError(QCoreApplication::translate(
                "StudioWindowGeometry", "Unable to repair the stored window position."));
            return false;
        }
        return true;
    }
    bool x_ok = false;
    bool y_ok = false;
    bool width_ok = false;
    bool height_ok = false;
    const int x = settings.value(QLatin1String(kXKey)).toInt(&x_ok);
    const int y = settings.value(QLatin1String(kYKey)).toInt(&y_ok);
    const int width = settings.value(QLatin1String(kWidthKey)).toInt(&width_ok);
    const int height = settings.value(QLatin1String(kHeightKey)).toInt(&height_ok);
    const QVariant maximized_value = settings.value(QLatin1String(kMaximizedKey));
    const bool maximized_ok = maximized_value.canConvert<bool>();
    if (!x_ok || !y_ok || !width_ok || !height_ok || !maximized_ok ||
        !validWindowed(x, y, width, height))
    {
        if (!clearStored())
        {
            setError(QCoreApplication::translate(
                "StudioWindowGeometry", "Unable to repair the stored window position."));
            return false;
        }
        return true;
    }
    x_ = x;
    y_ = y;
    width_ = width;
    height_ = height;
    maximized_ = maximized_value.toBool();
    has_stored_ = true;
    return true;
}

bool StudioWindowGeometry::clearStored()
{
    QSettings settings;
    settings.remove(QLatin1String(kXKey));
    settings.remove(QLatin1String(kYKey));
    settings.remove(QLatin1String(kWidthKey));
    settings.remove(QLatin1String(kHeightKey));
    settings.remove(QLatin1String(kMaximizedKey));
    settings.sync();
    x_ = 0;
    y_ = 0;
    width_ = kDefaultWidth;
    height_ = kDefaultHeight;
    maximized_ = false;
    has_stored_ = false;
    return settings.status() == QSettings::NoError;
}

void StudioWindowGeometry::setError(QString message)
{
    if (last_error_ == message)
        return;
    last_error_ = std::move(message);
    emit errorChanged();
}

bool StudioWindowGeometry::validWindowed(const int x, const int y, const int width,
                                         const int height) noexcept
{
    return width >= kMinimumWidth && height >= kMinimumHeight && width <= kMaximumExtent &&
           height <= kMaximumExtent && x >= -kMaximumOrigin && x <= kMaximumOrigin &&
           y >= -kMaximumOrigin && y <= kMaximumOrigin;
}

QRect StudioWindowGeometry::fitToScreens(QRect candidate)
{
    const auto screens = QGuiApplication::screens();
    if (screens.isEmpty())
        return candidate;
    int max_width = 0;
    int max_height = 0;
    for (const auto *screen : screens)
    {
        const QRect available = screen->availableGeometry();
        max_width = std::max(max_width, available.width());
        max_height = std::max(max_height, available.height());
    }
    candidate.setWidth(std::clamp(candidate.width(), kMinimumWidth, max_width));
    candidate.setHeight(std::clamp(candidate.height(), kMinimumHeight, max_height));
    for (const auto *screen : screens)
    {
        const QRect hit = screen->availableGeometry().intersected(candidate);
        if (hit.width() >= kMinimumVisibleEdge && hit.height() >= kMinimumVisibleEdge)
            return candidate;
    }
    const QScreen *home = QGuiApplication::primaryScreen() != nullptr ?
                              QGuiApplication::primaryScreen() :
                              screens.front();
    const QRect available = home->availableGeometry();
    candidate.setWidth(std::min(candidate.width(), available.width()));
    candidate.setHeight(std::min(candidate.height(), available.height()));
    candidate.moveLeft(available.x() + std::max(0, (available.width() - candidate.width()) / 2));
    candidate.moveTop(available.y() + std::max(0, (available.height() - candidate.height()) / 2));
    return candidate;
}

} // namespace ravo
