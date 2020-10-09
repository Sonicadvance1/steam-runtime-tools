/*
 * libudev-based input device monitor, adapted from SDL code.
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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#ifndef VERSION
#define VERSION "(unknown)"
#endif

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/input.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <libudev.h>

#define VID_MICROSOFT 0x045e

static const char *
nonnull (const char *s)
{
  if (s != NULL)
    return s;

  return "(null)";
}

enum
{
  OPTION_HELP = 1,
  OPTION_ONCE,
  OPTION_VERSION,
};

struct option long_options[] =
{
    { "once", no_argument, NULL, OPTION_ONCE },
    { "version", no_argument, NULL, OPTION_VERSION },
    { "help", no_argument, NULL, OPTION_HELP },
    { NULL, 0, NULL, 0 }
};

static void usage (int code) __attribute__((__noreturn__));

/*
 * Print usage information and exit with status @code.
 */
static void
usage (int code)
{
  FILE *fp;

  if (code == 0)
    fp = stdout;
  else
    fp = stderr;

  fprintf (fp, "Usage: %s [--once]\n",
           program_invocation_short_name);
  exit (code);
}

static volatile sig_atomic_t signalled = 0;

static void signal_handler (int signum)
{
  signalled = signum;
}

#define SDL_arraysize(arr)      (sizeof(arr)/sizeof(arr[0]))
#define BITS_PER_LONG           (sizeof(unsigned long) * 8)
#define NBITS(x)                ((((x)-1)/BITS_PER_LONG)+1)
#define OFF(x)                  ((x)%BITS_PER_LONG)
#define LONG(x)                 ((x)/BITS_PER_LONG)
#define test_bit(bit, array)    ((array[LONG(bit)] >> OFF(bit)) & 1)

static void
get_caps (struct udev_device *dev,
          struct udev_device *parent,
          const char *attr,
          unsigned long *bitmask,
          size_t bitmask_len)
{
  const char *value;
  char *text;
  char *word;
  int i;
  unsigned long v;

  memset(bitmask, 0, bitmask_len * sizeof(*bitmask));
  value = udev_device_get_sysattr_value (parent, attr);

  if (!value)
    return;

  text = strdup (value);
  i = 0;

  while ((word = strrchr(text, ' ')) != NULL)
    {
      v = strtoul (word + 1, NULL, 16);

      if (i < bitmask_len)
          bitmask[i] = v;

      ++i;
      *word = '\0';
    }

  v = strtoul(text, NULL, 16);

  if (i < bitmask_len)
    bitmask[i] = v;

  free (text);
}

static int
wine_is_xbox_gamepad (unsigned vendor,
                      unsigned product)
{
  if (vendor == VID_MICROSOFT)
    {
      switch (product)
        {
          case 0x0202:
          case 0x0285:
          case 0x0289:
          case 0x028e:
          case 0x028f:
          case 0x02d1:
          case 0x02dd:
          case 0x02e0:
          case 0x02e3:
          case 0x02e6:
          case 0x02ea:
          case 0x02fd:
          case 0x0719:
            return 1;
          default:
            break;
        }
    }

  return 0;
}

static int
fallback_is_joystick (int fd)
{
    struct input_id inpid;
    unsigned long evbit[NBITS(EV_MAX)] = { 0 };
    unsigned long keybit[NBITS(KEY_MAX)] = { 0 };
    unsigned long absbit[NBITS(ABS_MAX)] = { 0 };

    if ((ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), evbit) < 0) ||
        (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybit)), keybit) < 0) ||
        (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbit)), absbit) < 0)) {
        return (0);
    }

    if (!(test_bit(EV_KEY, evbit) && test_bit(EV_ABS, evbit) &&
          test_bit(ABS_X, absbit) && test_bit(ABS_Y, absbit))) {
        return 0;
    }

    if (ioctl(fd, EVIOCGID, &inpid) < 0) {
        return 0;
    }

    return 1;
}

static void
print_device_details (struct udev_device *dev,
                      const char *indent)
{
  struct udev_list_entry *entry;

  printf ("%sdevnode: %s\n",
          indent, nonnull (udev_device_get_devnode (dev)));
  printf ("%ssyspath: %s\n",
          indent, nonnull (udev_device_get_syspath (dev)));
  printf ("%ssysname: %s\n",
          indent, nonnull (udev_device_get_sysname (dev)));
  printf ("%ssysnum: %s\n",
          indent, nonnull (udev_device_get_sysnum (dev)));
  printf ("%sdevpath: %s\n",
          indent, nonnull (udev_device_get_devpath (dev)));
  printf ("%sdevnum: %u,%u\n",
          indent,
          gnu_dev_major (udev_device_get_devnum (dev)),
          gnu_dev_minor (udev_device_get_devnum (dev)));
  printf ("%sdevtype: %s\n", indent, udev_device_get_devtype (dev));
  printf ("%ssubsystem: %s\n",
          indent, nonnull (udev_device_get_subsystem (dev)));
  printf ("%sdriver: %s\n",
          indent, nonnull (udev_device_get_driver (dev)));

  printf ("%ssysfs attributes:\n", indent);

  for (entry = udev_device_get_sysattr_list_entry (dev);
       entry != NULL;
       entry = udev_list_entry_get_next (entry))
    {
      const char *name = udev_list_entry_get_name (entry);
      const char *value = udev_device_get_sysattr_value (dev, name);

      if (value == NULL || strchr (value, '\n') == NULL)
        {
          printf ("%s\t%s=%s\n",
                  indent,
                  nonnull (name),
                  nonnull (value));
        }
      else
        {
          char *copy = strdup (value);
          char *saveptr = NULL;
          char *line;

          printf ("%s\t%s=\n",
                  indent,
                  nonnull (name));

          for (line = strtok_r (copy, "\n", &saveptr);
               line != NULL;
               line = strtok_r (NULL, "\n", &saveptr))
            printf ("%s\t\t%s\n", indent, line);

          free (copy);
        }
    }
  printf ("%s\t.\n", indent);

  printf ("%sproperties:\n", indent);

  for (entry = udev_device_get_properties_list_entry (dev);
       entry != NULL;
       entry = udev_list_entry_get_next (entry))
    {
      printf ("%s\t%s=%s\n",
              indent,
              nonnull (udev_list_entry_get_name (entry)),
              nonnull (udev_list_entry_get_value (entry)));
    }

  printf ("%s\t.\n", indent);
}

static void
added (struct udev_device *dev)
{
  struct udev_device *parent;
  struct input_id inpid;
  unsigned long bitmask_ev[NBITS(EV_MAX)];
  unsigned long bitmask_abs[NBITS(ABS_MAX)];
  unsigned long bitmask_key[NBITS(KEY_MAX)];
  unsigned long bitmask_rel[NBITS(REL_MAX)];
  unsigned long keyboard_mask;
  const char *val;
  bool guessed = false;
  int fd;

  printf ("added: %p\n", dev);

  print_device_details (dev, "\t");

  parent = udev_device_get_parent_with_subsystem_devtype (dev, "hid", NULL);

  if (parent != NULL)
    {
      printf ("\tHas a parent device in HID subsystem (Wine would use this):\n");
      print_device_details (parent, "\t\t");

    }

  parent = dev;

  while (parent != NULL
         && udev_device_get_sysattr_value (parent, "bcdDevice") == NULL)
    parent = udev_device_get_parent (parent);

  if (parent != NULL)
    {
      printf ("\tHas an ancestor device with bcdDevice (Wine would use this):\n");
      print_device_details (parent, "\t\t");
    }

  parent = dev;

  while (parent != NULL
         && udev_device_get_sysattr_value (parent, "capabilities/ev") == NULL)
    parent = udev_device_get_parent_with_subsystem_devtype (parent, "input", NULL);

  if (parent != NULL)
    {
      printf ("\tHas an ancestor device with input capabilities (SDL would use this):\n");
      print_device_details (parent, "\t\t");

      get_caps(dev, parent, "capabilities/ev", bitmask_ev, SDL_arraysize(bitmask_ev));
      get_caps(dev, parent, "capabilities/abs", bitmask_abs, SDL_arraysize(bitmask_abs));
      get_caps(dev, parent, "capabilities/rel", bitmask_rel, SDL_arraysize(bitmask_rel));
      get_caps(dev, parent, "capabilities/key", bitmask_key, SDL_arraysize(bitmask_key));

      if (test_bit (EV_ABS, bitmask_ev) &&
          test_bit (ABS_X, bitmask_abs) &&
          test_bit(ABS_Y, bitmask_abs))
        {
          if (test_bit(BTN_STYLUS, bitmask_key) || test_bit(BTN_TOOL_PEN, bitmask_key))
            {
              printf ("\tSDL libudev would guess this is a tablet\n");
              guessed = true;
            }
          else if (test_bit(BTN_TOOL_FINGER, bitmask_key) && !test_bit(BTN_TOOL_PEN, bitmask_key))
            {
              printf ("\tSDL libudev would guess this is a touchpad\n");
              guessed = true;
            }
          else if (test_bit(BTN_MOUSE, bitmask_key))
            {
              printf ("\tSDL libudev would guess this is a mouse with absolute axes\n");
              guessed = true;
            }
          else if (test_bit(BTN_TOUCH, bitmask_key))
            {
              printf ("\tSDL libudev would guess this is a touchscreen or multitouch touchpad\n");
              guessed = true;
            }

          if (test_bit(BTN_TRIGGER, bitmask_key) ||
              test_bit(BTN_A, bitmask_key) ||
              test_bit(BTN_1, bitmask_key) ||
              test_bit(ABS_RX, bitmask_abs) ||
              test_bit(ABS_RY, bitmask_abs) ||
              test_bit(ABS_RZ, bitmask_abs) ||
              test_bit(ABS_THROTTLE, bitmask_abs) ||
              test_bit(ABS_RUDDER, bitmask_abs) ||
              test_bit(ABS_WHEEL, bitmask_abs) ||
              test_bit(ABS_GAS, bitmask_abs) ||
              test_bit(ABS_BRAKE, bitmask_abs))
            {
              printf ("\tSDL libudev would guess this is a joystick or game controller\n");
              guessed = true;
            }
        }

      if (test_bit(EV_REL, bitmask_ev) &&
          test_bit(REL_X, bitmask_rel) &&
          test_bit(REL_Y, bitmask_rel) &&
          test_bit(BTN_MOUSE, bitmask_key))
        {
          printf ("\tSDL libudev would guess this is a mouse with relative axes\n");
          guessed = true;
        }

      /* the first 32 bits are ESC, numbers, and Q to D; if we have any of
       * those, consider it a keyboard device; do not test KEY_RESERVED, though */
      keyboard_mask = 0xFFFFFFFE;
      if ((bitmask_key[0] & keyboard_mask) != 0)
        {
          printf ("\tSDL libudev would guess this is a keyboard\n");
          guessed = true;
        }

      if (!guessed)
        printf ("\tSDL libudev backend would not be able to guess this\n");
    }
  else
    {
      printf ("\tSDL would be unable to guess device type from caps\n");
    }

  val = udev_device_get_devnode (dev);

  if (val != NULL)
    fd = open (val, O_CLOEXEC|O_NOCTTY|O_RDONLY);
  else
    fd = -1;

  if (fd >= 0)
    {
      char device_uid[255] = { '\0' };

      if (ioctl (fd, EVIOCGID, &inpid) >= 0)
        {
          printf ("\tEVIOCGID bus type: 0x%04x\n", inpid.bustype);
          printf ("\tEVIOCGID vendor: 0x%04x\n", inpid.vendor);
          printf ("\tEVIOCGID product: 0x%04x\n", inpid.product);
          printf ("\tEVIOCGID version: 0x%04x\n", inpid.version);
        }
      else
        {
          printf ("\tUnable to ID device from EVIOCGID: %s\n",
                  strerror (errno));
          inpid.vendor = 0;
          inpid.product = 0;
        }

      if (ioctl (fd, EVIOCGUNIQ(254), device_uid) >= 0)
        {
          printf ("\tEVIOCGUNIQ serial: %s\n", device_uid);
        }
      else
        {
          printf ("\tUnable to ID device serial number from EVIOCGUNIQ: %s\n",
                  strerror (errno));
        }

      if (fallback_is_joystick (fd))
        printf ("\tSDL fallback backend would guess this is a joystick\n");
      else
        printf ("\tSDL fallback backend would guess this is not a joystick\n");

      if (wine_is_xbox_gamepad (inpid.vendor, inpid.product))
        printf ("\tWine would guess this is an Xbox gamepad based on VID/PID\n");
      else
        printf ("\tWine would guess this is not an Xbox gamepad based on VID/PID\n");
    }
  else
    {
      printf ("\tUnable to open device node, SDL would ignore this\n");
    }
}

static void
removed (struct udev_device *dev)
{
  printf ("removed: %p\n", dev);
  printf ("\tdevnode: %s\n", nonnull (udev_device_get_devnode (dev)));
}

int
main (int argc, char **argv)
{
  struct udev *udev;
  struct udev_device *dev;
  struct udev_enumerate *enumerator;
  struct udev_monitor *monitor;
  struct udev_list_entry *devs;
  struct udev_list_entry *item;
  struct pollfd poll_monitor = {};
  int opt;
  struct sigaction handler = {};

  while ((opt = getopt_long (argc, argv, "", long_options, NULL)) != -1)
    {
      switch (opt)
        {
          case OPTION_ONCE:
            signalled = SIGINT;
            break;

          case OPTION_VERSION:
            printf (
                "%s:\n"
                " Package: steam-runtime-tools\n"
                " Version: %s\n",
                argv[0], VERSION);
            return 0;

          case OPTION_HELP:
            usage (0);
            break;  /* not reached */

          case '?':
          default:
            usage (1);
            break;  /* not reached */
        }
    }

  if (optind != argc)
    usage (1);

  handler.sa_handler = signal_handler;
  sigaction (SIGINT, &handler, NULL);

  udev = udev_new ();

  if (udev == NULL)
    err (1, "udev_new");

  monitor = udev_monitor_new_from_netlink (udev, "udev");

  if (monitor == NULL)
    err (1, "udev_monitor_new_from_netlink");

  enumerator = udev_enumerate_new (udev);

  if (enumerator == NULL)
    err (1, "udev_enumerate_new");

  udev_monitor_filter_add_match_subsystem_devtype (monitor, "hidraw", NULL);
  udev_monitor_filter_add_match_subsystem_devtype (monitor, "input", NULL);
  udev_monitor_enable_receiving (monitor);

  udev_enumerate_add_match_subsystem (enumerator, "hidraw");
  udev_enumerate_add_match_subsystem (enumerator, "input");
  udev_enumerate_add_match_subsystem (enumerator, "sound");
  udev_enumerate_scan_devices (enumerator);
  devs = udev_enumerate_get_list_entry (enumerator);

  for (item = devs; item != NULL; item = udev_list_entry_get_next (item))
    {
      const char *syspath = udev_list_entry_get_name (item);

      dev = udev_device_new_from_syspath (udev, syspath);

      if (dev != NULL)
        {
          added (dev);
        }
      else
        {
          warn ("udev_device_new_from_syspath \"%s\"", syspath);
        }

      udev_device_unref (dev);
    }

  poll_monitor.fd = udev_monitor_get_fd (monitor);
  poll_monitor.events = POLLIN;

  while (signalled == 0)
    {
      while (poll (&poll_monitor, 1, -1) > 0)
        {
          const char *action;

          dev = udev_monitor_receive_device (monitor);

          if (dev == NULL)
            break;

          printf ("\n");
          action = udev_device_get_action (dev);

          if (action == NULL)
            continue;   /* shouldn't happen */
          else if (strcmp (action, "add") == 0)
            added (dev);    /* SDL sleeps 100ms first, do we need to? */
          else if (strcmp (action, "remove") == 0)
            removed (dev);

          udev_device_unref (dev);
        }
    }

  udev_enumerate_unref (enumerator);
  udev_monitor_unref (monitor);
  udev_unref (udev);
  return 0;
}
