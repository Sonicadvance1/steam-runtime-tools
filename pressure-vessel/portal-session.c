/*
 * Copyright © 2017 Red Hat, Inc
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

#include "config.h"
#include "subprojects/libglnx/config.h"

#include "portal-session.h"
#include "portal-request.h"

#include <string.h>

enum
{
  PROP_0,

  PROP_SENDER,
  PROP_TOKEN,
  PROP_CONNECTION,

  PROP_LAST
};

static GParamSpec *obj_props[PROP_LAST];

G_LOCK_DEFINE (sessions);
static GHashTable *sessions;

static void g_initable_iface_init (GInitableIface *iface);
static void session_skeleton_iface_init (XdpSessionIface *iface);

G_DEFINE_TYPE_WITH_CODE (PvPortalSession, pv_portal_session, XDP_TYPE_SESSION_SKELETON,
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE,
                                                g_initable_iface_init)
                         G_IMPLEMENT_INTERFACE (XDP_TYPE_SESSION,
                                                session_skeleton_iface_init))

PvPortalSession *
pv_portal_session_acquire (const char *session_handle,
                           PvPortalRequest *request)
{
  g_autoptr(PvPortalSession) session = NULL;

  G_LOCK (sessions);
  session = g_hash_table_lookup (sessions, session_handle);
  if (session)
    g_object_ref (session);
  G_UNLOCK (sessions);

  if (!session)
    return NULL;

  if (session->connection != request->connection)
    return NULL;

  if (g_strcmp0 (session->sender, request->sender) != 0)
    return NULL;

  return g_steal_pointer (&session);
}

static void session_register (PvPortalSession *session);

gboolean
pv_portal_session_export (PvPortalSession *session,
                          GError **error)
{
  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (session),
                                         session->connection,
                                         session->id,
                                         error))
    return FALSE;

  g_object_ref (session);
  session->exported = TRUE;
  session_register (session);

  return TRUE;
}

static void
session_unexport (PvPortalSession *session)
{
  session->exported = FALSE;
  g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (session));
  g_object_unref (session);
}

static void
session_register (PvPortalSession *session)
{
  G_LOCK (sessions);
  g_hash_table_insert (sessions, session->id, session);
  G_UNLOCK (sessions);
}

static void
session_unregister (PvPortalSession *session)
{
  G_LOCK (sessions);
  g_hash_table_remove (sessions, session->id);
  G_UNLOCK (sessions);
}

void
pv_portal_session_close (PvPortalSession *session,
                         gboolean notify_closed)
{
  if (session->closed)
    return;

  PV_PORTAL_SESSION_GET_CLASS (session)->close (session);

  if (session->exported)
    session_unexport (session);

  session_unregister (session);

  session->closed = TRUE;

  if (notify_closed)
    {
      GVariantBuilder details_builder;

      g_variant_builder_init (&details_builder, G_VARIANT_TYPE_VARDICT);
      g_signal_emit_by_name (session, "closed",
                             g_variant_builder_end (&details_builder));
    }

  g_object_unref (session);
}

static gboolean
handle_close (XdpSession *object,
              GDBusMethodInvocation *invocation)
{
  PvPortalSession *session = PV_PORTAL_SESSION (object);

  SESSION_AUTOLOCK_UNREF (g_object_ref (session));

  pv_portal_session_close (session, FALSE);

  xdp_session_complete_close (object, invocation);

  return TRUE;
}

static void
session_skeleton_iface_init (XdpSessionIface *iface)
{
  iface->handle_close = handle_close;
}

static gboolean
close_peer_sessions (gpointer user_data)
{
  GDBusConnection *peer = G_DBUS_CONNECTION (user_data);
  GSList *list = NULL;
  GSList *l;
  GHashTableIter iter;
  gpointer v;

  G_LOCK (sessions);
  if (sessions)
    {
      g_hash_table_iter_init (&iter, sessions);
      while (g_hash_table_iter_next (&iter, NULL, &v))
        {
          PvPortalSession *session = v;

          if (peer == session->connection)
            list = g_slist_prepend (list, g_object_ref (session));
        }
    }
  G_UNLOCK (sessions);

  for (l = list; l; l = l->next)
    {
      PvPortalSession *session = l->data;

      SESSION_AUTOLOCK (session);

      if (session->exported)
        session_unexport (session);
    }

  g_slist_free_full (list, g_object_unref);

  return G_SOURCE_REMOVE;
}

void
pv_portal_session_close_for_peer (GDBusConnection *connection)
{
  g_idle_add_full (G_PRIORITY_DEFAULT_IDLE, close_peer_sessions,
                   g_object_ref (connection), g_object_unref);
}

static gboolean
close_sessions (gpointer user_data)
{
  const char *sender = user_data;
  GSList *list = NULL;
  GSList *l;
  GHashTableIter iter;
  gpointer v;

  G_LOCK (sessions);
  if (sessions)
    {
      g_hash_table_iter_init (&iter, sessions);
      while (g_hash_table_iter_next (&iter, NULL, &v))
        {
          PvPortalSession *session = v;

          if (strcmp (sender, session->sender) == 0)
            list = g_slist_prepend (list, g_object_ref (session));
        }
    }
  G_UNLOCK (sessions);

  for (l = list; l; l = l->next)
    {
      PvPortalSession *session = l->data;

      SESSION_AUTOLOCK (session);
      pv_portal_session_close (session, FALSE);
    }

  g_slist_free_full (list, g_object_unref);

  return G_SOURCE_REMOVE;
}

void
pv_portal_session_close_for_session_bus (const char *sender)
{
  g_idle_add_full (G_PRIORITY_DEFAULT_IDLE, close_sessions,
                   g_strdup (sender), g_free);
}

static gboolean
session_authorize_callback (GDBusInterfaceSkeleton *interface,
                            GDBusMethodInvocation  *invocation,
                            gpointer                user_data)
{
  PvPortalSession *session = PV_PORTAL_SESSION (interface);
  GDBusConnection *connection = g_dbus_method_invocation_get_connection (invocation);
  const gchar *sender = g_dbus_method_invocation_get_sender (invocation);

  if (connection != session->connection)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Portal operation not allowed: Unmatched connection");
      return FALSE;
    }

  if (g_strcmp0 (sender, session->sender) != 0)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Portal operation not allowed, Unmatched caller");
      return FALSE;
    }

  return TRUE;
}

static gboolean
session_initable_init (GInitable *initable,
                       GCancellable *cancellable,
                       GError **error)
{
  PvPortalSession *session = PV_PORTAL_SESSION (initable);
  g_autofree char *sender_escaped = NULL;
  g_autofree char *id = NULL;
  int i;

  if (session->sender == NULL)
    {
      sender_escaped = g_strdup ("_");
    }
  else
    {
      sender_escaped = g_strdup (session->sender + 1);

      for (i = 0; sender_escaped[i]; i++)
        {
          if (sender_escaped[i] == '.')
            sender_escaped[i] = '_';
        }
    }

  if (!session->token)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Missing token");
      return FALSE;
    }

  id = g_strdup_printf ("/org/freedesktop/portal/desktop/session/%s/%s",
                        sender_escaped, session->token);

  session->id = g_steal_pointer (&id);

  g_dbus_interface_skeleton_set_flags (G_DBUS_INTERFACE_SKELETON (session),
                                       G_DBUS_INTERFACE_SKELETON_FLAGS_HANDLE_METHOD_INVOCATIONS_IN_THREAD);
  g_signal_connect (session, "g-authorize-method",
                    G_CALLBACK (session_authorize_callback),
                    NULL);

  return TRUE;
}

static void
g_initable_iface_init (GInitableIface *iface)
{
  iface->init = session_initable_init;
}

static void
session_set_property (GObject *object,
                      guint prop_id,
                      const GValue *value,
                      GParamSpec *pspec)
{
  PvPortalSession *session = PV_PORTAL_SESSION (object);

  switch (prop_id)
    {
    case PROP_SENDER:
      session->sender = g_strdup (g_value_get_string (value));
      break;

    case PROP_TOKEN:
      session->token = g_strdup (g_value_get_string (value));
      break;

    case PROP_CONNECTION:
      g_set_object (&session->connection, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
session_get_property (GObject *object,
                      guint prop_id,
                      GValue *value,
                      GParamSpec *pspec)
{
  PvPortalSession *session = PV_PORTAL_SESSION (object);

  switch (prop_id)
    {
    case PROP_SENDER:
      g_value_set_string (value, session->sender);
      break;

    case PROP_TOKEN:
      g_value_set_string (value, session->token);
      break;

    case PROP_CONNECTION:
      g_value_set_object (value, session->connection);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
session_finalize (GObject *object)
{
  PvPortalSession *session = PV_PORTAL_SESSION (object);

  g_assert (!session->id || !g_hash_table_lookup (sessions, session->id));

  g_free (session->sender);
  g_clear_object (&session->connection);

  g_free (session->id);

  g_mutex_clear (&session->mutex);

  G_OBJECT_CLASS (pv_portal_session_parent_class)->finalize (object);
}

static void
pv_portal_session_init (PvPortalSession *session)
{
  g_mutex_init (&session->mutex);
}

static void
pv_portal_session_class_init (PvPortalSessionClass *klass)
{
  GObjectClass *gobject_class;

  sessions = g_hash_table_new_full (g_str_hash, g_str_equal,
                                    NULL, NULL);

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize = session_finalize;
  gobject_class->set_property = session_set_property;
  gobject_class->get_property = session_get_property;

  obj_props[PROP_SENDER] =
    g_param_spec_string ("sender", "Sender", "Sender",
                         NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  obj_props[PROP_TOKEN] =
    g_param_spec_string ("token", "token", "Token",
                         NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  obj_props[PROP_CONNECTION] =
    g_param_spec_object ("connection", "connection",
                         "DBus connection",
                         G_TYPE_DBUS_CONNECTION,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (gobject_class, PROP_LAST, obj_props);
}
