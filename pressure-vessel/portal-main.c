/*
 * pressure-vessel-portal — accept IPC requests from the container
 *
 * Copyright © 2016-2018 Red Hat, Inc.
 * Copyright © 2020 Collabora Ltd.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Based on xdg-desktop-portal, flatpak-portal and flatpak-spawn.
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 *       Matthias Clasen <mclasen@redhat.com>
 */

#include "config.h"
#include "subprojects/libglnx/config.h"

#include <locale.h>
#include <sysexits.h>
#include <sys/prctl.h>

#include <glib.h>
#include <glib-unix.h>
#include <gio/gio.h>

#include "steam-runtime-tools/glib-backports-internal.h"
#include "steam-runtime-tools/utils-internal.h"
#include "libglnx/libglnx.h"

#include "portal-input.h"
#include "portal-listener.h"
#include "portal-request.h"
#include "portal-session.h"
#include "utils.h"

static PvPortalListener *global_listener = NULL;
static GMainLoop *main_loop = NULL;
static gboolean ever_acquired_bus_name = FALSE;
static GError *failed_to_export = NULL;

static void
name_owner_changed_cb (GDBusConnection *connection,
                       const gchar *sender_name,
                       const gchar *object_path,
                       const gchar *interface_name,
                       const gchar *signal_name,
                       GVariant *parameters,
                       gpointer user_data)
{
  const char *name, *from, *to;

  g_variant_get (parameters, "(&s&s&s)", &name, &from, &to);

  if (name[0] == ':' &&
      strcmp (name, from) == 0 &&
      strcmp (to, "") == 0)
    {
      pv_portal_request_close_for_session_bus (name);
      pv_portal_session_close_for_session_bus (name);
    }
}

static void
bus_acquired_cb (PvPortalListener *listener,
                 GDBusConnection *connection,
                 gpointer user_data)
{
  if (failed_to_export != NULL)
    return;

  g_dbus_connection_set_exit_on_close (connection, FALSE);
  g_dbus_connection_signal_subscribe (connection,
                                      DBUS_NAME_DBUS,
                                      DBUS_INTERFACE_DBUS,
                                      "NameOwnerChanged",
                                      DBUS_PATH_DBUS,
                                      NULL,
                                      G_DBUS_SIGNAL_FLAGS_NONE,
                                      name_owner_changed_cb,
                                      NULL, NULL);

  if (!pv_input_portal_export (connection, &failed_to_export))
    {
      g_main_loop_quit (main_loop);
      return;
    }

  g_debug ("Ready to receive requests");
}

static void
name_acquired_cb (PvPortalListener *listener,
                  GDBusConnection *connection,
                  const gchar *name,
                  gpointer user_data)
{
  g_debug ("%s acquired", name);
  ever_acquired_bus_name = TRUE;
  pv_portal_listener_close_info_fh (listener, name);
}

static void
name_lost_cb (PvPortalListener *listener,
              GDBusConnection *connection,
              const gchar *name,
              gpointer user_data)
{
  g_main_loop_quit (main_loop);
}

static void
peer_connection_closed_cb (GDBusConnection *connection,
                           gboolean remote_peer_vanished,
                           GError *error,
                           gpointer user_data)
{
  pv_portal_request_close_for_peer (connection);
  pv_portal_session_close_for_peer (connection);

  /* Paired with g_object_ref() in new_connection_cb() */
  g_object_unref (connection);
}

static gboolean
new_connection_cb (PvPortalListener *listener,
                   GDBusConnection *connection,
                   gpointer user_data)
{
  GError *error = NULL;

  /* Paired with g_object_unref() in peer_connection_closed_cb() */
  g_object_ref (connection);
  g_signal_connect (connection, "closed",
                    G_CALLBACK (peer_connection_closed_cb), NULL);

  if (!pv_input_portal_export (connection, &error))
    {
      g_warning ("Unable to export object: %s", error->message);
      g_dbus_connection_close (connection, NULL, NULL, NULL);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gchar *opt_bus_name = NULL;
static gint opt_info_fd = -1;
static gboolean opt_replace = FALSE;
static gchar *opt_socket = NULL;
static gchar *opt_socket_directory = NULL;
static gboolean opt_verbose = FALSE;
static gboolean opt_version = FALSE;

static GOptionEntry options[] =
{
  { "bus-name", '\0',
    G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, &opt_bus_name,
    "Use this well-known name on the D-Bus session bus.",
    "NAME" },
  { "info-fd", '\0',
    G_OPTION_FLAG_NONE, G_OPTION_ARG_INT, &opt_info_fd,
    "Indicate readiness and print details of how to connect on this "
    "file descriptor instead of stdout.",
    "FD" },
  { "replace", '\0',
    G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, &opt_replace,
    "Replace a previous instance with the same bus name. "
    "Ignored if --bus-name is not used.",
    NULL },
  { "socket", '\0',
    G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, &opt_socket,
    "Listen on this AF_UNIX socket.",
    "ABSPATH|@ABSTRACT" },
  { "socket-directory", '\0',
    G_OPTION_FLAG_NONE, G_OPTION_ARG_FILENAME, &opt_socket_directory,
    "Listen on an arbitrary AF_UNIX socket in this directory. "
    "Print the filename (socket=/path/to/socket), the "
    "D-Bus address (dbus_address=unix:...) and possibly other "
    "fields on stdout, one per line.",
    "PATH" },
  { "verbose", '\0',
    G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, &opt_verbose,
    "Be more verbose.", NULL },
  { "version", '\0',
    G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, &opt_version,
    "Print version number and exit.", NULL },
  { NULL }
};

static int my_pid = -1;

static void
cli_log_func (const gchar *log_domain,
              GLogLevelFlags log_level,
              const gchar *message,
              gpointer user_data)
{
  g_printerr ("%s[%d]: %s\n", (const char *) user_data, my_pid, message);
}

static gboolean
sigint_cb (gpointer user_data)
{
  guint *interrupt_id = user_data;

  *interrupt_id = 0;
  g_main_loop_quit (main_loop);

  return G_SOURCE_REMOVE;
}

static gboolean
run (int argc,
     char **argv,
     GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  GBusNameOwnerFlags flags;
  guint interrupt_id = 0;

  my_pid = getpid ();
  setlocale (LC_ALL, "");
  g_set_prgname ("pressure-vessel-portal");
  g_log_set_handler (G_LOG_DOMAIN,
                     G_LOG_LEVEL_WARNING | G_LOG_LEVEL_MESSAGE,
                     cli_log_func, (void *) g_get_prgname ());

  global_listener = pv_portal_listener_new ();

  context = g_option_context_new ("");
  g_option_context_set_summary (context,
                                "Accept IPC requests from the container.");

  g_option_context_add_main_entries (context, options, NULL);
  opt_verbose = pv_boolean_environment ("PRESSURE_VESSEL_VERBOSE", FALSE);

  if (!g_option_context_parse (context, &argc, &argv, error))
    return FALSE;

  if (opt_version)
    {
      g_print ("%s:\n"
               " Package: pressure-vessel\n"
               " Version: %s\n",
               g_get_prgname (), VERSION);
      return TRUE;
    }

  if (opt_verbose)
    g_log_set_handler (G_LOG_DOMAIN,
                       G_LOG_LEVEL_DEBUG | G_LOG_LEVEL_INFO,
                       cli_log_func, (void *) g_get_prgname ());

  if (prctl (PR_SET_PDEATHSIG, SIGTERM, 0, 0, 0) != 0)
    glnx_throw (error, "Failed to set up parent-death signal");

  if (!pv_portal_listener_set_up_info_fd (global_listener,
                                          opt_info_fd,
                                          error))
    return FALSE;

  _srt_setenv_disable_gio_modules ();

  if (argc >= 2 && strcmp (argv[1], "--") == 0)
    {
      argv++;
      argc--;
    }

  if (argc != 1)
    {
      g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
                   "Usage: %s [OPTIONS]", g_get_prgname ());
      return FALSE;
    }

  if (!pv_portal_listener_check_socket_arguments (global_listener,
                                                  opt_bus_name,
                                                  opt_socket,
                                                  opt_socket_directory,
                                                  error))
    return FALSE;

  g_signal_connect (global_listener,
                    "new-peer-connection", G_CALLBACK (new_connection_cb),
                    NULL);
  g_signal_connect (global_listener,
                    "session-bus-connected", G_CALLBACK (bus_acquired_cb),
                    NULL);
  g_signal_connect (global_listener,
                    "session-bus-name-acquired", G_CALLBACK (name_acquired_cb),
                    NULL);
  g_signal_connect (global_listener,
                    "session-bus-name-lost", G_CALLBACK (name_lost_cb),
                    NULL);

  flags = G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT;

  if (opt_replace)
    flags |= G_BUS_NAME_OWNER_FLAGS_REPLACE;

  if (!pv_portal_listener_listen (global_listener,
                                  opt_bus_name,
                                  flags,
                                  opt_socket,
                                  opt_socket_directory,
                                  error))
    return FALSE;

  g_debug ("Entering main loop");

  main_loop = g_main_loop_new (NULL, FALSE);
  interrupt_id = g_unix_signal_add (SIGINT, sigint_cb, &interrupt_id);
  g_main_loop_run (main_loop);

  if (interrupt_id != 0)
    g_source_remove (interrupt_id);

  if (failed_to_export)
    {
      g_propagate_prefixed_error (error, g_steal_pointer (&failed_to_export),
                                  "Unable to export interfaces");
      return FALSE;
    }

  if (opt_bus_name != NULL && !ever_acquired_bus_name)
    return glnx_throw (error, "Unable to acquire bus name");

  return TRUE;
}

int
main (int argc,
      char *argv[])
{
  g_autoptr(GError) local_error = NULL;
  int ret = EX_UNAVAILABLE;

  if (run (argc, argv, &local_error))
    ret = 0;

  /* Clean up globals */
  g_free (opt_bus_name);
  g_free (opt_socket);
  g_free (opt_socket_directory);
  g_clear_object (&global_listener);
  pv_input_portal_tear_down ();

  if (local_error != NULL)
    {
      g_warning ("%s", local_error->message);

      if (local_error->domain == G_OPTION_ERROR)
        ret = EX_USAGE;
      else
        ret = EX_UNAVAILABLE;
    }

  g_clear_error (&local_error);

  g_debug ("Exiting with status %d", ret);
  return ret;
}
