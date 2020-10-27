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

#include "portal-input.h"

#include <glib.h>
#include <gio/gio.h>

#include "steam-runtime-tools/glib-backports-internal.h"
#include "steam-runtime-tools/input-device-internal.h"
#include "steam-runtime-tools/direct-input-device-internal.h"
#include "steam-runtime-tools/udev-input-device-internal.h"
#include "steam-runtime-tools/utils-internal.h"
#include "libglnx/libglnx.h"

#include "portal.h"
#include "portal-request.h"
#include "portal-session.h"

#ifdef MOCK_PORTAL
# include "tests/mock-input-device.h"
#endif

static PvInputPortal1 *input_portal;

struct _PvPortalInputSession
{
  PvPortalSession parent;
  GHashTable *devices;
  GHashTable *device_handles;
  SrtInputDeviceMonitor *monitor;
  enum
  {
    PV_PORTAL_INPUT_SESSION_STATE_INIT,
    PV_PORTAL_INPUT_SESSION_STATE_STARTED,
    PV_PORTAL_INPUT_SESSION_STATE_STOPPED
  }
  state;
  gulong added_id;
  gulong removed_id;
  gulong all_for_now_id;
  gsize next_device_number;
  gboolean want_hidraw;
  gboolean want_evdev;
  gboolean once;
};

struct _PvPortalInputSessionClass
{
  PvPortalSessionClass parent_class;
};

G_DEFINE_TYPE (PvPortalInputSession, pv_portal_input_session,
               PV_TYPE_PORTAL_SESSION)

static void
pv_portal_input_session_init (PvPortalInputSession *self)
{
  g_debug ("Creating input session %p", self);
  self->devices = g_hash_table_new_full (NULL, NULL, g_object_unref, g_free);
  self->device_handles = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                g_free, g_object_unref);
}

static void
clear_signal_handler (gpointer object,
                      gulong *handler_id)
{
  if (object != NULL && *handler_id != 0)
    g_signal_handler_disconnect (object, *handler_id);

  *handler_id = 0;
}

static void
pv_portal_input_session_close (PvPortalSession *session)
{
  PvPortalInputSession *self = PV_PORTAL_INPUT_SESSION (session);

  g_debug ("Closing portal session");
  self->state = PV_PORTAL_INPUT_SESSION_STATE_STOPPED;

  if (self->monitor != NULL)
    {
      srt_input_device_monitor_stop (self->monitor);
      clear_signal_handler (self->monitor, &self->added_id);
      clear_signal_handler (self->monitor, &self->removed_id);
      clear_signal_handler (self->monitor, &self->all_for_now_id);
    }

  g_clear_object (&self->monitor);
}

static void
pv_portal_input_session_dispose (GObject *object)
{
  g_debug ("Destroying input session %p", object);

  pv_portal_input_session_close (PV_PORTAL_SESSION (object));

  G_OBJECT_CLASS (pv_portal_input_session_parent_class)->dispose (object);
}

static void
pv_portal_input_session_class_init (PvPortalInputSessionClass *cls)
{
  GObjectClass *object_class = G_OBJECT_CLASS (cls);
  PvPortalSessionClass *session_class = PV_PORTAL_SESSION_CLASS (cls);

  object_class->dispose = pv_portal_input_session_dispose;

  session_class->close = pv_portal_input_session_close;
}

static PvPortalInputSession *
pv_portal_input_session_new (GVariant *options,
                             GDBusMethodInvocation *invocation,
                             const char *token,
                             GError **error)
{
  GDBusConnection *connection = g_dbus_method_invocation_get_connection (invocation);
  const gchar *sender = g_dbus_method_invocation_get_sender (invocation);

  return g_initable_new (PV_TYPE_PORTAL_INPUT_SESSION, NULL, error,
                         "sender", sender,
                         "token", token,
                         "connection", connection,
                         NULL);
}

static gboolean
handle_input_create_session (PvInputPortal1 *object,
                             GDBusMethodInvocation *invocation,
                             GVariant *arg_options)
{
  g_autoptr(GError) error = NULL;
  PvPortalSession *session = NULL;
  const char *token;

  if (!g_variant_lookup (arg_options, "session_handle_token", "&s", &token))
    token = "t";

  if (!_srt_check_valid_object_path_component (token, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  session = PV_PORTAL_SESSION (pv_portal_input_session_new (arg_options,
                                                            invocation,
                                                            token,
                                                            &error));

  if (session == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!pv_portal_session_export (session, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      g_debug ("unable to export session");
      pv_portal_session_close (session, FALSE);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  pv_input_portal1_complete_create_session (object, invocation, session->id);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

/* Must be called while the request is locked. */
static void
start_request_send_response (PvPortalRequest *request,
                             XdgDesktopPortalResponseEnum response)
{
  GVariantBuilder opt_builder;

  g_debug ("sending response: %d", response);
  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);
  g_dbus_connection_emit_signal (request->connection,
                                 request->sender,   /* unicast */
                                 request->id,
                                 PV_PORTAL_REQUEST_IFACE,
                                 "Response",
                                 g_variant_new ("(a{sv})", &opt_builder),
                                 NULL);
  pv_portal_request_unexport (request);
}

static void
asv_builder_add_string_if_utf8 (GVariantBuilder *asv_builder,
                                const char *name,
                                const char *value)
{
  if (value != NULL && g_utf8_validate (value, -1, NULL))
    g_variant_builder_add (asv_builder, "{sv}",
                           name, g_variant_new_string (value));
}

static void
asv_builder_take_bytestring (GVariantBuilder *asv_builder,
                             const char *name,
                             gchar *value)
{
  if (value == NULL)
    return;

  g_variant_builder_add (asv_builder, "{sv}",
                         name, g_variant_new_bytestring (value));
  g_free (value);
}

static void
device_added_cb (SrtInputDeviceMonitor *monitor,
                 SrtInputDevice *device,
                 gpointer user_data)
{
  PvPortalInputSession *input_session = PV_PORTAL_INPUT_SESSION (user_data);
  PvPortalSession *session = &input_session->parent;
  GVariantBuilder asv_builder;
  const gchar *device_handle;
  g_autofree gchar *new_device_handle = NULL;
  g_auto(GStrv) udev_properties = NULL;
  const char *ancestor;
  const char *manufacturer;
  const char *name;
  const char *phys;
  const char *uniq;
  unsigned int bus_type, vendor_id, product_id, version;

  SESSION_AUTOLOCK_UNREF (g_object_ref (session));

  device_handle = g_hash_table_lookup (input_session->devices, device);

  if (device_handle != NULL)
    {
      g_debug ("Device handle %s already found for %p",
               device_handle, device);
      return;
    }

  new_device_handle = g_strdup_printf ("%s/dev%zu", input_session->parent.id,
                                       input_session->next_device_number++);

  device_handle = new_device_handle;
  g_hash_table_replace (input_session->device_handles, g_strdup (device_handle),
                        g_object_ref (device));
  g_hash_table_replace (input_session->devices, g_object_ref (device),
                        g_steal_pointer (&new_device_handle));

  g_variant_builder_init (&asv_builder, G_VARIANT_TYPE_VARDICT);

  g_variant_builder_add (&asv_builder, "{sv}",
                         "type_flags",
                         g_variant_new_uint32 (srt_input_device_get_type_flags (device)));
  g_variant_builder_add (&asv_builder, "{sv}",
                         "interface_flags",
                         g_variant_new_uint32 (srt_input_device_get_interface_flags (device)));
  asv_builder_add_string_if_utf8 (&asv_builder, "sys_path",
                                  srt_input_device_get_sys_path (device));
  asv_builder_add_string_if_utf8 (&asv_builder, "dev_node",
                                  srt_input_device_get_dev_node (device));
  asv_builder_add_string_if_utf8 (&asv_builder, "subsystem",
                                  srt_input_device_get_subsystem (device));
  asv_builder_take_bytestring (&asv_builder, "uevent",
                               srt_input_device_dup_uevent (device));

  bus_type = vendor_id = product_id = version = 0;

  if (srt_input_device_get_identity (device, &bus_type, &vendor_id,
                                     &product_id, &version))
    {
      if (bus_type != 0)
        g_variant_builder_add (&asv_builder, "{sv}", "bus_type",
                               g_variant_new_uint32 (bus_type));

      if (vendor_id != 0)
        g_variant_builder_add (&asv_builder, "{sv}", "vendor_id",
                               g_variant_new_uint32 (vendor_id));

      if (product_id != 0)
        g_variant_builder_add (&asv_builder, "{sv}", "product_id",
                               g_variant_new_uint32 (product_id));

      if (version != 0)
        g_variant_builder_add (&asv_builder, "{sv}", "version",
                               g_variant_new_uint32 (version));
    }

  if (srt_input_device_get_interface_flags (device)
      & SRT_INPUT_DEVICE_INTERFACE_FLAGS_EVENT)
    {
      GVariantBuilder auay_builder;
      unsigned int type;
      unsigned long buf[LONGS_FOR_BITS (HIGHEST_EVENT_CODE)];
      size_t n, i;

      g_variant_builder_init (&auay_builder, G_VARIANT_TYPE ("a{uay}"));

      for (type = 0; type < EV_MAX; type++)
        {
          n = srt_input_device_get_event_capabilities (device, type, buf,
                                                       G_N_ELEMENTS (buf));

          if (n == 0)
            continue;

          if (n > G_N_ELEMENTS (buf))
            n = G_N_ELEMENTS (buf);

          /* We represent them as little-endian to make the byte order
           * within a word consistent with the order of words: the first
           * word is the least-significant. This only matters on a
           * big-endian host system. */
          for (i = 0; i < n; i++)
            buf[i] = GULONG_TO_LE (buf[i]);

          g_variant_builder_add (&auay_builder, "{u@ay}",
                                 type,
                                 g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                                                            buf,
                                                            sizeof (buf),
                                                            1));
        }

      g_variant_builder_add (&asv_builder, "{sv}", "evdev_capabilities",
                             g_variant_new ("a{uay}", &auay_builder));

      n = srt_input_device_get_input_properties (device, buf, G_N_ELEMENTS (buf));

      if (n > 0)
        {
          if (n > G_N_ELEMENTS (buf))
            n = G_N_ELEMENTS (buf);

          for (i = 0; i < n; i++)
            buf[i] = GULONG_TO_LE (buf[i]);

          g_variant_builder_add (&asv_builder, "{sv}", "input_properties",
                                 g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                                                            buf, sizeof (buf),
                                                            1));
        }
    }

  ancestor = srt_input_device_get_hid_sys_path (device);

  bus_type = vendor_id = product_id = 0;
  name = phys = uniq = NULL;

  if (srt_input_device_get_hid_identity (device, &bus_type, &vendor_id,
                                         &product_id, &name, &phys, &uniq)
      || ancestor != NULL)
    {
      GVariantBuilder ancestor_builder;

      g_variant_builder_init (&ancestor_builder, G_VARIANT_TYPE_VARDICT);
      asv_builder_add_string_if_utf8 (&ancestor_builder, "sys_path",
                                      srt_input_device_get_hid_sys_path (device));
      asv_builder_take_bytestring (&ancestor_builder, "uevent",
                                   srt_input_device_dup_hid_uevent (device));

      if (bus_type != 0)
        g_variant_builder_add (&ancestor_builder, "{sv}", "bus_type",
                               g_variant_new_uint32 (bus_type));

      if (vendor_id != 0)
        g_variant_builder_add (&ancestor_builder, "{sv}", "vendor_id",
                               g_variant_new_uint32 (vendor_id));

      if (product_id != 0)
        g_variant_builder_add (&ancestor_builder, "{sv}", "product_id",
                               g_variant_new_uint32 (product_id));

      asv_builder_add_string_if_utf8 (&ancestor_builder, "name", name);
      asv_builder_add_string_if_utf8 (&ancestor_builder, "phys", phys);
      asv_builder_add_string_if_utf8 (&ancestor_builder, "uniq", uniq);

      g_variant_builder_add (&asv_builder, "{sv}",
                             "hid_ancestor",
                             g_variant_new ("a{sv}", &ancestor_builder));
    }

  ancestor = srt_input_device_get_input_sys_path (device);

  bus_type = vendor_id = product_id = version = 0;
  name = phys = uniq = NULL;

  if (srt_input_device_get_input_identity (device, &bus_type, &vendor_id,
                                           &product_id, &version, &name,
                                           &phys, &uniq)
      || ancestor != NULL)
    {
      GVariantBuilder ancestor_builder;

      g_variant_builder_init (&ancestor_builder, G_VARIANT_TYPE_VARDICT);
      asv_builder_add_string_if_utf8 (&ancestor_builder, "sys_path",
                                      srt_input_device_get_input_sys_path (device));
      asv_builder_take_bytestring (&ancestor_builder, "uevent",
                                   srt_input_device_dup_input_uevent (device));

      if (bus_type != 0)
        g_variant_builder_add (&ancestor_builder, "{sv}", "bus_type",
                               g_variant_new_uint32 (bus_type));

      if (vendor_id != 0)
        g_variant_builder_add (&ancestor_builder, "{sv}", "vendor_id",
                               g_variant_new_uint32 (vendor_id));

      if (product_id != 0)
        g_variant_builder_add (&ancestor_builder, "{sv}", "product_id",
                               g_variant_new_uint32 (product_id));

      if (version != 0)
        g_variant_builder_add (&ancestor_builder, "{sv}", "version",
                               g_variant_new_uint32 (version));

      asv_builder_add_string_if_utf8 (&ancestor_builder, "name", name);
      asv_builder_add_string_if_utf8 (&ancestor_builder, "phys", phys);
      asv_builder_add_string_if_utf8 (&ancestor_builder, "uniq", uniq);

      g_variant_builder_add (&asv_builder, "{sv}",
                             "input_ancestor",
                             g_variant_new ("a{sv}", &ancestor_builder));
    }

  ancestor = srt_input_device_get_usb_device_sys_path (device);

  vendor_id = product_id = version = 0;
  manufacturer = name = uniq = NULL;

  if (srt_input_device_get_usb_device_identity (device, &vendor_id,
                                                &product_id, &version,
                                                &manufacturer, &name, &uniq)
      || ancestor != NULL)
    {
      GVariantBuilder ancestor_builder;

      g_variant_builder_init (&ancestor_builder, G_VARIANT_TYPE_VARDICT);
      asv_builder_add_string_if_utf8 (&ancestor_builder, "sys_path",
                                      srt_input_device_get_usb_device_sys_path (device));
      asv_builder_take_bytestring (&ancestor_builder, "uevent",
                                   srt_input_device_dup_usb_device_uevent (device));

      if (vendor_id != 0)
        g_variant_builder_add (&ancestor_builder, "{sv}", "vendor_id",
                               g_variant_new_uint32 (vendor_id));

      if (product_id != 0)
        g_variant_builder_add (&ancestor_builder, "{sv}", "product_id",
                               g_variant_new_uint32 (product_id));

      if (version != 0)
        g_variant_builder_add (&ancestor_builder, "{sv}", "version",
                               g_variant_new_uint32 (version));

      /* The terminology is a little bit different here, so the keys
       * don't all match the variable names */
      asv_builder_add_string_if_utf8 (&ancestor_builder, "manufacturer", manufacturer);
      asv_builder_add_string_if_utf8 (&ancestor_builder, "product", name);
      asv_builder_add_string_if_utf8 (&ancestor_builder, "serial", uniq);

      g_variant_builder_add (&asv_builder, "{sv}",
                             "usb_device_ancestor",
                             g_variant_new ("a{sv}", &ancestor_builder));
    }

  udev_properties = srt_input_device_dup_udev_properties (device);

  if (udev_properties != NULL)
    {
      g_variant_builder_add (&asv_builder, "{sv}",
                             "udev_properties",
                             g_variant_new_bytestring_array ((const char * const *) udev_properties, -1));
    }

  g_debug ("Emitting DeviceAdded for %s (%p)", device_handle, device);
  g_dbus_connection_emit_signal (session->connection,
                                 session->sender,   /* unicast */
                                 PV_PORTAL_INPUT1_PATH,
                                 PV_PORTAL_INPUT1_IFACE,
                                 "DeviceAdded",
                                 g_variant_new ("(ooa{sv})",
                                                session->id,
                                                device_handle,
                                                &asv_builder),
                                 NULL);
}

static void
device_removed_cb (SrtInputDeviceMonitor *monitor,
                   SrtInputDevice *device,
                   gpointer user_data)
{
  PvPortalInputSession *input_session = PV_PORTAL_INPUT_SESSION (user_data);
  PvPortalSession *session = &input_session->parent;
  const char *device_handle;

  SESSION_AUTOLOCK_UNREF (g_object_ref (session));

  device_handle = g_hash_table_lookup (input_session->devices, device);

  if (device_handle == NULL)
    {
      g_debug ("No device handle found for %p", device);
      return;
    }

  g_debug ("Emitting DeviceRemoved for %s (%p)", device_handle, device);
  g_dbus_connection_emit_signal (session->connection,
                                 session->sender,   /* unicast */
                                 PV_PORTAL_INPUT1_PATH,
                                 PV_PORTAL_INPUT1_IFACE,
                                 "DeviceRemoved",
                                 g_variant_new ("(oo)",
                                                session->id,
                                                device_handle),
                                 NULL);

  g_hash_table_remove (input_session->devices, device);
}

static void
all_for_now_cb (SrtInputDeviceMonitor *monitor,
                gpointer user_data)
{
  PvPortalRequest *request = PV_PORTAL_REQUEST (user_data);

  g_debug ("All for now");

  REQUEST_AUTOLOCK (request);
  start_request_send_response (request, XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS);
  g_signal_handlers_disconnect_by_func (monitor, all_for_now_cb, request);
}

static gboolean
handle_input_start (PvInputPortal1 *object,
                    GDBusMethodInvocation *invocation,
                    const char *arg_session_handle,
                    const char *arg_parent_window,
                    GVariant *arg_options)
{
  g_autoptr(PvPortalRequest) request = NULL;
  PvPortalSession *session;
  PvPortalInputSession *input_session;
  g_autoptr(GError) error = NULL;
  const char *token = NULL;
  XdgDesktopPortalResponseEnum response = XDG_DESKTOP_PORTAL_RESPONSE_OTHER;
  SrtInputDeviceMonitorFlags flags = SRT_INPUT_DEVICE_MONITOR_FLAGS_NONE;

  if (!g_variant_lookup (arg_options, "handle_token", "&s", &token))
    token = "t";

  if (!_srt_check_valid_object_path_component (token, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  /* In a "real" portal, the parent window would be used for a permission
   * prompt. We don't do that here: pressure-vessel is not a
   * security boundary. */

  request = pv_portal_request_new (invocation, token);

  REQUEST_AUTOLOCK (request);

  session = pv_portal_session_acquire (arg_session_handle, request);

  if (!session)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  SESSION_AUTOLOCK_UNREF (session);

  input_session = PV_PORTAL_INPUT_SESSION (session);

  switch (input_session->state)
    {
      case PV_PORTAL_INPUT_SESSION_STATE_INIT:
        break;

      case PV_PORTAL_INPUT_SESSION_STATE_STARTED:
        g_dbus_method_invocation_return_error (invocation,
                                               G_DBUS_ERROR,
                                               G_DBUS_ERROR_INVALID_ARGS,
                                               "Start can only be called once");
        return G_DBUS_METHOD_INVOCATION_HANDLED;

      case PV_PORTAL_INPUT_SESSION_STATE_STOPPED:
      default:
        g_dbus_method_invocation_return_error (invocation,
                                               G_DBUS_ERROR,
                                               G_DBUS_ERROR_ACCESS_DENIED,
                                               "Invalid session");
        return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  request->session = g_object_ref (session);
  pv_portal_request_export (request);

  g_variant_lookup (arg_options, "evdev", "b", &input_session->want_evdev);
  g_variant_lookup (arg_options, "hidraw", "b", &input_session->want_hidraw);
  g_variant_lookup (arg_options, "once", "b", &input_session->once);

  input_session->state = PV_PORTAL_INPUT_SESSION_STATE_STARTED;

  pv_input_portal1_complete_start (object, invocation, request->id);

  /* In a real portal that crosses a security boundary, we'd want to
   * ask user permission to access input devices - but since pressure-vessel
   * is not intended to be a security boundary, we just go for it. */
  response = XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS;

  if (input_session->once)
    flags |= SRT_INPUT_DEVICE_MONITOR_FLAGS_ONCE;

  /* Ensure that the signals will arrive in the main thread. */
  g_main_context_push_thread_default (g_main_context_default ());
    {
      g_autoptr(SrtInputDeviceMonitor) monitor = NULL;

#ifdef MOCK_PORTAL
      monitor = SRT_INPUT_DEVICE_MONITOR (mock_input_device_monitor_new (flags));
#else
      monitor = srt_udev_input_device_monitor_new (flags, &error);

      if (monitor == NULL)
        {
          g_warning ("%s", error->message);
          g_clear_error (&error);
          monitor = srt_direct_input_device_monitor_new (flags);
        }
#endif
      input_session->monitor = g_object_ref (monitor);

      input_session->added_id = g_signal_connect (monitor, "added",
                                                  G_CALLBACK (device_added_cb),
                                                  input_session);
      input_session->removed_id = g_signal_connect (monitor, "removed",
                                                    G_CALLBACK (device_removed_cb),
                                                    input_session);
      g_signal_connect (monitor, "all-for-now",
                        G_CALLBACK (all_for_now_cb), g_object_ref (request));

      if (input_session->want_evdev)
        srt_input_device_monitor_request_evdev (monitor);

      if (input_session->want_hidraw)
        srt_input_device_monitor_request_raw_hid (monitor);

      /* Temporarily unlock, because signal handlers can be called
       * from start() */
      g_mutex_unlock (&session->mutex);
      g_mutex_unlock (&request->mutex);

      if (srt_input_device_monitor_start (monitor, &error))
        {
          g_debug ("Started input device monitor");
        }
      else
        {
          g_debug ("Failed to start input device monitor: %s", error->message);
          response = XDG_DESKTOP_PORTAL_RESPONSE_OTHER;
        }

      g_mutex_lock (&request->mutex);
      g_mutex_lock (&session->mutex);
    }
  g_main_context_pop_thread_default (g_main_context_default ());

  if (response != XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS)
    {
      start_request_send_response (request, response);
      g_signal_handlers_disconnect_by_func (input_session->monitor,
                                            all_for_now_cb, request);
      g_debug ("closing session");
      pv_portal_session_close (session, FALSE);
    }

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_input_open_device (PvInputPortal1 *object,
                          GDBusMethodInvocation *invocation,
                          GUnixFDList *fd_list,
                          const char *arg_session_handle,
                          const char *arg_device_handle,
                          GVariant *arg_options)
{
  g_autoptr(PvPortalRequest) request = NULL;
  g_autoptr(GUnixFDList) out_fd_list = NULL;
  g_autoptr(GError) error = NULL;
  glnx_autofd int fd = -1;
  PvPortalSession *session;
  PvPortalInputSession *input_session;
  SrtInputDevice *device;
  gboolean r, w, block;
  int mode;

  request = pv_portal_request_new (invocation, "_");

  REQUEST_AUTOLOCK (request);

  session = pv_portal_session_acquire (arg_session_handle, request);

  if (!session)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Invalid session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  SESSION_AUTOLOCK_UNREF (session);

  input_session = PV_PORTAL_INPUT_SESSION (session);

  switch (input_session->state)
    {
      case PV_PORTAL_INPUT_SESSION_STATE_STARTED:
        break;

      case PV_PORTAL_INPUT_SESSION_STATE_INIT:
      case PV_PORTAL_INPUT_SESSION_STATE_STOPPED:
      default:
        g_dbus_method_invocation_return_error (invocation,
                                               G_DBUS_ERROR,
                                               G_DBUS_ERROR_ACCESS_DENIED,
                                               "Invalid session");
        return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_variant_lookup (arg_options, "read", "b", &r);
  g_variant_lookup (arg_options, "write", "b", &w);
  g_variant_lookup (arg_options, "block", "b", &block);

  if (!r && !w)
    {
      g_autoptr(GVariant) r_variant = g_variant_lookup_value (arg_options, "read", NULL);
      g_autoptr(GVariant) w_variant = g_variant_lookup_value (arg_options, "write", NULL);

      if (r_variant == NULL && w_variant == NULL)
        {
          /* For convenience, if both are unspecified, the default is rw */
          r = w = TRUE;
        }
      else
        {
          g_dbus_method_invocation_return_error (invocation,
                                                 G_DBUS_ERROR,
                                                 G_DBUS_ERROR_INVALID_ARGS,
                                                 "read and write cannot both be false");
          return G_DBUS_METHOD_INVOCATION_HANDLED;
        }
    }

  if (r && w)
    mode = O_RDWR;
  else if (w)
    mode = O_WRONLY;
  else
    mode = O_RDONLY;

  if (!block)
    mode |= O_NONBLOCK;

  device = g_hash_table_lookup (input_session->device_handles, arg_device_handle);

  if (device == NULL)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_INVALID_ARGS,
                                             "Device handle %s not found",
                                             arg_device_handle);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  fd = srt_input_device_open (device, mode, &error);

  if (fd < 0)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  out_fd_list = g_unix_fd_list_new ();

  if (g_unix_fd_list_append (out_fd_list, fd, &error) < 0)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  pv_input_portal1_complete_open_device (object, invocation, out_fd_list,
                                         g_variant_new_handle (0));
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

gboolean
pv_input_portal_export (GDBusConnection *connection,
                        GError **error)
{
  if (input_portal == NULL)
    {
      input_portal = pv_input_portal1_skeleton_new ();

      pv_input_portal1_set_version (input_portal, 0);
      g_signal_connect (input_portal, "handle-create-session",
                        G_CALLBACK (handle_input_create_session), NULL);
      g_signal_connect (input_portal, "handle-start",
                        G_CALLBACK (handle_input_start), NULL);
      g_signal_connect (input_portal, "handle-open-device",
                        G_CALLBACK (handle_input_open_device), NULL);
    }

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (input_portal),
                                         connection,
                                         PV_PORTAL_INPUT1_PATH,
                                         error))
    return FALSE;

  return TRUE;
}

void
pv_input_portal_tear_down (void)
{
  if (input_portal != NULL)
    g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (input_portal));

  g_clear_object (&input_portal);
}
