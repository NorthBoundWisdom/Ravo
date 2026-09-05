#pragma once

#include <QString>
#include "ravo/foundation/error.h"

namespace ravo
{
// Desktop-only preference. No service or catalog state is stored in QSettings.
class StudioImportPreferences final
{
public:
    [[nodiscard]] Result<QString> loadLastDestination() const;
    [[nodiscard]] Result<void> rememberDestination(const QString &path) const;
};
} // namespace ravo
