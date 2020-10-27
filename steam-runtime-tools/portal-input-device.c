/*
 * Portal-based input device monitor, very loosely based on SDL code.
 *
 * Copyright © 1997-2020 Sam Lantinga <slouken@libsdl.org>
 * Copyright © 2020 Collabora Ltd.
 *
 * SPDX-License-Identifier: Zlib
 *
 * This software is provided 'as-is', without any express or implied
 * warranty.  In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 */

#include "steam-runtime-tools/portal-input-device-internal.h"

#include <libglnx.h>

#include "steam-runtime-tools/glib-backports-internal.h"
#include "steam-runtime-tools/input-device.h"
#include "steam-runtime-tools/input-device-internal.h"
#include "steam-runtime-tools/simple-input-device-internal.h"
#include "steam-runtime-tools/utils-internal.h"

#include "pressure-vessel/portal.h"

static void srt_portal_input_device_iface_init (SrtInputDeviceInterface *iface);
static void srt_portal_input_device_monitor_iface_init (SrtInputDeviceMonitorInterface *iface);
static void srt_portal_input_device_monitor_initable_iface_init (GInitableIface *iface);

/* We separate out the details of our connection to the portal as a
 * separately-refcounted struct, so that SrtPortalInputDevice can have
 * a srt_input_device_open() method that still works after the
 * device monitor has been freed. */
typedef struct
{
  gsize refcount;
  GDBusConnection *connection;
  gchar *bus_name;
  gchar *object_path;
  gchar *session_handle;
  guint session_closed_id;
} Portal;

static void
clear_subscription (GDBusConnection *connection,
                    guint *id)
{
  if (*id != 0)
    g_dbus_connection_signal_unsubscribe (connection, *id);

  *id = 0;
}

static void
portal_unref (Portal *self)
{
  g_return_if_fail (self->refcount > 0);

  if (--self->refcount > 0)
    return;

  if (self->connection != NULL)
    clear_subscription (self->connection, &self->session_closed_id);

  g_clear_object (&self->connection);
  g_free (self->bus_name);
  g_free (self->object_path);
  g_free (self->session_handle);
  g_free (self);
}

static Portal *
portal_ref (Portal *self)
{
  g_return_val_if_fail (self->refcount > 0, self);

  self->refcount++;
  return self;
}

static Portal *
portal_new (GDBusConnection *connection,
            const char *bus_name,
            const char *object_path)
{
  Portal *self = g_new0 (Portal, 1);

  self->refcount = 1;
  self->connection = g_object_ref (connection);
  self->bus_name = g_strdup (bus_name);
  self->object_path = g_strdup (object_path);
  return self;
}

static void
portal_call_void_ignore_result (Portal *self,
                                const char *path,
                                const char *iface,
                                const char *method)
{
  g_return_if_fail (self != NULL);
  g_return_if_fail (self->refcount > 0);
  g_return_if_fail (path != NULL);
  g_return_if_fail (iface != NULL);
  g_return_if_fail (method != NULL);

  g_dbus_connection_call (self->connection,
                          self->bus_name,
                          path,
                          iface,
                          method,
                          NULL,         /* no parameters */
                          NULL,         /* don't care what it returns */
                          G_DBUS_CALL_FLAGS_NO_AUTO_START,
                          -1,
                          NULL,         /* not cancellable */
                          NULL, NULL);  /* fire and forget */
}

struct _SrtPortalInputDevice
{
  SrtSimpleInputDevice parent;
  Portal *portal;
  gchar *device_handle;
};

struct _SrtPortalInputDeviceClass
{
  SrtSimpleInputDeviceClass parent;
};

struct _SrtPortalInputDeviceMonitor
{
  GObject parent;

  Portal *portal;
  GHashTable *devices;
  GMainContext *monitor_context;
  GSource *monitor_source;
  gchar *start_request_handle;
  gchar *socket_path;

  gboolean want_evdev;
  gboolean want_hidraw;
  SrtInputDeviceMonitorFlags flags;
  enum
  {
    NOT_STARTED = 0,
    ENUMERATING,
    MONITORING,
    STOPPED
  } state;
  guint request_response_id;
  guint request_closed_id;
  guint device_added_id;
  guint device_removed_id;
};

struct _SrtPortalInputDeviceMonitorClass
{
  GObjectClass parent;
};

G_DEFINE_TYPE_WITH_CODE (SrtPortalInputDevice,
                         _srt_portal_input_device,
                         SRT_TYPE_SIMPLE_INPUT_DEVICE,
                         G_IMPLEMENT_INTERFACE (SRT_TYPE_INPUT_DEVICE,
                                                srt_portal_input_device_iface_init))

G_DEFINE_TYPE_WITH_CODE (SrtPortalInputDeviceMonitor,
                         _srt_portal_input_device_monitor,
                         G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE,
                                                srt_portal_input_device_monitor_initable_iface_init)
                         G_IMPLEMENT_INTERFACE (SRT_TYPE_INPUT_DEVICE_MONITOR,
                                                srt_portal_input_device_monitor_iface_init))

static void
_srt_portal_input_device_init (SrtPortalInputDevice *self)
{
}

static void
srt_portal_input_device_dispose (GObject *object)
{
  SrtPortalInputDevice *self = SRT_PORTAL_INPUT_DEVICE (object);

  g_clear_pointer (&self->portal, portal_unref);

  G_OBJECT_CLASS (_srt_portal_input_device_parent_class)->dispose (object);
}

static void
srt_portal_input_device_finalize (GObject *object)
{
  SrtPortalInputDevice *self = SRT_PORTAL_INPUT_DEVICE (object);

  g_free (self->device_handle);

  G_OBJECT_CLASS (_srt_portal_input_device_parent_class)->finalize (object);
}

static void
_srt_portal_input_device_class_init (SrtPortalInputDeviceClass *cls)
{
  GObjectClass *object_class = G_OBJECT_CLASS (cls);

  object_class->dispose = srt_portal_input_device_dispose;
  object_class->finalize = srt_portal_input_device_finalize;
}

static int
srt_portal_input_device_open_device (SrtInputDevice *device,
                                     int flags,
                                     GError **error)
{
  SrtPortalInputDevice *self = SRT_PORTAL_INPUT_DEVICE (device);
  g_auto(GVariantBuilder) options_builder = {};
  g_autoptr(GVariant) tuple = NULL;
  g_autoptr(GUnixFDList) fd_list = NULL;
  glnx_autofd int ret = -1;
  gboolean r = FALSE;
  gboolean w = FALSE;
  guint32 handle;
  int unhandled_flags;
  int *fds;
  gsize i;

  g_variant_builder_init (&options_builder, G_VARIANT_TYPE_VARDICT);

  if (!_srt_input_device_check_open_flags (flags, error))
    return -1;

  switch (flags & (O_RDONLY | O_RDWR | O_WRONLY))
    {
      case O_RDONLY:
        r = TRUE;
        break;

      case O_RDWR:
        r = w = TRUE;
        break;

      case O_WRONLY:
        w = TRUE;
        break;

      default:
        /* _srt_input_device_check_open_flags should have caught this */
        g_return_val_if_reached (-1);
    }

  g_variant_builder_add (&options_builder, "{sv}", "read",
                         g_variant_new_boolean (r));
  g_variant_builder_add (&options_builder, "{sv}", "write",
                         g_variant_new_boolean (w));

  unhandled_flags = flags & ~(O_RDONLY | O_RDWR | O_WRONLY);

  if (unhandled_flags & O_NONBLOCK)
    {
      g_variant_builder_add (&options_builder, "{sv}", "block",
                             g_variant_new_boolean (FALSE));
      unhandled_flags &= ~O_NONBLOCK;
    }
  else
    {
      g_variant_builder_add (&options_builder, "{sv}", "block",
                             g_variant_new_boolean (TRUE));
    }

  g_return_val_if_fail (unhandled_flags == 0, -1);

  tuple = g_dbus_connection_call_with_unix_fd_list_sync (self->portal->connection,
                                                         self->portal->bus_name,
                                                         self->portal->object_path,
                                                         PV_PORTAL_INPUT1_IFACE,
                                                         "OpenDevice",
                                                         g_variant_new ("(ooa{sv})",
                                                                        self->portal->session_handle,
                                                                        self->device_handle,
                                                                        &options_builder),
                                                         G_VARIANT_TYPE ("(h)"),
                                                         G_DBUS_CALL_FLAGS_NONE,
                                                         -1,
                                                         NULL,
                                                         &fd_list,
                                                         NULL,
                                                         error);

  if (tuple == NULL)
    return -1;

  g_variant_get (tuple, "(h)", &handle);

  fds = g_unix_fd_list_steal_fds (fd_list, NULL);

  for (i = 0; fds[i] >= 0; i++)
    {
      if (i == handle)
        ret = glnx_steal_fd (&fds[i]);
      else
        glnx_close_fd (&fds[i]);
    }

  g_free (fds);

  if (ret < 0)
    {
      g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                   "Invalid handle in D-Bus message");
      return -1;
    }

  return glnx_steal_fd (&ret);
}

static void
srt_portal_input_device_iface_init (SrtInputDeviceInterface *iface)
{
#define IMPLEMENT(x) iface->x = srt_portal_input_device_ ## x
  IMPLEMENT (open_device);
#undef IMPLEMENT
}

typedef enum
{
  PROP_0,
  PROP_FLAGS,
  PROP_IS_ACTIVE,
  PROP_SOCKET,
  N_PROPERTIES
} Property;

static void
_srt_portal_input_device_monitor_init (SrtPortalInputDeviceMonitor *self)
{
  self->monitor_context = g_main_context_ref_thread_default ();
  self->state = NOT_STARTED;
  self->devices = g_hash_table_new_full (g_str_hash,
                                         g_str_equal,
                                         NULL,
                                         g_object_unref);
}

static void
srt_portal_input_device_monitor_get_property (GObject *object,
                                            guint prop_id,
                                            GValue *value,
                                            GParamSpec *pspec)
{
  SrtPortalInputDeviceMonitor *self = SRT_PORTAL_INPUT_DEVICE_MONITOR (object);

  switch ((Property) prop_id)
    {
      case PROP_FLAGS:
        g_value_set_flags (value, self->flags);
        break;

      case PROP_IS_ACTIVE:
        g_value_set_boolean (value, (self->state == ENUMERATING
                                     || self->state == MONITORING));
        break;

      case PROP_0:
      case PROP_SOCKET:
      case N_PROPERTIES:
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
srt_portal_input_device_monitor_set_property (GObject *object,
                                            guint prop_id,
                                            const GValue *value,
                                            GParamSpec *pspec)
{
  SrtPortalInputDeviceMonitor *self = SRT_PORTAL_INPUT_DEVICE_MONITOR (object);

  switch ((Property) prop_id)
    {
      case PROP_FLAGS:
        /* Construct-only */
        g_return_if_fail (self->flags == 0);
        self->flags = g_value_get_flags (value);
        break;

      case PROP_SOCKET:
        /* Construct-only */
        g_return_if_fail (self->socket_path == NULL);
        self->socket_path = g_value_dup_string (value);
        break;

      case PROP_IS_ACTIVE:
      case PROP_0:
      case N_PROPERTIES:
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
srt_portal_input_device_monitor_dispose (GObject *object)
{
  srt_input_device_monitor_stop (SRT_INPUT_DEVICE_MONITOR (object));
  G_OBJECT_CLASS (_srt_portal_input_device_monitor_parent_class)->dispose (object);
}

static void
srt_portal_input_device_monitor_finalize (GObject *object)
{
  srt_input_device_monitor_stop (SRT_INPUT_DEVICE_MONITOR (object));
  G_OBJECT_CLASS (_srt_portal_input_device_monitor_parent_class)->finalize (object);
}

static void
_srt_portal_input_device_monitor_class_init (SrtPortalInputDeviceMonitorClass *cls)
{
  GObjectClass *object_class = G_OBJECT_CLASS (cls);

  object_class->get_property = srt_portal_input_device_monitor_get_property;
  object_class->set_property = srt_portal_input_device_monitor_set_property;
  object_class->dispose = srt_portal_input_device_monitor_dispose;
  object_class->finalize = srt_portal_input_device_monitor_finalize;

  g_object_class_override_property (object_class, PROP_FLAGS, "flags");
  g_object_class_override_property (object_class, PROP_IS_ACTIVE, "is-active");
  g_object_class_install_property (object_class, PROP_SOCKET,
      g_param_spec_string ("socket",
                           "Socket",
                           "Private socket to communicate with portal",
                           NULL,
                           G_PARAM_WRITABLE
                           | G_PARAM_CONSTRUCT_ONLY
                           | G_PARAM_STATIC_STRINGS));
}

static gboolean
srt_portal_input_device_monitor_initable_init (GInitable *initable,
                                               GCancellable *cancellable G_GNUC_UNUSED,
                                               GError **error)
{
  SrtPortalInputDeviceMonitor *self = SRT_PORTAL_INPUT_DEVICE_MONITOR (initable);
  g_autoptr(GDBusConnection) conn = NULL;

  if (self->socket_path != NULL)
    {
      g_autofree gchar *address = NULL;
      g_autofree gchar *escaped = NULL;

      if (self->socket_path[0] == '@')
        {
          escaped = g_dbus_address_escape_value (&self->socket_path[1]);
          address = g_strdup_printf ("unix:abstract=%s", escaped);
        }
      else if (self->socket_path[0] == '/')
        {
          escaped = g_dbus_address_escape_value (self->socket_path);
          address = g_strdup_printf ("unix:path=%s", escaped);
        }
      else
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Invalid socket address '%s'", self->socket_path);
          return FALSE;
        }

      conn = g_dbus_connection_new_for_address_sync (address,
                                                     G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT,
                                                     NULL,
                                                     NULL,
                                                     error);

      if (conn == NULL)
        return FALSE;

      self->portal = portal_new (conn, NULL, PV_PORTAL_INPUT1_PATH);
    }
  else
    {
      conn = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, error);

      if (conn == NULL)
        return FALSE;

      self->portal = portal_new (conn, PV_PORTAL_BUS_NAME, PV_PORTAL_INPUT1_PATH);
    }

  /* TODO: Do we want to ping the service synchronously here? */

  return TRUE;
}

static void
srt_portal_input_device_monitor_request_raw_hid (SrtInputDeviceMonitor *monitor)
{
  SrtPortalInputDeviceMonitor *self = SRT_PORTAL_INPUT_DEVICE_MONITOR (monitor);

  g_return_if_fail (SRT_IS_PORTAL_INPUT_DEVICE_MONITOR (monitor));
  g_return_if_fail (self->state == NOT_STARTED);

  self->want_hidraw = TRUE;
}

static void
srt_portal_input_device_monitor_request_evdev (SrtInputDeviceMonitor *monitor)
{
  SrtPortalInputDeviceMonitor *self = SRT_PORTAL_INPUT_DEVICE_MONITOR (monitor);

  g_return_if_fail (SRT_IS_PORTAL_INPUT_DEVICE_MONITOR (monitor));
  g_return_if_fail (self->state == NOT_STARTED);

  self->want_evdev = TRUE;
}

static void
device_added_cb (GDBusConnection *connection,
                 const gchar *sender_name,
                 const gchar *object_path,
                 const gchar *interface_name,
                 const gchar *signal_name,
                 GVariant *parameters,
                 gpointer user_data)
{
  SrtPortalInputDeviceMonitor *self = SRT_PORTAL_INPUT_DEVICE_MONITOR (user_data);
  g_autoptr(SrtPortalInputDevice) device = NULL;
  SrtSimpleInputDevice *simple;
  g_autoptr(GVariant) asv = NULL;
  g_autoptr(GVariant) variant = NULL;
  g_autoptr(GVariantIter) iter = NULL;
  const char *session_handle;
  const char *device_handle;
  const char *s;
  guint32 u32;

  g_return_if_fail (g_main_context_is_owner (self->monitor_context));
  g_return_if_fail (self->portal != NULL);

  if (!g_variant_is_of_type (parameters, G_VARIANT_TYPE ("(ooa{sv})")))
    return;

  g_variant_get (parameters, "(&o&o@a{sv})",
                 &session_handle, &device_handle, &asv);

  if (g_strcmp0 (session_handle, self->portal->session_handle) != 0)
    {
      g_debug ("Device %s is for a different session", device_handle);
      return;
    }

  g_debug ("New device %s", device_handle);
  device = g_object_new (SRT_TYPE_PORTAL_INPUT_DEVICE,
                         NULL);
  device->portal = portal_ref (self->portal);
  device->device_handle = g_strdup (device_handle);

  simple = SRT_SIMPLE_INPUT_DEVICE (device);

  if (g_variant_lookup (asv, "type_flags", "u", &u32))
    simple->type_flags = u32;

  if (g_variant_lookup (asv, "interface_flags", "u", &u32))
    simple->iface_flags = u32;

  if (g_variant_lookup (asv, "bus_type", "u", &u32))
    simple->bus_type = u32;

  if (g_variant_lookup (asv, "vendor_id", "u", &u32))
    simple->vendor_id = u32;

  if (g_variant_lookup (asv, "product_id", "u", &u32))
    simple->product_id = u32;

  if (g_variant_lookup (asv, "version", "u", &u32))
    simple->version = u32;

  if (g_variant_lookup (asv, "dev_node", "&s", &s))
    simple->dev_node = g_strdup (s);

  if (g_variant_lookup (asv, "sys_path", "&s", &s))
    simple->sys_path = g_strdup (s);

  if (g_variant_lookup (asv, "subsystem", "&s", &s))
    simple->subsystem = g_strdup (s);

  g_variant_lookup (asv, "udev_properties", "^aay", &simple->udev_properties);
  g_variant_lookup (asv, "uevent", "^ay", &simple->uevent);

  if (g_variant_lookup (asv, "hid_ancestor", "@a{sv}", &variant))
    {
      if (g_variant_lookup (variant, "bus_type", "u", &u32))
        simple->hid_ancestor.bus_type = u32;

      if (g_variant_lookup (variant, "vendor_id", "u", &u32))
        simple->hid_ancestor.vendor_id = u32;

      if (g_variant_lookup (variant, "product_id", "u", &u32))
        simple->hid_ancestor.product_id = u32;

      g_variant_lookup (variant, "sys_path", "s",
                        &simple->hid_ancestor.sys_path);
      g_variant_lookup (variant, "name", "s",
                        &simple->hid_ancestor.name);
      g_variant_lookup (variant, "phys", "s",
                        &simple->hid_ancestor.phys);
      g_variant_lookup (variant, "uniq", "s",
                        &simple->hid_ancestor.uniq);
      g_variant_lookup (variant, "uevent", "^ay",
                        &simple->hid_ancestor.uevent);

      g_clear_pointer (&variant, g_variant_unref);
    }

  if (g_variant_lookup (asv, "input_ancestor", "@a{sv}", &variant))
    {
      if (g_variant_lookup (variant, "bus_type", "u", &u32))
        simple->input_ancestor.bus_type = u32;

      if (g_variant_lookup (variant, "vendor_id", "u", &u32))
        simple->input_ancestor.vendor_id = u32;

      if (g_variant_lookup (variant, "product_id", "u", &u32))
        simple->input_ancestor.product_id = u32;

      if (g_variant_lookup (variant, "version", "u", &u32))
        simple->input_ancestor.version = u32;

      g_variant_lookup (variant, "sys_path", "s",
                        &simple->input_ancestor.sys_path);
      g_variant_lookup (variant, "name", "s",
                        &simple->input_ancestor.name);
      g_variant_lookup (variant, "phys", "s",
                        &simple->input_ancestor.phys);
      g_variant_lookup (variant, "uniq", "s",
                        &simple->input_ancestor.uniq);
      g_variant_lookup (variant, "uevent", "^ay",
                        &simple->input_ancestor.uevent);

      g_clear_pointer (&variant, g_variant_unref);
    }

  if (g_variant_lookup (asv, "usb_device_ancestor", "@a{sv}", &variant))
    {
      if (g_variant_lookup (variant, "vendor_id", "u", &u32))
        simple->usb_device_ancestor.vendor_id = u32;

      if (g_variant_lookup (variant, "product_id", "u", &u32))
        simple->usb_device_ancestor.product_id = u32;

      if (g_variant_lookup (variant, "version", "u", &u32))
        simple->usb_device_ancestor.device_version = u32;

      g_variant_lookup (variant, "sys_path", "s",
                        &simple->usb_device_ancestor.sys_path);
      g_variant_lookup (variant, "manufacturer", "s",
                        &simple->usb_device_ancestor.manufacturer);
      g_variant_lookup (variant, "product", "s",
                        &simple->usb_device_ancestor.product);
      g_variant_lookup (variant, "serial", "s",
                        &simple->usb_device_ancestor.serial);
      g_variant_lookup (variant, "uevent", "^ay",
                        &simple->usb_device_ancestor.uevent);

      g_clear_pointer (&variant, g_variant_unref);
    }

  if (g_variant_lookup (asv, "evdev_capabilities", "a{uay}", &iter))
    {
      guint32 type = 0;
      GVariant *ay = NULL;

      while (g_variant_iter_loop (iter, "{u@ay}", &type, &ay))
        {
          const unsigned char *bytes = NULL;
          unsigned long *buf = NULL;
          size_t n_bytes = 0;
          size_t n = 0;
          size_t i;

          bytes = g_variant_get_fixed_array (ay, &n_bytes, 1);

          switch (type)
            {
              case 0:
                buf = &simple->evdev_caps.ev[0];
                n = G_N_ELEMENTS (simple->evdev_caps.ev);
                break;

              case EV_KEY:
                buf = &simple->evdev_caps.keys[0];
                n = G_N_ELEMENTS (simple->evdev_caps.keys);
                break;

              case EV_ABS:
                buf = &simple->evdev_caps.abs[0];
                n = G_N_ELEMENTS (simple->evdev_caps.abs);
                break;

              case EV_REL:
                buf = &simple->evdev_caps.rel[0];
                n = G_N_ELEMENTS (simple->evdev_caps.rel);
                break;

              case EV_FF:
                buf = &simple->evdev_caps.ff[0];
                n = G_N_ELEMENTS (simple->evdev_caps.ff);
                break;

              default:
                continue;
            }

          memset (buf, '\0', n * sizeof (long));
          memcpy (buf, bytes, MIN (n * sizeof (long), n_bytes));

          for (i = 0; i < n; i++)
            buf[i] = GULONG_FROM_LE (buf[i]);
        }

      g_clear_pointer (&iter, g_variant_iter_free);
    }

  if (g_variant_lookup (asv, "input_properties", "@ay", &variant))
    {
      const unsigned char *bytes = NULL;
      size_t n_bytes = 0;
      size_t i;

      bytes = g_variant_get_fixed_array (variant, &n_bytes, 1);

      memset (simple->evdev_caps.props, '\0', sizeof (simple->evdev_caps.props));
      memcpy (simple->evdev_caps.props, bytes, MIN (sizeof (simple->evdev_caps.props), n_bytes));

      for (i = 0; i < G_N_ELEMENTS (simple->evdev_caps.props); i++)
        simple->evdev_caps.props[i] = GULONG_FROM_LE (simple->evdev_caps.props[i]);

      g_clear_pointer (&variant, g_variant_unref);
    }

  g_hash_table_replace (self->devices, device->device_handle,
                        g_object_ref (device));
  _srt_input_device_monitor_emit_added (SRT_INPUT_DEVICE_MONITOR (self),
                                        SRT_INPUT_DEVICE (device));
}

static void
device_removed_cb (GDBusConnection *connection,
                   const gchar *sender_name,
                   const gchar *object_path,
                   const gchar *interface_name,
                   const gchar *signal_name,
                   GVariant *parameters,
                   gpointer user_data)
{
  SrtPortalInputDeviceMonitor *self = SRT_PORTAL_INPUT_DEVICE_MONITOR (user_data);
  const char *session_handle;
  const char *device_handle;
  gpointer old;

  g_return_if_fail (g_main_context_is_owner (self->monitor_context));

  if (!g_variant_is_of_type (parameters, G_VARIANT_TYPE ("(oo)")))
    return;

  g_variant_get (parameters, "(&o&o)", &session_handle, &device_handle);

  if (g_strcmp0 (session_handle, self->portal->session_handle) != 0)
    {
      g_debug ("Device %s is for a different session", device_handle);
      return;
    }

  if (g_hash_table_lookup_extended (self->devices, device_handle, NULL, &old))
    {
      g_debug ("Removed device %s", device_handle);
      g_hash_table_steal (self->devices, device_handle);
      _srt_input_device_monitor_emit_removed (SRT_INPUT_DEVICE_MONITOR (self),
                                              old);
      g_object_unref (old);
    }
  else
    {
      g_debug ("Device %s doesn't exist", device_handle);
    }
}

static void
session_closed_cb (GDBusConnection *connection,
                   const gchar *sender_name,
                   const gchar *object_path,
                   const gchar *interface_name,
                   const gchar *signal_name,
                   GVariant *parameters,
                   gpointer user_data)
{
  Portal *portal = user_data;

  g_debug ("Session %s closed", object_path);
  clear_subscription (portal->connection, &portal->session_closed_id);
  g_clear_pointer (&portal->session_handle, g_free);
}

static void
request_closed_cb (GDBusConnection *connection,
                   const gchar *sender_name,
                   const gchar *object_path,
                   const gchar *interface_name,
                   const gchar *signal_name,
                   GVariant *parameters,
                   gpointer user_data)
{
  SrtPortalInputDeviceMonitor *self = SRT_PORTAL_INPUT_DEVICE_MONITOR (user_data);

  g_return_if_fail (g_main_context_is_owner (self->monitor_context));
  g_debug ("Request %s closed", object_path);
  g_clear_pointer (&self->start_request_handle, g_free);
}

static void
request_response_cb (GDBusConnection *connection,
                     const gchar *sender_name,
                     const gchar *object_path,
                     const gchar *interface_name,
                     const gchar *signal_name,
                     GVariant *parameters,
                     gpointer user_data)
{
  SrtPortalInputDeviceMonitor *self = SRT_PORTAL_INPUT_DEVICE_MONITOR (user_data);
  SrtInputDeviceMonitor *monitor = SRT_INPUT_DEVICE_MONITOR (user_data);

  g_return_if_fail (g_main_context_is_owner (self->monitor_context));
  g_return_if_fail (self->state == ENUMERATING);

  self->state = MONITORING;

  g_dbus_connection_signal_unsubscribe (self->portal->connection,
                                        self->request_response_id);
  self->request_response_id = 0;
  g_clear_pointer (&self->start_request_handle, g_free);

  if (self->flags & SRT_INPUT_DEVICE_MONITOR_FLAGS_ONCE)
    srt_input_device_monitor_stop (monitor);

  _srt_input_device_monitor_emit_all_for_now (monitor);
}

static gboolean
srt_portal_input_device_monitor_start (SrtInputDeviceMonitor *monitor,
                                       GError **error)
{
  SrtPortalInputDeviceMonitor *self = SRT_PORTAL_INPUT_DEVICE_MONITOR (monitor);
  /* Make sure the signals come out in the correct main-context */
  G_GNUC_UNUSED
  g_autoptr(GMainContextPusher) pusher = g_main_context_pusher_new (self->monitor_context);
  g_autoptr(GString) session_handle = NULL;
  g_autoptr(GString) request_handle = NULL;
  g_autoptr(GVariant) tuple = NULL;
  g_autofree gchar *handle_token = NULL;
  GVariantBuilder create_session_options = {};
  GVariantBuilder start_options = {};
  const char *unique_name;
  GDBusSignalFlags arg0_flags;
  const char *arg0 = NULL;

  g_return_val_if_fail (SRT_IS_PORTAL_INPUT_DEVICE_MONITOR (monitor), FALSE);
  g_return_val_if_fail (self->state == NOT_STARTED, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  self->state = ENUMERATING;

  unique_name = g_dbus_connection_get_unique_name (self->portal->connection);

  /* Peer-to-peer connections don't normally have a unique name at all */
  if (unique_name == NULL)
    unique_name = ":_";
  else
    g_assert (unique_name[0] == ':');

  handle_token = g_strdup_printf ("%08zx", GPOINTER_TO_SIZE (self));

  session_handle = g_string_new (unique_name + 1);
  g_strdelimit (session_handle->str, ".", '_');

  g_string_prepend (session_handle, PV_PORTAL_SESSION_PATH_PREFIX);
  g_string_append_c (session_handle, '/');
  g_string_append (session_handle, handle_token);

  self->portal->session_closed_id = g_dbus_connection_signal_subscribe (self->portal->connection,
                                                                        self->portal->bus_name,
                                                                        PV_PORTAL_SESSION_IFACE,
                                                                        "Closed",
                                                                        session_handle->str,
                                                                        NULL,
                                                                        G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
                                                                        session_closed_cb,
                                                                        self->portal,
                                                                        NULL);

  g_variant_builder_init (&create_session_options, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&create_session_options, "{sv}",
                         "session_handle_token",
                         g_variant_new_string (handle_token));
  tuple = g_dbus_connection_call_sync (self->portal->connection,
                                       self->portal->bus_name,
                                       self->portal->object_path,
                                       PV_PORTAL_INPUT1_IFACE,
                                       "CreateSession",
                                       g_variant_new ("(a{sv})",
                                                      &create_session_options),
                                       G_VARIANT_TYPE ("(o)"),
                                       G_DBUS_CALL_FLAGS_NONE,
                                       -1,
                                       NULL,
                                       error);

  if (tuple == NULL)
    return FALSE;

  g_variant_get (tuple, "(o)", &self->portal->session_handle);
  g_clear_pointer (&tuple, g_variant_unref);
  g_warn_if_fail (g_strcmp0 (self->portal->session_handle, session_handle->str) == 0);

  arg0_flags = G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE;
#if 0 && GLIB_CHECK_VERSION (2, 37, 0)
  arg0 = self->portal->session_handle;
  arg0_flags |= G_DBUS_SIGNAL_FLAGS_MATCH_ARG0_PATH;
#endif

  /* These signal emissions will happen in self->monitor_context */
  self->device_added_id = g_dbus_connection_signal_subscribe (self->portal->connection,
                                                              self->portal->bus_name,
                                                              PV_PORTAL_INPUT1_IFACE,
                                                              "DeviceAdded",
                                                              self->portal->object_path,
                                                              arg0,
                                                              arg0_flags,
                                                              device_added_cb,
                                                              self,
                                                              NULL);
  self->device_removed_id = g_dbus_connection_signal_subscribe (self->portal->connection,
                                                                self->portal->bus_name,
                                                                PV_PORTAL_INPUT1_IFACE,
                                                                "DeviceRemoved",
                                                                self->portal->object_path,
                                                                arg0,
                                                                arg0_flags,
                                                                device_removed_cb,
                                                                self,
                                                                NULL);

  request_handle = g_string_new (unique_name + 1);
  g_strdelimit (request_handle->str, ".", '_');

  g_string_prepend (request_handle, PV_PORTAL_REQUEST_PATH_PREFIX);
  g_string_append_c (request_handle, '/');
  g_string_append (request_handle, handle_token);

  self->request_closed_id = g_dbus_connection_signal_subscribe (self->portal->connection,
                                                                self->portal->bus_name,
                                                                PV_PORTAL_SESSION_IFACE,
                                                                "Closed",
                                                                request_handle->str,
                                                                NULL,
                                                                G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
                                                                request_closed_cb,
                                                                self,
                                                                NULL);

  self->request_response_id = g_dbus_connection_signal_subscribe (self->portal->connection,
                                                                  self->portal->bus_name,
                                                                  PV_PORTAL_REQUEST_IFACE,
                                                                  "Response",
                                                                  request_handle->str,
                                                                  NULL,
                                                                  G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
                                                                  request_response_cb,
                                                                  self,
                                                                  NULL);

  g_variant_builder_init (&start_options, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&start_options, "{sv}", "handle_token",
                         g_variant_new_string (handle_token));
  g_variant_builder_add (&start_options, "{sv}", "evdev",
                         g_variant_new_boolean (self->want_evdev));
  g_variant_builder_add (&start_options, "{sv}", "hidraw",
                         g_variant_new_boolean (self->want_hidraw));

  tuple = g_dbus_connection_call_sync (self->portal->connection,
                                       self->portal->bus_name,
                                       self->portal->object_path,
                                       PV_PORTAL_INPUT1_IFACE,
                                       "Start",
                                       g_variant_new ("(osa{sv})",
                                                      self->portal->session_handle,
                                                      "", /* no parent window */
                                                      &start_options),
                                       G_VARIANT_TYPE ("(o)"),
                                       G_DBUS_CALL_FLAGS_NONE,
                                       -1,
                                       NULL,
                                       error);

  if (tuple == NULL)
    return FALSE;

  g_variant_get (tuple, "(o)", &self->start_request_handle);
  g_warn_if_fail (g_strcmp0 (self->start_request_handle, request_handle->str) == 0);

  /* We'll emit all-for-now from the Reponse callback */
  return TRUE;
}

static void
srt_portal_input_device_monitor_stop (SrtInputDeviceMonitor *monitor)
{
  SrtPortalInputDeviceMonitor *self = SRT_PORTAL_INPUT_DEVICE_MONITOR (monitor);

  self->state = STOPPED;

  if (self->portal != NULL && self->portal->session_handle != NULL)
    portal_call_void_ignore_result (self->portal, self->portal->session_handle,
                                    PV_PORTAL_SESSION_IFACE, "Close");

  if (self->portal != NULL)
    {
      clear_subscription (self->portal->connection, &self->request_response_id);
      clear_subscription (self->portal->connection, &self->request_closed_id);
      clear_subscription (self->portal->connection, &self->device_added_id);
      clear_subscription (self->portal->connection, &self->device_removed_id);
    }

  if (self->portal != NULL && self->start_request_handle != NULL)
    portal_call_void_ignore_result (self->portal, self->start_request_handle,
                                    PV_PORTAL_REQUEST_IFACE, "Close");

  g_clear_pointer (&self->start_request_handle, g_free);
  g_clear_pointer (&self->socket_path, g_free);

  if (self->monitor_source != NULL)
    g_source_destroy (self->monitor_source);

  g_clear_pointer (&self->monitor_source, g_source_unref);
  g_clear_pointer (&self->monitor_context, g_main_context_unref);
  g_clear_pointer (&self->portal, portal_unref);
  g_clear_pointer (&self->devices, g_hash_table_unref);
}

static void
srt_portal_input_device_monitor_iface_init (SrtInputDeviceMonitorInterface *iface)
{
#define IMPLEMENT(x) iface->x = srt_portal_input_device_monitor_ ## x
  IMPLEMENT (request_raw_hid);
  IMPLEMENT (request_evdev);
  IMPLEMENT (start);
  IMPLEMENT (stop);
#undef IMPLEMENT
}

static void
srt_portal_input_device_monitor_initable_iface_init (GInitableIface *iface)
{
  iface->init = srt_portal_input_device_monitor_initable_init;
}
