/*
 * A cut-down version of common/flatpak-utils from Flatpak
 *
 * Copyright © 2014 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
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
 */

#include "config.h"

#include "flatpak-utils-private.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/ioctl.h>
#include <termios.h>

#define RUNNING_ON_VALGRIND 0

/* Sometimes this is /var/run which is a symlink, causing weird issues when we pass
 * it as a path into the sandbox */
char *
flatpak_get_real_xdg_runtime_dir (void)
{
  return realpath (g_get_user_runtime_dir (), NULL);
}

char *
flatpak_get_timezone (void)
{
  g_autofree gchar *symlink = NULL;
  gchar *etc_timezone = NULL;
  const gchar *tzdir;

  tzdir = getenv ("TZDIR");
  if (tzdir == NULL)
    tzdir = "/usr/share/zoneinfo";

  symlink = flatpak_resolve_link ("/etc/localtime", NULL);
  if (symlink != NULL)
    {
      /* Resolve relative path */
      g_autofree gchar *canonical = flatpak_canonicalize_filename (symlink);
      char *canonical_suffix;

      /* Strip the prefix and slashes if possible. */
      if (g_str_has_prefix (canonical, tzdir))
        {
          canonical_suffix = canonical + strlen (tzdir);
          while (*canonical_suffix == '/')
            canonical_suffix++;

          return g_strdup (canonical_suffix);
        }
    }

  if (g_file_get_contents ("/etc/timezeone", &etc_timezone,
                           NULL, NULL))
    {
      g_strchomp (etc_timezone);
      return etc_timezone;
    }

  /* Final fall-back is UTC */
  return g_strdup ("UTC");
}

static gboolean
needs_quoting (const char *arg)
{
  while (*arg != 0)
    {
      char c = *arg;
      if (!g_ascii_isalnum (c) &&
          !(c == '-' || c == '/' || c == '~' ||
            c == ':' || c == '.' || c == '_' ||
            c == '=' || c == '@'))
        return TRUE;
      arg++;
    }
  return FALSE;
}

char *
flatpak_quote_argv (const char *argv[],
                    gssize      len)
{
  GString *res = g_string_new ("");
  int i;

  if (len == -1)
    len = g_strv_length ((char **) argv);

  for (i = 0; i < len; i++)
    {
      if (i != 0)
        g_string_append_c (res, ' ');

      if (needs_quoting (argv[i]))
        {
          g_autofree char *quoted = g_shell_quote (argv[i]);
          g_string_append (res, quoted);
        }
      else
        g_string_append (res, argv[i]);
    }

  return g_string_free (res, FALSE);
}

char *
flatpak_readlink (const char *path,
                  GError    **error)
{
  return glnx_readlinkat_malloc (-1, path, NULL, error);
}

char *
flatpak_resolve_link (const char *path,
                      GError    **error)
{
  g_autofree char *link = flatpak_readlink (path, error);
  g_autofree char *dirname = NULL;

  if (link == NULL)
    return NULL;

  if (g_path_is_absolute (link))
    return g_steal_pointer (&link);

  dirname = g_path_get_dirname (path);
  return g_build_filename (dirname, link, NULL);
}

char *
flatpak_canonicalize_filename (const char *path)
{
  g_autoptr(GFile) file = g_file_new_for_path (path);
  return g_file_get_path (file);
}

/* If memfd_create() is available, generate a sealed memfd with contents of
 * @str. Otherwise use an O_TMPFILE @tmpf in anonymous mode, write @str to
 * @tmpf, and lseek() back to the start. See also similar uses in e.g.
 * rpm-ostree for running dracut.
 */
gboolean
flatpak_buffer_to_sealed_memfd_or_tmpfile (GLnxTmpfile *tmpf,
                                           const char  *name,
                                           const char  *str,
                                           size_t       len,
                                           GError     **error)
{
  if (len == -1)
    len = strlen (str);
  glnx_autofd int memfd = memfd_create (name, MFD_CLOEXEC | MFD_ALLOW_SEALING);
  int fd; /* Unowned */
  if (memfd != -1)
    {
      fd = memfd;
    }
  else
    {
      /* We use an anonymous fd (i.e. O_EXCL) since we don't want
       * the target container to potentially be able to re-link it.
       */
      if (!G_IN_SET (errno, ENOSYS, EOPNOTSUPP))
        return glnx_throw_errno_prefix (error, "memfd_create");
      if (!glnx_open_anonymous_tmpfile (O_RDWR | O_CLOEXEC, tmpf, error))
        return FALSE;
      fd = tmpf->fd;
    }
  if (ftruncate (fd, len) < 0)
    return glnx_throw_errno_prefix (error, "ftruncate");
  if (glnx_loop_write (fd, str, len) < 0)
    return glnx_throw_errno_prefix (error, "write");
  if (lseek (fd, 0, SEEK_SET) < 0)
    return glnx_throw_errno_prefix (error, "lseek");
  if (memfd != -1)
    {
      /* Valgrind doesn't currently handle G_ADD_SEALS, so lets not seal when debugging... */
      if ((!RUNNING_ON_VALGRIND) &&
          fcntl (memfd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL) < 0)
        return glnx_throw_errno_prefix (error, "fcntl(F_ADD_SEALS)");
      /* The other values can stay default */
      tmpf->fd = glnx_steal_fd (&memfd);
      tmpf->initialized = TRUE;
    }
  return TRUE;
}
