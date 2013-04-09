/* GIO - GLib Input, Output and Streaming Library
 *
 * Copyright (C) 2013 Cédric Bosdonnat <cbosdonnat@suse.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 * Author: Cédric Bosdonnat <cbosdonnat@suse.com>
 */

#include <string.h>
#include <stdio.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>

#include <libcmis-c/error.h>
#include <libcmis-c/session-factory.h>

#include "gvfsbackendcmis.h"
#include "gvfskeyring.h"

G_DEFINE_TYPE (GVfsBackendCmis, g_vfs_backend_cmis, G_VFS_TYPE_BACKEND)

static void
logger (const gchar* domain,
        GLogLevelFlags level,
        const gchar* message,
        gpointer data)
{
    FILE* fd = fopen ("/home/cbosdo/gvfscmisbackend.log", "a");
    fwrite (message, 1, strlen(message), fd);
    fclose (fd);
}

static void
do_mount (GVfsBackend *backend,
          GVfsJobMount *job,
          GMountSpec *mount_spec,
          GMountSource *mount_source,
          gboolean is_automount)
{
    char *spec_str;
    const char *host;
    const char *user;
    char* username = NULL;
    char *binding_url = NULL;
    char *repository_id = NULL;
    char *password = NULL;
    gboolean failed = FALSE;
    GPasswordSave password_save = G_PASSWORD_SAVE_NEVER;
    char *prompt = NULL;

    GVfsBackendCmis *cmis_backend = G_VFS_BACKEND_CMIS (backend);

    spec_str = g_mount_spec_to_string (mount_spec);
    g_log ("cmis", G_LOG_LEVEL_DEBUG, "mount_spec: %s\n", spec_str);
    g_free (spec_str);

    /* In CMIS urls, the host is the url-encoded binding URL */
    host = g_mount_spec_get (mount_spec, "host");

    if (host == NULL || *host == 0)
    {
        g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                          G_IO_ERROR_INVALID_ARGUMENT,
                          _("Invalid mount spec"));
        return;
    }
    binding_url = g_uri_unescape_string (host, ":/");

    /* TODO Get the hostname from the binding_url as display host */

    /* TODO We may have the repository ID encoded after the binding URL.
       both elements are separated by a '#'. */

    user = g_mount_spec_get (mount_spec, "user");

    /* Ask for username / password if missing */
    if (!g_vfs_keyring_lookup_password (user,
                binding_url,
                NULL,
                "cmis",
                NULL,
                NULL,
                0,
                &username,
                NULL,
                &password) ||
         username == NULL ||
         password == NULL)
    {
        GAskPasswordFlags flags = G_ASK_PASSWORD_NEED_PASSWORD;
        gboolean aborted;
        if (username == NULL)
        {
            prompt = g_strdup_printf (_("Enter password for %s"), binding_url);
            flags |= G_ASK_PASSWORD_NEED_USERNAME;
        }
        else
            prompt = g_strdup_printf (_("Enter password for %s on %s"), username, binding_url);
     
        if (g_vfs_keyring_is_available ())
            flags |= G_ASK_PASSWORD_SAVING_SUPPORTED;

        if (!g_mount_source_ask_password (
                    mount_source,
                    prompt,
                    user,
                    NULL,
                    flags,
                    &aborted,
                    &password,
                    &username,
                    NULL,
                    NULL,
                    &password_save) ||
            aborted)
        {
            g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                                 aborted ? G_IO_ERROR_FAILED_HANDLED : G_IO_ERROR_PERMISSION_DENIED,
                 	       _("Password dialog cancelled"));
            failed = TRUE;
        }
    }
    
    if (!failed)
    {
        /* TODO Setup the proxy settings on libcmis if any */

        /* Try to create the CMIS session */
        libcmis_ErrorPtr error = libcmis_error_create ();
        g_log ("cmis", G_LOG_LEVEL_DEBUG, "about to create session\n");
        cmis_backend->session = libcmis_createSession (binding_url,
                repository_id, username, password, NULL, false, error);
        g_log ("cmis", G_LOG_LEVEL_DEBUG, "createSession finished\n");

        if (libcmis_error_getMessage (error) != NULL || libcmis_error_getType(error) != NULL)
        {
            const char* error_type = libcmis_error_getType (error);
            g_log ("cmis", G_LOG_LEVEL_DEBUG, "Got an error when creating libcmis session: [%]%s\n",
                    error_type, libcmis_error_getMessage(error));
            g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                                 strcmp (error_type, "permissionDenied") == 0 ?
                                    G_IO_ERROR_PERMISSION_DENIED : G_IO_ERROR_FAILED,
                                 libcmis_error_getMessage (error) );
        }
        else
        {
            char* display_name;

            /* Save password if we prompted it */
            if (prompt)
            {
                g_vfs_keyring_save_password (username,
                                             binding_url,
                                             NULL,
                                             "cmis",
                                             NULL,
                                             NULL,
                                             0,
                                             password,
                                             password_save);
                g_free (prompt);
            }

            display_name = g_strdup_printf (_("CMIS: %s on %s"), username, binding_url);
            g_vfs_backend_set_display_name (backend, display_name);
            g_free (display_name);

            g_vfs_backend_set_mount_spec (backend, mount_spec);
            g_vfs_backend_set_icon_name (backend, "folder-remote");
            g_vfs_backend_set_symbolic_icon_name (backend, "folder-remote-symbolic");
            g_vfs_job_succeeded (G_VFS_JOB (job));
        }
        libcmis_error_free (error);
    }

    /* Cleanup */
    g_free (binding_url);
    if (repository_id != NULL)
        g_free (repository_id);
    if (username != NULL)
        g_free (username);
    if (password != NULL)
        g_free (password);
}

static void
do_unmount (GVfsBackend *   backend,
            GVfsJobUnmount *job,
            GMountUnmountFlags flags,
            GMountSource *mount_source)
{
    GVfsBackendCmis *cmis_backend = G_VFS_BACKEND_CMIS (backend);

    if (cmis_backend->session)
        libcmis_session_free (cmis_backend->session);
    g_vfs_job_succeeded (G_VFS_JOB (job));
}

static void
do_open_for_read (GVfsBackend *backend,
                  GVfsJobOpenForRead *job,
                  const char *filename)
{
    g_print ( "TODO Implement do_open_for_read\n");
}

static void
do_close_read (GVfsBackend *     backend,
               GVfsJobCloseRead *job,
               GVfsBackendHandle handle)
{
    g_print ( "TODO Implement do_close_read\n");
}

static void
do_read (GVfsBackend *     backend,
         GVfsJobRead *     job,
         GVfsBackendHandle handle,
         char *            buffer,
         gsize             bytes_requested)
{
    g_print ( "TODO Implement do_read\n");
}

static void
do_create (GVfsBackend *backend,
           GVfsJobOpenForWrite *job,
           const char *filename,
           GFileCreateFlags flags)
{
    g_print ( "TODO Implement do_create\n");
}

static void
do_append (GVfsBackend *backend,
           GVfsJobOpenForWrite *job,
           const char *filename,
           GFileCreateFlags flags)
{
    g_print ( "TODO Implement do_append\n");
}

static void
do_replace (GVfsBackend *backend,
            GVfsJobOpenForWrite *job,
            const char *filename,
            const char *etag,
            gboolean make_backup,
            GFileCreateFlags flags)
{
    g_print ( "TODO Implement do_replace\n");
}

static void
do_close_write (GVfsBackend *backend,
                GVfsJobCloseWrite *job,
                GVfsBackendHandle handle)
{
    g_print ( "TODO Implement do_close_write\n");
}

static void
do_write (GVfsBackend *backend,
          GVfsJobWrite *job,
          GVfsBackendHandle handle,
          char *buffer,
          gsize buffer_size)
{
    g_print ( "TODO Implement do_write\n");
}

static void
do_query_info (GVfsBackend *backend,
               GVfsJobQueryInfo *job,
               const char *filename,
               GFileQueryInfoFlags query_flags,
               GFileInfo *info,
               GFileAttributeMatcher *matcher)
{
    g_print ( "TODO Implement do_query_info\n");
}

static void
do_enumerate (GVfsBackend *backend,
              GVfsJobEnumerate *job,
              const char *dirname,
              GFileAttributeMatcher *matcher,
              GFileQueryInfoFlags query_flags)
{
    g_print ( "TODO Implement do_enumerate\n");
}

static void
do_set_display_name (GVfsBackend *backend,
                     GVfsJobSetDisplayName *job,
                     const char *filename,
                     const char *display_name)
{
    g_print ( "TODO Implement do_set_display_name\n");
}

static void
do_delete (GVfsBackend *backend,
           GVfsJobDelete *job,
           const char *filename)
{
    g_print ( "TODO Implement do_delete\n");
}

static void
do_make_directory (GVfsBackend *backend,
                   GVfsJobMakeDirectory *job,
                   const char *filename)
{
    g_print ( "TODO Implement do_make_directory\n");
}

static void
do_move (GVfsBackend *backend,
         GVfsJobMove *job,
         const char *source,
         const char *destination,
         GFileCopyFlags flags,
         GFileProgressCallback progress_callback,
         gpointer progress_callback_data)
{
    g_print ( "TODO Implement do_move\n");
}

static gboolean
try_query_settable_attributes (GVfsBackend *backend,
			       GVfsJobQueryAttributes *job,
			       const char *filename)
{
    g_print ( "TODO Implement try_query_settable_attributes\n");
}

static void
do_set_attribute (GVfsBackend *backend,
                  GVfsJobSetAttribute *job,
                  const char *filename,
                  const char *attribute,
                  GFileAttributeType type,
                  gpointer value_p,
                  GFileQueryInfoFlags flags)
{
    g_print ( "TODO Implement do_set_attribute\n");
}

static void
do_pull (GVfsBackend *         backend,
         GVfsJobPull *         job,
         const char *          source,
         const char *          local_path,
         GFileCopyFlags        flags,
         gboolean              remove_source,
         GFileProgressCallback progress_callback,
         gpointer              progress_callback_data)
{
    g_print ( "TODO Implement do_pull\n");
}

static void
g_vfs_backend_cmis_init (GVfsBackendCmis *cmis_backend)
{
    cmis_backend->session = NULL; 
}

static void
g_vfs_backend_cmis_finalize (GObject *object)
{
    GVfsBackendCmis *cmis_backend = G_VFS_BACKEND_CMIS (object);

    /* Should have been done by do_unmount, but better be sure it's clean */
    if (cmis_backend->session)
        libcmis_session_free (cmis_backend->session);
    
    G_OBJECT_CLASS (g_vfs_backend_cmis_parent_class)->finalize (object);
}

static void
g_vfs_backend_cmis_class_init (GVfsBackendCmisClass *klass)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
    GVfsBackendClass *backend_class = G_VFS_BACKEND_CLASS (klass);

    gobject_class->finalize = g_vfs_backend_cmis_finalize;

    backend_class->mount = do_mount;
    backend_class->try_mount = NULL;
    backend_class->unmount = do_unmount;
    backend_class->open_for_read = do_open_for_read;
    backend_class->close_read = do_close_read;
    backend_class->read = do_read;
    backend_class->create = do_create;
    backend_class->append_to = do_append;
    backend_class->replace = do_replace;
    backend_class->close_write = do_close_write;
    backend_class->write = do_write;
    backend_class->query_info = do_query_info;
    backend_class->enumerate = do_enumerate;
    backend_class->set_display_name = do_set_display_name;
    backend_class->delete = do_delete;
    backend_class->make_directory = do_make_directory;
    backend_class->move = do_move;
    backend_class->try_query_settable_attributes = try_query_settable_attributes;
    backend_class->set_attribute = do_set_attribute;
    backend_class->pull = do_pull;

    /* Set logger for debugging purpose */
    g_log_set_handler ("cmis", G_LOG_LEVEL_MASK, logger, NULL); 
}
