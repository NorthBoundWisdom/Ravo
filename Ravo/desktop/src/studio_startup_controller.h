#pragma once

#include <QObject>
#include <QString>

namespace ravo
{
class StudioPresenter;

// Main-thread startup presentation owner. The presenter outlives this controller
// and retains ownership of library I/O and worker shutdown.
class StudioStartupController final : public QObject
{
    Q_OBJECT

public:
    StudioStartupController(StudioPresenter &presenter, QString default_catalog_path,
                            QObject *parent = nullptr);
    Q_INVOKABLE void start();

signals:
    // Emitted once, after the initial listing or error has been published.
    void finished(bool create_library);

private:
    void finish(bool create_library = false);
    StudioPresenter &presenter_;
    const QString default_catalog_path_;
    bool started_ = false;
    bool finished_ = false;
};
} // namespace ravo
