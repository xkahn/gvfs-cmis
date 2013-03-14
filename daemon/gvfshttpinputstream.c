/* gvfshttpinputstream.c: seekable wrapper around SoupRequestHTTP
 *
 * Copyright (C) 2006, 2007, 2012 Red Hat, Inc.
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
 */

#include <config.h>

#include <string.h>

#include <glib.h>
#include <gio/gio.h>

#define LIBSOUP_USE_UNSTABLE_REQUEST_API
#include <libsoup/soup.h>
#include <libsoup/soup-requester.h>
#include <libsoup/soup-request-http.h>

#include "gvfshttpinputstream.h"

static void g_vfs_http_input_stream_seekable_iface_init (GSeekableIface *seekable_iface);

G_DEFINE_TYPE_WITH_CODE (GVfsHttpInputStream, g_vfs_http_input_stream, G_TYPE_INPUT_STREAM,
			 G_IMPLEMENT_INTERFACE (G_TYPE_SEEKABLE,
						g_vfs_http_input_stream_seekable_iface_init))

typedef struct {
  SoupURI *uri;
  SoupRequester *requester;
  SoupRequest *req;
  SoupMessage *msg;
  GInputStream *stream;

  char *range;
  goffset offset;

} GVfsHttpInputStreamPrivate;
#define G_VFS_HTTP_INPUT_STREAM_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), G_VFS_TYPE_HTTP_INPUT_STREAM, GVfsHttpInputStreamPrivate))

static void
g_vfs_http_input_stream_init (GVfsHttpInputStream *stream)
{
  ;
}

static void
g_vfs_http_input_stream_finalize (GObject *object)
{
  GVfsHttpInputStream *stream = G_VFS_HTTP_INPUT_STREAM (object);
  GVfsHttpInputStreamPrivate *priv = G_VFS_HTTP_INPUT_STREAM_GET_PRIVATE (stream);

  g_clear_pointer (&priv->uri, soup_uri_free);
  g_clear_object (&priv->requester);
  g_clear_object (&priv->req);
  g_clear_object (&priv->msg);
  g_clear_object (&priv->stream);
  g_free (priv->range);

  G_OBJECT_CLASS (g_vfs_http_input_stream_parent_class)->finalize (object);
}

/**
 * g_vfs_http_input_stream_new:
 * @session: a #SoupSession
 * @uri: a #SoupURI
 * 
 * Prepares to send a GET request for @uri on @session, and returns a
 * #GInputStream that can be used to read the response.
 *
 * The request will not be sent until the first read call; if you need
 * to look at the status code or response headers before reading the
 * body, you can use g_vfs_http_input_stream_send() or
 * g_vfs_http_input_stream_send_async() to force the message to be
 * sent and the response headers read.
 *
 * Returns: a new #GInputStream.
 **/
GInputStream *
g_vfs_http_input_stream_new (SoupSession *session,
			     SoupURI     *uri)
{
  GVfsHttpInputStream *stream;
  GVfsHttpInputStreamPrivate *priv;

  stream = g_object_new (G_VFS_TYPE_HTTP_INPUT_STREAM, NULL);
  priv = G_VFS_HTTP_INPUT_STREAM_GET_PRIVATE (stream);

  priv->requester = (SoupRequester *)soup_session_get_feature (session, SOUP_TYPE_REQUESTER);
  g_object_ref (priv->requester);
  priv->uri = soup_uri_copy (uri);

  return G_INPUT_STREAM (stream);
}

static SoupRequest *
g_vfs_http_input_stream_ensure_request (GInputStream *stream)
{
  GVfsHttpInputStreamPrivate *priv = G_VFS_HTTP_INPUT_STREAM_GET_PRIVATE (stream);

  if (!priv->req)
    {
      GError *error = NULL;

      priv->req = soup_requester_request_uri (priv->requester, priv->uri, &error);
      g_assert_no_error (error);
      priv->msg = soup_request_http_get_message (SOUP_REQUEST_HTTP (priv->req));
      priv->offset = 0;

      if (priv->range)
	soup_message_headers_replace (priv->msg->request_headers, "Range", priv->range);
    }

  return priv->req;
}

/**
 * g_vfs_http_input_stream_send:
 * @stream: a #GVfsHttpInputStream
 * @cancellable: optional #GCancellable object, %NULL to ignore.
 * @error: location to store the error occuring, or %NULL to ignore
 *
 * Synchronously sends the HTTP request associated with @stream, and
 * reads the response headers. Call this after g_vfs_http_input_stream_new()
 * and before the first g_input_stream_read() if you want to check the
 * HTTP status code before you start reading.
 *
 * Return value: %TRUE if msg was successfully sent, %FALSE if not
 **/
gboolean
g_vfs_http_input_stream_send (GInputStream  *stream,
			      GCancellable  *cancellable,
			      GError       **error)
{
  GVfsHttpInputStreamPrivate *priv;

  g_return_val_if_fail (G_VFS_IS_HTTP_INPUT_STREAM (stream), FALSE);
  priv = G_VFS_HTTP_INPUT_STREAM_GET_PRIVATE (stream);

  if (priv->stream)
    return TRUE;

  if (!g_input_stream_set_pending (stream, error))
    return FALSE;
  g_vfs_http_input_stream_ensure_request (stream);
  priv->stream = soup_request_send (priv->req, cancellable, error);
  g_input_stream_clear_pending (stream);

  return priv->stream != NULL;
}

static gssize
g_vfs_http_input_stream_read (GInputStream  *stream,
			      void          *buffer,
			      gsize          count,
			      GCancellable  *cancellable,
			      GError       **error)
{
  GVfsHttpInputStreamPrivate *priv = G_VFS_HTTP_INPUT_STREAM_GET_PRIVATE (stream);
  gssize nread;

  if (!priv->stream)
    {
      g_vfs_http_input_stream_ensure_request (stream);
      priv->stream = soup_request_send (priv->req, cancellable, error);
      if (!priv->stream)
	return -1;
    }

  nread = g_input_stream_read (priv->stream, buffer, count, cancellable, error);
  if (nread > 0)
    priv->offset += nread;
  return nread;
}

static gboolean
g_vfs_http_input_stream_close (GInputStream  *stream,
			       GCancellable  *cancellable,
			       GError       **error)
{
  GVfsHttpInputStreamPrivate *priv = G_VFS_HTTP_INPUT_STREAM_GET_PRIVATE (stream);

  if (priv->stream)
    {
      if (!g_input_stream_close (priv->stream, cancellable, error))
	return FALSE;
      g_clear_object (&priv->stream);
    }

  return TRUE;
}

static void
send_callback (GObject      *object,
	       GAsyncResult *result,
	       gpointer      user_data)
{
  GTask *task = user_data;
  GInputStream *http_stream = g_task_get_source_object (task);
  GVfsHttpInputStreamPrivate *priv = G_VFS_HTTP_INPUT_STREAM_GET_PRIVATE (http_stream);
  GError *error = NULL;

  g_input_stream_clear_pending (http_stream);

  priv->stream = soup_request_send_finish (SOUP_REQUEST (object), result, &error);
  if (priv->stream)
    g_task_return_boolean (task, TRUE);
  else
    g_task_return_error (task, error);
  g_object_unref (task);
}

/**
 * g_vfs_http_input_stream_send_async:
 * @stream: a #GVfsHttpInputStream
 * @io_priority: the io priority of the request.
 * @cancellable: optional #GCancellable object, %NULL to ignore.
 * @callback: callback to call when the request is satisfied
 * @user_data: the data to pass to callback function
 *
 * Asynchronously sends the HTTP request associated with @stream, and
 * reads the response headers. Call this after g_vfs_http_input_stream_new()
 * and before the first g_input_stream_read_async() if you want to
 * check the HTTP status code before you start reading.
 **/
void
g_vfs_http_input_stream_send_async (GInputStream        *stream,
				    int                  io_priority,
				    GCancellable        *cancellable,
				    GAsyncReadyCallback  callback,
				    gpointer             user_data)
{
  GVfsHttpInputStreamPrivate *priv;
  GError *error = NULL;
  GTask *task;

  g_return_if_fail (G_VFS_IS_HTTP_INPUT_STREAM (stream));
  priv = G_VFS_HTTP_INPUT_STREAM_GET_PRIVATE (stream);

  task = g_task_new (stream, cancellable, callback, user_data);
  g_task_set_priority (task, io_priority);

  if (priv->stream)
    {
      g_task_return_boolean (task, TRUE);
      g_object_unref (task);
      return;
    }

  if (!g_input_stream_set_pending (stream, &error))
    {
      g_task_return_error (task, error);
      g_object_unref (task);
      return;
    }

  g_vfs_http_input_stream_ensure_request (stream);
  soup_request_send_async (priv->req, cancellable,
			   send_callback, task);
}

/**
 * g_vfs_http_input_stream_send_finish:
 * @stream: a #GVfsHttpInputStream
 * @result: a #GAsyncResult.
 * @error: a #GError location to store the error occuring, or %NULL to 
 * ignore.
 *
 * Finishes a g_vfs_http_input_stream_send_async() operation.
 *
 * Return value: %TRUE if the message was sent successfully, %FALSE if
 * not.
 **/
gboolean
g_vfs_http_input_stream_send_finish (GInputStream  *stream,
				     GAsyncResult  *result,
				     GError       **error)
{
  g_return_val_if_fail (g_task_is_valid (result, stream), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

static void
read_callback (GObject      *object,
	       GAsyncResult *result,
	       gpointer      user_data)
{
  GTask *task = user_data;
  GInputStream *vfsstream = g_task_get_source_object (task);
  GVfsHttpInputStreamPrivate *priv = G_VFS_HTTP_INPUT_STREAM_GET_PRIVATE (vfsstream);
  GError *error = NULL;
  gssize nread;

  nread = g_input_stream_read_finish (G_INPUT_STREAM (object), result, &error);
  if (nread >= 0)
    {
      priv->offset += nread;
      g_task_return_int (task, nread);
    }
  else
    g_task_return_error (task, error);
  g_object_unref (task);
}

typedef struct {
  gpointer buffer;
  gsize    count;
} ReadAfterSendData;

static void
read_send_callback (GObject      *object,
		    GAsyncResult *result,
		    gpointer      user_data)
{
  GTask *task = user_data;
  GInputStream *vfsstream = g_task_get_source_object (task);
  GVfsHttpInputStreamPrivate *priv = G_VFS_HTTP_INPUT_STREAM_GET_PRIVATE (vfsstream);
  ReadAfterSendData *rasd = g_task_get_task_data (task);
  GError *error = NULL;

  if (!soup_request_send_finish (SOUP_REQUEST (object), result, &error))
    {
      g_task_return_error (task, error);
      g_object_unref (task);
      return;
    }
  if (!SOUP_STATUS_IS_SUCCESSFUL (priv->msg->status_code))
    {
      g_task_return_new_error (task,
			       SOUP_HTTP_ERROR,
			       priv->msg->status_code,
			       "%s", priv->msg->reason_phrase);
      g_object_unref (task);
      return;
    }

  g_input_stream_read_async (priv->stream, rasd->buffer, rasd->count,
			     g_task_get_priority (task),
			     g_task_get_cancellable (task),
			     read_callback, task);
}

static void
g_vfs_http_input_stream_read_async (GInputStream        *stream,
				    void                *buffer,
				    gsize                count,
				    int                  io_priority,
				    GCancellable        *cancellable,
				    GAsyncReadyCallback  callback,
				    gpointer             user_data)
{
  GVfsHttpInputStreamPrivate *priv = G_VFS_HTTP_INPUT_STREAM_GET_PRIVATE (stream);
  GTask *task;

  task = g_task_new (stream, cancellable, callback, user_data);
  g_task_set_priority (task, io_priority);

  if (!priv->stream)
    {
      ReadAfterSendData *rasd;

      rasd = g_new (ReadAfterSendData, 1);
      rasd->buffer = buffer;
      rasd->count = count;
      g_task_set_task_data (task, rasd, g_free);

      g_vfs_http_input_stream_ensure_request (stream);
      soup_request_send_async (priv->req, cancellable,
			       read_send_callback, task);
      return;
    }

  g_input_stream_read_async (priv->stream, buffer, count, io_priority,
			     cancellable, read_callback, task);
}

static gssize
g_vfs_http_input_stream_read_finish (GInputStream  *stream,
				     GAsyncResult  *result,
				     GError       **error)
{
  g_return_val_if_fail (g_task_is_valid (result, stream), -1);

  return g_task_propagate_int (G_TASK (result), error);
}

static void
close_callback (GObject      *object,
		GAsyncResult *result,
		gpointer      user_data)
{
  GTask *task = user_data;
  GError *error = NULL;

  if (g_input_stream_close_finish (G_INPUT_STREAM (object), result, &error))
    g_task_return_boolean (task, TRUE);
  else
    g_task_return_error (task, error);
  g_object_unref (task);
}

static void
g_vfs_http_input_stream_close_async (GInputStream       *stream,
				     int                 io_priority,
				     GCancellable       *cancellable,
				     GAsyncReadyCallback callback,
				     gpointer            user_data)
{
  GVfsHttpInputStreamPrivate *priv = G_VFS_HTTP_INPUT_STREAM_GET_PRIVATE (stream);
  GTask *task;

  task = g_task_new (stream, cancellable, callback, user_data);
  g_task_set_priority (task, io_priority);

  if (priv->stream == NULL)
    {
      g_task_return_boolean (task, TRUE);
      return;
    }

  g_input_stream_close_async (priv->stream, io_priority,
			      cancellable, close_callback, task);
}

static gboolean
g_vfs_http_input_stream_close_finish (GInputStream  *stream,
				      GAsyncResult  *result,
				      GError       **error)
{
  g_return_val_if_fail (g_task_is_valid (result, stream), -1);

  return g_task_propagate_boolean (G_TASK (result), error);
}

static goffset
g_vfs_http_input_stream_tell (GSeekable *seekable)
{
  GVfsHttpInputStreamPrivate *priv = G_VFS_HTTP_INPUT_STREAM_GET_PRIVATE (seekable);

  return priv->offset;
}

static gboolean
g_vfs_http_input_stream_can_seek (GSeekable *seekable)
{
  return TRUE;
}

static gboolean
g_vfs_http_input_stream_seek (GSeekable     *seekable,
			      goffset        offset,
			      GSeekType      type,
			      GCancellable  *cancellable,
			      GError       **error)
{
  GInputStream *stream = G_INPUT_STREAM (seekable);
  GVfsHttpInputStreamPrivate *priv = G_VFS_HTTP_INPUT_STREAM_GET_PRIVATE (seekable);

  if (type == G_SEEK_END && priv->msg)
    {
      goffset content_length = soup_message_headers_get_content_length (priv->msg->response_headers);

      if (content_length)
	{
	  type = G_SEEK_SET;
	  offset = content_length - offset;
	}
    }

  if (type == G_SEEK_END)
    {
      /* We could send "bytes=-offset", but since we don't know the
       * Content-Length, we wouldn't be able to answer a tell()
       * properly after that. We could maybe find the Content-Length
       * by doing a HEAD... but that would require blocking.
       */
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
	                   "G_SEEK_END not supported");
      return FALSE;
    }

  if (!g_input_stream_set_pending (stream, error))
    return FALSE;

  if (priv->stream)
    {
      if (!g_input_stream_close (priv->stream, NULL, error))
	return FALSE;
      g_clear_object (&priv->stream);
    }

  g_clear_pointer (&priv->range, g_free);

  switch (type)
    {
    case G_SEEK_CUR:
      offset += priv->offset;
      /* fall through */

    case G_SEEK_SET:
      priv->range = g_strdup_printf ("bytes=%"G_GUINT64_FORMAT"-", (guint64)offset);
      priv->offset = offset;
      break;

    case G_SEEK_END:
      g_return_val_if_reached (FALSE);
      break;

    default:
      g_return_val_if_reached (FALSE);
    }

  g_input_stream_clear_pending (stream);
  return TRUE;
}
  
static gboolean
g_vfs_http_input_stream_can_truncate (GSeekable *seekable)
{
  return FALSE;
}

static gboolean
g_vfs_http_input_stream_truncate (GSeekable     *seekable,
				  goffset        offset,
				  GCancellable  *cancellable,
				  GError       **error)
{
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
		       "Truncate not allowed on input stream");
  return FALSE;
}

SoupMessage *
g_vfs_http_input_stream_get_message (GInputStream *stream)
{
  GVfsHttpInputStreamPrivate *priv = G_VFS_HTTP_INPUT_STREAM_GET_PRIVATE (stream);

  g_vfs_http_input_stream_ensure_request (stream);
  return g_object_ref (priv->msg);
}


static void
g_vfs_http_input_stream_class_init (GVfsHttpInputStreamClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GInputStreamClass *stream_class = G_INPUT_STREAM_CLASS (klass);
  
  g_type_class_add_private (klass, sizeof (GVfsHttpInputStreamPrivate));
  
  gobject_class->finalize = g_vfs_http_input_stream_finalize;

  stream_class->read_fn = g_vfs_http_input_stream_read;
  stream_class->close_fn = g_vfs_http_input_stream_close;
  stream_class->read_async = g_vfs_http_input_stream_read_async;
  stream_class->read_finish = g_vfs_http_input_stream_read_finish;
  stream_class->close_async = g_vfs_http_input_stream_close_async;
  stream_class->close_finish = g_vfs_http_input_stream_close_finish;
}

static void
g_vfs_http_input_stream_seekable_iface_init (GSeekableIface *seekable_iface)
{
  seekable_iface->tell = g_vfs_http_input_stream_tell;
  seekable_iface->can_seek = g_vfs_http_input_stream_can_seek;
  seekable_iface->seek = g_vfs_http_input_stream_seek;
  seekable_iface->can_truncate = g_vfs_http_input_stream_can_truncate;
  seekable_iface->truncate_fn = g_vfs_http_input_stream_truncate;
}
