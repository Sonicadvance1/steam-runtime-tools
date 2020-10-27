/*
 * Adapted from xdg-desktop-portal
 *
 * Copyright © 2016 Red Hat, Inc
 * Copyright © 2020 Collabora Ltd.
 *
 * SPDX-License-Identifier: LGPL-2-or-later
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 *       Matthias Clasen <mclasen@redhat.com>
 */

#pragma once

#include "steam-runtime-tools/glib-backports-internal.h"
#include "libglnx/libglnx.h"

#include "portal-session.h"
#include "xdp-dbus.h"

typedef enum {
  XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS = 0,
  XDG_DESKTOP_PORTAL_RESPONSE_CANCELLED,
  XDG_DESKTOP_PORTAL_RESPONSE_OTHER
} XdgDesktopPortalResponseEnum;

struct _PvPortalRequest
{
  XdpRequestSkeleton parent_instance;

  gboolean exported;
  char *id;
  char *token;
  GDBusConnection *connection;
  char *sender;
  GMutex mutex;
  PvPortalSession *session;
  int fd;
};

struct _PvPortalRequestClass
{
  XdpRequestSkeletonClass parent_class;
};

#define PV_TYPE_PORTAL_REQUEST (pv_portal_request_get_type ())
#define PV_PORTAL_REQUEST(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), PV_TYPE_PORTAL_REQUEST, PvPortalRequest))
#define PV_PORTAL_REQUEST_CLASS(cls) (G_TYPE_CHECK_CLASS_CAST ((cls), PV_TYPE_PORTAL_REQUEST, PvPortalRequestClass))
#define PV_IS_PORTAL_REQUEST(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), PV_TYPE_PORTAL_REQUEST))
#define PV_IS_PORTAL_REQUEST_CLASS(cls) (G_TYPE_CHECK_CLASS_TYPE ((cls), PV_TYPE_PORTAL_REQUEST))
#define PV_PORTAL_REQUEST_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS((obj), PV_TYPE_PORTAL_REQUEST, PvPortalRequestClass))
GType pv_portal_request_get_type (void);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (PvPortalRequest, g_object_unref)

PvPortalRequest *pv_portal_request_new (GDBusMethodInvocation *invocation,
                                        const char *token);

void pv_portal_request_export (PvPortalRequest *request);
void pv_portal_request_unexport (PvPortalRequest *request);

void pv_portal_request_close_for_peer (GDBusConnection *connection);
void pv_portal_request_close_for_session_bus (const char *sender);

static inline void
auto_unlock_helper (GMutex **mutex)
{
  if (*mutex)
    g_mutex_unlock (*mutex);
}

static inline GMutex *
auto_lock_helper (GMutex *mutex)
{
  if (mutex)
    g_mutex_lock (mutex);
  return mutex;
}

#define REQUEST_AUTOLOCK(request) G_GNUC_UNUSED __attribute__((cleanup (auto_unlock_helper))) GMutex * G_PASTE (request_auto_unlock, __LINE__) = auto_lock_helper (&request->mutex);
