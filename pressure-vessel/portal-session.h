/*
 * Adapted from xdg-desktop-portal
 *
 * Copyright © 2017 Red Hat, Inc
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
 */

#pragma once

#include "steam-runtime-tools/glib-backports-internal.h"
#include "libglnx/libglnx.h"

#include "xdp-dbus.h"

/* Forward declaration */
typedef struct _PvPortalRequest PvPortalRequest;
typedef struct _PvPortalRequestClass PvPortalRequestClass;

typedef struct _PvPortalSession PvPortalSession;
typedef struct _PvPortalSessionClass PvPortalSessionClass;

struct _PvPortalSession
{
  XdpSessionSkeleton parent;

  GMutex mutex;

  gboolean exported;
  gboolean closed;

  char *id;
  char *token;

  char *sender;
  GDBusConnection *connection;
};

struct _PvPortalSessionClass
{
  XdpSessionSkeletonClass parent_class;

  void (*close) (PvPortalSession *session);
};

#define PV_TYPE_PORTAL_SESSION (pv_portal_session_get_type ())
#define PV_PORTAL_SESSION(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), PV_TYPE_PORTAL_SESSION, PvPortalSession))
#define PV_PORTAL_SESSION_CLASS(cls) (G_TYPE_CHECK_CLASS_CAST ((cls), PV_TYPE_PORTAL_SESSION, PvPortalSessionClass))
#define PV_IS_PORTAL_SESSION(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), PV_TYPE_PORTAL_SESSION))
#define PV_IS_PORTAL_SESSION_CLASS(cls) (G_TYPE_CHECK_CLASS_TYPE ((cls), PV_TYPE_PORTAL_SESSION))
#define PV_PORTAL_SESSION_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS((obj), PV_TYPE_PORTAL_SESSION, PvPortalSessionClass))
GType pv_portal_session_get_type (void);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (PvPortalSession, g_object_unref)

PvPortalSession *pv_portal_session_acquire (const char *session_handle,
                                            PvPortalRequest *request);

gboolean pv_portal_session_export (PvPortalSession *session,
                                   GError **error);

void pv_portal_session_close (PvPortalSession *session,
                              gboolean notify_close);

void pv_portal_session_close_for_peer (GDBusConnection *connection);
void pv_portal_session_close_for_session_bus (const char *sender);

static inline void
auto_session_unlock_unref_helper (PvPortalSession **session)
{
  if (!*session)
    return;

  g_mutex_unlock (&(*session)->mutex);
  g_object_unref (*session);
}

static inline PvPortalSession *
auto_session_lock_helper (PvPortalSession *session)
{
  if (session)
    g_mutex_lock (&session->mutex);
  return session;
}

#define SESSION_AUTOLOCK(session) \
  G_GNUC_UNUSED __attribute__((cleanup (auto_unlock_helper))) \
  GMutex * G_PASTE (session_auto_unlock, __LINE__) = \
    auto_lock_helper (&session->mutex);

#define SESSION_AUTOLOCK_UNREF(session) \
  G_GNUC_UNUSED __attribute__((cleanup (auto_session_unlock_unref_helper))) \
  PvPortalSession * G_PASTE (session_auto_unlock_unref, __LINE__) = \
    auto_session_lock_helper (session);
