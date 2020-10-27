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

#include "portal-session.h"
#include "input-portal.h"
#include "xdp-dbus.h"

typedef struct _PvPortalInputSession PvPortalInputSession;
typedef struct _PvPortalInputSessionClass PvPortalInputSessionClass;

#define PV_TYPE_PORTAL_INPUT_SESSION (pv_portal_input_session_get_type ())
#define PV_PORTAL_INPUT_SESSION(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), PV_TYPE_PORTAL_INPUT_SESSION, PvPortalInputSession))
#define PV_PORTAL_INPUT_SESSION_CLASS(cls) (G_TYPE_CHECK_CLASS_CAST ((cls), PV_TYPE_PORTAL_INPUT_SESSION, PvPortalInputSessionClass))
#define PV_IS_PORTAL_INPUT_SESSION(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), PV_TYPE_PORTAL_INPUT_SESSION))
#define PV_IS_PORTAL_INPUT_SESSION_CLASS(cls) (G_TYPE_CHECK_CLASS_TYPE ((cls), PV_TYPE_PORTAL_INPUT_SESSION))
#define PV_PORTAL_INPUT_SESSION_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS((obj), PV_TYPE_PORTAL_INPUT_SESSION, PvPortalInputSessionClass)
GType pv_portal_input_session_get_type (void);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (PvPortalInputSession, g_object_unref)

gboolean pv_input_portal_export (GDBusConnection *connection,
                                 GError **error);

void pv_input_portal_tear_down (void);
