#pragma once

#include <array>
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
    bool rename_enabled = false;
    // Ordered components: 0=None, 1=original stem, 2=capture date, 3=sequence.
    // The first component is required; separators: 0=underscore, 1=hyphen, 2=none.
    std::array<int, 3> rename_parts{2, 1, 3};
    int rename_separator = 0;
    QString preview_policy = QStringLiteral("standard");
    bool destination_valid = false;
    QString destination_error;
};

} // namespace ravo
