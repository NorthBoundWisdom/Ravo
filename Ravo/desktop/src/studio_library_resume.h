#pragma once

#include <optional>
#include <QTimer>
#include "ravo/domain/types.h"

namespace ravo
{
struct StudioLibraryPosition
{
    QString asset_id;
    QString mode = QStringLiteral("grid");
    LibraryQuery query;
    bool collapse_stacks = true;
    bool last_import_selected = false;
    std::size_t last_import_count = 0;
};

// Desktop preferences only, on the presenter's UI thread. Catalog/recipe data
// remains owned by services. One immutable JSON setting is written atomically.
struct StudioLibraryResume
{
    QTimer persist_timer;
    bool suspended = false;
    QString initial_mode = QStringLiteral("grid");
    std::optional<StudioLibraryPosition> pending;
};

[[nodiscard]] Result<QString> last_studio_catalog();
} // namespace ravo
