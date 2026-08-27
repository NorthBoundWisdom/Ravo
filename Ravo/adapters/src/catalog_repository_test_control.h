#pragma once

#include <cstdint>

#include "ravo/adapters/sqlite_catalog.h"

namespace ravo::testing
{

enum class SqliteImportFailure : std::uint8_t
{
    kNone,
    kTransactionBegin,
    kAssetBind,
    kAssetWrite,
    kCaptureBind,
    kCaptureWrite,
    kRevisionUpdate,
    kRevisionRead,
    kCommit,
    kRollback,
};

// Source-private, repository-instance fault control. Product callers cannot
// reach this header through the installed adapter interface.
class SqliteCatalogTestControl
{
public:
    static void inject(SqliteCatalogRepository &repository, SqliteImportFailure failure) noexcept;
};

} // namespace ravo::testing
