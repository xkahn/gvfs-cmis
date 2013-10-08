
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

#include <libcmis-c/document.h>
#include <libcmis-c/error.h>
#include <libcmis-c/folder.h>
#include <libcmis-c/object.h>
#include <libcmis-c/object-type.h>
#include <libcmis-c/repository.h>
#include <libcmis-c/session.h>
#include <libcmis-c/session-factory.h>
#include <libcmis-c/property.h>
#include <libcmis-c/property-type.h>
#include <libcmis-c/types.h>
#include <libcmis-c/vectors.h>

#include <gvfsuriutils.h>
#include "gvfsbackendcmis.h"
#include "gvfskeyring.h"
#include "gvfsjobenumerate.h"

G_DEFINE_TYPE (GVfsBackendCmis, g_vfs_backend_cmis, G_VFS_TYPE_BACKEND)

/** Internal structure to pass a temporary file and it's stream for
    read/write operations.
 */
struct TmpHandle
{
    GFile *file;
    GFileIOStream *stream;
    char *filename;
};

static void
output_cmis_error (GVfsJob *job, libcmis_ErrorPtr error)
{
    const char* error_type = libcmis_error_getType (error);
    /* TODO Handle NotFound errors */
    g_vfs_job_failed (job, G_IO_ERROR,
                         strcmp (error_type, "permissionDenied") == 0 ?
                            G_IO_ERROR_PERMISSION_DENIED : G_IO_ERROR_FAILED,
                         libcmis_error_getMessage (error) );
}

static void
repository_to_file_info (libcmis_RepositoryPtr repository,
                         GFileInfo *info)
{
    char *id;
    char *name;
    GIcon *icon = NULL;
    GIcon *symbolic_icon = NULL;

    id = libcmis_repository_getId (repository);
    name = libcmis_repository_getName (repository);

    g_file_info_set_name (info, id);
    g_file_info_set_display_name (info, name);

    /* Repositories can't be edited through CMIS */
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, FALSE);
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE, FALSE);
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, FALSE);

    g_file_info_set_file_type (info, G_FILE_TYPE_DIRECTORY);
	g_file_info_set_content_type (info, "inode/directory");

    icon = g_themed_icon_new ("folder-remote");
    symbolic_icon = g_themed_icon_new ("folder-remote-symbolic");

    g_file_info_set_icon (info, icon);
    g_object_unref (icon);
    g_file_info_set_symbolic_icon (info, symbolic_icon);
    g_object_unref (symbolic_icon);

    g_free (id);
    g_free (name);
}

static void
cmis_object_to_file_info (libcmis_ObjectPtr object,
                          GFileInfo *info)
{
    char *id;
    char *name;
    char *content_type;
    bool is_folder;
    bool is_document;
    GIcon *icon = NULL;
    GIcon *symbolic_icon = NULL;
    time_t mod_time;
    time_t create_time;
    libcmis_vector_property_Ptr plist;
    libcmis_AllowableActionsPtr allowable_actions;
    bool can_read;
    bool can_write;
    bool can_delete;
    bool can_rename;
    int i;

    id = libcmis_object_getId (object);
    name = libcmis_object_getName (object);
    g_file_info_set_name (info, name);
    g_file_info_set_display_name (info, name);

    create_time = libcmis_object_getCreationDate (object);
    g_file_info_set_attribute_uint64 (info,
                                      G_FILE_ATTRIBUTE_TIME_CREATED,
                                      create_time);
    mod_time = libcmis_object_getLastModificationDate (object);
    g_file_info_set_attribute_uint64 (info,
                                      G_FILE_ATTRIBUTE_TIME_MODIFIED,
                                      mod_time);

    /* Don't assume not being a folder means we have a document
     * as this is no longer true with CMIS v1.1 */
    is_folder = libcmis_is_folder (object);
    is_document = libcmis_is_document (object);

    if (is_folder)
    {
        content_type = g_strdup ("inode/directory");
        icon = g_themed_icon_new ("folder");
        symbolic_icon = g_themed_icon_new ("folder-symbolic");
        g_file_info_set_file_type (info, G_FILE_TYPE_DIRECTORY);
    }
    else if (is_document)
    {
        libcmis_DocumentPtr document;
        long content_size = 0;

        g_file_info_set_file_type (info, G_FILE_TYPE_REGULAR);

        document = libcmis_document_cast (object);
        content_type = libcmis_document_getContentType (document);

        icon = g_content_type_get_icon (content_type);
        if (icon == NULL)
            icon = g_themed_icon_new ("text-x-generic");

        symbolic_icon = g_content_type_get_symbolic_icon (content_type);
        if (symbolic_icon == NULL)
            symbolic_icon = g_themed_icon_new ("text-x-generic-symbolic");

        content_size = libcmis_document_getContentLength (document);
        g_file_info_set_size (info, content_size);
    }

    /* Set the permissions based on the Allowable Actions*/
    allowable_actions = libcmis_object_getAllowableActions (object);
    can_read = libcmis_allowable_actions_isAllowed (allowable_actions, libcmis_GetContentStream);
    can_write = libcmis_allowable_actions_isAllowed (allowable_actions, libcmis_SetContentStream);
    can_delete = libcmis_allowable_actions_isAllowed (allowable_actions, libcmis_DeleteObject);
    can_rename = libcmis_allowable_actions_isAllowed (allowable_actions, libcmis_UpdateProperties);

    if (libcmis_allowable_actions_isDefined (allowable_actions, libcmis_GetContentStream)) {
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ, can_read);
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME, can_rename);
    }
    if (libcmis_allowable_actions_isDefined (allowable_actions, libcmis_SetContentStream))
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, can_write);
    if (libcmis_allowable_actions_isDefined (allowable_actions, libcmis_DeleteObject)) {
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE, can_delete);
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, can_delete);
    }
    libcmis_allowable_actions_free (allowable_actions);
   
    g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_ID_FILE, id);

    g_file_info_set_content_type (info, content_type);
    g_file_info_set_icon (info, icon);
    g_file_info_set_symbolic_icon (info, symbolic_icon);

    /* Insert raw metadata into the object so others can get it. */
    
    plist = libcmis_object_getProperties(object);
    for (i = 0; libcmis_vector_property_get(plist, i); i++) {
      libcmis_vector_string_Ptr vs;
      libcmis_vector_bool_Ptr vb;
      libcmis_vector_long_Ptr vl;
      libcmis_vector_double_Ptr vd;
      libcmis_vector_time_Ptr vt;

      gchar *nt;

      libcmis_PropertyPtr p = libcmis_vector_property_get(plist, i);
      libcmis_PropertyTypePtr ptype = libcmis_property_getPropertyType(p);
      gchar *title =  g_strdup_printf ("cmis::%s", g_strdup(libcmis_property_type_getId(ptype)));
      switch (libcmis_property_type_getType(ptype)) {
      case libcmis_String:
	vs = libcmis_property_getStrings(p);
	if (libcmis_vector_string_size(vs) > 1) 
	  g_print ("cmis_object_to_file_info: Property %s (string) is using multivalue; not yet supported\n", title);
	g_file_info_set_attribute_string (info, title, libcmis_vector_string_get(vs, 0));
	libcmis_vector_string_free(vs);
	break;
	
      case libcmis_Integer:
	vl = libcmis_property_getLongs(p);
	if (libcmis_vector_long_size(vl) > 1) 
	  g_print ("cmis_object_to_file_info: Property %s (integer) is using multivalue; not yet supported\n", title);
	g_file_info_set_attribute_int64 (info, title, libcmis_vector_long_get(vl, 0));
	libcmis_vector_long_free(vl);
	break;

      case libcmis_Decimal:
	vd = libcmis_property_getDoubles(p);
	if (libcmis_vector_double_size(vd) > 1) 
	  g_print ("cmis_object_to_file_info: Property %s (double) is using multivalue; not yet supported\n", title);
	nt = g_strdup_printf ("[d] %f", libcmis_vector_double_get(vd, 0));
	g_file_info_set_attribute_string (info, title, nt);
	g_free (nt);
	libcmis_vector_double_free(vd);
	break;

      case libcmis_Bool:
	vb = libcmis_property_getBools(p);
	if (libcmis_vector_bool_size(vb) > 1)
	  g_print ("cmis_object_to_file_info: Property %s (bool) is using multivalue; not yet supported\n", title);
	g_file_info_set_attribute_boolean (info, title, libcmis_vector_bool_get(vb, 0));
	libcmis_vector_bool_free(vb);
	break;

      case libcmis_DateTime:
	vt = libcmis_property_getDateTimes(p);
	if (libcmis_vector_time_size(vt) > 1) 
	  g_print ("cmis_object_to_file_info: Property %s (double) is using multivalue; not yet supported\n", title);
	nt = g_strdup_printf ("[t] %d", libcmis_vector_time_get(vt, 0));
	g_file_info_set_attribute_string (info, title, nt);
	g_free (nt);
	libcmis_vector_time_free (vt);
	break;

      default:
	g_print ("cmis_object_to_file_info: (II) type '%s' received for %s was not supported\n", libcmis_property_type_getXmlType(ptype), title);
	break;
      }

    }

    /* Cleanup */
    g_free (id);
    g_free (name);
    g_free (content_type);
    g_object_unref (icon);
    g_object_unref (symbolic_icon);
}

static gchar*
extract_repository_from_path (const char* path,
                              gchar** out_path)
{
    gsize max_size = 0;
    gchar **segments;
    gchar **it;
    gchar *repository_id = NULL;
    GString *out_path_str;

    max_size = strlen (path);
    segments = g_strsplit (path, "/", 0);
    out_path_str = g_string_sized_new (max_size);
    for (it = segments; *it != NULL; it++)
    {
        size_t segment_size;

        segment_size = strlen (*it);
        if (!repository_id && segment_size > 0)
            repository_id = g_strdup (*it);
        else if (repository_id)
        {
            g_string_append_c (out_path_str, '/');
            g_string_append (out_path_str, *it);
        }
    }

    g_strfreev (segments);
    *out_path = g_string_free (out_path_str, FALSE);

    return repository_id;
}

static bool
is_cmis_session_ready (GVfsBackendCmis *backend, GVfsJob *job)
{
    bool success = true;
    if (backend->session == NULL)
    {
        g_vfs_job_failed (job, G_IO_ERROR,
                             G_IO_ERROR_NOT_MOUNTED,
                       _("CMIS session not initialized"));
        success = false;
    }
    return success;
}

/** Get the CMIS object using its path. The root path is handled as a folder, not as a repository.

    This function will set errors on the job if needed.

    \return
        the CMIS object or NULL. The resulting object needs to be freed using libcmis_object_free().
  */
libcmis_ObjectPtr get_cmis_object (GVfsJob *job,
                                   libcmis_SessionPtr session,
                                   const char *repository_id,
                                   const char *path)
{
    libcmis_ObjectPtr object = NULL;

    if (libcmis_session_setRepository (session, repository_id))
    {
        if (path && strlen(path) > 0)
        {
            libcmis_ErrorPtr error;

            error = libcmis_error_create ();
            object = libcmis_session_getObjectByPath (session, path, error);

            if (libcmis_error_getMessage (error) != NULL || libcmis_error_getType(error) != NULL)
                output_cmis_error (job, error);

            libcmis_error_free (error);
        }
    }
    else
    {
        char *message;
        message = g_strdup_printf (_("No such repository: %s"), repository_id);
        g_vfs_job_failed (job, G_IO_ERROR,
                          G_IO_ERROR_NOT_FOUND,
                          message );
        g_free (message);
    }
    return object;
}

size_t write_to_io_stream (const void* ptr, size_t size, size_t nmemb, void* data)
{
    size_t read = 0;
    GFileIOStream *stream = (GFileIOStream*) data;
    GOutputStream *out_stream;
    gsize bytes_written = 0;

    out_stream = g_io_stream_get_output_stream (G_IO_STREAM (stream));
    g_output_stream_write_all (out_stream, ptr,
            size * nmemb, &bytes_written, NULL, NULL);

    read = bytes_written / size;

    return read;
}

size_t write_to_g_output_stream (const void* ptr, size_t size, size_t nmemb, void* data)
{
    size_t read = 0;
    GOutputStream *out_stream = (GOutputStream*) data;
    gsize bytes_written = 0;

    g_output_stream_write_all (out_stream, ptr,
            size * nmemb, &bytes_written, NULL, NULL);

    read = bytes_written / size;

    return read;
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
    char *password = NULL;
    gboolean failed = FALSE;
    GPasswordSave password_save = G_PASSWORD_SAVE_NEVER;
    char *prompt = NULL;
    GDecodedUri *decoded = NULL;

    GVfsBackendCmis *cmis_backend = G_VFS_BACKEND_CMIS (backend);

    spec_str = g_mount_spec_to_string (mount_spec);
    g_print ("mount_spec: %s\n", spec_str);
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
    binding_url = g_uri_unescape_string (host, NULL);

    /* Get the hostname from the binding_url as display host */
    decoded = g_vfs_decode_uri (binding_url);
    if (decoded && decoded->host)
    {
        cmis_backend->display_name = g_strdup (decoded->host);
        if (decoded)
            g_vfs_decoded_uri_free (decoded);
    }
    else
        cmis_backend->display_name = g_strdup (binding_url);

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
            prompt = g_strdup_printf (_("Enter password for %s"), cmis_backend->display_name);
            flags |= G_ASK_PASSWORD_NEED_USERNAME;
        }
        else
            prompt = g_strdup_printf (_("Enter password for %s on %s"),
                                      username, cmis_backend->display_name);
     
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
        cmis_backend->session = libcmis_createSession (binding_url,
                NULL, username, password, false, NULL, false, error);

        if (libcmis_error_getMessage (error) != NULL || libcmis_error_getType(error) != NULL)
            output_cmis_error (G_VFS_JOB (job), error);
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

            display_name = g_strdup_printf (_("CMIS: %s on %s"), username, cmis_backend->display_name);
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
    {
        libcmis_session_free (cmis_backend->session);
        cmis_backend->session = NULL;
    }

    if (cmis_backend->display_name)
    {
        g_free (cmis_backend->display_name);
        cmis_backend->display_name = NULL;
    }
    g_vfs_job_succeeded (G_VFS_JOB (job));
}

static void
do_open_for_read (GVfsBackend *backend,
                   GVfsJobOpenForRead *job,
                   const char *filename)
{
    GVfsBackendCmis *cmis_backend = G_VFS_BACKEND_CMIS (backend);
    struct TmpHandle *handle;
    GError *gerror = NULL;
    libcmis_ObjectPtr object = NULL;
    libcmis_DocumentPtr document = NULL;
    char *repository_id = NULL;
    char *path = NULL;

    g_print ("+do_open_for_read: %s\n", filename);
   
    if (!is_cmis_session_ready (cmis_backend, G_VFS_JOB (job)))
        return;

    /* Open a tmp file and put the document content stream in it */
    handle = g_new (struct TmpHandle, 1);
    handle->file = g_file_new_tmp ("gvfs-cmis-stream-XXXXXX", &handle->stream, &gerror);
    if (gerror)
    {
        g_vfs_job_failed_from_error (G_VFS_JOB (job), gerror );
        g_error_free (gerror);
	g_free (handle);
        return;
    }

    repository_id = extract_repository_from_path (filename, &path);
    if (!path || strlen (path) == 0)
    {
        g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_NOT_REGULAR_FILE,
                _("Root folder can't be opened for reading"));
	g_free (handle);
    }
    else
    {
        object = get_cmis_object (G_VFS_JOB (job), cmis_backend->session, repository_id, path);

        if (object && libcmis_is_document (object))
        {
            libcmis_ErrorPtr error;

            document = libcmis_document_cast (object);
            error = libcmis_error_create ();

            /* Get the content stream and write it to the GIOStream */
            libcmis_document_getContentStream (document, write_to_io_stream, handle->stream, error);

            if (libcmis_error_getMessage (error) != NULL || libcmis_error_getType(error) != NULL)
                output_cmis_error (G_VFS_JOB (job), error);
            else
            {
                /* Put the cursor back to the begining for reading */
                GSeekable *seekable = G_SEEKABLE (handle->stream);
                g_seekable_seek (seekable, 0, G_SEEK_SET, NULL, &gerror);
                if (gerror)
                {
                    g_vfs_job_failed_from_error (G_VFS_JOB (job), gerror);
                    g_error_free (gerror);
                }
                else
                {
                    /* We need to delete the tmp file later, so we need both
                     * stream and file to be passed around. */
                    g_vfs_job_open_for_read_set_handle (job, handle);
                    g_vfs_job_succeeded (G_VFS_JOB (job));
                }
            }

            libcmis_object_free (object);
            libcmis_error_free (error);
        }
    }
    
    g_print ("-do_open_for_read\n");

    /* Cleanup */
    if (path)
        g_free (path);
    if (repository_id)
        g_free (repository_id);

    return;
}

static void
do_close_read (GVfsBackend *     backend,
               GVfsJobCloseRead *job,
               GVfsBackendHandle handle)
{
    struct TmpHandle *tmp_handle = (struct TmpHandle*) handle;
    GError *error = NULL;

    g_print ("+do_close_read\n");

    g_io_stream_close (G_IO_STREAM (tmp_handle->stream), NULL, &error);
    if (error)
    {
        g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
        g_error_free (error);
    }
    else
        g_vfs_job_succeeded (G_VFS_JOB (job));


    g_file_delete (tmp_handle->file, NULL, NULL);
    g_object_unref (tmp_handle->file);

    g_free (tmp_handle);
    
    g_print ("-do_close_read\n");
}

static void
do_read (GVfsBackend *     backend,
         GVfsJobRead *     job,
         GVfsBackendHandle handle,
         char *            buffer,
         gsize             bytes_requested)
{
    struct TmpHandle *tmp_handle = (struct TmpHandle*) handle;
    GInputStream *in_stream;
    gsize bytes_read = 0;
    GError *error = NULL;

    g_print ("+do_read\n");
    in_stream = g_io_stream_get_input_stream ( G_IO_STREAM (tmp_handle->stream));

    g_input_stream_read_all (in_stream, buffer, bytes_requested, &bytes_read, NULL, &error);

    if (error != NULL)
    {
        g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
        g_error_free (error);
    }
    else
    {
        g_vfs_job_read_set_size (job, bytes_read);
        g_vfs_job_succeeded (G_VFS_JOB (job));
    }
}

static void
do_seek_on_read (GVfsBackend *backend,
                  GVfsJobSeekRead *job,
                  GVfsBackendHandle handle,
                  goffset    offset,
                  GSeekType  type)
{
  GError *error = NULL;
  struct TmpHandle *tmp_handle = (struct TmpHandle*) handle;

  g_print ("+do_seek_on_read: (handle = '%lx', offset = %ld) \n", (long int)handle, (long int)offset);

  g_assert (tmp_handle != NULL);

  if (g_seekable_seek (G_SEEKABLE (G_IO_STREAM (tmp_handle->stream)), offset, type, G_VFS_JOB (job)->cancellable, &error)) {
    g_vfs_job_seek_read_set_offset (job, g_seekable_tell (G_SEEKABLE (G_IO_STREAM (tmp_handle->stream))));
    g_print ("(II) try_seek_on_read success. \n");
  } else  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), error); 
    g_print ("  (EE) try_seek_on_read: g_file_input_stream_seek() failed, error: %s \n", error->message);
    g_error_free (error);
  }
  
  g_print ("-do_seek_on_read\n");

}

static void
do_create (GVfsBackend *backend,
           GVfsJobOpenForWrite *job,
           const char *filename,
           GFileCreateFlags flags)
{
    GVfsBackendCmis *cmis_backend = G_VFS_BACKEND_CMIS (backend);
    struct TmpHandle *handle;
    GError *gerror = NULL;
    char *repository_id = NULL;
    char *path = NULL;
    libcmis_ObjectPtr object = NULL;

    g_print ("+do_create: %s\n", filename);

    if (!is_cmis_session_ready (cmis_backend, G_VFS_JOB (job)))
        return;

    /* Create a temporary file to get the content before pushing it to the server */
    handle = g_new (struct TmpHandle, 1);
    handle->file = g_file_new_tmp ("gvfs-cmis-stream-XXXXXX", &handle->stream, &gerror);
    handle->filename = g_strdup (filename);
    if (gerror)
    {
        g_vfs_job_failed_from_error (G_VFS_JOB (job), gerror);
        g_error_free (gerror);
        return;
    }

    repository_id = extract_repository_from_path (filename, &path);
    if (!path || strlen (path) == 0)
    {
        g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_NOT_REGULAR_FILE,
                _("Root folder can't be opened for writing"));
    }
    else
    {
        /* Make sure that the object exists */
        object = get_cmis_object (G_VFS_JOB (job), cmis_backend->session, repository_id, path);

        if (object && libcmis_is_document (object))
        {
            g_vfs_job_open_for_write_set_can_seek (job, TRUE);
            g_vfs_job_open_for_write_set_handle (job, handle);
            g_vfs_job_succeeded (G_VFS_JOB (job));
        }
        else
        {
            g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_NOT_REGULAR_FILE,
                    _("Can't be opened for writing"));
        }

    }

    g_print ("-do_create\n");
}

static void
do_append (GVfsBackend *backend,
           GVfsJobOpenForWrite *job,
           const char *filename,
           GFileCreateFlags flags)
{
    g_print ("TODO Implement do_append\n");
    g_vfs_job_succeeded (G_VFS_JOB (job));
}

static void
do_replace (GVfsBackend *backend,
            GVfsJobOpenForWrite *job,
            const char *filename,
            const char *etag,
            gboolean make_backup,
            GFileCreateFlags flags)
{
    g_print ("TODO Implement do_replace\n");
    g_vfs_job_succeeded (G_VFS_JOB (job));
}

static void
do_close_write (GVfsBackend *backend,
                GVfsJobCloseWrite *job,
                GVfsBackendHandle handle)
{
    GVfsBackendCmis *cmis_backend = G_VFS_BACKEND_CMIS (backend);
    struct TmpHandle *tmp_handle = (struct TmpHandle*) handle;
    GError *error = NULL;
    char *repository_id = NULL;
    char *path = NULL;
    libcmis_ObjectPtr object = NULL;

    g_print ("+do_close_write\n");

    /* TODO Set the content stream */
    
    if (!is_cmis_session_ready (cmis_backend, G_VFS_JOB (job)))
        return;

    repository_id = extract_repository_from_path (tmp_handle->filename, &path);
    object = get_cmis_object (G_VFS_JOB (job), cmis_backend->session, repository_id, path);

    if (object && libcmis_is_document (object))
    {
        libcmis_DocumentPtr document = libcmis_document_cast (object);
        /* libcmis_document_setContentStream (document, read_from_io_stream, handle->stream, ); */
    }

    /* Close the streams and delete the tmp file */
    g_io_stream_close (G_IO_STREAM (tmp_handle->stream), NULL, &error);
    if (error)
    {
        g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
        g_error_free (error);
    }
    else
        g_vfs_job_succeeded (G_VFS_JOB (job));


    g_file_delete (tmp_handle->file, NULL, NULL);
    g_object_unref (tmp_handle->file);
    g_free (tmp_handle->filename);
    g_free (tmp_handle);
}

static void
do_write (GVfsBackend *backend,
          GVfsJobWrite *job,
          GVfsBackendHandle handle,
          char *buffer,
          gsize buffer_size)
{
    g_print ("TODO Implement do_write\n");
    g_vfs_job_succeeded (G_VFS_JOB (job));
}

static void
do_query_info (GVfsBackend *backend,
               GVfsJobQueryInfo *job,
               const char *filename,
               GFileQueryInfoFlags query_flags,
               GFileInfo *info,
               GFileAttributeMatcher *matcher)
{
    GVfsBackendCmis *cmis_backend = G_VFS_BACKEND_CMIS (backend);
    gchar *repository_id = NULL;
    gchar *path = NULL;

    g_print ("+do_query_info: %s\n", filename);

    if (!is_cmis_session_ready (cmis_backend, G_VFS_JOB (job)))
        return;

    repository_id = extract_repository_from_path (filename, &path);

    if (repository_id)
    {
        if (libcmis_session_setRepository (cmis_backend->session, repository_id))
        {
            if (!path || strlen(path) == 0)
            {
                libcmis_RepositoryPtr repo;

                repo = libcmis_session_getRepository (cmis_backend->session, NULL);
                if (repo)
                {
                    repository_to_file_info (repo, info);
                    g_vfs_job_succeeded (G_VFS_JOB (job));
                }
                else
                {
                    g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR,
                                         _("Failed to get repository infos"));
                }

                libcmis_repository_free (repo);
            }
            else
            {
                libcmis_ObjectPtr object;

                object = get_cmis_object (G_VFS_JOB (job), cmis_backend->session, repository_id, path);

                if (object)
                {
                    cmis_object_to_file_info (object, info);
                    g_vfs_job_succeeded (G_VFS_JOB (job));

                    libcmis_object_free (object);
                }
            }
        }
        else
        {
            char *message;
            message = g_strdup_printf (_("No such repository: %s"), repository_id);
            g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                                 G_IO_ERROR_NOT_FOUND,
                                 message );
            g_free (message);
        }
    }
    else
    {
        GIcon *icon = NULL;
        GIcon *symbolic_icon = NULL;

        /* Not much infos to provide for the mounted server itself */
        g_file_info_set_name (info, "/");
        g_file_info_set_display_name (info, cmis_backend->display_name);

        /* Repositories can't be edited through CMIS */
        g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, FALSE);
        g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE, FALSE);
        g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, FALSE);

        g_file_info_set_file_type (info, G_FILE_TYPE_DIRECTORY);
        g_file_info_set_content_type (info, "inode/directory");

        icon = g_themed_icon_new ("folder-remote");
        symbolic_icon = g_themed_icon_new ("folder-remote-symbolic");

        g_file_info_set_icon (info, icon);
        g_object_unref (icon);
        g_file_info_set_symbolic_icon (info, symbolic_icon);
        g_object_unref (symbolic_icon);

        g_vfs_job_succeeded (G_VFS_JOB (job));
    }

    g_print ("-do_query_info\n");
}

static void
do_enumerate (GVfsBackend *backend,
              GVfsJobEnumerate *job,
              const char *dirname,
              GFileAttributeMatcher *matcher,
              GFileQueryInfoFlags query_flags)
{
    g_print ("+do_enumerate: %s\n", dirname);

    GVfsBackendCmis *cmis_backend = G_VFS_BACKEND_CMIS (backend);
    gchar *repository_id = NULL;
    gchar *path = NULL;

    if (!is_cmis_session_ready (cmis_backend, G_VFS_JOB (job)))
        return;

    /* Split the dirname into repository and path.
     * Repository id will be the first segment, path is what follows.
     * Not having any repository id will just enumerate the repositories. */
    repository_id = extract_repository_from_path (dirname, &path);

    if ((!repository_id || strlen(repository_id) == 0))
    {
        /* We don't have a repository, then list the repositories as folders,
         * whatever the dirname is */
        libcmis_vector_Repository_Ptr repositories = NULL;

        repositories = libcmis_session_getRepositories (cmis_backend->session);

        size_t repositories_count = libcmis_vector_repository_size (repositories);
        size_t i;
        
        for (i = 0; i < repositories_count; ++i)
        {
            libcmis_RepositoryPtr repo;
            GFileInfo *info;

            repo = libcmis_vector_repository_get (repositories, i);
            info = g_file_info_new ();

            repository_to_file_info (repo, info);

            g_vfs_job_enumerate_add_info (job, info);
        }

        libcmis_vector_repository_free (repositories);
        g_vfs_job_succeeded (G_VFS_JOB (job));
        g_vfs_job_enumerate_done (G_VFS_JOB_ENUMERATE (job));
    }
    else
    {
        libcmis_ObjectPtr object = NULL;
        libcmis_ErrorPtr error;

        if (strlen (path) == 0)
        {
            g_free (path);
            path = g_strdup ("/");
        }

        /* List the files and folders for the given directory name */
        object = get_cmis_object (G_VFS_JOB (job), cmis_backend->session, repository_id, path);


        error = libcmis_error_create ();

        if (object && libcmis_is_folder (object))
        {
            libcmis_FolderPtr parent = NULL;
            libcmis_vector_object_Ptr children = NULL;

            parent = libcmis_folder_cast (object);
            children = libcmis_folder_getChildren (parent, error);

            if (libcmis_error_getMessage (error) != NULL || libcmis_error_getType(error) != NULL)
                output_cmis_error (G_VFS_JOB (job), error);
            else
            {
                size_t objects_count = 0;
                size_t i;

                /* Convert all instances of libcmis_Object into GFileInfo */
                objects_count = libcmis_vector_object_size (children);
                for (i = 0; i < objects_count; ++i)
                {
                    libcmis_ObjectPtr child;
                    GFileInfo *info;

                    child = libcmis_vector_object_get (children, i);
                    info = g_file_info_new ();

                    cmis_object_to_file_info (child, info);

                    g_vfs_job_enumerate_add_info (job, info);
                }

                g_vfs_job_succeeded (G_VFS_JOB (job));
                g_vfs_job_enumerate_done (G_VFS_JOB_ENUMERATE (job));
            }

            libcmis_vector_object_free (children);
            libcmis_object_free (object);
        }
        else
        {
            char *message;
            message = g_strdup_printf (_("Not a valid directory: %s"), path);
            g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY,
                                 message );
            g_free (message);
        }

        libcmis_error_free (error);
    }

    /* Clean up */
    g_free (repository_id);
    g_free (path);
    g_print ("-do_enumerate\n");
}

static void
do_set_display_name (GVfsBackend *backend,
                     GVfsJobSetDisplayName *job,
                     const char *filename,
                     const char *display_name)
{
    g_print ("TODO Implement do_set_display_name\n");
    g_vfs_job_succeeded (G_VFS_JOB (job));
}

static void
do_delete (GVfsBackend *backend,
           GVfsJobDelete *job,
           const char *filename)
{
    g_print ("TODO Implement do_delete\n");
    g_vfs_job_succeeded (G_VFS_JOB (job));
}

static void
do_make_directory (GVfsBackend *backend,
                   GVfsJobMakeDirectory *job,
                   const char *filename)
{
    g_print ("TODO Implement do_make_directory\n");
    g_vfs_job_succeeded (G_VFS_JOB (job));
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
    g_print ("TODO Implement do_move\n");
    g_vfs_job_succeeded (G_VFS_JOB (job));
}

static gboolean
try_query_settable_attributes (GVfsBackend *backend,
			       GVfsJobQueryAttributes *job,
			       const char *filename)
{
    g_print ("TODO Implement try_query_settable_attributes\n");
    g_vfs_job_succeeded (G_VFS_JOB (job));
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
    g_print ("TODO Implement do_set_attribute\n");
    g_vfs_job_succeeded (G_VFS_JOB (job));
}

static void
do_pull (GVfsBackend *         backend,
         GVfsJobPull *         job,
         const char *          filename,
         const char *          local_path,
         GFileCopyFlags        flags,
         gboolean              remove_source,
         GFileProgressCallback progress_callback,
         gpointer              progress_callback_data)
{
    GVfsBackendCmis *cmis_backend = G_VFS_BACKEND_CMIS (backend);
    GError *gerror = NULL;
    libcmis_ObjectPtr object = NULL;
    libcmis_DocumentPtr document = NULL;
    char *repository_id = NULL;
    char *path = NULL;
    libcmis_ErrorPtr error;
    GFile *file;
    GFileOutputStream *stream;

    g_print ("+do_pull\n");
    if (!is_cmis_session_ready (cmis_backend, G_VFS_JOB (job)))
        return;

    file = g_file_new_for_path (local_path);
    if (flags & G_FILE_COPY_OVERWRITE)
      stream = g_file_replace (file,
			       NULL, /* We don't have etags */
			       flags & G_FILE_COPY_BACKUP ? TRUE : FALSE,
			       G_FILE_CREATE_REPLACE_DESTINATION,
			       NULL, /* We can't cancel yet */
			       &gerror);
    else
      stream = g_file_create (file,
			      G_FILE_CREATE_NONE,
			      NULL, /* We can't cancel yet */
			      &gerror);
    
    if (stream == NULL) 
      {
	g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
			  G_IO_ERROR_INVALID_ARGUMENT,
			  _("File could not be created"));
	goto do_pull_finish;
      }

    repository_id = extract_repository_from_path (filename, &path);
    if (!path || strlen (path) == 0)
    {
        g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_NOT_REGULAR_FILE,
                _("Root folder can't be opened for reading"));
	goto do_pull_finish;
    }
    
    object = get_cmis_object (G_VFS_JOB (job), cmis_backend->session, repository_id, path);

    if (!object || !libcmis_is_document (object))
      {
	output_cmis_error (G_VFS_JOB (job), error);
	goto do_pull_finish;
      }

    document = libcmis_document_cast (object);
    error = libcmis_error_create ();
    
    /* Get the content stream and write it to the GIOStream */
    libcmis_document_getContentStream (document, write_to_g_output_stream, stream, error);
    
    if (libcmis_error_getMessage (error) != NULL || libcmis_error_getType(error) != NULL) 
      {
	output_cmis_error (G_VFS_JOB (job), error);
	goto do_pull_finish;
      }
    
    g_output_stream_close ((GOutputStream*) stream, NULL, &gerror);
    if (gerror)
      {
        g_vfs_job_failed_from_error (G_VFS_JOB (job), gerror);
	goto do_pull_finish;
      }

    if (remove_source)
      {
	/* FIXME: We need to initiate a delete operation */
      }

    g_vfs_job_succeeded (G_VFS_JOB (job));

 do_pull_finish:
    g_object_unref (file);
    g_print ("-do_pull\n");
}

static void
g_vfs_backend_cmis_init (GVfsBackendCmis *cmis_backend)
{
    cmis_backend->session = NULL; 
    cmis_backend->display_name = NULL;
}

static void
g_vfs_backend_cmis_finalize (GObject *object)
{
    GVfsBackendCmis *cmis_backend = G_VFS_BACKEND_CMIS (object);

    /* Should have been done by do_unmount, but better be sure it's clean */
    if (cmis_backend->session)
        libcmis_session_free (cmis_backend->session);

    if (cmis_backend->display_name)
        g_free (cmis_backend->display_name);
    
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
    backend_class->seek_on_read = do_seek_on_read;
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
}
