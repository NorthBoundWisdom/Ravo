/*
    This file is part of darktable,
    Copyright (C) 2011-2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "common/atomic.h"
#include "common/database.h"
#include "common/database_schema.h"
#include "common/darktable.h"
#include "common/datetime.h"
#include "common/debug.h"
#include "common/file_location.h"
#include "common/iop_order.h"
#include "common/styles.h"
#include "common/history.h"
#include "common/metadata.h"
#include "common/metadata.h"
#include "common/sqliteicu.h"
#include "control/conf.h"
#include "control/control.h"

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <sqlite3.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#ifndef _WIN32
#include <sys/file.h>
#else
#include <io.h>
#include <windows.h>
#endif
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define USE_NESTED_TRANSACTIONS
#define MAX_NESTED_TRANSACTIONS 5
/* transaction id */
static dt_atomic_int _trxid;
#if !DT_BUILD_DEVMODE
static int _application_instance_lock_fd = -1;
static gchar *_application_instance_lockfile = NULL;

static int _application_instance_lock_acquire(const int fd)
{
#ifdef _WIN32
    const intptr_t handle_value = _get_osfhandle(fd);
    if (handle_value == -1)
    {
        errno = EBADF;
        return -1;
    }

    OVERLAPPED overlapped = {0};
    if (LockFileEx((HANDLE)handle_value, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                   0, MAXDWORD, MAXDWORD, &overlapped))
        return 0;

    const DWORD lock_error = GetLastError();
    errno = (lock_error == ERROR_LOCK_VIOLATION || lock_error == ERROR_SHARING_VIOLATION) ?
                EWOULDBLOCK :
                EIO;
    return -1;
#else
    return flock(fd, LOCK_EX | LOCK_NB);
#endif
}

static void _application_instance_lock_release(const int fd)
{
#ifdef _WIN32
    const intptr_t handle_value = _get_osfhandle(fd);
    if (handle_value != -1)
    {
        OVERLAPPED overlapped = {0};
        UnlockFileEx((HANDLE)handle_value, 0, MAXDWORD, MAXDWORD, &overlapped);
    }
#else
    flock(fd, LOCK_UN);
#endif
}
#endif

typedef struct dt_database_t
{
    gboolean lock_acquired;
    gboolean application_instance_lock_failed;
    gboolean application_instance_lock_contended;

    /* data database filename */
    gchar *dbfilename_data, *lockfile_data;

    /* library database filename */
    gchar *dbfilename_library, *lockfile_library;

    /* ondisk DB */
    sqlite3 *handle;

    gchar *error_message, *error_dbfilename;
    int error_other_pid;
} dt_database_t;

int32_t dt_database_last_insert_rowid(const dt_database_t *db)
{
    return (int32_t)sqlite3_last_insert_rowid(db->handle);
}

/* Create the 0.9 library database schema. */
static void _create_library_schema(dt_database_t *db)
{
    sqlite3_stmt *stmt;
    ////////////////////////////// db_info
    // clang-format off
  sqlite3_exec
    (db->handle, "CREATE TABLE main.db_info (key VARCHAR PRIMARY KEY, value VARCHAR)",
     NULL, NULL, NULL);
  sqlite3_prepare_v2
    (db->handle, "INSERT OR REPLACE INTO main.db_info (key, value) VALUES ('version', ?1)",
     -1, &stmt, NULL);
    // clang-format on
    sqlite3_bind_int(stmt, 1, DT_DATABASE_SCHEMA_VERSION_LIBRARY);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    ////////////////////////////// film_rolls
    // clang-format off
  sqlite3_exec(db->handle,
               "CREATE TABLE main.film_rolls "
               "(id INTEGER PRIMARY KEY, access_timestamp INTEGER, "
               //                        "folder VARCHAR(1024), external_drive VARCHAR(1024))", //
               //                        Schema changes require an explicit new database contract.
               "folder VARCHAR(1024) NOT NULL)",
               NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE INDEX main.film_rolls_folder_index ON film_rolls (folder)", NULL, NULL, NULL);
  ////////////////////////////// maker
  sqlite3_exec(db->handle,
               "CREATE TABLE main.makers"
               " (id INTEGER PRIMARY KEY AUTOINCREMENT,"
               "  name VARCHAR)",
               NULL, NULL, NULL);
  sqlite3_exec
    (db->handle,
     "CREATE INDEX makers_name ON makers (name)",
     NULL, NULL, NULL);
  ////////////////////////////// model
  sqlite3_exec(db->handle,
               "CREATE TABLE main.models"
               " (id INTEGER PRIMARY KEY AUTOINCREMENT,"
               "  name VARCHAR)",
               NULL, NULL, NULL);
  sqlite3_exec
    (db->handle,
     "CREATE INDEX models_name ON models (name)",
     NULL, NULL, NULL);
  ////////////////////////////// lens
  sqlite3_exec(db->handle,
               "CREATE TABLE main.lens"
               " (id INTEGER PRIMARY KEY AUTOINCREMENT,"
               "  name VARCHAR)",
               NULL, NULL, NULL);
  sqlite3_exec
    (db->handle,
     "CREATE INDEX lens_name ON lens (name)",
     NULL, NULL, NULL);
  ////////////////////////////// cameras
  sqlite3_exec(db->handle,
               "CREATE TABLE cameras"
               " (id INTEGER PRIMARY KEY AUTOINCREMENT,"
               "  maker VARCHAR, model VARCHAR,"
               "  alias VARCHAR)",
               NULL, NULL, NULL);
  sqlite3_exec
    (db->handle,
     "CREATE UNIQUE INDEX cameras_name ON cameras (maker, model, alias)",
     NULL, NULL, NULL);
  ////////////////////////////// white balance
  sqlite3_exec(db->handle,
               "CREATE TABLE whitebalance"
               " (id INTEGER PRIMARY KEY AUTOINCREMENT,"
               "  name VARCHAR)",
               NULL, NULL, NULL);
  sqlite3_exec
    (db->handle,
     "CREATE UNIQUE INDEX whitebalance_name ON whitebalance (name)",
     NULL, NULL, NULL);
  ////////////////////////////// flash
  sqlite3_exec(db->handle,
               "CREATE TABLE flash"
               " (id INTEGER PRIMARY KEY AUTOINCREMENT,"
               "  name VARCHAR)",
               NULL, NULL, NULL);
  sqlite3_exec
    (db->handle,
     "CREATE UNIQUE INDEX flash_name ON flash (name)",
     NULL, NULL, NULL);
  ////////////////////////////// exposure program
  sqlite3_exec(db->handle,
               "CREATE TABLE exposure_program"
               " (id INTEGER PRIMARY KEY AUTOINCREMENT,"
               "  name VARCHAR)",
               NULL, NULL, NULL);
  sqlite3_exec
    (db->handle,
     "CREATE UNIQUE INDEX exposure_program_name ON exposure_program (name)",
     NULL, NULL, NULL);
  ////////////////////////////// metering mode
  sqlite3_exec(db->handle,
               "CREATE TABLE metering_mode"
               " (id INTEGER PRIMARY KEY AUTOINCREMENT,"
               "  name VARCHAR)",
               NULL, NULL, NULL);
  sqlite3_exec
    (db->handle,
     "CREATE UNIQUE INDEX metering_mode_name ON metering_mode (name)",
     NULL, NULL, NULL);
  ////////////////////////////// images
  sqlite3_exec(
      db->handle,
      "CREATE TABLE main.images"
      " (id INTEGER PRIMARY KEY AUTOINCREMENT, group_id INTEGER, film_id INTEGER,"
      "  width INTEGER, height INTEGER, filename VARCHAR,"
      "  maker_id INTEGER, model_id INTEGER, lens_id INTEGER, camera_id INTEGER,"
      "  exposure REAL, aperture REAL, iso REAL, focal_length REAL,"
      "  focus_distance REAL, datetime_taken INTEGER, flags INTEGER,"
      "  output_width INTEGER, output_height INTEGER, crop REAL,"
      "  raw_black INTEGER, raw_maximum INTEGER,"
      "  orientation INTEGER, longitude REAL,"
      "  latitude REAL, altitude REAL, color_matrix BLOB, colorspace INTEGER,"
      "  version INTEGER, max_version INTEGER, write_timestamp INTEGER,"
      "  history_end INTEGER, position INTEGER, aspect_ratio REAL, exposure_bias REAL,"
      "  import_timestamp INTEGER DEFAULT -1, change_timestamp INTEGER DEFAULT -1, "
      "  export_timestamp INTEGER DEFAULT -1, print_timestamp INTEGER DEFAULT -1, "
      "  thumb_timestamp INTEGER DEFAULT -1, thumb_maxmip INTEGER DEFAULT 0, "
      "  whitebalance_id INTEGER, flash_id INTEGER, "
      "  exposure_program_id INTEGER, metering_mode_id INTEGER, flash_tagvalue INTEGER DEFAULT -1, "
      "FOREIGN KEY(maker_id) REFERENCES makers(id) ON DELETE CASCADE ON UPDATE CASCADE, "
      "FOREIGN KEY(model_id) REFERENCES models(id) ON DELETE CASCADE ON UPDATE CASCADE, "
      "FOREIGN KEY(lens_id) REFERENCES lens(id) ON DELETE CASCADE ON UPDATE CASCADE, "
      "FOREIGN KEY(camera_id) REFERENCES cameras(id) ON DELETE CASCADE ON UPDATE CASCADE, "
      "FOREIGN KEY(film_id) REFERENCES film_rolls(id) ON DELETE CASCADE ON UPDATE CASCADE, "
      "FOREIGN KEY(group_id) REFERENCES images(id) ON DELETE RESTRICT ON UPDATE CASCADE, "
      "FOREIGN KEY(whitebalance_id) REFERENCES whitebalance(id) ON DELETE CASCADE ON UPDATE CASCADE, "
      "FOREIGN KEY(flash_id) REFERENCES flash(id) ON DELETE CASCADE ON UPDATE CASCADE, "
      "FOREIGN KEY(exposure_program_id) REFERENCES exposure_program(id) ON DELETE CASCADE ON UPDATE CASCADE, "
      "FOREIGN KEY(metering_mode_id) REFERENCES metering_mode(id) ON DELETE CASCADE ON UPDATE CASCADE)",
      NULL, NULL, NULL);
  sqlite3_exec(db->handle,
               "CREATE INDEX main.images_group_id_index ON images (group_id, id)",
               NULL, NULL, NULL);
  sqlite3_exec(db->handle,
               "CREATE INDEX main.images_film_id_index ON images (film_id, filename)",
               NULL, NULL, NULL);
  sqlite3_exec(db->handle,
               "CREATE INDEX main.images_filename_index ON images (filename, version)",
               NULL, NULL, NULL);
  sqlite3_exec(db->handle,
               "CREATE INDEX main.image_position_index ON images (position)",
               NULL, NULL, NULL);
  sqlite3_exec(db->handle,
               "CREATE INDEX main.images_datetime_taken_nc ON images (datetime_taken)",
               NULL, NULL, NULL);

  ////////////////////////////// selected_images
  sqlite3_exec(db->handle,
               "CREATE TABLE main.selected_images (num INTEGER PRIMARY KEY AUTOINCREMENT,"
               "                                   imgid INTEGER)",
               NULL, NULL, NULL);

  sqlite3_exec(db->handle,
               "CREATE UNIQUE INDEX main.selected_images_ni"
               " ON selected_images (imgid)",
               NULL, NULL, NULL);
  ////////////////////////////// history
  sqlite3_exec(
      db->handle,
      "CREATE TABLE main.history (imgid INTEGER, num INTEGER, module INTEGER, "
      "operation VARCHAR(256), op_params BLOB, enabled INTEGER, "
      "blendop_params BLOB, blendop_version INTEGER, multi_priority INTEGER, multi_name VARCHAR(256), multi_name_hand_edited INTEGER, "
      "FOREIGN KEY(imgid) REFERENCES images(id) ON UPDATE CASCADE ON DELETE CASCADE)",
      NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE INDEX main.history_imgid_op_index ON history (imgid, operation)", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE INDEX main.history_imgid_num_index ON history (imgid, num DESC)", NULL, NULL, NULL);
  ////////////////////////////// masks history
  sqlite3_exec(db->handle,
               "CREATE TABLE main.masks_history (imgid INTEGER, num INTEGER, formid INTEGER, form INTEGER, name VARCHAR(256), "
               "version INTEGER, points BLOB, points_count INTEGER, source BLOB, "
                "FOREIGN KEY(imgid) REFERENCES images(id) ON UPDATE CASCADE ON DELETE CASCADE)",
                 NULL, NULL, NULL);

  sqlite3_exec(db->handle,
      "CREATE INDEX main.masks_history_imgid_index ON masks_history (imgid, num)",
      NULL, NULL, NULL);

  sqlite3_exec(db->handle, "CREATE INDEX main.images_latlong_index ON images (latitude DESC, longitude DESC)",
      NULL, NULL, NULL);

  ////////////////////////////// tagged_images
  sqlite3_exec(db->handle, "CREATE TABLE main.tagged_images (imgid INTEGER, tagid INTEGER, position INTEGER, "
                           "PRIMARY KEY (imgid, tagid),"
                           "FOREIGN KEY(imgid) REFERENCES images(id) ON UPDATE CASCADE ON DELETE CASCADE)", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE INDEX main.tagged_images_tagid_index ON tagged_images (tagid)", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE INDEX main.tagged_images_position_index ON tagged_images (position)", NULL, NULL, NULL);
  ////////////////////////////// color_labels
  sqlite3_exec(db->handle, "CREATE TABLE main.color_labels (imgid INTEGER, color INTEGER)", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE UNIQUE INDEX main.color_labels_idx ON color_labels (imgid, color)", NULL, NULL,
               NULL);
  ////////////////////////////// meta_data
  sqlite3_exec(db->handle, "CREATE TABLE main.meta_data (id INTEGER, key INTEGER, value VARCHAR, "
                           "FOREIGN KEY(id) REFERENCES images(id) ON DELETE CASCADE ON UPDATE CASCADE)", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE UNIQUE INDEX main.metadata_index ON meta_data (id, key, value)", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE INDEX main.metadata_index_key ON meta_data (key)", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE INDEX main.metadata_index_value ON meta_data (value)", NULL, NULL, NULL);

  sqlite3_exec(db->handle, "CREATE TABLE main.module_order (imgid INTEGER PRIMARY KEY, version INTEGER, iop_list VARCHAR)",
               NULL, NULL, NULL);
  sqlite3_exec
    (db->handle, "CREATE TABLE main.history_hash"
     " (imgid INTEGER PRIMARY KEY,"
     "  basic_hash BLOB, auto_hash BLOB, current_hash BLOB,"
     "  mipmap_hash BLOB,"
     "  FOREIGN KEY(imgid) REFERENCES images(id) ON UPDATE CASCADE ON DELETE CASCADE)",
     NULL, NULL, NULL);

  // v34
  sqlite3_exec(db->handle, "CREATE INDEX main.images_datetime_taken_nc ON images (datetime_taken COLLATE NOCASE)",
               NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE INDEX main.metadata_index_key ON meta_data (key)", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE INDEX main.metadata_index_value ON meta_data (value)", NULL, NULL, NULL);

  sqlite3_exec
    (db->handle,
     "CREATE TABLE harmony_guide"
     " (imgid INTEGER PRIMARY KEY,"
     "  type INTEGER, rotation INTEGER, width INTEGER,"
     "  FOREIGN KEY(imgid) REFERENCES images(id)"
     "    ON UPDATE CASCADE ON DELETE CASCADE)",
     NULL, NULL, NULL);

  sqlite3_exec
    (db->handle,
     "CREATE TABLE overlay"
     " (imgid INTEGER, overlay_id INTEGER,"
     "  PRIMARY KEY (imgid, overlay_id),"
     "  FOREIGN KEY(imgid) REFERENCES images(id)"
     "    ON UPDATE CASCADE ON DELETE CASCADE,"
     "  FOREIGN KEY(overlay_id) REFERENCES images(id)"
     "    ON UPDATE CASCADE ON DELETE RESTRICT)",
     NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE INDEX main.overlay_overlay_id_index ON overlay (overlay_id)", NULL, NULL, NULL);

  // Some triggers to remove possible dangling refs in makers/models/lens/cameras
  sqlite3_exec
    (db->handle,
     "CREATE TRIGGER remove_makers AFTER DELETE ON images"
     " BEGIN"
     "  DELETE FROM makers"
     "    WHERE id = OLD.maker_id"
     "      AND NOT EXISTS (SELECT 1 FROM images WHERE maker_id = OLD.maker_id);"
     " END",
     NULL, NULL, NULL);

  sqlite3_exec
    (db->handle,
     "CREATE TRIGGER remove_models AFTER DELETE ON images"
     " BEGIN"
     "  DELETE FROM models"
     "    WHERE id = OLD.model_id"
     "      AND NOT EXISTS (SELECT 1 FROM images WHERE model_id = OLD.model_id);"
     " END",
     NULL, NULL, NULL);

  sqlite3_exec
    (db->handle,
     "CREATE TRIGGER remove_lens AFTER DELETE ON images"
     " BEGIN"
     "  DELETE FROM lens"
     "    WHERE id = OLD.lens_id"
     "      AND NOT EXISTS (SELECT 1 FROM images WHERE lens_id = OLD.lens_id);"
     " END",
     NULL, NULL, NULL);

  sqlite3_exec
    (db->handle,
     "CREATE TRIGGER remove_cameras AFTER DELETE ON images"
     " BEGIN"
     "  DELETE FROM cameras"
     "    WHERE id = OLD.camera_id"
     "      AND NOT EXISTS (SELECT 1 FROM images WHERE camera_id = OLD.camera_id);"
     " END",
     NULL, NULL, NULL);

  // Finally some views to ease walking the data

  // NOTE: datetime_taken is in nano-second since "0001-01-01 00:00:00"
  sqlite3_exec
    (db->handle,
    "CREATE VIEW v_images AS"
    " SELECT mi.id AS id, mk.name AS maker, md.name AS model, ln.name AS lens,"
    "        cm.maker || ' ' || cm.model AS normalized_camera, "
    "        cm.alias AS camera_alias,"
    "        exposure, aperture, iso,"
    "        datetime(datetime_taken/1000000"
    "                 + unixepoch('0001-01-01 00:00:00'), 'unixepoch') AS datetime,"
    "        fr.folder AS folders, filename"
    " FROM images AS mi,"
    "      makers AS mk, models AS md, lens AS ln, cameras AS cm, film_rolls AS fr"
    " WHERE mi.maker_id = mk.id"
    "   AND mi.model_id = md.id"
    "   AND mi.lens_id = ln.id"
    "   AND mi.camera_id = cm.id"
    "   AND mi.film_id = fr.id"
    " ORDER BY normalized_camera, folders",
     NULL, NULL, NULL);
    // clang-format on
}

/* Create the 0.9 data database schema. */
static void _create_data_schema(dt_database_t *db)
{
    sqlite3_stmt *stmt;
    // clang-format off
  ////////////////////////////// db_info
  sqlite3_exec
    (db->handle, "CREATE TABLE data.db_info (key VARCHAR PRIMARY KEY, value VARCHAR)",
     NULL, NULL, NULL);
  sqlite3_prepare_v2
    (db->handle, "INSERT OR REPLACE INTO data.db_info (key, value) VALUES ('version', ?1)",
     -1, &stmt, NULL);
  sqlite3_bind_int(stmt, 1, DT_DATABASE_SCHEMA_VERSION_DATA);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  ////////////////////////////// tags
  sqlite3_exec(db->handle, "CREATE TABLE data.tags (id INTEGER PRIMARY KEY, name VARCHAR, "
                           "synonyms VARCHAR, flags INTEGER)", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE UNIQUE INDEX data.tags_name_idx ON tags (name)", NULL, NULL, NULL);
  ////////////////////////////// styles
  sqlite3_exec(db->handle, "CREATE TABLE data.styles (id INTEGER PRIMARY KEY, name VARCHAR, description VARCHAR, iop_list VARCHAR)",
                        NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE INDEX data.styles_name_index ON styles (name)", NULL, NULL, NULL);
  ////////////////////////////// style_items
  sqlite3_exec(
      db->handle,
      "CREATE TABLE data.style_items (styleid INTEGER, num INTEGER, module INTEGER,"
      "                               operation VARCHAR(256), op_params BLOB, enabled INTEGER,"
      "                               blendop_params BLOB, blendop_version INTEGER,"
      "                               multi_priority INTEGER, multi_name VARCHAR(256),"
      "                               multi_name_hand_edited INTEGER)",
      NULL, NULL, NULL);
  sqlite3_exec(
      db->handle,
      "CREATE INDEX IF NOT EXISTS data.style_items_styleid_index ON style_items (styleid)",
      NULL, NULL, NULL);
  ////////////////////////////// presets
  sqlite3_exec(db->handle, "CREATE TABLE data.presets (name VARCHAR, description VARCHAR, operation "
                           "VARCHAR, op_version INTEGER, op_params BLOB, "
                           "enabled INTEGER, blendop_params BLOB, blendop_version INTEGER, "
                           "multi_priority INTEGER, multi_name VARCHAR(256), "
                           "multi_name_hand_edited INTEGER, "
                           "model VARCHAR, maker VARCHAR, lens VARCHAR, iso_min REAL, iso_max REAL, "
                           "exposure_min REAL, exposure_max REAL, "
                           "aperture_min REAL, aperture_max REAL, focal_length_min REAL, "
                           "focal_length_max REAL, writeprotect INTEGER, "
                           "autoapply INTEGER, filter INTEGER, def INTEGER, format INTEGER)",
               NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE UNIQUE INDEX data.presets_idx ON presets (name, operation, op_version)",
               NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE TABLE data.meta_data (key INTEGER PRIMARY KEY, tagname VARCHAR, "
                           "name VARCHAR, internal INTEGER, visible INTEGER, private INTEGER, display_order INTEGER)",
               NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE UNIQUE INDEX data.meta_data_tagname_idx ON meta_data (tagname)", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE UNIQUE INDEX data.meta_data_name_idx ON meta_data (name)", NULL, NULL, NULL);
    // clang-format on

    const char *metadata_fields[][3] = {
        {"Xmp.dc.creator", "creator", "plugins/lighttable/metadata/creator_flag"},
        {"Xmp.dc.publisher", "publisher", "plugins/lighttable/metadata/publisher_flag"},
        {"Xmp.dc.title", "title", "plugins/lighttable/metadata/title_flag"},
        {"Xmp.dc.description", "description", "plugins/lighttable/metadata/description_flag"},
        {"Xmp.dc.rights", "rights", "plugins/lighttable/metadata/rights_flag"},
        {"Xmp.acdsee.notes", "notes", "plugins/lighttable/metadata/notes_flag"},
        {"Xmp.darktable.version_name", "version name",
         "plugins/lighttable/metadata/version name_flag"},
        {"Xmp.darktable.image_id", "image id", NULL},
        {"Xmp.xmpMM.PreservedFileName", "preserved filename",
         "plugins/lighttable/metadata/preserved filename_flag"},
    };
    const int display_order[] = {2, 3, 0, 1, 4, 5, 6, 7, 8};

    sqlite3_prepare_v2(
        db->handle,
        "INSERT INTO data.meta_data (key, tagname, name, internal, visible, private, display_order) "
        "VALUES (?1, ?2, ?3, ?4, ?5, 0, ?6)",
        -1, &stmt, NULL);
    for (int i = 0; i < G_N_ELEMENTS(metadata_fields); i++)
    {
        sqlite3_bind_int(stmt, 1, i);
        sqlite3_bind_text(stmt, 2, metadata_fields[i][0], -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, metadata_fields[i][1], -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 4, i == 7);
        sqlite3_bind_int(stmt, 5,
                         metadata_fields[i][2] ? !(dt_conf_get_int(metadata_fields[i][2]) & 1) : 0);
        sqlite3_bind_int(stmt, 6, display_order[i]);
        sqlite3_step(stmt);
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
    }
    sqlite3_finalize(stmt);
}

// create the in-memory tables
// temporary stuff for some ops, need this for some reason with newer sqlite3:
static void _create_memory_schema(dt_database_t *db)
{
    // clang-format off
  sqlite3_exec(db->handle, "CREATE TABLE memory.color_labels_temp (imgid INTEGER PRIMARY KEY)", NULL, NULL, NULL);
  sqlite3_exec(
      db->handle,
      "CREATE TABLE memory.collected_images (rowid INTEGER PRIMARY KEY AUTOINCREMENT, imgid INTEGER)", NULL,
      NULL, NULL);
  sqlite3_exec(db->handle, "CREATE TABLE memory.tmp_selection (imgid INTEGER PRIMARY KEY)", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE TABLE memory.taglist "
                           "(tmpid INTEGER PRIMARY KEY, id INTEGER UNIQUE ON CONFLICT IGNORE, "
                           "count INTEGER DEFAULT 0, count2 INTEGER DEFAULT 0)",
               NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE TABLE memory.similar_tags (tagid INTEGER PRIMARY KEY)", NULL, NULL, NULL);
  sqlite3_exec(db->handle, "CREATE TABLE memory.darktable_tags (tagid INTEGER PRIMARY KEY)", NULL, NULL, NULL);
  sqlite3_exec(
      db->handle,
      "CREATE TABLE memory.history (imgid INTEGER, num INTEGER, module INTEGER, "
      "operation VARCHAR(256), op_params BLOB, enabled INTEGER, "
      "blendop_params BLOB, blendop_version INTEGER, multi_priority INTEGER, multi_name VARCHAR(256), multi_name_hand_edited INTEGER, CONSTRAINT opprio UNIQUE (operation, multi_priority))",
      NULL, NULL, NULL);
  sqlite3_exec(
      db->handle,
      "CREATE TABLE memory.snapshot_history (id INTEGER, imgid INTEGER, num INTEGER, module INTEGER, "
      "operation VARCHAR(256), op_params BLOB, enabled INTEGER, "
      "blendop_params BLOB, blendop_version INTEGER, multi_priority INTEGER, multi_name VARCHAR(256), multi_name_hand_edited INTEGER)",
      NULL, NULL, NULL);
  sqlite3_exec(
      db->handle,
      "CREATE TABLE memory.snapshot_masks_history (id INTEGER, imgid INTEGER, num INTEGER, formid INTEGER,"
      " form INTEGER, name VARCHAR(256), version INTEGER, points BLOB, points_count INTEGER, source BLOB)",
      NULL, NULL, NULL);
  sqlite3_exec(
      db->handle,
      "CREATE TABLE memory.snapshot_module_order (id INTEGER, imgid INTEGER, version INTEGER, iop_list VARCHAR)",
      NULL, NULL, NULL);
  sqlite3_exec(db->handle,
      "CREATE TABLE memory.darktable_iop_names (operation VARCHAR(256) PRIMARY KEY, name VARCHAR(256))",
      NULL, NULL, NULL);
  sqlite3_exec(db->handle,
      "CREATE TABLE memory.film_folder (id INTEGER PRIMARY KEY, status INTEGER)",
      NULL, NULL, NULL);
    // clang-format on
}

static void _sanitize_db(dt_database_t *db)
{
    sqlite3_stmt *stmt, *innerstmt;
    // clang-format off
  /* first let's get rid of non-utf8 tags. */
  sqlite3_prepare_v2(db->handle, "SELECT id, name FROM data.tags", -1, &stmt, NULL);
  sqlite3_prepare_v2(db->handle, "UPDATE data.tags SET name = ?1 WHERE id = ?2", -1, &innerstmt, NULL);
    // clang-format on
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const int id = sqlite3_column_int(stmt, 0);
        const char *tag = (const char *)sqlite3_column_text(stmt, 1);

        if (tag && !g_utf8_validate(tag, -1, NULL))
        {
            gchar *new_tag = dt_util_foo_to_utf8(tag);
            dt_print(DT_DEBUG_ALWAYS, "[init]: tag `%s' is not valid utf8, replacing it with `%s'",
                     tag, new_tag);
            sqlite3_bind_text(innerstmt, 1, new_tag, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(innerstmt, 2, id);
            sqlite3_step(innerstmt);
            sqlite3_reset(innerstmt);
            sqlite3_clear_bindings(innerstmt);
            g_free(new_tag);
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_finalize(innerstmt);
    // clang-format off
  // make sure film_roll folders don't end in "/", that will result in empty entries in the collect module
  sqlite3_exec(db->handle,
               "UPDATE main.film_rolls SET folder = substr(folder, 1, length(folder) - 1) WHERE folder LIKE '%/'",
               NULL, NULL, NULL);
    // clang-format on
}

// in library we keep the names of the tags used in tagged_images. however, using that table at runtime results
// in some overhead not necessary so instead we just use the used_tags table to update tagged_images on startup
#define TRY_EXEC(_query, _message)                                                                 \
    do                                                                                             \
    {                                                                                              \
        if (sqlite3_exec(db->handle, _query, NULL, NULL, NULL) != SQLITE_OK)                       \
        {                                                                                          \
            dt_print(DT_DEBUG_ALWAYS, "TRY_EXEC: %s, sql: %s", _message,                           \
                     sqlite3_errmsg(db->handle));                                                  \
            FINALIZE;                                                                              \
            sqlite3_exec(db->handle, "ROLLBACK TRANSACTION", NULL, NULL, NULL);                    \
            return FALSE;                                                                          \
        }                                                                                          \
    } while (0)

#define TRY_STEP(_stmt, _expected, _message)                                                       \
    do                                                                                             \
    {                                                                                              \
        if (sqlite3_step(_stmt) != _expected)                                                      \
        {                                                                                          \
            dt_print(DT_DEBUG_ALWAYS, "TRY_STEP: %s, sql: %s", _message,                           \
                     sqlite3_errmsg(db->handle));                                                  \
            FINALIZE;                                                                              \
            sqlite3_exec(db->handle, "ROLLBACK TRANSACTION", NULL, NULL, NULL);                    \
            return FALSE;                                                                          \
        }                                                                                          \
    } while (0)

#define TRY_PREPARE(_stmt, _query, _message)                                                       \
    do                                                                                             \
    {                                                                                              \
        if (sqlite3_prepare_v2(db->handle, _query, -1, &_stmt, NULL) != SQLITE_OK)                 \
        {                                                                                          \
            dt_print(DT_DEBUG_ALWAYS, "TRY_PREPARE: %s, sql: %s", _message,                        \
                     sqlite3_errmsg(db->handle));                                                  \
            FINALIZE;                                                                              \
            sqlite3_exec(db->handle, "ROLLBACK TRANSACTION", NULL, NULL, NULL);                    \
            return FALSE;                                                                          \
        }                                                                                          \
    } while (0)

#define FINALIZE                                                                                   \
    do                                                                                             \
    {                                                                                              \
        sqlite3_finalize(stmt);                                                                    \
        stmt = NULL; /* NULL so that finalize becomes a NOP */                                     \
    } while (0)

#undef TRY_EXEC
#undef TRY_STEP
#undef TRY_PREPARE
#undef FINALIZE

void dt_database_show_error(const dt_database_t *db, const char *dblabel)
{
    if (db->application_instance_lock_failed)
    {
        char *label_text = NULL;
        if (db->application_instance_lock_contended)
        {
            char *owner_suffix = db->error_other_pid > 0
                                     ? g_strdup_printf(_(" (current owner process ID %d)"),
                                                       db->error_other_pid)
                                     : g_strdup("");
            label_text = g_markup_printf_escaped(
                _("\n"
                  "  Another darktable instance is already running.\n"
                  "\n"
                  "  DarkTableNext production builds allow one graphical instance per user.\n"
                  "  Close the running instance before opening another one%s.\n"),
                owner_suffix);
            g_free(owner_suffix);
        }
        else
        {
            label_text = g_markup_printf_escaped(
                _("\n"
                  "  DarkTableNext could not acquire its application instance lock.\n"
                  "\n"
                  "  %s\n"),
                db->error_message ? db->error_message : _("unknown lock error"));
        }
        dt_gui_show_standalone_yes_no_dialog(_("darktable is already running"), label_text,
                                             _("_ok"), NULL);
        g_free(label_text);
    }
    else if (!db->lock_acquired)
    {
        char lck_pathname[1024];
        snprintf(lck_pathname, sizeof(lck_pathname), "%s.lock", db->error_dbfilename);
        char *lck_dirname = g_strdup(lck_pathname);
        char *last_dirsep_position = g_strrstr(lck_dirname, G_DIR_SEPARATOR_S);
        char library_lock_basename[1024];
        if (strlen(dblabel) == 0)
            strncpy(library_lock_basename, "library.db.lock", sizeof(library_lock_basename));
        else
            snprintf(library_lock_basename, sizeof(library_lock_basename), "library-%s.db.lock",
                     dblabel);

        if (last_dirsep_position)
            *last_dirsep_position = '\0'; // make lck_dirname contain only the directory name

        // clang-format off
    char *label_text = g_markup_printf_escaped(
        _("\n"
          "  Sorry, darktable could not be started (database is locked)\n"
          "\n"
          "  How to solve this problem?\n"
          "\n"
          "  1 - If another darktable instance is already open, \n"
          "      click cancel and either use that instance or close it before attempting to rerun darktable \n"
          "      (process ID <i><b>%d</b></i> created the database locks)\n"
          "\n"
          "  2 - If you closed darktable within the past few minutes, it may still be running in the background \n"
          "      to export images, update sidecar files, or perform database maintenance. Try again once \n"
          "      this processing finishes.\n"
          "\n"
          "  3 - If you are not confident in your ability to correctly deal with processes in the OS, \n"
          "      it would be safer to restart the session or reboot your computer after some time (few minutes). \n"
          "      This will close all running programs and hopefully close the databases correctly. \n"
          "\n"
          "  4 - If you have done this or are certain that no other instances of darktable are running, \n"
          "      this probably means that the last instance was ended abnormally. \n"
          "      Click on the \"delete database lock files\" button to delete the files <i>data.db.lock</i> and <i>%s</i>. \n"
          "\n\n"
          "      <i><u>Caution!</u> Do not delete these files without first undertaking the above checks, \n"
          "      otherwise you risk generating serious inconsistencies in your database.</i>\n"),
      db->error_other_pid, library_lock_basename);
        // clang-format on

        gboolean delete_lockfiles =
            dt_gui_show_standalone_yes_no_dialog(_("error starting darktable"), label_text,
                                                 _("_cancel"), _("_delete database lock files"));

        if (delete_lockfiles)
        {
            gboolean really_delete_lockfiles = dt_gui_show_standalone_yes_no_dialog(
                _("are you sure?"), _("\ndo you really want to delete the lock files?\n"), _("_no"),
                _("_yes"));
            if (really_delete_lockfiles)
            {
                int status = 0;

                char *lck_filename = g_strconcat(lck_dirname, "/data.db.lock", NULL);
                if (g_access(lck_filename, F_OK) != -1)
                    status += remove(lck_filename);
                g_free(lck_filename);

                lck_filename =
                    g_strconcat(lck_dirname, G_DIR_SEPARATOR_S, library_lock_basename, NULL);
                if (g_access(lck_filename, F_OK) != -1)
                    status += remove(lck_filename);

                if (status == 0)
                    dt_gui_show_standalone_yes_no_dialog(
                        _("done"),
                        _("\nsuccessfully deleted the lock files.\nyou can now restart darktable\n"),
                        _("_ok"), NULL);
                else
                    dt_gui_show_standalone_yes_no_dialog(
                        _("error"),
                        g_markup_printf_escaped(
                            _("\nat least one file could not be deleted.\n"
                              "you may try to manually delete the files <i>data.db.lock</i> and <i>%s</i>\n"
                              "in folder <a href=\"file:///%s\">%s</a>.\n"),
                            lck_filename, lck_dirname, lck_dirname),
                        _("_ok"), NULL);
                g_free(lck_filename);
            }
        }

        g_free(lck_dirname);
        g_free(label_text);
    }

    g_free(db->error_message);
    g_free(db->error_dbfilename);
    ((dt_database_t *)db)->error_other_pid = 0;
    ((dt_database_t *)db)->error_message = NULL;
    ((dt_database_t *)db)->error_dbfilename = NULL;
}

static gboolean pid_is_alive(int pid)
{
    gboolean pid_is_alive;

#ifdef _WIN32
    pid_is_alive = FALSE;
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (h)
    {
        wchar_t wfilename[MAX_PATH];
        long unsigned int n_filename = sizeof(wfilename);
        int ret = QueryFullProcessImageNameW(h, 0, wfilename, &n_filename);
        char *filename = g_utf16_to_utf8(wfilename, -1, NULL, NULL, NULL);
        if (ret && n_filename > 0 && filename && g_str_has_suffix(filename, "darktable.exe"))
            pid_is_alive = TRUE;
        g_free(filename);
        CloseHandle(h);
    }
#else
    pid_is_alive = !((kill(pid, 0) == -1) && errno == ESRCH);

#ifdef __linux__
    // If this is Linux, we can query /proc to see if the pid is
    // actually a darktable instance.
    if (pid_is_alive)
    {
        gchar *contents;
        gsize length;
        gchar filename[64];
        snprintf(filename, sizeof(filename), "/proc/%d/cmdline", pid);

        if (g_file_get_contents("", &contents, &length, NULL))
        {
            if (strstr(contents, "darktable") == NULL)
            {
                pid_is_alive = FALSE;
            }
            g_free(contents);
        }
    }
#endif

#endif

    return pid_is_alive;
}

static gboolean _lock_single_database(dt_database_t *db, const char *dbfilename, char **lockfile)
{
    gboolean lock_acquired = FALSE;
    mode_t old_mode;
    int lock_tries = 0;
    gchar *pid = g_strdup_printf("%d", getpid());

    if (!strcmp(dbfilename, ":memory:"))
    {
        lock_acquired = TRUE;
    }
    else
    {
        *lockfile = g_strconcat(dbfilename, ".lock", NULL);
    lock_again:
        lock_tries++;
        old_mode = umask(0);
        int fd = g_open(*lockfile, O_RDWR | O_CREAT | O_EXCL, 0666);
        umask(old_mode);

        if (fd != -1) // the lockfile was successfully created - write our PID into it
        {
            if (write(fd, pid, strlen(pid) + 1) > -1)
                lock_acquired = TRUE;
            close(fd);
        }
        else // the lockfile already exists - see if it's a stale one left over from a crashed instance
        {
            char buf[64];
            memset(buf, 0, sizeof(buf));
            fd = g_open(*lockfile, O_RDWR | O_CREAT, 0666);
            if (fd != -1)
            {
                const int bytes_read = read(fd, buf, sizeof(buf) - 1);
                if (bytes_read > 0)
                {
                    db->error_other_pid = atoi(buf);
                    if (!pid_is_alive(db->error_other_pid))
                    {
                        // The other process appears to no longer exist. So we can safely
                        // unlink the .lock file and try again, hopefully we will be able
                        // to create a new .lock file and thus take over the lock.
                        close(fd); // close the file, otherwise g_unlink will fail
                        g_unlink(*lockfile);
                        // Let's limit the number of attempts. This avoids a theoretically
                        // possible infinite loop if, after unlinking the lock file, we
                        // cannot create it for some reason and end up here again.
                        // Not that this is a realistically expected situation in practice,
                        // but still...
                        if (lock_tries < 5)
                        {
                            goto lock_again;
                        }
                    }
                    else
                    {
                        dt_print(
                            DT_DEBUG_ALWAYS,
                            "[init] the database lock file contains a pid that seems to be alive in your system: %d",
                            db->error_other_pid);
                        db->error_message = g_strdup_printf(
                            _("the database lock file contains a pid that seems to be alive in your system: %d"),
                            db->error_other_pid);
                        close(fd);
                    }
                }
                else
                {
                    dt_print(DT_DEBUG_ALWAYS, "[init] the database lock file seems to be empty");
                    db->error_message =
                        g_strdup_printf(_("the database lock file seems to be empty"));
                    close(fd);
                }
            }
            else
            {
                int err = errno;
                dt_print(DT_DEBUG_ALWAYS,
                         "[init] error opening the database lock file for reading: %s",
                         strerror(err));
                db->error_message = g_strdup_printf(
                    _("error opening the database lock file for reading: %s"), strerror(err));
            }
        }
    }

    g_free(pid);

    if (db->error_message)
        db->error_dbfilename = g_strdup(dbfilename);

    return lock_acquired;
}

static gboolean _lock_databases(dt_database_t *db)
{
    if (!_lock_single_database(db, db->dbfilename_data, &db->lockfile_data))
        return FALSE;
    if (!_lock_single_database(db, db->dbfilename_library, &db->lockfile_library))
    {
        // unlock data.db to not leave a stale lock file around
        g_unlink(db->lockfile_data);
        return FALSE;
    }
    return TRUE;
}

#if !DT_BUILD_DEVMODE
static void _read_application_instance_owner(dt_database_t *db, const int fd)
{
    char buf[64] = {0};
    if (lseek(fd, 0, SEEK_SET) < 0)
        return;

    const int bytes_read = read(fd, buf, sizeof(buf) - 1);
    if (bytes_read > 0)
        db->error_other_pid = atoi(buf);
}

static gboolean _lock_application_instance(dt_database_t *db)
{
    if (_application_instance_lock_fd >= 0)
        return TRUE;

    gchar *lock_directory =
        g_build_filename(g_get_user_config_dir(), "darktable", "Locks", NULL);
    if (g_mkdir_with_parents(lock_directory, 0700) != 0)
    {
        db->application_instance_lock_failed = TRUE;
        db->error_message =
            g_strdup_printf(_("cannot create application instance lock directory: %s"),
                            strerror(errno));
        g_free(lock_directory);
        return FALSE;
    }

    gchar *lockfile = g_build_filename(lock_directory, "application-instance.lock", NULL);
    g_free(lock_directory);

    const int fd = g_open(lockfile, O_CREAT | O_RDWR, 0600);
    if (fd < 0)
    {
        db->application_instance_lock_failed = TRUE;
        db->error_message = g_strdup_printf(_("cannot open application instance lock: %s"),
                                             strerror(errno));
        g_free(lockfile);
        return FALSE;
    }

    if (_application_instance_lock_acquire(fd) != 0)
    {
        const int lock_errno = errno;
        if (lock_errno == EWOULDBLOCK || lock_errno == EAGAIN)
        {
            _read_application_instance_owner(db, fd);
            db->application_instance_lock_failed = TRUE;
            db->application_instance_lock_contended = TRUE;
        }
        else
        {
            db->application_instance_lock_failed = TRUE;
            db->error_message =
                g_strdup_printf(_("cannot acquire application instance lock: %s"),
                                strerror(lock_errno));
        }
        close(fd);
        g_free(lockfile);
        return FALSE;
    }

    gchar *pid = g_strdup_printf("%d\n", getpid());
    const size_t pid_length = strlen(pid);
    const gboolean wrote_pid = ftruncate(fd, 0) == 0 && lseek(fd, 0, SEEK_SET) >= 0 &&
                                write(fd, pid, pid_length) == (ssize_t)pid_length;
    g_free(pid);
    if (!wrote_pid)
    {
        const int write_errno = errno;
        _application_instance_lock_release(fd);
        close(fd);
        db->application_instance_lock_failed = TRUE;
        db->error_message = g_strdup_printf(_("cannot write application instance lock: %s"),
                                             strerror(write_errno));
        g_free(lockfile);
        return FALSE;
    }

    _application_instance_lock_fd = fd;
    _application_instance_lockfile = lockfile;
    return TRUE;
}
#endif

static gboolean _upgrade_camera_table(const dt_database_t *db)
{
    gboolean res = TRUE;
    sqlite3_stmt *stmt;
    sqlite3_stmt *innerstmt;

    sqlite3_prepare_v2(db->handle,
                       "SELECT mi.id, mk.name, md.name"
                       " FROM main.images AS mi, main.makers AS mk, main.models AS md"
                       " WHERE mi.maker_id = mk.id"
                       "   AND mi.model_id = md.id",
                       -1, &stmt, NULL);

    sqlite3_prepare_v2(db->handle, "UPDATE main.images SET camera_id = ?1 WHERE id = ?2", -1,
                       &innerstmt, NULL);

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const dt_imgid_t imgid = sqlite3_column_int(stmt, 0);
        const char *maker = (const char *)sqlite3_column_text(stmt, 1);
        const char *model = (const char *)sqlite3_column_text(stmt, 2);

        const int32_t camera_id = dt_image_get_camera_id(maker, model);

        sqlite3_bind_int(innerstmt, 1, camera_id);
        sqlite3_bind_int(innerstmt, 2, imgid);
        sqlite3_step(innerstmt);
        sqlite3_reset(innerstmt);
        sqlite3_clear_bindings(innerstmt);
    }

    sqlite3_finalize(stmt);
    sqlite3_finalize(innerstmt);

    return res;
}

static gboolean _database_schema_is_compatible(const gchar *filename,
                                                const dt_database_schema_t schema,
                                                const int stored_version,
                                                const gboolean has_gui)
{
    const int current_version = dt_database_schema_current_version(schema);
    const dt_database_schema_compatibility_t compatibility =
        dt_database_schema_check(schema, stored_version);
    if (compatibility == DT_DATABASE_SCHEMA_COMPATIBLE)
        return TRUE;

    dt_print(DT_DEBUG_ALWAYS,
             "[init] %s database `%s' uses schema version %d; supported schema version is %d",
             dt_database_schema_name(schema), filename, stored_version, current_version);

    if (!has_gui)
        return FALSE;

    char *label_text = g_markup_printf_escaped(
        _("the %s database\n"
          "\n"
          "<span style='italic'>%s</span>\n"
          "\n"
          "uses schema version %d, but this build supports schema version %d.\n"
          "Application releases do not change this requirement; only database schema changes do.\n"),
        dt_database_schema_name(schema), filename, stored_version, current_version);
    dt_gui_show_standalone_yes_no_dialog(_("DarkTableNext - unsupported database"), label_text,
                                         _("_quit darktable"), NULL);
    g_free(label_text);

    return FALSE;
}

static void _database_backup_for_schema(const char *filename,
                                        const dt_database_schema_t schema)
{
    if (!g_file_test(filename, G_FILE_TEST_EXISTS))
        return;

    const int schema_version = dt_database_schema_current_version(schema);
    gchar *backup = g_strdup_printf("%s-pre-schema-%d", filename, schema_version);

    GError *gerror = NULL;
    if (!g_file_test(backup, G_FILE_TEST_EXISTS))
    {
        GFile *src = g_file_new_for_path(filename);
        GFile *dest = g_file_new_for_path(backup);
        const gboolean copy_status =
            g_file_copy(src, dest, G_FILE_COPY_NONE, NULL, NULL, NULL, &gerror);
        if (!copy_status)
            dt_print(DT_DEBUG_ALWAYS, "[backup failed] %s -> %s", filename, backup);

        g_object_unref(src);
        g_object_unref(dest);
        g_clear_error(&gerror);
    }

    g_free(backup);
}

int _get_pragma_int_val(sqlite3 *db, const char *pragma)
{
    gchar *query = g_strdup_printf("PRAGMA %s", pragma);
    int val = -1;
    sqlite3_stmt *stmt;
    const int rc = sqlite3_prepare_v2(db, query, -1, &stmt, NULL);
    if (rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW)
    {
        val = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    g_free(query);

    return val;
}

gchar *_get_pragma_string_val(sqlite3 *db, const char *pragma)
{
    gchar *query = g_strdup_printf("PRAGMA %s", pragma);
    sqlite3_stmt *stmt;
    gchar *val = NULL;
    const int rc = sqlite3_prepare_v2(db, query, -1, &stmt, NULL);
    if (rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW)
    {
        val = g_strdup((const char *)sqlite3_column_text(stmt, 0));
        while (sqlite3_step(stmt) == SQLITE_ROW)
        {
            gchar *cur_val = g_strdup((const char *)sqlite3_column_text(stmt, 0));
            gchar *tmp_val = g_strdup(val);
            g_free(val);
            val = g_strconcat(tmp_val, "\n", cur_val, NULL);
            g_free(cur_val);
            g_free(tmp_val);
        }
    }
    sqlite3_finalize(stmt);
    g_free(query);
    return val;
}

dt_database_t *dt_database_init(const char *alternative, const gboolean load_data,
                                const gboolean has_gui)
{
    /*  set the threading mode to Serialized */
    sqlite3_config(SQLITE_CONFIG_SERIALIZED);

    sqlite3_initialize();

start:;
    /* lets construct the db filename  */
    gchar *dbname = NULL;
    gchar *dbfilename_library = NULL;
    gchar datadir[PATH_MAX] = {0};

    dt_loc_get_user_config_dir(datadir, sizeof(datadir));

    if (alternative == NULL)
    {
        dbname = dt_conf_get_string("database");
        if (!dbname)
            dbfilename_library = g_build_filename(datadir, "library.db", NULL);
        else if (!strcmp(dbname, ":memory:"))
            dbfilename_library = g_strdup(dbname);
        else if (dbname[0] != '/')
            dbfilename_library = g_build_filename(datadir, dbname, NULL);
        else
            dbfilename_library = g_strdup(dbname);
    }
    else
    {
        dbfilename_library = g_strdup(alternative);

        GFile *galternative = g_file_new_for_path(alternative);
        dbname = g_file_get_basename(galternative);
        g_object_unref(galternative);
    }

    /* we also need a 2nd db with permanent data like presets, styles and tags */
    gchar *dbfilename_data = NULL;
    if (load_data)
        dbfilename_data = g_build_filename(datadir, "data.db", NULL);
    else
        dbfilename_data = g_strdup(":memory:");

    // It may happen that we will not have write access to the database restored
    // from a backup or snapshot. Running darktable with a database that cannot
    // be written to may result in incorrect operation, the cause of which will
    // be difficult to diagnose. Let's check if we can continue.
    if ((access(dbfilename_library, F_OK) == 0 && access(dbfilename_library, W_OK) != 0) ||
        (access(dbfilename_data, F_OK) == 0 && access(dbfilename_data, W_OK) != 0))
    {
        dt_print(DT_DEBUG_ALWAYS, "at least one of the dt databases (%s, %s) is not writeable",
                 dbfilename_library, dbfilename_data);
        if (has_gui)
        {
            char *readonly_db_text = g_markup_printf_escaped(
                _("you do not have write access to at least one of the darktable databases:\n"
                  "\n"
                  "<span style='italic'>%s</span>\n"
                  "<span style='italic'>%s</span>\n"
                  "\n"
                  "please fix this and then run darktable again"),
                dbfilename_library, dbfilename_data);
            dt_gui_show_standalone_yes_no_dialog(_("darktable - read-only database detected"),
                                                 readonly_db_text, _("_quit darktable"), NULL);
            // There is no REAL need to free the string before exiting, but we do it
            // to avoid creating a code pattern that could be mistakenly copy-pasted
            // somewhere else where freeing memory would actually be needed.
            g_free(readonly_db_text);
        }
        exit(1);
    }

    /* create database */
    dt_database_t *db = g_malloc0(sizeof(dt_database_t));
    db->dbfilename_data = g_strdup(dbfilename_data);
    db->dbfilename_library = g_strdup(dbfilename_library);

    dt_atomic_set_int(&_trxid, 0);

#if !DT_BUILD_DEVMODE
    if (!_lock_application_instance(db))
    {
        dt_print(DT_DEBUG_ALWAYS,
                 "[init] application instance lock is unavailable; aborting before database access");
        g_free(dbname);
        g_free(dbfilename_data);
        g_free(dbfilename_library);
        return db;
    }
#endif

    /* make sure the folder exists. this might not be the case for new databases */
    /* also check if a database backup is needed */
    if (g_strcmp0(dbfilename_data, ":memory:"))
    {
        char *data_path = g_path_get_dirname(dbfilename_data);
        g_mkdir_with_parents(data_path, 0750);
        g_free(data_path);
        _database_backup_for_schema(dbfilename_data, DT_DATABASE_SCHEMA_DATA);
    }
    if (g_strcmp0(dbfilename_library, ":memory:"))
    {
        char *library_path = g_path_get_dirname(dbfilename_library);
        g_mkdir_with_parents(library_path, 0750);
        g_free(library_path);
        _database_backup_for_schema(dbfilename_library, DT_DATABASE_SCHEMA_LIBRARY);
    }

    dt_print(DT_DEBUG_SQL, "[init sql] library: %s, data: %s", dbfilename_library, dbfilename_data);

    /* having more than one instance of darktable using the same database is a bad idea */
    /* try to get locks for the databases */
    db->lock_acquired = _lock_databases(db);

    if (!db->lock_acquired)
    {
        dt_print(DT_DEBUG_ALWAYS,
                 "[init] database is locked, probably another process is already using it");
        g_free(dbname);
        g_free(dbfilename_data);
        g_free(dbfilename_library);
        return db;
    }

    /* opening / creating database */
    if (sqlite3_open(db->dbfilename_library, &db->handle) != SQLITE_OK)
    {
        dt_print(DT_DEBUG_ALWAYS, "[init] could not find database %s%s%s", dbname ? " `" : "",
                 dbname ? dbname : "", dbname ? "'!" : "");
        dt_print(DT_DEBUG_ALWAYS, "[init] maybe your %s/darktablerc is corrupt?", datadir);
        char packagedatadir[PATH_MAX] = {0};
        dt_loc_get_datadir(packagedatadir, sizeof(packagedatadir));
        dt_print(DT_DEBUG_ALWAYS, "[init] try `cp %s/darktablerc %s/darktablerc'", packagedatadir,
                 datadir);
        sqlite3_close(db->handle);
        g_free(dbname);
        g_free(db->lockfile_data);
        g_free(db->dbfilename_data);
        g_free(db->lockfile_library);
        g_free(db->dbfilename_library);
        g_free(db);
        g_free(dbfilename_data);
        g_free(dbfilename_library);
        return NULL;
    }

    /* attach a memory database to db connection for use with temporary tables
     used during instance life time, which is discarded on exit.
  */
    sqlite3_exec(db->handle, "attach database ':memory:' as memory", NULL, NULL, NULL);

    // attach the data database which contains presets, styles, tags and similar things not tied to single images
    sqlite3_stmt *stmt;
    gboolean have_data_db = load_data && g_file_test(dbfilename_data, G_FILE_TEST_EXISTS);
    int rc = sqlite3_prepare_v2(db->handle, "ATTACH DATABASE ?1 AS data", -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, dbfilename_data, -1, SQLITE_TRANSIENT);
    if (rc != SQLITE_OK || sqlite3_step(stmt) != SQLITE_DONE)
    {
        sqlite3_finalize(stmt);
        dt_print(DT_DEBUG_ALWAYS, "[init] database `%s' couldn't be opened. aborting",
                 dbfilename_data);
        dt_database_destroy(db);
        db = NULL;
        goto error;
    }
    sqlite3_finalize(stmt);

    // some sqlite3 config
    sqlite3_exec(db->handle, "PRAGMA synchronous = OFF", NULL, NULL, NULL);
    sqlite3_exec(db->handle, "PRAGMA journal_mode = MEMORY", NULL, NULL, NULL);
    sqlite3_exec(db->handle, "PRAGMA page_size = 32768", NULL, NULL, NULL);

    // WARNING: the foreign_keys pragma must not be used, the integrity of the
    // database rely on it.
    sqlite3_exec(db->handle, "PRAGMA foreign_keys = ON", NULL, NULL, NULL);

    /* now that we got functional databases that are locked for us we can make sure that the schema is set up */

    // first we update the data database to the latest version so that we can potentially move data from the library
    // over when updating that one
    if (!have_data_db)
    {
        _create_data_schema(db); // a brand new db it seems
    }
    else
    {
        gchar *data_status = _get_pragma_string_val(db->handle, "data.quick_check");
        rc = sqlite3_prepare_v2(db->handle, "SELECT value FROM data.db_info WHERE key = 'version'",
                                -1, &stmt, NULL);
        if (!g_strcmp0(data_status, "ok") && rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW)
        {
            g_free(data_status); // status is OK and we don't need to care :)
            // Schema compatibility is independent from the application version.
            const int db_version = sqlite3_column_int(stmt, 0);
            sqlite3_finalize(stmt);
            if (!_database_schema_is_compatible(dbfilename_data, DT_DATABASE_SCHEMA_DATA,
                                                db_version, has_gui))
            {
                dt_database_destroy(db);
                db = NULL;
                goto error;
            }
        }
        else
        {
            // oh, bad situation. the database is corrupt and can't be read!
            // we inform the user here and let him decide what to do: exit or delete and try again.

            gchar *quick_check_text = NULL;
            if (g_strcmp0(data_status, "ok")) // data_status is not ok
            {
                quick_check_text = g_strdup_printf(_("quick_check said:\n"
                                                     "%s \n"),
                                                   data_status);
            }
            else
            {
                quick_check_text = g_strdup(""); // a trick;
            }

            gchar *data_snap = dt_database_get_most_recent_snap(dbfilename_data);

            GtkWidget *dialog;
            GtkDialogFlags dflags = GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT;

            const char *label_options = NULL;

            if (data_snap)
            {
                dialog = gtk_dialog_new_with_buttons(
                    _("darktable - error opening database"), NULL, dflags, _("_close darktable"),
                    GTK_RESPONSE_CLOSE, _("_attempt restore"), GTK_RESPONSE_ACCEPT,
                    _("_delete database"), GTK_RESPONSE_REJECT, NULL);
                gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
                label_options = _("do you want to close darktable now to manually restore\n"
                                  "the database from a backup, attempt an automatic restore\n"
                                  "from the most recent snapshot or delete the corrupted database\n"
                                  "and start with a new one?");
            }
            else
            {
                dialog = gtk_dialog_new_with_buttons(
                    _("darktable - error opening database"), NULL, dflags, _("_close darktable"),
                    GTK_RESPONSE_CLOSE, _("_delete database"), GTK_RESPONSE_REJECT, NULL);
                gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CLOSE);
                label_options = _("do you want to close darktable now to manually restore\n"
                                  "the database from a backup or delete the corrupted database\n"
                                  "and start with a new one?");
            }

            char *label_text = g_markup_printf_escaped(
                _("an error has occurred while trying to open the database from\n"
                  "\n"
                  "<span style='italic'>%s</span>\n"
                  "\n"
                  "it seems that the database is corrupted.\n"
                  "%s"
                  "%s"),
                dbfilename_data, quick_check_text, label_options);

            g_free(quick_check_text);
            g_free(data_status);

            GtkWidget *label = gtk_label_new(NULL);
            gtk_label_set_markup(GTK_LABEL(label), label_text);
            g_free(label_text);
            dt_gui_dialog_add(GTK_DIALOG(dialog), label);

            gtk_widget_show_all(dialog);

            const int resp = gtk_dialog_run(GTK_DIALOG(dialog));

            gtk_widget_destroy(dialog);

            dt_database_destroy(db);
            db = NULL;

            if (resp != GTK_RESPONSE_ACCEPT && resp != GTK_RESPONSE_REJECT)
            {
                dt_print(
                    DT_DEBUG_ALWAYS,
                    "[init] database `%s' is corrupt and can't be opened! either replace it from a backup or "
                    "delete the file so that darktable can create a new one the next time. aborting",
                    dbfilename_data);
                g_free(data_snap);
                goto error;
            }

            //here were sure that response is either accept (restore from snap) or reject (just delete the damaged db)
            dt_print(DT_DEBUG_ALWAYS, "[init] deleting `%s' on user request: %s", dbfilename_data,
                     g_unlink(dbfilename_data) == 0 ? "ok" : "failed");

            if (resp == GTK_RESPONSE_ACCEPT && data_snap)
            {
                GError *gerror = NULL;
                if (!g_file_test(dbfilename_data, G_FILE_TEST_EXISTS))
                {
                    GFile *src = g_file_new_for_path(data_snap);
                    GFile *dest = g_file_new_for_path(dbfilename_data);
                    gboolean copy_status = TRUE;
                    if (g_file_test(data_snap, G_FILE_TEST_EXISTS))
                    {
                        copy_status =
                            g_file_copy(src, dest, G_FILE_COPY_NONE, NULL, NULL, NULL, &gerror);
                        if (copy_status)
                            copy_status = g_chmod(dbfilename_data,
                                                  S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH) == 0;
                    }
                    else
                    {
                        // there is nothing to restore, create an empty file
                        const int fd =
                            g_open(dbfilename_data, O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
                        if (fd < 0 || !g_close(fd, &gerror))
                            copy_status = FALSE;
                    }
                    dt_print(DT_DEBUG_ALWAYS, "[init] restoring `%s' from `%s' :%s",
                             dbfilename_data, data_snap, copy_status ? "success" : "failed!");
                    g_object_unref(src);
                    g_object_unref(dest);
                }
            }
            g_free(data_snap);
            g_free(dbname);
            g_free(dbfilename_data);
            g_free(dbfilename_library);
            goto start;
        }
    }

    gchar *libdb_status = _get_pragma_string_val(db->handle, "main.quick_check");
    // next we are looking at the library database
    // does the db contain the new 'db_info' table?
    rc = sqlite3_prepare_v2(db->handle, "SELECT value FROM main.db_info WHERE key = 'version'", -1,
                            &stmt, NULL);
    if (!g_strcmp0(libdb_status, "ok") && rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW)
    {
        g_free(libdb_status); //it's ok :)

        // Schema compatibility is independent from the application version.
        const int db_version = sqlite3_column_int(stmt, 0);

        sqlite3_finalize(stmt);
        if (!_database_schema_is_compatible(dbfilename_library, DT_DATABASE_SCHEMA_LIBRARY,
                                            db_version, has_gui))
        {
            dt_database_destroy(db);
            db = NULL;
            goto error;
        }
    }
    else if (g_strcmp0(libdb_status, "ok") || rc == SQLITE_CORRUPT || rc == SQLITE_NOTADB)
    {
        // oh, bad situation. the database is corrupt and can't be read!
        // we inform the user here and let him decide what to do: exit or delete and try again.

        gchar *quick_check_text = NULL;
        if (g_strcmp0(libdb_status, "ok")) // data_status is not ok
        {
            quick_check_text = g_strdup_printf(_("quick_check said:\n"
                                                 "%s \n"),
                                               libdb_status);
        }
        else
        {
            quick_check_text = g_strdup(""); // a trick;
        }

        gchar *data_snap = dt_database_get_most_recent_snap(dbfilename_library);

        GtkWidget *dialog;
        GtkDialogFlags dflags = GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT;

        const char *label_options = NULL;

        if (data_snap)
        {
            dialog = gtk_dialog_new_with_buttons(_("darktable - error opening database"), NULL,
                                                 dflags, _("_close darktable"), GTK_RESPONSE_CLOSE,
                                                 _("_attempt restore"), GTK_RESPONSE_ACCEPT,
                                                 _("_delete database"), GTK_RESPONSE_REJECT, NULL);
            gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
            label_options = _("do you want to close darktable now to manually restore\n"
                              "the database from a backup, attempt an automatic restore\n"
                              "from the most recent snapshot or delete the corrupted database\n"
                              "and start with a new one?");
        }
        else
        {
            dialog = gtk_dialog_new_with_buttons(_("darktable - error opening database"), NULL,
                                                 dflags, _("_close darktable"), GTK_RESPONSE_CLOSE,
                                                 _("_delete database"), GTK_RESPONSE_REJECT, NULL);
            gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CLOSE);
            label_options = _("do you want to close darktable now to manually restore\n"
                              "the database from a backup or delete the corrupted database\n"
                              "and start with a new one?");
        }

        char *label_text = g_markup_printf_escaped(
            _("an error has occurred while trying to open the database from\n"
              "\n"
              "<span style='italic'>%s</span>\n"
              "\n"
              "it seems that the database is corrupted.\n"
              "%s"
              "%s"),
            dbfilename_data, quick_check_text, label_options);

        g_free(quick_check_text);
        g_free(libdb_status);

        GtkWidget *label = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(label), label_text);
        g_free(label_text);
        dt_gui_dialog_add(GTK_DIALOG(dialog), label);

        gtk_widget_show_all(dialog);

        const int resp = gtk_dialog_run(GTK_DIALOG(dialog));

        gtk_widget_destroy(dialog);

        dt_database_destroy(db);
        db = NULL;

        if (resp != GTK_RESPONSE_ACCEPT && resp != GTK_RESPONSE_REJECT)
        {
            dt_print(
                DT_DEBUG_ALWAYS,
                "[init] database `%s' is corrupt and can't be opened! either replace it from a backup or "
                "delete the file so that darktable can create a new one the next time. aborting",
                dbfilename_library);
            g_free(data_snap);
            goto error;
        }

        //here were sure that response is either accept (restore from snap) or reject (just delete the damaged db)

        dt_print(DT_DEBUG_ALWAYS, "[init] deleting `%s' on user request ...%s", dbfilename_library,
                 g_unlink(dbfilename_library) == 0 ? "OK" : "failed");

        if (resp == GTK_RESPONSE_ACCEPT && data_snap)
        {
            GError *gerror = NULL;
            if (!g_file_test(dbfilename_library, G_FILE_TEST_EXISTS))
            {
                GFile *src = g_file_new_for_path(data_snap);
                GFile *dest = g_file_new_for_path(dbfilename_library);
                gboolean copy_status = TRUE;
                if (g_file_test(data_snap, G_FILE_TEST_EXISTS))
                {
                    copy_status =
                        g_file_copy(src, dest, G_FILE_COPY_NONE, NULL, NULL, NULL, &gerror);
                    if (copy_status)
                        copy_status =
                            g_chmod(dbfilename_library, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH) == 0;
                }
                else
                {
                    // there is nothing to restore, create an empty file to prevent further backup attempts
                    const int fd =
                        g_open(dbfilename_library, O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
                    if (fd < 0 || !g_close(fd, &gerror))
                        copy_status = FALSE;
                }
                dt_print(DT_DEBUG_ALWAYS, "[init] restoring `%s' from `%s'... %s",
                         dbfilename_library, data_snap, copy_status ? "success" : "failed");
                g_object_unref(src);
                g_object_unref(dest);
            }
        }
        g_free(data_snap);
        g_free(dbname);
        g_free(dbfilename_data);
        g_free(dbfilename_library);
        goto start;
    }
    else
    {
        // A legacy database without explicit schema metadata is not imported.
        sqlite3_finalize(stmt);
        rc = sqlite3_prepare_v2(db->handle, "SELECT settings FROM main.settings", -1, &stmt, NULL);
        if (rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW)
        {
            sqlite3_finalize(stmt);
            _database_schema_is_compatible(dbfilename_library, DT_DATABASE_SCHEMA_LIBRARY, 0,
                                           has_gui);
            dt_print(DT_DEBUG_ALWAYS,
                     "[init] database `%s' has no supported schema version. aborting", dbname);
            dt_database_destroy(db);
            db = NULL;
            goto error;
        }
        else
        {
            sqlite3_finalize(stmt);
            _create_library_schema(db); // a brand new db it seems
        }
    }

    // create the in-memory tables
    _create_memory_schema(db);

    // drop table settings -- we don't want old versions of dt to drop our tables
    sqlite3_exec(db->handle, "DROP TABLE main.settings", NULL, NULL, NULL);

    // take care of potential bad data in the db.
    _sanitize_db(db);

    // check if sqlite is already icu enabled
    // if not enabled expected error: no such function:icu_load_collation
    rc = sqlite3_prepare_v2(db->handle, "SELECT icu_load_collation('en_US', 'english')", -1, &stmt,
                            NULL);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_OK)
    {
        rc = sqlite3IcuInit(db->handle);
        if (rc != SQLITE_OK)
            dt_print(DT_DEBUG_ALWAYS, "[sqlite] init icu extension error %d", rc);
    }
error:
    g_free(dbname);
    g_free(dbfilename_data);
    g_free(dbfilename_library);

    return db;
}

void dt_upgrade_maker_model(const dt_database_t *db)
{
    sqlite3_stmt *stmt = NULL;
    int mapping_version = 0;

    if (sqlite3_prepare_v2(db->handle,
                           "SELECT value"
                           " FROM main.db_info"
                           " WHERE key = 'camera_mapping_version'",
                           -1, &stmt, NULL) != SQLITE_OK)
    {
        dt_print(DT_DEBUG_ALWAYS, "[init] can't read camera mapping version: %s",
                 sqlite3_errmsg(db->handle));
        return;
    }

    if (sqlite3_step(stmt) == SQLITE_ROW)
        mapping_version = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    // This maintenance epoch changes only when camera-id reconciliation does,
    // never merely because the application version changed.
    if (mapping_version >= DT_DATABASE_CAMERA_MAPPING_VERSION)
        return;

    if (!_upgrade_camera_table(db))
        return;

    if (sqlite3_prepare_v2(db->handle,
                           "INSERT OR REPLACE"
                           " INTO main.db_info (key, value)"
                           " VALUES ('camera_mapping_version', ?1)",
                           -1, &stmt, NULL) != SQLITE_OK)
    {
        dt_print(DT_DEBUG_ALWAYS, "[init] can't store camera mapping version: %s",
                 sqlite3_errmsg(db->handle));
        return;
    }

    sqlite3_bind_int(stmt, 1, DT_DATABASE_CAMERA_MAPPING_VERSION);
    if (sqlite3_step(stmt) != SQLITE_DONE)
        dt_print(DT_DEBUG_ALWAYS, "[init] can't update camera mapping version: %s",
                 sqlite3_errmsg(db->handle));
    sqlite3_finalize(stmt);
}

void dt_database_destroy(const dt_database_t *db)
{
    if (db->handle)
        sqlite3_close(db->handle);
    if (db->lockfile_data)
    {
        g_unlink(db->lockfile_data);
        g_free(db->lockfile_data);
    }
    if (db->lockfile_library)
    {
        g_unlink(db->lockfile_library);
        g_free(db->lockfile_library);
    }
    g_free(db->dbfilename_data);
    g_free(db->dbfilename_library);
    g_free((dt_database_t *)db);

    sqlite3_shutdown();
}

void dt_database_release_application_instance_lock()
{
#if !DT_BUILD_DEVMODE
    if (_application_instance_lock_fd >= 0)
    {
        _application_instance_lock_release(_application_instance_lock_fd);
        close(_application_instance_lock_fd);
        _application_instance_lock_fd = -1;
    }
    g_clear_pointer(&_application_instance_lockfile, g_free);
#endif
}

sqlite3 *dt_database_get(const dt_database_t *db)
{
    return db ? db->handle : NULL;
}

const gchar *dt_database_get_path(const dt_database_t *db)
{
    return db->dbfilename_library;
}

gboolean dt_database_get_lock_acquired(const dt_database_t *db)
{
    return db->lock_acquired;
}

void dt_database_cleanup_busy_statements(const dt_database_t *db)
{
    sqlite3_stmt *stmt = NULL;
    while ((stmt = sqlite3_next_stmt(db->handle, NULL)) != NULL)
    {
        const char *sql = sqlite3_sql(stmt);
        if (sqlite3_stmt_busy(stmt))
        {
            dt_print(DT_DEBUG_SQL,
                     "[db busy stmt] non-finalized nor stepped through statement: '%s'", sql);
            sqlite3_reset(stmt);
        }
        else
        {
            dt_print(DT_DEBUG_SQL, "[db busy stmt] non-finalized statement: '%s'", sql);
        }
        sqlite3_finalize(stmt);
    }
}

#define ERRCHECK                                                                                   \
    {                                                                                              \
        if (err != NULL)                                                                           \
        {                                                                                          \
            dt_print(DT_DEBUG_SQL, "[db maintenance] maintenance error: '%s'", err);               \
            sqlite3_free(err);                                                                     \
            err = NULL;                                                                            \
        }                                                                                          \
    }

void dt_database_perform_maintenance(const dt_database_t *db)
{
    char *err = NULL;

    const int main_pre_free_count = _get_pragma_int_val(db->handle, "main.freelist_count");
    const int main_page_size = _get_pragma_int_val(db->handle, "main.page_size");
    const int data_pre_free_count = _get_pragma_int_val(db->handle, "data.freelist_count");
    const int data_page_size = _get_pragma_int_val(db->handle, "data.page_size");

    const guint64 calc_pre_size =
        (main_pre_free_count * main_page_size) + (data_pre_free_count * data_page_size);

    if (calc_pre_size == 0)
    {
        dt_print(DT_DEBUG_SQL,
                 "[db maintenance] maintenance deemed unnecessary, performing only analyze");
        DT_DEBUG_SQLITE3_EXEC(db->handle, "ANALYZE data", NULL, NULL, &err);
        ERRCHECK
        DT_DEBUG_SQLITE3_EXEC(db->handle, "ANALYZE main", NULL, NULL, &err);
        ERRCHECK
        DT_DEBUG_SQLITE3_EXEC(db->handle, "ANALYZE", NULL, NULL, &err);
        ERRCHECK
        return;
    }

    DT_DEBUG_SQLITE3_EXEC(db->handle, "VACUUM data", NULL, NULL, &err);
    ERRCHECK
    DT_DEBUG_SQLITE3_EXEC(db->handle, "VACUUM main", NULL, NULL, &err);
    ERRCHECK
    DT_DEBUG_SQLITE3_EXEC(db->handle, "ANALYZE data", NULL, NULL, &err);
    ERRCHECK
    DT_DEBUG_SQLITE3_EXEC(db->handle, "ANALYZE main", NULL, NULL, &err);
    ERRCHECK

    // for some reason this is needed in some cases
    // in case above performed vacuum+analyze properly, this is noop.
    DT_DEBUG_SQLITE3_EXEC(db->handle, "VACUUM", NULL, NULL, &err);
    ERRCHECK
    DT_DEBUG_SQLITE3_EXEC(db->handle, "ANALYZE", NULL, NULL, &err);
    ERRCHECK

    const int main_post_free_count = _get_pragma_int_val(db->handle, "main.freelist_count");
    const int data_post_free_count = _get_pragma_int_val(db->handle, "data.freelist_count");

    const guint64 calc_post_size =
        (main_post_free_count * main_page_size) + (data_post_free_count * data_page_size);
    const gint64 bytes_freed = calc_pre_size - calc_post_size;

    dt_print(DT_DEBUG_SQL, "[db maintenance] maintenance done, %" G_GINT64_FORMAT " bytes freed",
             bytes_freed);

    if (calc_post_size >= calc_pre_size)
    {
        dt_print(
            DT_DEBUG_SQL,
            "[db maintenance] maintenance problem. if no errors logged, it should work fine next time");
    }
}
#undef ERRCHECK

static inline gboolean _is_mem_db(const dt_database_t *db)
{
    return !g_strcmp0(db->dbfilename_data, ":memory:") ||
           !g_strcmp0(db->dbfilename_library, ":memory:");
}

gboolean dt_database_maybe_maintenance(const dt_database_t *db)
{
    if (_is_mem_db(db))
        return FALSE;

    // checking free pages
    const int main_free_count = _get_pragma_int_val(db->handle, "main.freelist_count");
    const int main_page_count = _get_pragma_int_val(db->handle, "main.page_count");
    const int main_page_size = _get_pragma_int_val(db->handle, "main.page_size");

    const int data_free_count = _get_pragma_int_val(db->handle, "data.freelist_count");
    const int data_page_count = _get_pragma_int_val(db->handle, "data.page_count");
    const int data_page_size = _get_pragma_int_val(db->handle, "data.page_size");

    dt_print(DT_DEBUG_SQL, "[db maintenance] main: [%d/%d pages], data: [%d/%d pages]",
             main_free_count, main_page_count, data_free_count, data_page_count);

    if (main_page_count <= 0 || data_page_count <= 0)
    {
        //something's wrong with PRAGMA page_size returns. early bail.
        dt_print(DT_DEBUG_SQL,
                 "[db maintenance] page_count <= 0 : main.page_count: %d, data.page_count: %d",
                 main_page_count, data_page_count);
        return FALSE;
    }

    // we don't need fine-grained percentages, so let's do ints
    const int main_free_percentage = (main_free_count * 100) / main_page_count;
    const int data_free_percentage = (data_free_count * 100) / data_page_count;

    const int freepage_ratio = dt_conf_get_int("database/maintenance_freepage_ratio");

    if ((main_free_percentage >= freepage_ratio) || (data_free_percentage >= freepage_ratio))
    {
        const guint64 calc_size =
            (main_free_count * main_page_size) + (data_free_count * data_page_size);
        dt_print(DT_DEBUG_SQL, "[db maintenance] maintenance, %" G_GUINT64_FORMAT " bytes to free",
                 calc_size);
        return TRUE;
    }

    return FALSE;
}

void dt_database_optimize(const dt_database_t *db)
{
    if (_is_mem_db(db))
        return;
    // optimize should in most cases be no-op and have no noticeable downsides
    // this should be ran on every exit
    // see: https://www.sqlite.org/pragma.html#pragma_optimize
    DT_DEBUG_SQLITE3_EXEC(db->handle, "PRAGMA optimize", NULL, NULL, NULL);
}

static void _print_backup_progress(int remaining, int total)
{
    // TODO if we have closing splashpage - this can be used to advance progressbar :)
    dt_print(DT_DEBUG_SQL, "[db backup] %d out of %d done", total - remaining, total);
}

static int _backup_db(sqlite3 *src_db,            // Database handle to back up
                      const char *src_db_name,    // Database name to back up
                      const char *dest_filename,  // Name of file to back up to
                      void (*xProgress)(int, int) // Progress function to invoke
)
{
    sqlite3 *dest_db; // Database connection opened on dest_filename

    // Open the database file identified by dest_filename
    int rc = sqlite3_open(dest_filename, &dest_db);

    if (rc == SQLITE_OK)
    {
        // Open the sqlite3_backup object used to accomplish the transfer
        sqlite3_backup *sb_dest = sqlite3_backup_init(dest_db, "main", src_db, src_db_name);
        if (sb_dest)
        {
            dt_print(DT_DEBUG_SQL, "[db backup] %s to %s", src_db_name, dest_filename);
            gchar *pragma = g_strdup_printf("%s.page_count", src_db_name);
            const int spc = _get_pragma_int_val(src_db, pragma);
            g_free(pragma);
            const int pc = MIN(spc, MAX(5, spc / 100));
            do
            {
                rc = sqlite3_backup_step(sb_dest, pc);
                if (xProgress)
                    xProgress(sqlite3_backup_remaining(sb_dest), sqlite3_backup_pagecount(sb_dest));
                if (rc == SQLITE_OK || rc == SQLITE_BUSY || rc == SQLITE_LOCKED)
                {
                    sqlite3_sleep(25);
                }
            } while (rc == SQLITE_OK || rc == SQLITE_BUSY || rc == SQLITE_LOCKED);

            // Release resources allocated by backup_init()
            (void)sqlite3_backup_finish(sb_dest);
        }
        rc = sqlite3_errcode(dest_db);
    }
    // Close the database connection opened on database file dest_filename
    // and return the result of this function
    (void)sqlite3_close(dest_db);
    return rc;
}

gboolean dt_database_snapshot(const dt_database_t *db)
{
    // backing up memory db is pointelss
    if (_is_mem_db(db))
        return FALSE;
    GDateTime *date_now = g_date_time_new_now_local();
    gchar *date_suffix = g_date_time_format(date_now, "%Y%m%d%H%M%S");
    g_date_time_unref(date_now);

    const char *file_pattern = "%s-snp-%s";
    const char *temp_pattern = "%s-tmp-%s";

    gchar *lib_backup_file = g_strdup_printf(file_pattern, db->dbfilename_library, date_suffix);
    gchar *lib_tmpbackup_file = g_strdup_printf(temp_pattern, db->dbfilename_library, date_suffix);

    int rc = _backup_db(db->handle, "main", lib_tmpbackup_file, _print_backup_progress);
    if (rc != SQLITE_OK)
    {
        g_unlink(lib_tmpbackup_file);
        g_free(lib_tmpbackup_file);
        g_free(lib_backup_file);
        g_free(date_suffix);
        return FALSE;
    }
    g_rename(lib_tmpbackup_file, lib_backup_file);
    g_free(lib_tmpbackup_file);
    g_free(lib_backup_file);

    gchar *dat_backup_file = g_strdup_printf(file_pattern, db->dbfilename_data, date_suffix);
    gchar *dat_tmpbackup_file = g_strdup_printf(temp_pattern, db->dbfilename_data, date_suffix);

    g_free(date_suffix);

    rc = _backup_db(db->handle, "data", dat_tmpbackup_file, _print_backup_progress);
    if (rc != SQLITE_OK)
    {
        g_unlink(dat_tmpbackup_file);
        g_free(dat_tmpbackup_file);
        g_free(dat_backup_file);
        return FALSE;
    }
    g_rename(dat_tmpbackup_file, dat_backup_file);
    g_free(dat_tmpbackup_file);
    g_free(dat_backup_file);

    return TRUE;
}

gboolean dt_database_maybe_snapshot(const dt_database_t *db)
{
    if (_is_mem_db(db))
        return FALSE;

    const char *config = dt_conf_get_string_const("database/create_snapshot");
    if (!g_strcmp0(config, "never"))
    {
        // early bail out on "never"
        dt_print(DT_DEBUG_SQL, "[db backup] please consider enabling database snapshots");
        return FALSE;
    }
    if (!g_strcmp0(config, "on close"))
    {
        // early bail out on "on close"
        dt_print(DT_DEBUG_SQL, "[db backup] performing unconditional snapshot");
        return TRUE;
    }

    GTimeSpan span_from_last_snap_required;

    if (!g_strcmp0(config, "once a day"))
    {
        span_from_last_snap_required = G_TIME_SPAN_DAY;
    }
    else if (!g_strcmp0(config, "once a week"))
    {
        span_from_last_snap_required = G_TIME_SPAN_DAY * 7;
    }
    else if (!g_strcmp0(config, "once a month"))
    {
        //average month ;)
        span_from_last_snap_required = G_TIME_SPAN_DAY * 30;
    }
    else
    {
        // early bail out on "invalid value"
        dt_print(
            DT_DEBUG_SQL,
            "[db backup] invalid timespan requirement expecting never/on close/once a [day/week/month], got %s",
            config);
        return TRUE;
    }

    //we're in trouble zone - we have to determine when was the last snapshot done (including version upgrade snapshot) :/
    //this could be easy if we wrote date of last successful backup to config, but that's not really an option since
    //backup may done as last db operation, way after config file is closed. Plus we might be mixing dates of backups for
    //various library.db

    dt_print(DT_DEBUG_SQL, "[db backup] checking snapshots existence");
    GFile *library = g_file_parse_name(db->dbfilename_library);
    GFile *parent = g_file_get_parent(library);

    if (parent == NULL)
    {
        dt_print(DT_DEBUG_SQL, "[db backup] couldn't get library parent!");
        g_object_unref(library);
        return FALSE;
    }

    GError *error = NULL;
    GFileEnumerator *library_dir_files = g_file_enumerate_children(
        parent, G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_TIME_MODIFIED,
        G_FILE_QUERY_INFO_NONE, NULL, &error);

    if (library_dir_files == NULL)
    {
        dt_print(DT_DEBUG_SQL, "[db backup] couldn't enumerate library parent: %s", error->message);
        g_object_unref(parent);
        g_object_unref(library);
        g_error_free(error);
        return FALSE;
    }

    gchar *lib_basename = g_file_get_basename(library);
    g_object_unref(library);

    gchar *lib_snap_format = g_strdup_printf("%s-snp-", lib_basename);
    gchar *lib_backup_format = g_strdup_printf("%s-pre-", lib_basename);
    g_free(lib_basename);

    GFileInfo *info = NULL;
    guint64 last_snap = 0;

    while ((info = g_file_enumerator_next_file(library_dir_files, NULL, &error)))
    {
        const char *fname = g_file_info_get_name(info);
        if (g_str_has_prefix(fname, lib_snap_format) || g_str_has_prefix(fname, lib_backup_format))
        {
            dt_print(DT_DEBUG_SQL, "[db backup] found file: %s", fname);
            if (last_snap == 0)
            {
                last_snap = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
                g_object_unref(info);
                continue;
            }
            const guint64 try_snap =
                g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
            if (try_snap > last_snap)
            {
                last_snap = try_snap;
            }
        }
        g_object_unref(info);
    }
    g_object_unref(parent);
    g_free(lib_snap_format);
    g_free(lib_backup_format);

    if (error)
    {
        dt_print(DT_DEBUG_SQL, "[db backup] problem enumerating library parent: %s",
                 error->message);
        g_file_enumerator_close(library_dir_files, NULL, NULL);
        g_object_unref(library_dir_files);
        g_error_free(error);
        return FALSE;
    }

    g_file_enumerator_close(library_dir_files, NULL, NULL);
    g_object_unref(library_dir_files);

    GDateTime *date_now = g_date_time_new_now_local();

    // Even if last_snap is 0 (didn't found any snaps) it produces proper date - unix epoch
    GDateTime *date_last_snap = g_date_time_new_from_unix_local(last_snap);

    gchar *now_txt = g_date_time_format(date_now, "%Y%m%d%H%M%S");
    gchar *ls_txt = g_date_time_format(date_last_snap, "%Y%m%d%H%M%S");
    dt_print(DT_DEBUG_SQL, "[db backup] last snap: %s; curr date: %s", ls_txt, now_txt);
    g_free(now_txt);
    g_free(ls_txt);

    GTimeSpan span_from_last_snap = g_date_time_difference(date_now, date_last_snap);

    g_date_time_unref(date_now);
    g_date_time_unref(date_last_snap);

    return span_from_last_snap > span_from_last_snap_required;
}

/* Parse integers in the form d (week days), dd (hours etc), ddd (ordinal days) or dddd (years) */
static gboolean _get_iso8601_int(const gchar *text, gsize length, gint *value)
{
    gsize i;
    guint v = 0;

    if (length < 1 || length > 4)
        return FALSE;

    for (i = 0; i < length; i++)
    {
        const gchar c = text[i];
        if (c < '0' || c > '9')
            return FALSE;
        v = v * 10 + (c - '0');
    }

    *value = v;
    return TRUE;
}

static gint _db_snap_sort(gconstpointer a, gconstpointer b, gpointer user_data)
{
    const gchar *e1 = (gchar *)a;
    const gchar *e2 = (gchar *)b;

    //we assume that both end with date in
    //"%Y%m%d%H%M%S" format

    gchar *datepos1 = g_strrstr(e1, "-snp-");
    gchar *datepos2 = g_strrstr(e2, "-snp-");
    if (!datepos1 || !datepos2)
        return 0;

    datepos1 += 5; //skip "-snp-"
    datepos2 += 5; //skip "-snp-"

    int year, month, day, hour, minute, second;

    if (!_get_iso8601_int(datepos1, 4, &year) || !_get_iso8601_int(datepos1 + 4, 2, &month) ||
        !_get_iso8601_int(datepos1 + 6, 2, &day) || !_get_iso8601_int(datepos1 + 8, 2, &hour) ||
        !_get_iso8601_int(datepos1 + 10, 2, &minute) ||
        !_get_iso8601_int(datepos1 + 12, 2, &second))
    {
        return 0;
    }

    GDateTime *d1 = g_date_time_new_local(year, month, day, hour, minute, second);

    if (!_get_iso8601_int(datepos2, 4, &year) || !_get_iso8601_int(datepos2 + 4, 2, &month) ||
        !_get_iso8601_int(datepos2 + 6, 2, &day) || !_get_iso8601_int(datepos2 + 8, 2, &hour) ||
        !_get_iso8601_int(datepos2 + 10, 2, &minute) ||
        !_get_iso8601_int(datepos2 + 12, 2, &second))
    {
        g_date_time_unref(d1);
        return 0;
    }

    GDateTime *d2 = g_date_time_new_local(year, month, day, hour, minute, second);

    const gint ret = g_date_time_compare(d1, d2);

    g_date_time_unref(d1);
    g_date_time_unref(d2);

    return ret;
}

char **dt_database_snaps_to_remove(const dt_database_t *db)
{
    if (_is_mem_db(db))
        return NULL;

    const int keep_snaps = dt_conf_get_int("database/keep_snapshots");

    if (keep_snaps < 0)
        return NULL;

    dt_print(DT_DEBUG_SQL, "[db backup] checking snapshots existence");
    GFile *lib_file = g_file_parse_name(db->dbfilename_library);
    GFile *lib_parent = g_file_get_parent(lib_file);

    if (lib_parent == NULL)
    {
        dt_print(DT_DEBUG_SQL, "[db backup] couldn't get library parent!");
        g_object_unref(lib_file);
        return NULL;
    }

    GFile *dat_file = g_file_parse_name(db->dbfilename_data);
    GFile *dat_parent = g_file_get_parent(dat_file);

    if (dat_parent == NULL)
    {
        dt_print(DT_DEBUG_SQL, "[db backup] couldn't get data parent!");
        g_object_unref(dat_file);
        g_object_unref(lib_file);
        g_object_unref(lib_parent);
    }

    gchar *lib_basename = g_file_get_basename(lib_file);
    g_object_unref(lib_file);
    gchar *lib_snap_format = g_strdup_printf("%s-snp-", lib_basename);
    gchar *lib_tmp_format = g_strdup_printf("%s-tmp-", lib_basename);
    g_free(lib_basename);

    gchar *dat_basename = g_file_get_basename(dat_file);
    g_object_unref(dat_file);
    gchar *dat_snap_format = g_strdup_printf("%s-snp-", dat_basename);
    gchar *dat_tmp_format = g_strdup_printf("%s-tmp-", dat_basename);
    g_free(dat_basename);

    GQueue *lib_snaps = g_queue_new();
    GQueue *dat_snaps = g_queue_new();
    GQueue *tmplib_snaps = g_queue_new();
    GQueue *tmpdat_snaps = g_queue_new();

    if (g_file_equal(lib_parent, dat_parent))
    {
        //slight optimization if library and data are in same dir, we only have to scan one
        GError *error = NULL;
        GFileEnumerator *library_dir_files = g_file_enumerate_children(
            lib_parent, G_FILE_ATTRIBUTE_STANDARD_NAME, G_FILE_QUERY_INFO_NONE, NULL, &error);

        if (library_dir_files == NULL)
        {
            dt_print(DT_DEBUG_SQL, "[db backup] couldn't enumerate library parent: %s",
                     error->message);
            g_object_unref(lib_parent);
            g_object_unref(dat_parent);
            g_free(lib_snap_format);
            g_free(dat_snap_format);
            g_free(lib_tmp_format);
            g_free(dat_tmp_format);
            g_queue_free(lib_snaps);
            g_queue_free(dat_snaps);
            g_queue_free(tmplib_snaps);
            g_queue_free(tmpdat_snaps);
            g_error_free(error);
            return NULL;
        }

        GFileInfo *info = NULL;

        while ((info = g_file_enumerator_next_file(library_dir_files, NULL, &error)))
        {
            const char *fname = g_file_info_get_name(info);
            if (g_str_has_prefix(fname, lib_snap_format))
            {
                dt_print(DT_DEBUG_SQL, "[db backup] found file: %s", fname);
                g_queue_insert_sorted(lib_snaps, g_strdup(fname), _db_snap_sort, NULL);
            }
            else if (g_str_has_prefix(fname, dat_snap_format))
            {
                dt_print(DT_DEBUG_SQL, "[db backup] found file: %s", fname);
                g_queue_insert_sorted(dat_snaps, g_strdup(fname), _db_snap_sort, NULL);
            }
            else if (g_str_has_prefix(fname, lib_tmp_format) ||
                     g_str_has_prefix(fname, dat_tmp_format))
            {
                //we insert into single queue, since it's just dependent on parent
                g_queue_push_head(tmplib_snaps, g_strdup(fname));
            }
            g_object_unref(info);
        }
        g_free(lib_snap_format);
        g_free(dat_snap_format);

        if (error)
        {
            dt_print(DT_DEBUG_SQL, "[db backup] problem enumerating library parent: %s",
                     error->message);
            g_object_unref(lib_parent);
            g_object_unref(dat_parent);
            g_free(lib_tmp_format);
            g_free(dat_tmp_format);
            g_queue_free_full(lib_snaps, g_free);
            g_queue_free_full(dat_snaps, g_free);
            g_queue_free_full(tmplib_snaps, g_free);
            g_queue_free_full(tmpdat_snaps, g_free);
            g_file_enumerator_close(library_dir_files, NULL, NULL);
            g_object_unref(library_dir_files);
            g_error_free(error);
            return NULL;
        }
        g_file_enumerator_close(library_dir_files, NULL, NULL);
        g_object_unref(library_dir_files);
    }
    else
    {
        //well... fun.

        GError *error = NULL;
        GFileEnumerator *library_dir_files = g_file_enumerate_children(
            lib_parent, G_FILE_ATTRIBUTE_STANDARD_NAME, G_FILE_QUERY_INFO_NONE, NULL, &error);
        if (library_dir_files == NULL)
        {
            dt_print(DT_DEBUG_SQL, "[db backup] couldn't enumerate library parent: %s",
                     error->message);
            g_object_unref(lib_parent);
            g_object_unref(dat_parent);
            g_free(lib_snap_format);
            g_free(dat_snap_format);
            g_free(lib_tmp_format);
            g_free(dat_tmp_format);
            g_error_free(error);
            g_queue_free(lib_snaps);
            g_queue_free(dat_snaps);
            g_queue_free(tmplib_snaps);
            g_queue_free(tmpdat_snaps);
            return NULL;
        }

        GFileEnumerator *data_dir_files = g_file_enumerate_children(
            dat_parent, G_FILE_ATTRIBUTE_STANDARD_NAME, G_FILE_QUERY_INFO_NONE, NULL, &error);
        if (data_dir_files == NULL)
        {
            dt_print(DT_DEBUG_SQL, "[db backup] couldn't enumerate data parent: %s",
                     error->message);
            g_object_unref(lib_parent);
            g_object_unref(dat_parent);
            g_free(lib_snap_format);
            g_free(dat_snap_format);
            g_free(lib_tmp_format);
            g_free(dat_tmp_format);
            g_file_enumerator_close(library_dir_files, NULL, NULL);
            g_object_unref(library_dir_files);
            g_error_free(error);
            g_queue_free(lib_snaps);
            g_queue_free(dat_snaps);
            g_queue_free(tmplib_snaps);
            g_queue_free(tmpdat_snaps);
            return NULL;
        }

        GFileInfo *info = NULL;

        while ((info = g_file_enumerator_next_file(library_dir_files, NULL, &error)))
        {
            const char *fname = g_file_info_get_name(info);
            if (g_str_has_prefix(fname, lib_snap_format))
            {
                dt_print(DT_DEBUG_SQL, "[db backup] found file: %s", fname);
                g_queue_insert_sorted(lib_snaps, g_strdup(fname), _db_snap_sort, NULL);
            }
            else if (g_str_has_prefix(fname, lib_tmp_format) ||
                     g_str_has_prefix(fname, dat_tmp_format))
            {
                // we remove all incomplete snaps matching pattern in BOTH dirs
                g_queue_push_head(tmplib_snaps, g_strdup(fname));
            }
            g_object_unref(info);
        }
        g_free(lib_snap_format);

        if (error)
        {
            dt_print(DT_DEBUG_SQL, "[db backup] problem enumerating library parent: %s",
                     error->message);
            g_object_unref(lib_parent);
            g_object_unref(dat_parent);
            g_free(lib_tmp_format);
            g_free(dat_tmp_format);
            g_queue_free_full(lib_snaps, g_free);
            g_queue_free(dat_snaps);
            g_queue_free_full(tmplib_snaps, g_free);
            g_queue_free(tmpdat_snaps);
            g_file_enumerator_close(library_dir_files, NULL, NULL);
            g_object_unref(library_dir_files);
            g_file_enumerator_close(data_dir_files, NULL, NULL);
            g_object_unref(data_dir_files);
            g_error_free(error);
            return NULL;
        }
        g_file_enumerator_close(library_dir_files, NULL, NULL);
        g_object_unref(library_dir_files);

        while ((info = g_file_enumerator_next_file(data_dir_files, NULL, &error)))
        {
            const char *fname = g_file_info_get_name(info);
            if (g_str_has_prefix(fname, dat_snap_format))
            {
                dt_print(DT_DEBUG_SQL, "[db backup] found file: `%s'", fname);
                g_queue_insert_sorted(dat_snaps, g_strdup(fname), _db_snap_sort, NULL);
            }
            else if (g_str_has_prefix(fname, lib_tmp_format) ||
                     g_str_has_prefix(fname, dat_tmp_format))
            {
                //we add to queue both matches - it just depends on parent
                g_queue_push_head(tmpdat_snaps, g_strdup(fname));
            }
            g_object_unref(info);
        }
        g_free(dat_snap_format);
        g_free(lib_tmp_format);
        g_free(dat_tmp_format);

        if (error)
        {
            dt_print(DT_DEBUG_SQL, "[db backup] problem enumerating data parent: %s",
                     error->message);
            g_object_unref(lib_parent);
            g_object_unref(dat_parent);
            g_queue_free_full(lib_snaps, g_free);
            g_queue_free_full(dat_snaps, g_free);
            g_queue_free_full(tmplib_snaps, g_free);
            g_queue_free_full(tmpdat_snaps, g_free);
            g_file_enumerator_close(data_dir_files, NULL, NULL);
            g_object_unref(data_dir_files);
            g_error_free(error);
            return NULL;
        }

        g_file_enumerator_close(data_dir_files, NULL, NULL);
        g_object_unref(data_dir_files);
    }

    // here we have list of snaps sorted in date order, now we have to
    // create from that list of snaps to be deleted and return that :D

    GPtrArray *ret = g_ptr_array_new();

    gchar *lib_parent_path = g_file_get_path(lib_parent);
    g_object_unref(lib_parent);

    while (g_queue_get_length(lib_snaps) > keep_snaps)
    {
        gchar *head = g_queue_pop_head(lib_snaps);
        g_ptr_array_add(ret, g_strconcat(lib_parent_path, G_DIR_SEPARATOR_S, head, NULL));
        g_free(head);
    }
    while (!g_queue_is_empty(tmplib_snaps))
    {
        gchar *head = g_queue_pop_head(tmplib_snaps);
        g_ptr_array_add(ret, g_strconcat(lib_parent_path, G_DIR_SEPARATOR_S, head, NULL));
        g_free(head);
    }
    g_free(lib_parent_path);
    g_queue_free_full(lib_snaps, g_free);
    g_queue_free_full(tmplib_snaps,
                      g_free); // should be totally freed, but eh - this won't make doublefree

    gchar *dat_parent_path = g_file_get_path(dat_parent);
    g_object_unref(dat_parent);

    while (g_queue_get_length(dat_snaps) > keep_snaps)
    {
        gchar *head = g_queue_pop_head(dat_snaps);
        g_ptr_array_add(ret, g_strconcat(dat_parent_path, G_DIR_SEPARATOR_S, head, NULL));
        g_free(head);
    }
    while (!g_queue_is_empty(tmpdat_snaps))
    {
        gchar *head = g_queue_pop_head(tmpdat_snaps);
        g_ptr_array_add(ret, g_strconcat(dat_parent_path, G_DIR_SEPARATOR_S, head, NULL));
        g_free(head);
    }
    g_free(dat_parent_path);
    g_queue_free_full(dat_snaps, g_free);
    g_queue_free_full(tmpdat_snaps, g_free);

    g_ptr_array_add(ret, NULL);

    return (char **)g_ptr_array_free(ret, FALSE);
}

gchar *dt_database_get_most_recent_snap(const char *db_filename)
{
    if (!g_strcmp0(db_filename, ":memory:"))
        return NULL;

    dt_print(DT_DEBUG_SQL, "[db backup] checking snapshots existence");
    GFile *db_file = g_file_parse_name(db_filename);
    GFile *parent = g_file_get_parent(db_file);

    if (parent == NULL)
    {
        dt_print(DT_DEBUG_SQL, "[db backup] couldn't get database parent!");
        g_object_unref(db_file);
        return NULL;
    }

    GError *error = NULL;
    GFileEnumerator *db_dir_files = g_file_enumerate_children(
        parent, G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_TIME_MODIFIED,
        G_FILE_QUERY_INFO_NONE, NULL, &error);

    if (db_dir_files == NULL)
    {
        dt_print(DT_DEBUG_SQL, "[db backup] couldn't enumerate database parent: %s",
                 error->message);
        g_object_unref(parent);
        g_object_unref(db_file);
        g_error_free(error);
        return NULL;
    }

    gchar *db_basename = g_file_get_basename(db_file);
    g_object_unref(db_file);

    gchar *db_snap_format = g_strdup_printf("%s-snp-", db_basename);
    gchar *db_backup_format = g_strdup_printf("%s-pre-", db_basename);
    g_free(db_basename);

    GFileInfo *info = NULL;
    guint64 last_snap = 0;
    gchar *last_snap_name = NULL;

    while ((info = g_file_enumerator_next_file(db_dir_files, NULL, &error)))
    {
        const char *fname = g_file_info_get_name(info);
        if (g_str_has_prefix(fname, db_snap_format) || g_str_has_prefix(fname, db_backup_format))
        {
            dt_print(DT_DEBUG_SQL, "[db backup] found file: `%s'", fname);
            if (last_snap == 0)
            {
                last_snap = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
                last_snap_name = g_strdup(fname);
                g_object_unref(info);
                continue;
            }
            guint64 try_snap =
                g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
            if (try_snap > last_snap)
            {
                last_snap = try_snap;
                g_free(last_snap_name);
                last_snap_name = g_strdup(fname);
            }
        }
        g_object_unref(info);
    }
    g_free(db_snap_format);
    g_free(db_backup_format);

    if (error)
    {
        dt_print(DT_DEBUG_SQL, "[db backup] problem enumerating database parent: %s",
                 error->message);
        g_file_enumerator_close(db_dir_files, NULL, NULL);
        g_object_unref(db_dir_files);
        g_error_free(error);
        g_free(last_snap_name);
        return NULL;
    }

    g_file_enumerator_close(db_dir_files, NULL, NULL);
    g_object_unref(db_dir_files);

    if (!last_snap_name)
    {
        g_object_unref(parent);
        return NULL;
    }

    gchar *parent_path = g_file_get_path(parent);
    g_object_unref(parent);

    gchar *ret = g_strconcat(parent_path, G_DIR_SEPARATOR_S, last_snap_name, NULL);
    g_free(parent_path);
    g_free(last_snap_name);

    return ret;
}

// Nested transactions support
//
// NOTE: the nested support is actually not activated (see || TRUE below). This current
//       implementation is just a refactoring of the previous code using:
//          - dt_database_start_transaction()
//          - dt_database_release_transaction()
//          - dt_database_rollback_transaction()
//
//       With this refactoring we can count and check for nested transaction and unmatched
//       transaction routines. And it has been done to help further implementation for
//       proper threading and nested transaction support.
//
void dt_database_start_transaction(const dt_database_t *db)
{
    const int trxid = dt_atomic_add_int(&_trxid, 1);

    // if top level a simple unamed transaction is used BEGIN / COMMIT / ROLLBACK
    // otherwise we use a savepoint (named transaction).

    if (trxid == 0)
    {
        // In theads application it may be safer to use an IMMEDIATE transaction:
        // "BEGIN IMMEDIATE TRANSACTION"
        DT_DEBUG_SQLITE3_EXEC(dt_database_get(db), "BEGIN TRANSACTION", NULL, NULL, NULL);
    }
    else
#ifdef USE_NESTED_TRANSACTIONS
    {
        char SQLTRX[32] = {0};
        g_snprintf(SQLTRX, sizeof(SQLTRX), "SAVEPOINT trx%d", trxid);
        DT_DEBUG_SQLITE3_EXEC(dt_database_get(db), SQLTRX, NULL, NULL, NULL);
    }
#else
    {
        dt_print(DT_DEBUG_ALWAYS,
                 "[dt_database_start_transaction] nested transaction detected (%d)", trxid);
    }
#endif

    if (trxid > MAX_NESTED_TRANSACTIONS)
        dt_print(DT_DEBUG_ALWAYS, "[dt_database_start_transaction] more than %d nested transaction",
                 MAX_NESTED_TRANSACTIONS);
}

void dt_database_release_transaction(const dt_database_t *db)
{
    const int trxid = dt_atomic_sub_int(&_trxid, 1);

    if (trxid <= 0)
        dt_print(DT_DEBUG_ALWAYS, "[dt_database_release_transaction] COMMIT outside a transaction");

    if (trxid == 1)
    {
        DT_DEBUG_SQLITE3_EXEC(dt_database_get(db), "COMMIT TRANSACTION", NULL, NULL, NULL);
    }
    else
#ifdef USE_NESTED_TRANSACTIONS
    {
        char SQLTRX[64] = {0};
        g_snprintf(SQLTRX, sizeof(SQLTRX), "RELEASE SAVEPOINT trx%d", trxid - 1);
        DT_DEBUG_SQLITE3_EXEC(dt_database_get(db), SQLTRX, NULL, NULL, NULL);
    }
#else
    {
        dt_print(DT_DEBUG_ALWAYS, "[dt_database_end_transaction] nested transaction detected (%d)",
                 trxid);
    }
#endif
}

void dt_database_rollback_transaction(const dt_database_t *db)
{
    const int trxid = dt_atomic_sub_int(&_trxid, 1);

    if (trxid <= 0)
        dt_print(DT_DEBUG_ALWAYS,
                 "[dt_database_rollback_transaction] ROLLBACK outside a transaction");

    if (trxid == 1)
    {
        DT_DEBUG_SQLITE3_EXEC(dt_database_get(db), "ROLLBACK TRANSACTION", NULL, NULL, NULL);
    }
    else
#ifdef USE_NESTED_TRANSACTIONS
    {
        char SQLTRX[64] = {0};
        g_snprintf(SQLTRX, sizeof(SQLTRX), "ROLLBACK TRANSACTION TO SAVEPOINT trx%d", trxid - 1);
        DT_DEBUG_SQLITE3_EXEC(dt_database_get(db), SQLTRX, NULL, NULL, NULL);
    }
#else
    {
        dt_print(DT_DEBUG_ALWAYS,
                 "[dt_database_rollback_transaction] nested transaction detected (%d)", trxid);
    }
#endif
}
