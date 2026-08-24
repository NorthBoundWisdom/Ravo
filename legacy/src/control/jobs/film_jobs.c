/*
    This file is part of darktable,
    Copyright (C) 2010-2024 darktable developers.

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
#include "control/jobs/film_jobs.h"
#include "common/darktable.h"
#include "common/collection.h"
#include "common/film.h"
#include <stdlib.h>

typedef struct dt_film_import1_t
{
    dt_film_t *film;
    GList *imagelist;
} dt_film_import1_t;

static void _film_import1(dt_job_t *job, dt_film_t *film, GList *images);

static int32_t dt_film_import1_run(dt_job_t *job)
{
    dt_film_import1_t *params = dt_control_job_get_params(job);
    _film_import1(job, params->film, NULL); // import the given film, collecting its images
    dt_pthread_mutex_lock(&params->film->images_mutex);
    params->film->ref--;
    dt_pthread_mutex_unlock(&params->film->images_mutex);
    if (params->film->ref <= 0)
    {
        if (dt_film_is_empty(params->film->id))
        {
            dt_film_remove(params->film->id);
        }
    }

    // notify the user via the window manager
    dt_ui_notify_user();

    return 0;
}

static void dt_film_import1_cleanup(void *p)
{
    dt_film_import1_t *params = p;

    dt_film_cleanup(params->film);
    free(params->film);

    free(params);
}

dt_job_t *dt_film_import1_create(dt_film_t *film)
{
    dt_job_t *job =
        dt_control_job_create(&dt_film_import1_run, "cache load raw images for preview");
    if (!job)
        return NULL;
    dt_film_import1_t *params = calloc(1, sizeof(dt_film_import1_t));
    if (!params)
    {
        dt_control_job_dispose(job);
        return NULL;
    }
    dt_control_job_add_progress(job, _("import images"), TRUE);
    dt_control_job_set_params(job, params, dt_film_import1_cleanup);
    params->film = film;
    dt_pthread_mutex_lock(&film->images_mutex);
    film->ref++;
    dt_pthread_mutex_unlock(&film->images_mutex);
    return job;
}

static int32_t _pathlist_import_run(dt_job_t *job)
{
    dt_film_import1_t *params = dt_control_job_get_params(job);
    if (params->imagelist)
        _film_import1(
            job, NULL,
            params->imagelist); // import the specified images, creating filmrolls as needed
    params->imagelist = NULL;   // the import will have freed the image list

    // notify the user via the window manager
    dt_ui_notify_user();
    return 0;
}

static void _pathlist_import_cleanup(void *p)
{
    dt_film_import1_t *params = p;
    free(params);
}

dt_job_t *dt_pathlist_import_create(int argc, char *argv[])
{
    dt_job_t *job = dt_control_job_create(&_pathlist_import_run, "import commandline images");
    if (!job)
        return NULL;
    dt_film_import1_t *params = calloc(1, sizeof(dt_film_import1_t));
    if (!params)
    {
        dt_control_job_dispose(job);
        return NULL;
    }
    dt_control_job_add_progress(job, _("import images"), TRUE);
    dt_control_job_set_params(job, params, _pathlist_import_cleanup);
    params->film = NULL;
    // now collect all of the images to be imported
    params->imagelist = NULL;
    for (int i = 1; i < argc; i++)
    {
        char *path = dt_util_normalize_path(argv[i]);
        if (!g_file_test(path, G_FILE_TEST_IS_DIR))
        {
            // add just the given name to the list of images to import
            params->imagelist = g_list_prepend(params->imagelist, path);
        }
        else
        {
            // iterate over the directory, extracting image files
            GDir *cdir = g_dir_open(path, 0, NULL);
            if (cdir)
            {
                while (TRUE)
                {
                    const gchar *fname = g_dir_read_name(cdir);
                    if (!fname)
                        break; // no more files in directory
                    if (fname[0] == '.')
                        continue; // skip hidden files
                    gchar *fullname = g_build_filename(path, fname, NULL);
                    if (!g_file_test(fullname, G_FILE_TEST_IS_DIR) && dt_supported_image(fname))
                        params->imagelist = g_list_prepend(params->imagelist, fullname);
                    else
                        g_free(fullname);
                }
            }
            g_dir_close(cdir);
            g_free(path);
        }
    }
    params->imagelist = g_list_reverse(params->imagelist);
    return job;
}

static GList *_film_recursive_get_files(const gchar *path, gboolean recursive, GList **result)
{
    gchar *fullname;

    /* let's try open current dir */
    GDir *cdir = g_dir_open(path, 0, NULL);
    if (!cdir)
        return *result;

    /* lets read all files in current dir, recurse
     into directories if we should import recursive.
   */
    do
    {
        /* get the current filename */
        const gchar *filename = g_dir_read_name(cdir);

        /* return if no more files are in current dir */
        if (!filename)
            break;
        if (filename[0] == '.')
            continue;

        /* build full path for filename */
        fullname = g_build_filename(path, filename, NULL);

        /* recurse into directory if we hit one and we doing a recursive import */
        if (recursive && g_file_test(fullname, G_FILE_TEST_IS_DIR))
        {
            *result = _film_recursive_get_files(fullname, recursive, result);
            g_free(fullname);
        }
        /* or test if we found a supported image format to import */
        else if (!g_file_test(fullname, G_FILE_TEST_IS_DIR) && dt_supported_image(filename))
            *result = g_list_prepend(*result, fullname);
        else
            g_free(fullname);

    } while (TRUE);

    /* cleanup and return results */
    g_dir_close(cdir);

    return *result;
}

/* compare used for sorting the list of files to import
   only sort on basename of full path eg. the actually filename.
*/
static int _film_filename_cmp(gchar *a, gchar *b)
{
    gchar *a_basename = g_path_get_basename(a);
    gchar *b_basename = g_path_get_basename(b);
    const int ret = g_strcmp0(a_basename, b_basename);
    g_free(a_basename);
    g_free(b_basename);
    return ret;
}

static void _film_import1(dt_job_t *job, dt_film_t *film, GList *images)
{
    // first, gather all images to import if not already given
    if (!images)
    {
        const gboolean recursive = dt_conf_get_bool("ui_last/import_recursive");

        images = _film_recursive_get_files(film->dirname, recursive, &images);
        if (images == NULL)
        {
            dt_control_log(_("no supported images were found to be imported"));
            return;
        }
    }

    /* we got ourself a list of images, lets sort and start import */
    images = g_list_sort(images, (GCompareFunc)_film_filename_cmp);

    /* let's start import of images */
    double fraction = 0;
    const guint total = g_list_length(images);
    dt_control_job_set_progress_message(
        job, ngettext("importing %d image", "importing %d images", total), total);

    GList *imgs = NULL;

    /* loop thru the images and import to current film roll */
    dt_film_t *cfr = film;
    int pending = 0;
    double last_update = dt_get_wtime();
    for (GList *image = images; image; image = g_list_next(image))
    {
        gchar *cdn = g_path_get_dirname((const gchar *)image->data);

        /* check if we need to initialize a new filmroll */
        if (!cfr || g_strcmp0(cfr->dirname, cdn) != 0)
        {
            /* cleanup previously imported filmroll*/
            if (cfr && cfr != film)
            {
                if (dt_film_is_empty(cfr->id))
                {
                    dt_film_remove(cfr->id);
                }
                dt_film_cleanup(cfr);
                free(cfr);
                cfr = NULL;
            }

            /* initialize and create a new film to import to */
            cfr = malloc(sizeof(dt_film_t));
            dt_film_init(cfr);
            dt_film_new(cfr, cdn);
        }

        g_free(cdn);

        /* import image */
        const dt_imgid_t imgid = dt_image_import(cfr->id, (const gchar *)image->data, FALSE, FALSE);
        pending++; // we have another image which hasn't been reported yet
        fraction += 1.0 / total;
        dt_control_job_set_progress(job, fraction);

        imgs = g_list_append(imgs, GINT_TO_POINTER(imgid));
        const double curr_time = dt_get_wtime();
        // if we've imported at least four images without an update, and it's been at least half a second since the last
        //   one, update the interface
        if (pending >= 4 && curr_time - last_update > 0.5)
        {
            dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD,
                                       DT_COLLECTION_PROP_UNDEF, g_list_copy(imgs));
            g_list_free(imgs);
            imgs = NULL;
            // restart the update count and timer
            pending = 0;
            last_update = curr_time;
        }
        if (dt_control_job_get_state(job) == DT_JOB_STATE_CANCELLED)
            break;
    }

    g_list_free_full(images, g_free);
    // only redraw at the end, to not spam the cpu with exposure events
    dt_control_queue_redraw_center();
    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_TAG_CHANGED);

    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_FILMROLLS_IMPORTED, film ? film->id : cfr->id);

    /* cleanup previously imported filmroll*/
    if (cfr && cfr != film)
    {
        dt_film_cleanup(cfr);
        free(cfr);
    }
}
