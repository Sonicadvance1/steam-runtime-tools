/*
 * Copyright © 2020 Collabora Ltd.
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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#ifndef VERSION
#define VERSION "(unknown)"
#endif

#include <errno.h>
#include <getopt.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-default"
#include <SDL.h>
#pragma GCC diagnostic pop

static const char *
nonnull (const char *s)
{
  if (s != NULL)
    return s;

  return "(null)";
}

static const char *
stringify_joystick_type (SDL_JoystickType t)
{
  switch (t)
    {
      case SDL_JOYSTICK_TYPE_UNKNOWN:
        return "(unknown)";

      case SDL_JOYSTICK_TYPE_GAMECONTROLLER:
        return "game controller";

      case SDL_JOYSTICK_TYPE_WHEEL:
        return "wheel";

      case SDL_JOYSTICK_TYPE_ARCADE_STICK:
        return "arcade stick";

      case SDL_JOYSTICK_TYPE_FLIGHT_STICK:
        return "flight stick";

      case SDL_JOYSTICK_TYPE_DANCE_PAD:
        return "dance pad";

      case SDL_JOYSTICK_TYPE_GUITAR:
        return "guitar";

      case SDL_JOYSTICK_TYPE_DRUM_KIT:
        return "drum kit";

      case SDL_JOYSTICK_TYPE_ARCADE_PAD:
        return "arcade pad";

      case SDL_JOYSTICK_TYPE_THROTTLE:
        return "throttle";

      default:
        return "(known to SDL but not us)";
    }
}

#if SDL_VERSION_ATLEAST(2, 10, 12)
static const char *
stringify_controller_type (SDL_GameControllerType t)
{
  switch (t)
    {
      case SDL_CONTROLLER_TYPE_UNKNOWN:
        return "(unknown)";

      case SDL_CONTROLLER_TYPE_XBOX360:
        return "Xbox 360";

      case SDL_CONTROLLER_TYPE_XBOXONE:
        return "Xbox One";

      case SDL_CONTROLLER_TYPE_PS3:
        return "Playstation 3";

      case SDL_CONTROLLER_TYPE_PS4:
        return "Playstation 4";

      case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:
        return "Nintendo Switch Pro";

      default:
        return "(known to SDL but not us)";
    }
}
#endif

static void
show_joysticks (int highlight)
{
  int i;

  SDL_LockJoysticks ();

  for (i = 0; i < SDL_NumJoysticks (); i++)
    {
      SDL_JoystickGUID guid;
      const char *marker;
      size_t j;

      if (highlight == i)
        marker = " <----";
      else
        marker = "";

      printf ("Index %d:%s\n", i, marker);
      printf ("\tname: %s\n", SDL_JoystickNameForIndex (i));
      printf ("\tplayer: %d\n", SDL_JoystickGetDevicePlayerIndex (i));
      printf ("\tGUID: ");

      guid = SDL_JoystickGetDeviceGUID (i);

      for (j = 0; j < sizeof (guid); j++)
        {
          if (j != 0 && (j % 2) == 0)
            printf (" ");

          if (j != 0 && (j % 4) == 0)
            printf (" ");

          printf ("%02x", guid.data[j]);
        }

      printf ("\n");
      printf ("\tvendor: 0x%04x\n", SDL_JoystickGetDeviceVendor (i));
      printf ("\tproduct: 0x%04x\n", SDL_JoystickGetDeviceProduct (i));
      printf ("\tproduct version: 0x%04x\n", SDL_JoystickGetDeviceProductVersion (i));
      printf ("\tdevice type: %s (%d)\n",
              stringify_joystick_type (SDL_JoystickGetDeviceType (i)),
              SDL_JoystickGetDeviceType (i));
      printf ("\tinstance ID: %d\n", SDL_JoystickGetDeviceInstanceID (i));

      if (SDL_IsGameController (i))
        {
          printf ("\tis a game controller:\n");

          printf ("\t\tname: %s\n", nonnull (SDL_GameControllerNameForIndex (i)));
#if SDL_VERSION_ATLEAST(2, 10, 12)
          printf ("\t\ttype: %s (%d)\n",
                  stringify_controller_type (SDL_GameControllerTypeForIndex (i)),
                  SDL_GameControllerTypeForIndex (i));
#endif
        }
      else
        {
          printf ("\tnot a game controller\n");
        }
    }

  SDL_UnlockJoysticks ();
}

enum
{
  OPTION_HELP = 1,
  OPTION_ONCE,
  OPTION_OPEN_CONTROLLERS,
  OPTION_OPEN_JOYSTICKS,
  OPTION_VERSION,
};

struct option long_options[] =
{
    { "once", no_argument, NULL, OPTION_ONCE },
    { "open-controllers", no_argument, NULL, OPTION_OPEN_CONTROLLERS },
    { "open-joysticks", no_argument, NULL, OPTION_OPEN_JOYSTICKS },
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

typedef struct _Item Item;

struct _Item {
  Item *next;
  void *data;
};

int
main (int argc, char **argv)
{
  SDL_bool open_controllers = SDL_FALSE;
  SDL_bool open_joysticks = SDL_FALSE;
  SDL_bool running = SDL_TRUE;
  SDL_Event event;
  Item *joysticks = NULL;
  Item *controllers = NULL;
  int opt;

  while ((opt = getopt_long (argc, argv, "", long_options, NULL)) != -1)
    {
      switch (opt)
        {
          case OPTION_ONCE:
            running = SDL_FALSE;
            break;

          case OPTION_OPEN_CONTROLLERS:
            open_controllers = SDL_TRUE;
            break;

          case OPTION_OPEN_JOYSTICKS:
            open_joysticks = SDL_TRUE;
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

  if (SDL_Init (SDL_INIT_JOYSTICK|SDL_INIT_GAMECONTROLLER) < 0)
    return 1;

  show_joysticks (-1);

  while (running && SDL_WaitEvent (&event))
    {
      SDL_Joystick *js;
      SDL_GameController *gc;
      Item *item, *next;

      switch (event.type)
        {
          case SDL_JOYDEVICEADDED:
            printf ("\n[%7u.%03u] Joystick device at index %d added\n",
                    event.jdevice.timestamp / 1000,
                    event.jdevice.timestamp % 1000,
                    event.jdevice.which);
            show_joysticks (event.jdevice.which);

            if (open_joysticks)
              {
                js = SDL_JoystickOpen (event.jdevice.which);

                if (js != NULL)
                  {
                    printf ("Joystick object %p\n", js);
                    item = calloc (1, sizeof (Item));
                    item->next = joysticks;
                    item->data = js;
                    joysticks = item;
                  }
                else
                  {
                    printf ("\nFailed to open joystick at index %d\n",
                            event.jdevice.which);
                  }
              }

            break;

          case SDL_JOYDEVICEREMOVED:
            printf ("\n[%7u.%03u] Joystick device with instance ID %d removed\n",
                    event.jdevice.timestamp / 1000,
                    event.jdevice.timestamp % 1000,
                    event.jdevice.which);
            show_joysticks (-1);

            item = joysticks;

            while (item != NULL)
              {
                js = item->data;
                next = item->next;

                if (SDL_JoystickInstanceID (js) == event.jdevice.which)
                  {
                    printf ("Removing joystick object %p\n", js);

                    if (item == joysticks)
                      joysticks = next;

                    SDL_JoystickClose (js);
                    free (item);
                    item = next;
                  }

                item = next;
              }

            break;

          case SDL_CONTROLLERDEVICEADDED:
            printf ("\n[%7u.%03u] Controller device at index %d added\n",
                    event.jdevice.timestamp / 1000,
                    event.jdevice.timestamp % 1000,
                    event.jdevice.which);
            show_joysticks (event.jdevice.which);

            if (open_controllers)
              {
                gc = SDL_GameControllerOpen (event.jdevice.which);

                if (gc != NULL)
                  {
                    printf ("Controller object %p\n", gc);
                    item = calloc (1, sizeof (Item));
                    item->next = controllers;
                    item->data = gc;
                    controllers = item;
                  }
                else
                  {
                    printf ("\nFailed to open game controller at index %d\n",
                            event.jdevice.which);
                  }
              }

            break;

          case SDL_CONTROLLERDEVICEREMOVED:
            printf ("\n[%7u.%03u] Controller device with instance ID %d removed\n",
                    event.jdevice.timestamp / 1000,
                    event.jdevice.timestamp % 1000,
                    event.jdevice.which);
            show_joysticks (-1);

            item = controllers;

            while (item != NULL)
              {
                gc = item->data;
                next = item->next;

                if (SDL_JoystickInstanceID (SDL_GameControllerGetJoystick (gc)) == event.jdevice.which)
                  {
                    printf ("Removing game controller object %p\n", gc);

                    if (item == controllers)
                      controllers = next;

                    SDL_GameControllerClose (gc);
                    free (item);
                    item = next;
                  }

                item = next;
              }

            break;

          case SDL_CONTROLLERDEVICEREMAPPED:
            printf ("\n[%7u.%03u] Controller device with instance ID %d remapped\n",
                    event.jdevice.timestamp / 1000,
                    event.jdevice.timestamp % 1000,
                    event.jdevice.which);
            break;

          case SDL_QUIT:
            running = SDL_FALSE;
            break;

          default:
            break;
        }
    }

  return 0;
}
