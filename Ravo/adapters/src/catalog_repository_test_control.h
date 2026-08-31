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

enum class SqliteRecoveryFailure : std::uint8_t
{
    kNone,
    kAcknowledge,
};

enum class SqliteFolderRelinkFailure : std::uint8_t
{
    kNone,
    kAfterFolderUpdate,
    kAfterFirstAssetUpdate,
    kBeforeCommit,
};

// Source-private, repository-instance fault control. Product callers cannot
// reach this header through the installed adapter interface.
class SqliteCatalogTestControl
{
public:
    static void inject(SqliteCatalogRepository &repository, SqliteImportFailure failure) noexcept;
    static void inject_recovery(SqliteCatalogRepository &repository,
                                SqliteRecoveryFailure failure) noexcept;
    static void inject_folder_relink(SqliteCatalogRepository &repository,
                                     SqliteFolderRelinkFailure failure) noexcept;
};

} // namespace ravo::testing
