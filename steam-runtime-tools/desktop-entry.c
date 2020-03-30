/*
 * Copyright © 2019 Collabora Ltd.
 *
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "steam-runtime-tools/desktop-entry.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>

#include "steam-runtime-tools/enums.h"
#include "steam-runtime-tools/glib-compat.h"
#include "steam-runtime-tools/utils.h"

/**
 * SECTION:steam
 * @title: Steam installation
 * @short_description: Information about the Steam installation
 * @include: steam-runtime-tools/steam-runtime-tools.h
 *
 * #SrtDesktopEntryIssues represents problems encountered with the Steam
 * installation.
 */

struct _SrtDesktopEntry
{
  /*< private >*/
  GObject parent;
  gchar *id;
  gchar *commandline;
  gchar *filename;
  gboolean is_default_handler;
  gboolean is_steam_handler;
};

struct _SrtDesktopEntryClass
{
  /*< private >*/
  GObjectClass parent_class;
};

enum {
  PROP_0,
  PROP_ID,
  PROP_COMMANDLINE,
  PROP_FILENAME,
  PROP_IS_DEFAULT_HANDLER,
  PROP_IS_STEAM_HANDLER,
  N_PROPERTIES
};

G_DEFINE_TYPE (SrtDesktopEntry, srt_desktop_entry, G_TYPE_OBJECT)

static void
srt_desktop_entry_init (SrtDesktopEntry *self)
{
}

static void
srt_desktop_entry_get_property (GObject *object,
                                guint prop_id,
                                GValue *value,
                                GParamSpec *pspec)
{
  SrtDesktopEntry *self = SRT_DESKTOP_ENTRY (object);

  switch (prop_id)
    {
      case PROP_ID:
        g_value_set_string (value, self->id);
        break;

      case PROP_COMMANDLINE:
        g_value_set_string (value, self->commandline);
        break;

      case PROP_FILENAME:
        g_value_set_string (value, self->filename);
        break;

      case PROP_IS_DEFAULT_HANDLER:
        g_value_set_boolean (value, self->is_default_handler);
        break;

      case PROP_IS_STEAM_HANDLER:
        g_value_set_boolean (value, self->is_steam_handler);
        break;

      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
srt_desktop_entry_set_property (GObject *object,
                                guint prop_id,
                                const GValue *value,
                                GParamSpec *pspec)
{
  SrtDesktopEntry *self = SRT_DESKTOP_ENTRY (object);

  switch (prop_id)
    {
      case PROP_ID:
        /* Construct-only */
        g_return_if_fail (self->id == NULL);
        self->id = g_value_dup_string (value);
        break;

      case PROP_COMMANDLINE:
        /* Construct only */
        g_return_if_fail (self->commandline == NULL);
        self->commandline = g_value_dup_string (value);
        break;

      case PROP_FILENAME:
        /* Construct only */
        g_return_if_fail (self->filename == NULL);
        self->filename = g_value_dup_string (value);
        break;

      case PROP_IS_DEFAULT_HANDLER:
        /* Construct only */
        self->is_default_handler = g_value_get_boolean (value);
        break;

      case PROP_IS_STEAM_HANDLER:
        /* Construct only */
        self->is_steam_handler = g_value_get_boolean (value);
        break;

      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
srt_desktop_entry_finalize (GObject *object)
{
  SrtDesktopEntry *self = SRT_DESKTOP_ENTRY (object);

  g_free (self->id);
  g_free (self->commandline);
  g_free (self->filename);

  G_OBJECT_CLASS (srt_desktop_entry_parent_class)->finalize (object);
}

static GParamSpec *properties[N_PROPERTIES] = { NULL };

static void
srt_desktop_entry_class_init (SrtDesktopEntryClass *cls)
{
  GObjectClass *object_class = G_OBJECT_CLASS (cls);

  object_class->get_property = srt_desktop_entry_get_property;
  object_class->set_property = srt_desktop_entry_set_property;
  object_class->finalize = srt_desktop_entry_finalize;

  properties[PROP_ID] =
    g_param_spec_string ("id", "ID", "Desktop entry ID",
                         NULL,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  properties[PROP_COMMANDLINE] =
    g_param_spec_string ("commandline", "Commandline",
                         "Commandline with which the application will be started",
                         NULL,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  properties[PROP_FILENAME] =
    g_param_spec_string ("filename", "Filename",
                         "Full path to the desktop entry file",
                         NULL,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  properties[PROP_IS_DEFAULT_HANDLER] =
    g_param_spec_boolean ("is-default-handler", "Is default handler?",
                          "TRUE if the entry is the default that handles the URI scheme \"steam\"",
                          FALSE,
                          G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS);

  properties[PROP_IS_STEAM_HANDLER] =
    g_param_spec_boolean ("is-steam-handler", "Is Steam handler?",
                          "TRUE if the entry is of the type \"x-scheme-handler/steam\"",
                          FALSE,
                          G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPERTIES, properties);
}



/**
 * srt_desktop_entry_get_soname:
 * @self: a library
 *
 * Return the SONAME (machine-readable runtime name) of @self.
 *
 * Returns: A string like `libz.so.1`, which is valid as long as @self
 *  is not destroyed.
 */
const char *
srt_desktop_entry_get_id (SrtDesktopEntry *self)
{
  g_return_val_if_fail (SRT_IS_DESKTOP_ENTRY (self), NULL);
  return self->id;
}
/**
 * srt_desktop_entry_get_soname:
 * @self: a library
 *
 * Return the SONAME (machine-readable runtime name) of @self.
 *
 * Returns: A string like `libz.so.1`, which is valid as long as @self
 *  is not destroyed.
 */
const char *
srt_desktop_entry_get_commandline (SrtDesktopEntry *self)
{
  g_return_val_if_fail (SRT_IS_DESKTOP_ENTRY (self), NULL);
  return self->commandline;
}
/**
 * srt_desktop_entry_get_soname:
 * @self: a library
 *
 * Return the SONAME (machine-readable runtime name) of @self.
 *
 * Returns: A string like `libz.so.1`, which is valid as long as @self
 *  is not destroyed.
 */
const char *
srt_desktop_entry_get_filename (SrtDesktopEntry *self)
{
  g_return_val_if_fail (SRT_IS_DESKTOP_ENTRY (self), NULL);
  return self->filename;
}

gboolean
srt_desktop_entry_is_default_handler (SrtDesktopEntry *self)
{
  g_return_val_if_fail (SRT_IS_DESKTOP_ENTRY (self), FALSE);
  return self->is_default_handler;
}

gboolean
srt_desktop_entry_is_steam_handler (SrtDesktopEntry *self)
{
  g_return_val_if_fail (SRT_IS_DESKTOP_ENTRY (self), FALSE);
  return self->is_steam_handler;
}



static SrtDesktopEntry *
srt_desktop_entry_new (const gchar *id,
                       const gchar *commandline,
                       const gchar *filename,
                       gboolean is_default_handler,
                       gboolean is_steam_handler)
{
  return g_object_new (SRT_TYPE_DESKTOP_ENTRY,
                       "id", id,
                       "commandline", commandline,
                       "filename", filename,
                       "is-default-handler", is_default_handler,
                       "is-steam-handler", is_steam_handler,
                       NULL);
}

GList *
srt_desktop_entry_list_desktop_entries (void)
{
  GList *ret = NULL;
  GList *app_list = NULL;
  GHashTable *found_handlers = NULL;
  GHashTable *known_ids = NULL;
  GAppInfo *default_handler = NULL;
  const char *default_handler_id = NULL;
  const GList *this_app;

  default_handler = g_app_info_get_default_for_uri_scheme ("steam");
  if (default_handler != NULL)
    default_handler_id = g_app_info_get_id (default_handler);

  app_list = g_app_info_get_all_for_type ("x-scheme-handler/steam");

  found_handlers = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  known_ids = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  /* The official Steam package or Debian */
  g_hash_table_add (known_ids, g_strdup ("steam.desktop"));
  /* Flathub */
  g_hash_table_add (known_ids, g_strdup ("com.valvesoftware.Steam.desktop"));
  /* Arch Linux Steam native */
  g_hash_table_add (known_ids, g_strdup ("steam-native.desktop"));

  for (this_app = app_list; this_app != NULL; this_app = this_app->next)
    {
      const char *id = NULL;
      const char *commandline = NULL;
      const char *filename = NULL;
      gboolean is_default_handler = FALSE;
      gboolean is_steam_handler = TRUE;
      SrtDesktopEntry *entry = NULL;

      id = g_app_info_get_id (this_app->data);
      commandline = g_app_info_get_commandline (this_app->data);
      if (G_IS_DESKTOP_APP_INFO (this_app->data))
        filename = g_desktop_app_info_get_filename (this_app->data);

      if (id != NULL)
        {
          g_hash_table_add (found_handlers, g_strdup (id));
          is_default_handler = (id == default_handler_id);
        }

      entry = srt_desktop_entry_new (id, commandline, filename, is_default_handler, is_steam_handler);
      ret = g_list_prepend (ret, entry);
    }

  g_list_free_full (app_list, g_object_unref);
  app_list = g_app_info_get_all ();

  for (this_app = app_list; this_app != NULL; this_app = this_app->next)
    {
      const char *id = NULL;
      const char *commandline = NULL;
      const char *filename = NULL;
      gboolean is_default_handler = FALSE;
      gboolean is_steam_handler = FALSE;
      SrtDesktopEntry *entry = NULL;

      id = g_app_info_get_id (this_app->data);
      if (!g_hash_table_contains (known_ids, id) || g_hash_table_contains (found_handlers, id))
        continue;

      commandline = g_app_info_get_commandline (this_app->data);
      if (G_IS_DESKTOP_APP_INFO (this_app->data))
        filename = g_desktop_app_info_get_filename (this_app->data);

      is_default_handler = (id == default_handler_id);

      entry = srt_desktop_entry_new (id, commandline, filename, is_default_handler, is_steam_handler);
      ret = g_list_prepend (ret, entry);
    }
  
  g_hash_table_unref (found_handlers);
  g_hash_table_unref (known_ids);
  g_list_free_full (app_list, g_object_unref);
  if (default_handler != NULL)
    g_object_unref (default_handler);

  return g_list_reverse (ret);
}
