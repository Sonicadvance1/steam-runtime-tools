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

#include "steam-runtime-tools/controller.h"
#include "steam-runtime-tools/controller-internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

#include <glib.h>
#include <sys/acl.h>
#include <acl/libacl.h>
#include <libudev.h>
#include <json-glib/json-glib.h>

#include "steam-runtime-tools/glib-compat.h"

/**
 * SECTION:controller
 * @title: Controller information
 * @short_description: Information about controllers
 * @include: steam-runtime-tools/steam-runtime-tools.h
 *
 * #SrtControllerIssues represents problems encountered with the
 * currently plugged in controllers.
 * #SrtUinputIssues represents problems that are likely to prevent Steam
 * from emulating additional input devices via `/dev/uinput`.
 */

static void
pair_id_free (PairID *pair_id)
{
  g_return_if_fail (pair_id != NULL);

  if (pair_id->id_vendor)
    g_pattern_spec_free (pair_id->id_vendor);
  if (pair_id->id_product)
    g_pattern_spec_free (pair_id->id_product);

  g_slice_free (PairID, pair_id);
}

static void
kernel_pattern_free (KernelPattern *kernel_pattern)
{
  g_return_if_fail (kernel_pattern != NULL);

  if (kernel_pattern->pattern)
    g_pattern_spec_free (kernel_pattern->pattern);

  g_slice_free (KernelPattern, kernel_pattern);
}

/*
 * _free_controller_patterns:
 * @patterns: The ControllerPatterns object to free
 *
 * Free the memory pointed to by @patterns.
 */
void
_free_controller_patterns (ControllerPatterns *patterns)
{
  g_return_if_fail (patterns != NULL);

  g_list_free_full (patterns->kernels_list, (GDestroyNotify) kernel_pattern_free);
  g_list_free_full (patterns->hidraw_id_list, (GDestroyNotify) pair_id_free);
  g_list_free_full (patterns->usb_subsystem_id_list, (GDestroyNotify) pair_id_free);
  g_slice_free (ControllerPatterns, patterns);
}

/*
 * _srt_controller_initialize_patterns:
 * @filename: (not nullable): Path to the JSON controllers expectations
 *
 * Create a #ControllerPatterns object filled with the
 * controllers expectations from the @filename JSON.
 *
 * Returns: (transfer full): A #ControllerPatterns object, or %NULL.
 *  Free with _free_controller_patterns
 */
ControllerPatterns *
_srt_controller_initialize_patterns (const gchar *filename)
{
  JsonNode *node = NULL;
  JsonArray *array = NULL;
  JsonObject *json_root;
  JsonObject *json;
  JsonParser *parser;
  const char *id_product;
  const char *id_vendor;
  char *kernels;
  gchar *id_product_up;
  gchar *id_vendor_up;
  GPatternSpec *id_product_pattern;
  GPatternSpec *id_vendor_pattern;
  ControllerPatterns *patterns = NULL;
  GError *error = NULL;

  g_return_val_if_fail (filename != NULL, NULL);

  parser = json_parser_new ();
  json_parser_load_from_file (parser, filename, &error);
  if (error)
    {
      g_debug ("Error parsing the controller expectation file: %s", error->message);
      g_clear_error (&error);
      goto out;
    }
  node = json_parser_get_root (parser);
  json_root = json_node_get_object (node);

  if (json_object_has_member (json_root, "all_usb"))
    {
      patterns = g_slice_new0 (ControllerPatterns);

      array = json_object_get_array_member (json_root, "all_usb");
      for (guint i = 0; i < json_array_get_length (array); i++)
        {
          PairID *pair_id;
          json = json_array_get_object_element (array, i);
          if (!json_object_has_member (json, "vendor"))
            {
              g_debug ("Malformed controller expectation file, missing 'vendor' attribute.");
              _free_controller_patterns (patterns);
              patterns = NULL;
              goto out;
            }
          pair_id = g_slice_new (PairID);
          id_vendor = json_object_get_string_member (json, "vendor");

          id_product = NULL;
          if (json_object_has_member (json, "product"))
            id_product = json_object_get_string_member (json, "product");

          if (id_product)
            id_product_pattern = g_pattern_spec_new (id_product);
          else
            id_product_pattern = NULL;

          pair_id->id_product = id_product_pattern;

          if (id_vendor)
            id_vendor_pattern = g_pattern_spec_new (id_vendor);
          else
            id_vendor_pattern = NULL;

          pair_id->id_vendor = id_vendor_pattern;

          if (json_object_has_member (json, "read"))
            pair_id->skip_read_check = !json_object_get_boolean_member (json, "read");

          if (json_object_has_member (json, "write"))
            pair_id->skip_write_check = !json_object_get_boolean_member (json, "write");

          patterns->usb_subsystem_id_list = g_list_prepend (patterns->usb_subsystem_id_list, pair_id);
        }
    }

  if (json_object_has_member (json_root, "hidraw"))
    {
      if (patterns == NULL)
        patterns = g_slice_new0 (ControllerPatterns);

      array = json_object_get_array_member (json_root, "hidraw");
      for (guint i = 0; i < json_array_get_length (array); i++)
        {
          PairID *pair_id;
          KernelPattern *kernel_pattern;
          json = json_array_get_object_element (array, i);
          if (!json_object_has_member (json, "vendor"))
            {
              g_debug ("Malformed controller expectation file, missing 'vendor' attribute.");
              _free_controller_patterns (patterns);
              patterns = NULL;
              goto out;
            }
          id_vendor = json_object_get_string_member (json, "vendor");

          id_product = NULL;
          if (json_object_has_member (json, "product"))
            id_product = json_object_get_string_member (json, "product");

          if (!json_object_has_member (json, "usb") || json_object_get_boolean_member (json, "usb"))
            {
              pair_id = g_slice_new0 (PairID);
              if (id_product)
                id_product_pattern = g_pattern_spec_new (id_product);
              else
                id_product_pattern = NULL;

              pair_id->id_product = id_product_pattern;

              if (id_vendor)
                id_vendor_pattern = g_pattern_spec_new (id_vendor);
              else
                id_vendor_pattern = NULL;

              pair_id->id_vendor = id_vendor_pattern;

              if (json_object_has_member (json, "read"))
                pair_id->skip_read_check = !json_object_get_boolean_member (json, "read");

              if (json_object_has_member (json, "write"))
                pair_id->skip_write_check = !json_object_get_boolean_member (json, "write");

              patterns->hidraw_id_list = g_list_prepend (patterns->hidraw_id_list, pair_id);
            }

          if (!json_object_has_member (json, "bluetooth") || json_object_get_boolean_member (json, "bluetooth"))
            {
              kernel_pattern = g_slice_new0 (KernelPattern);
              id_vendor_up = g_utf8_strup (id_vendor, -1);
              if (id_product)
                id_product_up = g_utf8_strup (id_product, -1);
              else
                id_product_up = g_strdup ("");

              if (json_object_has_member (json, "read"))
                kernel_pattern->skip_read_check = !json_object_get_boolean_member (json, "read");

              if (json_object_has_member (json, "write"))
                kernel_pattern->skip_write_check = !json_object_get_boolean_member (json, "write");

              kernels = g_strdup_printf ("*%s:%s*",
                                         id_vendor_up,
                                         id_product_up);
              kernel_pattern->pattern = g_pattern_spec_new (kernels);
              patterns->kernels_list = g_list_prepend (patterns->kernels_list, kernel_pattern);

              g_free (kernels);
              g_free (id_vendor_up);
              g_free (id_product_up);
            }
        }
    }

  out:
    g_object_unref (parser);
    return patterns;
}

/*
 * search_expectation_usb:
 * @dev: The device to search
 * @devnode: The device devnode, used to check for access permissions
 * @usb_hidraw: If %TRUE the USB hidraw patterns will be used.
 *  Otherwise the USB subsystem will be used.
 * @patterns: a ControllerPatterns object containing the expectations
 * @issues: (optional) (inout): After the permissions check the bitfield
 *  `SRT_CONTROLLER_ISSUES_NOT_ENOUGH_PERMISSIONS` might be added to the
 *  given @issues parameter
 *
 * Returns: %TRUE if @patterns contains the given @dev.
 */
static gboolean
search_expectation_usb (struct udev_device *dev,
                        const char* devnode,
                        gboolean usb_hidraw,
                        ControllerPatterns *patterns,
                        SrtControllerIssues *issues)
{
  GList *l;
  gsize vendor_length;
  gsize product_length;
  gchar *vendor_reversed = NULL;
  gchar *product_reversed = NULL;
  gboolean found = FALSE;
  PairID *pair_id = NULL;
  const char *id_vendor;
  const char *id_product;

  g_return_val_if_fail (dev != NULL, FALSE);
  g_return_val_if_fail (devnode != NULL, FALSE);
  g_return_val_if_fail (patterns != NULL, FALSE);

  id_vendor = udev_device_get_sysattr_value (dev, "idVendor");
  id_product = udev_device_get_sysattr_value (dev, "idProduct");
  if (!id_vendor)
    return FALSE;

  vendor_length = strlen (id_vendor);
  vendor_reversed = g_strreverse (g_strdup (id_vendor));

  if (id_product)
    {
      product_length = strlen (id_product);
      product_reversed = g_strreverse (g_strdup (id_product));
    }

  if (usb_hidraw)
    l = patterns->hidraw_id_list;
  else
    l = patterns->usb_subsystem_id_list;

  for (; l != NULL; l = l->next)
    {
      pair_id = l->data;
      if (pair_id->id_vendor &&
          !g_pattern_match (pair_id->id_vendor, vendor_length, id_vendor, vendor_reversed))
        {
          continue;
        }
      if (id_product &&
          pair_id->id_product &&
          !g_pattern_match (pair_id->id_product, product_length, id_product, product_reversed))
        {
          continue;
        }

      found = TRUE;

      int mode = F_OK;

      g_debug ("a controller has been found: %s:%s %s",
                id_vendor,
                id_product ? id_product : "",
                devnode);

      if (!pair_id->skip_read_check)
        mode |= R_OK;

      if (!pair_id->skip_write_check)
        mode |= W_OK;

      if (access (devnode, mode) != 0)
        {
          if (issues)
            *issues |= SRT_CONTROLLER_ISSUES_NOT_ENOUGH_PERMISSIONS;
          g_debug ("not enough permissions for the device: %s", devnode);
        }

      break;
    }

  g_free (vendor_reversed);
  g_free (product_reversed);
  return found;
}

/*
 * search_expectation_kernels:
 * @dev: The device to search
 * @devnode: The device devnode, used to check for access permissions
 * @patterns: a ControllerPatterns object containing the expectations
 * @issues: (optional) (inout): After the permissions check the bitfield
 *  `SRT_CONTROLLER_ISSUES_NOT_ENOUGH_PERMISSIONS` might be added to the
 *  given @issues parameter
 *
 * Returns: %TRUE if @patterns contains the given @dev.
 */
static gboolean
search_expectation_kernels (struct udev_device *dev,
                            const char* devnode,
                            ControllerPatterns *patterns,
                            SrtControllerIssues *issues)
{
  GList *l;
  gsize length;
  gchar *sysname_reversed = NULL;
  gboolean found = FALSE;
  KernelPattern *kernel_pattern = NULL;
  const char *sysname;

  g_return_val_if_fail (dev != NULL, FALSE);
  g_return_val_if_fail (devnode != NULL, FALSE);
  g_return_val_if_fail (patterns != NULL, FALSE);

  sysname = udev_device_get_sysname (dev);

  if (!sysname)
    return FALSE;

  length = strlen (sysname);
  sysname_reversed = g_strreverse (g_strdup (sysname));

  for (l = patterns->kernels_list; l != NULL; l = l->next)
    {
      kernel_pattern = l->data;
      if (g_pattern_match (kernel_pattern->pattern, length, sysname, sysname_reversed))
        {
          found = TRUE;
          g_debug ("a controller has been found: %s %s", sysname, devnode);

          int mode = F_OK;

          if (!kernel_pattern->skip_read_check)
            mode |= R_OK;

          if (!kernel_pattern->skip_write_check)
            mode |= W_OK;

          if (access (devnode, mode) != 0)
            {
              if (issues)
                *issues |= SRT_CONTROLLER_ISSUES_NOT_ENOUGH_PERMISSIONS;
              g_debug ("not enough permissions for the device: %s", devnode);
            }

          break;
        }
    }

    g_free (sysname_reversed);
    return found;
}

/*
 * _srt_controller_check_permissions:
 * @patterns: a ControllerPatterns object containing the expectations
 *
 * Check the currently plugged in controllers permissions
 * against the @patterns object.
 *
 * Returns: A bitfield containing problems, or %SRT_CONTROLLER_ISSUES_NONE
 *  if no problems were found.
 */
SrtControllerIssues
_srt_controller_check_permissions (ControllerPatterns *patterns)
{
  SrtControllerIssues issues = SRT_CONTROLLER_ISSUES_NONE;
  struct udev_list_entry *devices;
  struct udev_list_entry *entry;
  struct udev *udev;
  struct udev_enumerate* enumerate;
  const char *value;

  if (!patterns)
    {
      g_debug ("the controller expectation patterns are empty");
      issues |= SRT_CONTROLLER_ISSUES_UNKNOWN_EXPECTATIONS;
      return issues;
    }

  udev = udev_new ();
  enumerate = udev_enumerate_new (udev);

  udev_enumerate_add_match_subsystem (enumerate, "usb");
  udev_enumerate_add_match_subsystem (enumerate, "hidraw");
  udev_enumerate_scan_devices (enumerate);

  devices = udev_enumerate_get_list_entry (enumerate);

  udev_list_entry_foreach (entry, devices)
    {
      gboolean found = FALSE;
      const char* path = udev_list_entry_get_name (entry);
      struct udev_device* dev = udev_device_new_from_syspath (udev, path);
      if (!dev)
        {
          g_debug ("could not create udev device: %s", g_strerror (errno));
          issues |= SRT_CONTROLLER_ISSUES_CANNOT_INSPECT;
          continue;
        }

      struct udev_device *current_dev = dev;
      const char* devnode = udev_device_get_devnode (dev);

      value = udev_device_get_sysattr_value (current_dev, "subsystem");
      /* If we have an hidraw device we need to get the hid parent to gather
      * the information we need about the device */
      if (g_strcmp0 (value, "hidraw") == 0 && devnode)
        {
          current_dev = udev_device_get_parent_with_subsystem_devtype (current_dev, "hid", NULL);
          if (current_dev)
            found = search_expectation_kernels (current_dev, devnode, patterns, &issues);

          /* If we haven't found the device we try with the USB parent */
          if (!found && current_dev)
            {
              current_dev = udev_device_get_parent_with_subsystem_devtype (current_dev,
                                                                           "usb",
                                                                           "usb_device");

              if (current_dev)
                search_expectation_usb (current_dev, devnode, TRUE, patterns, &issues);
            }
        }

      if (g_strcmp0 (value, "usb") == 0 && current_dev && devnode)
        search_expectation_usb (current_dev, devnode, FALSE, patterns, &issues);

      udev_device_unref(dev);
    }

  udev_enumerate_unref (enumerate);
  udev_unref (udev);

  return issues;
}

/*
 * _srt_check_uinput:
 *
 * Check problems encountered with /dev/uinput permissions.
 * Being able to write in /dev/uinput is required for the Steam client
 * to emulate controllers, keyboards, mice and other input devices based
 * on input from the Steam Controller or a remote streaming client.
 *
 * Returns: A bitfield containing problems, or %SRT_UINPUT_ISSUES_NONE
 *  if no problems were found.
 */
SrtUinputIssues
_srt_check_uinput (void)
{
  SrtUinputIssues issues = SRT_UINPUT_ISSUES_NONE;
  acl_t acl = NULL;
  acl_entry_t entry;
  acl_permset_t permset;
  acl_tag_t tag;
  int entryId;
  int permVal;
  uid_t current_uid;
  uid_t *uidp;
  gboolean found_acl_current_user = FALSE;

  int fd = open ("/dev/uinput", O_WRONLY | O_NONBLOCK);

  if (fd < 0)
    {
      g_debug ("Failed to open /dev/uinput for writing: %s",
               g_strerror (errno));
      issues |= SRT_UINPUT_ISSUES_CANNOT_WRITE;
      goto out;
    }
  close (fd);

  acl = acl_get_file ("/dev/uinput", ACL_TYPE_ACCESS);

  if (acl == NULL)
    {
      g_debug ("Failed to get /dev/uinput ACL: %s", g_strerror (errno));
      issues |= SRT_UINPUT_ISSUES_CANNOT_INSPECT;
      goto out;
    }

  current_uid = getuid ();

  for (entryId = ACL_FIRST_ENTRY ; ; entryId = ACL_NEXT_ENTRY)
    {
      if (acl_get_entry (acl, entryId, &entry) != 1)
        break;

      if (acl_get_tag_type (entry, &tag) == -1)
        {
          g_debug ("Failed to get ACL tag type: %s", g_strerror (errno));
          issues |= SRT_UINPUT_ISSUES_CANNOT_INSPECT;
          goto out;
        }

      if (tag == ACL_USER || tag == ACL_MASK)
        {
          if (tag == ACL_USER)
            {
              uidp = acl_get_qualifier (entry);
              if (uidp == NULL)
                {
                  g_debug ("Failed to get ACL qualifier: %s", g_strerror (errno));
                  issues |= SRT_UINPUT_ISSUES_CANNOT_INSPECT;
                  goto out;
                }

              /* We just care about the current user */
              if (*uidp != current_uid)
                {
                  acl_free (uidp);
                  continue;
                }

              found_acl_current_user = TRUE;
              acl_free (uidp);
            }

          if (acl_get_permset(entry, &permset) == -1)
            {
              g_debug ("Failed to get ACL permset: %s", g_strerror (errno));
              issues |= SRT_UINPUT_ISSUES_CANNOT_INSPECT;
              goto out;
            }

          /* We expect both read and write permissions to be set */
          permVal = acl_get_perm (permset, ACL_READ);
          if (permVal == -1)
            {
              g_debug ("Failed to get ACL read permission: %s", g_strerror (errno));
              issues |= SRT_UINPUT_ISSUES_CANNOT_INSPECT;
              goto out;
            }
          if (permVal != 1)
            issues |= SRT_UINPUT_ISSUES_NOT_ENOUGH_PERMISSIONS;

          permVal = acl_get_perm (permset, ACL_WRITE);
          if (permVal == -1)
            {
              g_debug ("Failed to get ACL write permission: %s", g_strerror (errno));
              issues |= SRT_UINPUT_ISSUES_CANNOT_INSPECT;
              goto out;
            }
          if (permVal != 1)
            issues |= SRT_UINPUT_ISSUES_NOT_ENOUGH_PERMISSIONS;
        }
    }

  out:
    if (!found_acl_current_user)
      issues |= SRT_UINPUT_ISSUES_MISSING_USER_ACL;

    acl_free (acl);
    return issues;
}
