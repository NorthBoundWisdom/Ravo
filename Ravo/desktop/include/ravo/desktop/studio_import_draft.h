#pragma once

#include <QString>

#include "ravo/domain/types.h"

namespace ravo
{

// Typed Import workspace draft. Candidates/selection remain in
// ImportCandidateListModel; this holds configuration only.
struct ImportDraft
{
    QString source_root;
    QString destination;
    QString second_copy_destination;
    bool second_copy_enabled = false;
    QString organization = QStringLiteral("single"); // persisted string form
    QString mode = QStringLiteral("copy");
    QString filename_pattern; // template
    QString preview_policy = QStringLiteral("standard");
    bool destination_valid = false;
    QString destination_error;
};

} // namespace ravo
