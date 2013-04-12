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

#ifndef __G_VFS_BACKEND_CMIS_H__
#define __G_VFS_BACKEND_CMIS_H__

#include <libcmis-c/session.h>

#include <gvfsbackend.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_BACKEND_CMIS         (g_vfs_backend_cmis_get_type ())
#define G_VFS_BACKEND_CMIS(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_BACKEND_CMIS, GVfsBackendCmis))
#define G_VFS_BACKEND_CMIS_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_BACKEND_CMIS, GVfsBackendCmisClass))
#define G_VFS_IS_BACKEND_CMIS(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_BACKEND_CMIS))
#define G_VFS_IS_BACKEND_CMIS_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_BACKEND_CMIS))
#define G_VFS_BACKEND_CMIS_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_BACKEND_CMIS, GVfsBackendCmisClass))

typedef struct _GVfsBackendCmis         GVfsBackendCmis;
typedef struct _GVfsBackendCmisClass    GVfsBackendCmisClass;

struct _GVfsBackendCmis
{
    GVfsBackend     backend;
    libcmis_SessionPtr session;
    char* display_name;
};

struct _GVfsBackendCmisClass
{
    GVfsBackendClass parent_class;
};


G_END_DECLS

#endif /* __G_VFS_BACKEND_CMIS_H__ */
