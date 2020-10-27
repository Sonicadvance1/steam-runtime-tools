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

#include "config.h"
#include "subprojects/libglnx/config.h"

#include "portal-request.h"

#include <string.h>

static void request_skeleton_iface_init (XdpRequestIface *iface);

G_DEFINE_TYPE_WITH_CODE (PvPortalRequest, pv_portal_request, XDP_TYPE_REQUEST_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_TYPE_REQUEST, request_skeleton_iface_init))

/* Instead of sending the response as a broadcast, we send it as a
 * unicast to each connection on which the Request is exported
 * (in practice there is only one). */
static void
request_on_signal_response (XdpRequest *object,
                            guint arg_response,
                            GVariant *arg_results)
{
  PvPortalRequest *request = PV_PORTAL_REQUEST (object);
  XdpRequestSkeleton *skeleton = XDP_REQUEST_SKELETON (object);
  GList *connections, *l;
  GVariant *signal_variant;

  connections = g_dbus_interface_skeleton_get_connections (G_DBUS_INTERFACE_SKELETON (skeleton));

  signal_variant = g_variant_ref_sink (g_variant_new ("(u@a{sv})",
                                                      arg_response,
                                                      arg_results));
  for (l = connections; l != NULL; l = l->next)
    {
      GDBusConnection *connection = l->data;
      g_dbus_connection_emit_signal (connection,
                                     request->sender,
                                     g_dbus_interface_skeleton_get_object_path (G_DBUS_INTERFACE_SKELETON (skeleton)),
                                     "org.freedesktop.portal.Request",
                                     "Response",
                                     signal_variant,
                                     NULL);
    }
  g_variant_unref (signal_variant);
  g_list_free_full (connections, g_object_unref);
}

static gboolean
handle_close (XdpRequest *object,
              GDBusMethodInvocation *invocation)
{
  PvPortalRequest *request = PV_PORTAL_REQUEST (object);

  g_debug ("Handling Close");
  REQUEST_AUTOLOCK (request);

  if (request->exported)
    pv_portal_request_unexport (request);

  if (invocation)
    xdp_request_complete_close (XDP_REQUEST (request), invocation);

  return TRUE;
}

static void
request_skeleton_iface_init (XdpRequestIface *iface)
{
  iface->handle_close = handle_close;
  iface->response = request_on_signal_response;
}

G_LOCK_DEFINE (requests);
static GHashTable *requests;

static void
pv_portal_request_init (PvPortalRequest *request)
{
  g_mutex_init (&request->mutex);
}

static void
request_dispose (GObject *object)
{
  PvPortalRequest *request = PV_PORTAL_REQUEST (object);

  G_LOCK (requests);
  g_hash_table_remove (requests, request->id);
  G_UNLOCK (requests);

  g_clear_object (&request->connection);
  g_clear_object (&request->session);

  G_OBJECT_CLASS (pv_portal_request_parent_class)->dispose (object);
}

static void
request_finalize (GObject *object)
{
  PvPortalRequest *request = PV_PORTAL_REQUEST (object);

  /* It must have been unexported, which closes the fd, because a
   * ref is held as long as it's exported */
  g_return_if_fail (request->fd == -1);

  g_free (request->sender);
  g_free (request->id);
  g_mutex_clear (&request->mutex);

  G_OBJECT_CLASS (pv_portal_request_parent_class)->finalize (object);
}

static void
pv_portal_request_class_init (PvPortalRequestClass *klass)
{
  GObjectClass *gobject_class;

  requests = g_hash_table_new_full (g_str_hash, g_str_equal,
                                    NULL, NULL);

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->dispose  = request_dispose;
  gobject_class->finalize  = request_finalize;
}

static gboolean
request_authorize_callback (GDBusInterfaceSkeleton *interface,
                            GDBusMethodInvocation  *invocation,
                            gpointer                user_data)
{
  PvPortalRequest *request = PV_PORTAL_REQUEST (interface);
  GDBusConnection *connection = g_dbus_method_invocation_get_connection (invocation);
  const gchar *sender = g_dbus_method_invocation_get_sender (invocation);

  if (connection != request->connection)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Portal operation not allowed: Unmatched connection");
      return FALSE;
    }

  if (g_strcmp0 (sender, request->sender) != 0)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Portal operation not allowed: Unmatched caller");
      return FALSE;
    }

  return TRUE;
}

PvPortalRequest *
pv_portal_request_new (GDBusMethodInvocation *invocation,
                       const char *token)
{
  PvPortalRequest *request;
  guint32 r;
  char *id = NULL;
  g_autofree char *sender = NULL;
  int i;

  request = g_object_new (pv_portal_request_get_type (),
                          NULL);
  request->connection = g_object_ref (g_dbus_method_invocation_get_connection (invocation));
  request->sender = g_strdup (g_dbus_method_invocation_get_sender (invocation));
  request->fd = -1;

  if (request->sender == NULL)
    {
      sender = g_strdup ("_");
    }
  else
    {
      sender = g_strdup (request->sender + 1);

      for (i = 0; sender[i]; i++)
        {
          if (sender[i] == '.')
            sender[i] = '_';
        }
    }

  id = g_strdup_printf ("/org/freedesktop/portal/desktop/request/%s/%s", sender, token);

  G_LOCK (requests);

  while (g_hash_table_lookup (requests, id) != NULL)
    {
      r = g_random_int ();
      g_free (id);
      id = g_strdup_printf ("/org/freedesktop/portal/desktop/request/%s/%s/%u", sender, token, r);
    }

  request->id = id;
  g_hash_table_insert (requests, id, request);

  G_UNLOCK (requests);

  g_dbus_interface_skeleton_set_flags (G_DBUS_INTERFACE_SKELETON (request),
                                       G_DBUS_INTERFACE_SKELETON_FLAGS_HANDLE_METHOD_INVOCATIONS_IN_THREAD);
  g_signal_connect (request, "g-authorize-method",
                    G_CALLBACK (request_authorize_callback),
                    NULL);
  return request;
}

void
pv_portal_request_export (PvPortalRequest *request)
{
  g_autoptr(GError) error = NULL;

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (request),
                                         request->connection,
                                         request->id,
                                         &error))
    g_warning ("Error exporting request: %s", error->message);

  g_object_ref (request);
  request->exported = TRUE;
}

void
pv_portal_request_unexport (PvPortalRequest *request)
{
  g_return_if_fail (request->exported);

  if (request->fd != -1)
    {
      close (request->fd);
      request->fd = -1;
    }

  request->exported = FALSE;
  g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (request));
  g_object_unref (request);
}

static gboolean
close_peer_requests (gpointer user_data)
{
  GDBusConnection *peer = G_DBUS_CONNECTION (user_data);
  GSList *list = NULL;
  GSList *l;
  GHashTableIter iter;
  gpointer v;

  G_LOCK (requests);
  if (requests)
    {
      g_hash_table_iter_init (&iter, requests);
      while (g_hash_table_iter_next (&iter, NULL, &v))
        {
          PvPortalRequest *request = v;

          if (peer == request->connection)
            list = g_slist_prepend (list, g_object_ref (request));
        }
    }
  G_UNLOCK (requests);

  for (l = list; l; l = l->next)
    {
      PvPortalRequest *request = l->data;

      REQUEST_AUTOLOCK (request);

      if (request->exported)
        pv_portal_request_unexport (request);
    }

  g_slist_free_full (list, g_object_unref);
  return G_SOURCE_REMOVE;
}

void
pv_portal_request_close_for_peer (GDBusConnection *connection)
{
  g_idle_add_full (G_PRIORITY_DEFAULT_IDLE, close_peer_requests,
                   g_object_ref (connection), g_object_unref);
}

static gboolean
close_requests (gpointer user_data)
{
  const char *sender = user_data;
  GSList *list = NULL;
  GSList *l;
  GHashTableIter iter;
  gpointer v;

  G_LOCK (requests);
  if (requests)
    {
      g_hash_table_iter_init (&iter, requests);
      while (g_hash_table_iter_next (&iter, NULL, &v))
        {
          PvPortalRequest *request = v;

          if (g_strcmp0 (sender, request->sender) == 0)
            list = g_slist_prepend (list, g_object_ref (request));
        }
    }
  G_UNLOCK (requests);

  for (l = list; l; l = l->next)
    {
      PvPortalRequest *request = l->data;

      REQUEST_AUTOLOCK (request);

      if (request->exported)
        pv_portal_request_unexport (request);
    }

  g_slist_free_full (list, g_object_unref);
  return G_SOURCE_REMOVE;
}

void
pv_portal_request_close_for_session_bus (const char *sender)
{
  g_idle_add_full (G_PRIORITY_DEFAULT_IDLE, close_requests,
                   g_strdup (sender), g_free);
}
