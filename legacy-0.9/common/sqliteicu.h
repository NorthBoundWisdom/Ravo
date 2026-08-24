/*
** 2008 May 26
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
******************************************************************************
**
** This header file is used by programs that want to link against the
** ICU extension.  All it does is declare the sqlite3IcuInit() interface.
*/
#include "sqlite3.h"

G_BEGIN_DECLS

int sqlite3IcuInit(sqlite3 *db);

G_END_DECLS
