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

/*
 * This helper takes a SONAME as an argument, and optionally a filename for
 * symbols, and outputs a parsable JSON with the path, the dependencies and
 * the possible missing symbols of the requested library.
 *
 * For a usage example see `srt_check_library_presence` in
 * `steam-runtime-tools/library.c`.
 */

#include <argz.h>
#include <dlfcn.h>
#include <errno.h>
#include <getopt.h>
#include <link.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>

#define BASE "Base"

static void print_json_string_content (const char *s);
static bool has_symbol (void *handle, const char *symbol);
static bool has_versioned_symbol (void *handle,
                                  const char *symbol,
                                  const char * version);

static void oom (void) __attribute__((__noreturn__));
static void
oom (void)
{
  fprintf (stderr, "Out of memory");
  exit (EX_OSERR);
}

#define asprintf_or_die(...) \
do { \
    if (asprintf (__VA_ARGS__) < 0) \
      oom (); \
} while (0)

#define argz_add_or_die(...) \
do { \
    if (argz_add (__VA_ARGS__) != 0) \
      oom (); \
} while (0)

static inline void *
steal_pointer (void *pp)
{
    typedef void *__attribute__((may_alias)) voidp_alias;
    voidp_alias *pointer_to_pointer = pp;
    void *ret = *pointer_to_pointer;
    *pointer_to_pointer = NULL;
    return ret;
}

static void
clear_with_free (void *pp)
{
  free (steal_pointer (pp));
}

static void
clear_with_dlclose (void *pp)
{
  void *handle = steal_pointer (pp);

  if (handle != NULL)
    dlclose (handle);
}

static void
clear_with_fclose (void *pp)
{
  FILE *fh = steal_pointer (pp);

  if (fh != NULL)
    fclose (fh);
}

#define autodlclose __attribute__((__cleanup__(clear_with_dlclose)))
#define autofclose __attribute__((__cleanup__(clear_with_fclose)))
#define autofree __attribute__((__cleanup__(clear_with_free)))

enum
{
  OPTION_HELP = 1,
  OPTION_DEB_SYMBOLS,
  OPTION_HIDDEN_DEPENDENCY,
  OPTION_LINE_BASED,
  OPTION_VERSION,
};

struct option long_options[] =
{
    { "hidden-dependency", required_argument, NULL, OPTION_HIDDEN_DEPENDENCY },
    { "deb-symbols", no_argument, NULL, OPTION_DEB_SYMBOLS },
    { "help", no_argument, NULL, OPTION_HELP },
    { "line-based", no_argument, NULL, OPTION_LINE_BASED },
    { "version", no_argument, NULL, OPTION_VERSION },
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

  fprintf (fp, "Usage: %s [OPTIONS] SONAME [SYMBOLS_FILENAME]\n",
           program_invocation_short_name);
  exit (code);
}

static const char *
find_dyn_entry (const ElfW(Dyn) *entries,
                const ElfW(Addr) base,
                const ElfW(Sxword) tag)
{
  const ElfW(Dyn) *entry;
  ElfW(Addr) stab = 0;

  for (entry = entries; entry->d_tag != DT_NULL; entry++)
    {
      if (entry->d_tag == tag)
        stab = entry->d_un.d_ptr;
    }

  if (stab == 0)
    return NULL;
  else if (stab < base)
    return (const char *) base + stab;
  else
    return (const char *) stab;
}

static size_t
find_tag_value (const ElfW(Dyn) *dyn,
                const ElfW(Sxword) tag)
{
  for (; dyn->d_tag != DT_NULL; dyn++)
    {
      if (dyn->d_tag == tag)
        return dyn->d_un.d_val;
    }

  return (size_t) -1;
}

/*
 * Print a bytestring to stdout, escaping backslashes and control
 * characters in octal. The result can be parsed with g_strcompress().
 */
static void
print_strescape (const char *bytestring)
{
  const unsigned char *p;

  for (p = (const unsigned char *) bytestring; *p != '\0'; p++)
    {
      if (*p < ' ' || *p >= 0x7f || *p == '\\')
        printf ("\\%03o", *p);
      else
        putc (*p, stdout);
    }
}

int
main (int argc,
      char **argv)
{
  const char *entry;
  const char *soname;
  const char *version;
  const char *symbol;
  autodlclose void *handle = NULL;
  struct link_map *dep_map = NULL;
  struct link_map *the_library = NULL;
  autofclose FILE *fp = NULL;
  autofree char *missing_symbols = NULL;
  autofree char *misversioned_symbols = NULL;
  size_t missing_n = 0;
  size_t misversioned_n = 0;
  autofree char *line = NULL;
  size_t len = 0;
  ssize_t chars;
  bool first;
  bool deb_symbols = false;
  int opt;
  autofree char *hidden_deps = NULL;
  size_t hidden_deps_len = 0;
  const char *hidden_dep;
  bool line_based = false;

  while ((opt = getopt_long (argc, argv, "", long_options, NULL)) != -1)
    {
      switch (opt)
        {
          case OPTION_HIDDEN_DEPENDENCY:
            argz_add_or_die (&hidden_deps, &hidden_deps_len, optarg);
            break;

          case OPTION_DEB_SYMBOLS:
            deb_symbols = true;
            break;

          case OPTION_HELP:
            usage (0);
            break;

          case OPTION_LINE_BASED:
            line_based = true;
            break;

          case OPTION_VERSION:
            /* Output version number as YAML for machine-readability,
             * inspired by `ostree --version` and `docker version` */
            printf (
                "%s:\n"
                " Package: steam-runtime-tools\n"
                " Version: %s\n",
                argv[0], VERSION);
            return 0;

          case '?':
          default:
            usage (1);
            break;  /* not reached */
        }
    }

  if (argc < optind + 1 || argc > optind + 2)
    {
      usage (1);
    }

  soname = argv[optind];

  if (line_based)
    {
      fputs ("requested=", stdout);
      print_strescape (soname);
      putc ('\n', stdout);
    }
  else
    {
      printf ("{\n");
      printf ("  \"");
      print_json_string_content (soname);
      printf ("\": {");
    }

  for (hidden_dep = argz_next (hidden_deps, hidden_deps_len, NULL);
       hidden_dep != NULL;
       hidden_dep = argz_next (hidden_deps, hidden_deps_len, hidden_dep))
    {
      /* We don't call "dlclose" on global hidden dependencies, otherwise ubsan
       * will report an indirect memory leak */
      if (dlopen (hidden_dep, RTLD_NOW|RTLD_GLOBAL) == NULL)
        {
          fprintf (stderr, "Unable to find the dependency library: %s\n", dlerror ());
          return 1;
        }
    }

  handle = dlopen (soname, RTLD_NOW);
  if (handle == NULL)
    {
      fprintf (stderr, "Unable to find the library: %s\n", dlerror ());
      return 1;
    }
  /* Using RTLD_DI_LINKMAP insted of RTLD_DI_ORIGIN we don't need to worry
   * about allocating a big enough array for the path. */
  if (dlinfo (handle, RTLD_DI_LINKMAP, &the_library) != 0 || the_library == NULL)
    {
      fprintf (stderr, "Unable to obtain the path: %s\n", dlerror ());
      return 1;
    }

  const ElfW(Dyn) * const dyn_start = the_library->l_ld;
  const ElfW(Addr) load_addr = the_library->l_addr;
  const char *strtab = find_dyn_entry (dyn_start, load_addr, DT_STRTAB);
  size_t soname_val = find_tag_value (dyn_start, DT_SONAME);
  if (strtab != NULL && soname_val != (size_t) -1)
    {
      if (line_based)
        {
          fputs ("soname=", stdout);
          print_strescape (&strtab[soname_val]);
          putc ('\n', stdout);
        }
      else
        {
          printf ("\n    \"SONAME\": \"");
          print_json_string_content (&strtab[soname_val]);
          printf("\",");
        }
    }
  else
    {
      fprintf (stderr,
               "Warning: we were not able to get the SONAME of \"%s\"\n",
               soname);
    }

  if (line_based)
    {
      fputs ("path=", stdout);
      print_strescape (the_library->l_name);
      putc ('\n', stdout);
    }
  else
    {
      printf ("\n    \"path\": \"");
      print_json_string_content (the_library->l_name);
      printf ("\"");
    }

  if (argc >= optind + 2)
    {
      size_t soname_len = strlen (soname);
      bool found_our_soname = false;
      bool in_our_soname = false;

      if (strcmp(argv[optind + 1], "-") == 0)
        fp = stdin;
      else
        fp = fopen(argv[optind + 1], "r");

      if (fp == NULL)
        {
          int saved_errno = errno;

          fprintf (stderr, "Error reading \"%s\": %s\n",
                   argv[optind + 1], strerror (saved_errno));
          return 1;
        }

      while ((chars = getline(&line, &len, fp)) != -1)
        {
          if (line[chars - 1] == '\n')
            line[chars - 1] = '\0';

          /* Skip any empty line */
          if (chars > 1)
            {
              char *pointer_into_line;

              if (deb_symbols)
                {
                  if (line[0] == '#' || line[0] == '*' || line[0] == '|')
                    {
                      /* comment or metadata lines, ignore:
                       * "# comment"
                       * "* Field: Value"
                       * "| alternative-dependency" */
                      continue;
                    }
                  else if (line[0] == ' ')
                    {
                      /* this line represents a symbol:
                       * " symbol@Base 1.2-3~" */
                      if (!in_our_soname)
                        {
                          /* this is a symbol from a different library,
                           * ignore */
                          continue;
                        }
                    }
                  else
                    {
                      /* This line introduces a new SONAME, which might
                       * be the one we are interested in:
                       * "libz.so.1 zlib1g #MINVER#" */
                      if (strncmp (soname, line, soname_len) == 0
                          && (line[soname_len] == ' ' || line[soname_len] == '\t'))
                        {
                          found_our_soname = true;
                          in_our_soname = true;
                        }
                      else
                        {
                          in_our_soname = false;
                        }

                      /* This is not a symbol */
                      continue;
                    }

                  pointer_into_line = &line[1];
                }
              else
                {
                  pointer_into_line = line;
                }

              symbol = strsep (&pointer_into_line, "@");
              version = strsep (&pointer_into_line, deb_symbols ? "@ \t" : "@");
              if (symbol == NULL)
                {
                  fprintf (stderr, "Probably the symbol@version pair is mispelled.");
                  return 1;
                }

              if (version == NULL || strcmp (version, BASE) == 0)
                {
                  if (!has_symbol (handle, symbol))
                    argz_add_or_die (&missing_symbols, &missing_n, symbol);
                }
              else
                {
                  if (strcmp (symbol, version) == 0)
                    {
                      /* Ignore: dlsym() and dlvsym() don't find the
                       * special symbol representing the version itself,
                       * because it is neither data nor code. */
                    }
                  else if (!has_versioned_symbol (handle, symbol, version))
                    {
                      autofree char * merged_string = NULL;

                      asprintf_or_die (&merged_string, "%s@%s", symbol, version);
                      if (has_symbol (handle, symbol))
                          argz_add_or_die (&misversioned_symbols, &misversioned_n, merged_string);
                      else
                          argz_add_or_die (&missing_symbols, &missing_n, merged_string);
                    }
                }
            }
        }

      if (deb_symbols && !found_our_soname)
        {
          fprintf (stderr, "Warning: \"%s\" does not describe ABI of \"%s\"\n",
                   argv[optind + 1], soname);
        }

      first = true;
      entry = 0;

      if (!line_based)
        printf (",\n    \"missing_symbols\": [");

      while ((entry = argz_next (missing_symbols, missing_n, entry)))
        {
          if (first)
            first = false;
          else if (!line_based)
            printf (",");

          if (line_based)
            {
              fputs ("missing_symbol=", stdout);
              print_strescape (entry);
              putc ('\n', stdout);
            }
          else
            {
              printf ("\n      \"");
              print_json_string_content (entry);
              printf ("\"");
            }
        }

      if (!line_based)
        printf ("\n    ]");

      first = true;
      entry = 0;

      if (!line_based)
        printf (",\n    \"misversioned_symbols\": [");

      while ((entry = argz_next (misversioned_symbols, misversioned_n, entry)))
        {
          if (first)
            first = false;
          else if (!line_based)
            printf (",");

          if (line_based)
            {
              fputs ("misversioned_symbol=", stdout);
              print_strescape (entry);
              putc ('\n', stdout);
            }
          else
            {
              printf ("\n      \"");
              print_json_string_content (entry);
              printf ("\"");
            }
        }

      if (!line_based)
        printf ("\n    ]");
    }
  dep_map = the_library;

  /* Some loaded libraries may be before our handle.
   * To list them all we move the pointer at the beginning. */
  while (dep_map != NULL && dep_map->l_prev != NULL)
    dep_map = dep_map->l_prev;

  if (!line_based)
    printf (",\n    \"dependencies\": [");

  first = true;
  for (; dep_map != NULL; dep_map = dep_map->l_next)
    {
      if (dep_map == the_library || strcmp (dep_map->l_name, "") == 0)
        continue;

      if (first)
        {
          if (!line_based)
            printf ("\n      \"");

          first = false;
        }
      else if (!line_based)
        {
          printf (",\n      \"");
        }

      if (line_based)
        {
          fputs ("dependency=", stdout);
          print_strescape (dep_map->l_name);
          putc ('\n', stdout);
        }
      else
        {
          print_json_string_content (dep_map->l_name);
          printf ("\"");
        }
    }

  if (!line_based)
    {
      printf ("\n    ]\n  }");
      printf ("\n}\n");
    }

  return 0;
}

static void
print_json_string_content (const char *s)
{
  const unsigned char *p;

  for (p = (const unsigned char *) s; *p != '\0'; p++)
    {
      if (*p == '"' || *p == '\\' || *p <= 0x1F || *p >= 0x80)
        printf ("\\u%04x", *p);
      else
        printf ("%c", *p);
    }
}

static bool
has_symbol (void *handle,
            const char *symbol)
{
  (void) dlerror ();  /* clear old error indicator */
  (void) dlsym (handle, symbol);
  return (dlerror () == NULL);
}

static bool
has_versioned_symbol (void *handle,
                      const char *symbol,
                      const char *version)
{
  (void) dlerror ();  /* clear old error indicator */
  (void) dlvsym (handle, symbol, version);
  return (dlerror () == NULL);
}
