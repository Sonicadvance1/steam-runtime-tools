// Copyright © 2017-2020 Collabora Ltd
// SPDX-License-Identifier: LGPL-2.1-or-later

// This file is part of libcapsule.

// libcapsule is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as
// published by the Free Software Foundation; either version 2.1 of the
// License, or (at your option) any later version.

// libcapsule is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.

// You should have received a copy of the GNU Lesser General Public
// License along with libcapsule.  If not, see <http://www.gnu.org/licenses/>.

#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include <string.h>
#include <fcntl.h>

#include <libelf.h>
#include <gelf.h>
#include <dlfcn.h>

#include <capsule/capsule.h>
#include "capsule/capsule-private.h"

#include "utils/utils.h"
#include "utils/dump.h"
#include "utils/mmap-info.h"
#include "utils/process-pt-dynamic.h"
#include "utils/ld-cache.h"
#include "utils/ld-libs.h"

// ==========================================================================
// some pretty printers for debugging:

// dump out the contents of the ld cache to stderr:
static void
dump_ld_cache (ld_libs *ldlibs)
{
    ld_cache_foreach( &ldlibs->ldcache, ld_entry_dump, stderr );
}

/*
 * wrap:
 * @name:
 * @base: Starting address of the program header in memory.
 *  Addresses are normally relative to this, except for when they are
 *  absolute (see fix_addr()).
 * @dyn: An array of ElfW(Dyn) structures, somewhere after @base
 * @wrappers: Relocations to apply
 */
static void
wrap (const char *name,
      ElfW(Addr) base,
      ElfW(Dyn) *dyn,
      capsule_item *wrappers)
{
    int mmap_errno = 0;
    const char *mmap_error = NULL;
    ElfW(Addr) start = (ElfW(Addr)) dyn - base;
    // we don't know the size so we'll have to rely on the linker putting
    // well formed entries into the mmap()ed DSO region.
    // (tbf if the linker is putting duff entries here we're boned anyway)
    //
    // dyn is the address of the dynamic section
    // base is the start of the program header in memory
    // start should be the offset from the program header to its dyn section
    //
    // the utility functions expect an upper bound though so set that to
    // something suitably large:
    relocation_data rdata = { 0 };

    DEBUG( DEBUG_WRAPPERS,
           "\"%s\": base address %" PRIxPTR ", dynamic section at %p",
           name, base, dyn );

    rdata.debug     = debug_flags;
    rdata.error     = NULL;
    rdata.relocs    = wrappers;

    // if RELRO linking has happened we'll need to tweak the mprotect flags
    // before monkeypatching the symbol tables, for which we will need the
    // sizes, locations and current protections of any mmap()ed regions:
    rdata.mmap_info = load_mmap_info( &mmap_errno, &mmap_error );

    if( mmap_errno || mmap_error )
    {
        DEBUG( DEBUG_MPROTECT,
               "mmap/mprotect flags information load error (errno: %d): %s",
               mmap_errno, mmap_error );
        DEBUG( DEBUG_MPROTECT,
               "relocation will be unable to handle RELRO linked libraries" );
    }

    // make all the mmap()s writable:
    for( int i = 0; rdata.mmap_info[i].start != MAP_FAILED; i++ )
        if( mmap_entry_should_be_writable( &rdata.mmap_info[i] ) )
            add_mmap_protection( &rdata.mmap_info[i], PROT_WRITE );

    // if we're debugging wrapper installation in detail we
    // will end up in a path that's normally only DEBUG_ELF
    // debugged:
    if( debug_flags & DEBUG_WRAPPERS )
        debug_flags = debug_flags | DEBUG_RELOCS;

    // install any required wrappers inside the capsule:
    process_pt_dynamic( start,  // offset from phdr to dyn section
                        0,      //  fake size value
                        (void *) base,   //  address of phdr in memory
                        process_dt_rela,
                        process_dt_rel,
                        &rdata );

    // put the debug flags back in case we changed them
    debug_flags = rdata.debug;

    // put the mmap()/mprotect() permissions back the way they were:
    for( int i = 0; rdata.mmap_info[i].start != MAP_FAILED; i++ )
        if( mmap_entry_should_be_writable( &rdata.mmap_info[i] ) )
            reset_mmap_protection( &rdata.mmap_info[i] );

    free_mmap_info( rdata.mmap_info );
    rdata.mmap_info = NULL;
}

static inline int
excluded_from_wrap (const char *name, char **exclude)
{
    const char *dso = strrchr(name, '/');

    // we can't ever subvert the runtime linker itself:
    if( strncmp( "/ld-", dso, 4 ) == 0 )
        return 1;

    for( char **x = exclude; x && *x; x++ )
        if( strcmp ( *x, dso + 1 ) == 0 )
            return 1;

    return 0;
}

/*
 * @exclude: (array zero-terminated=1): same as for capsule_dlmopen()
 * @errcode: (out): errno
 * @error: (out) (transfer full) (optional): Error string
 */
// replace calls out to dlopen in the encapsulated DSO with a wrapper
// which should take care of preserving the /path-prefix and namespace
// wrapping of the original capsule_dlmopen() call.
//
// strictly speaking we can wrap things other than dlopen(),
// but that's currently all we use this for:
static int install_wrappers ( void *dl_handle,
                              capsule_item *wrappers,
                              const char **exclude,
                              int *errcode,
                              char **error)
{
    int replacements = 0;
    struct link_map *map;

    if( dlinfo( dl_handle, RTLD_DI_LINKMAP, &map ) != 0 )
    {
        const char *local_error = dlerror();

        if( error )
            *error = xstrdup( local_error );

        if( errcode )
            *errcode = EINVAL;

        DEBUG( DEBUG_WRAPPERS, "mangling capsule symbols: %s", local_error );

        return -1;
    }

    DEBUG( DEBUG_WRAPPERS, "link_map: %p <- %p -> %p",
               map ? map->l_next : NULL ,
               map ? map         : NULL ,
               map ? map->l_prev : NULL );

    // no guarantee that we're at either end of the link map:
    while( map->l_prev )
        map = map->l_prev;

    unsigned long df = debug_flags;

    if( debug_flags & DEBUG_WRAPPERS )
        debug_flags |= DEBUG_RELOCS;

    if (map->l_next)
    {
        for( struct link_map *m = map; m; m = m->l_next )
        {
            if( excluded_from_wrap(m->l_name, (char **)exclude) )
            {
                DEBUG( DEBUG_WRAPPERS, "%s excluded from wrapping",
                       m->l_name );
            }
            else
            {
                wrap( m->l_name, m->l_addr, m->l_ld, wrappers );
            }
        }
    }

    debug_flags = df;

    return replacements;
}

// dump the link map info for the given dl handle (NULL = default)
static void
dump_link_map( void *dl_handle )
{
    struct link_map *map;
    void *handle;

    if( !dl_handle )
        handle = dlopen( NULL, RTLD_LAZY|RTLD_NOLOAD );
    else
        handle = dl_handle;

    if( dlinfo( handle, RTLD_DI_LINKMAP, &map ) != 0 )
    {
        DEBUG( DEBUG_CAPSULE, "failed to access link_map for handle %p-%p: %s",
               dl_handle, handle, dlerror() );
        return;
    }

    // be kind, rewind the link map:
    while( map->l_prev )
        map = map->l_prev;

    fprintf( stderr, "(dl-handle %s", dl_handle ? "CAPSULE" : "DEFAULT" );
    for( struct link_map *m = map; m; m = m->l_next )
        fprintf( stderr, "\n  [prev: %p] %p: \"%s\" [next: %p]",
                 m->l_prev, m, m->l_name, m->l_next );
    fprintf( stderr, ")\n" );
}

// ==========================================================================
void *
_capsule_load (const capsule cap,
               capsule_item *wrappers,
               int *errcode,
               char **error)
{
    void *ret = NULL;
    ld_libs ldlibs = {};

    if( !ld_libs_init( &ldlibs,
                       (const char **) cap->ns->combined_exclude,
                       cap->ns->prefix, debug_flags, errcode, error ) )
        return NULL;

    // ==================================================================
    // read in the ldo.so.cache - this will contain all architectures
    // currently installed (x86_64, i386, x32) in no particular order
    if( ld_libs_load_cache( &ldlibs, errcode, error ) )
    {
        if( debug_flags & DEBUG_LDCACHE )
            dump_ld_cache( &ldlibs );
    }
    else
    {
        return NULL;
    }

    // ==================================================================
    // find the starting point of our capsule
    if( !ld_libs_set_target( &ldlibs, cap->meta->soname, errcode, error ) )
        goto cleanup;

    // ==================================================================
    // once we have the starting point recursively find all its DT_NEEDED
    // entries, except for the linker itself and libc, which must not
    // be different between the capsule and the "real" DSO environment:
    if( !ld_libs_find_dependencies( &ldlibs, errcode, error ) )
        goto cleanup;

    // ==================================================================
    // load the stack of DSOs we need:
    ret = ld_libs_load( &ldlibs, &cap->ns->ns, 0, errcode, error );

    if( debug_flags & DEBUG_CAPSULE )
    {
        dump_link_map( ret  );
        dump_link_map( NULL );
    }

    if( !ret )
        goto cleanup;

    // =====================================================================
    // stash any free/malloc/etc implementations _before_ we overwrite them:
    // (only really need to deal with functions that deal with pre-alloc'd
    //  pointers, ie free and realloc)
    if( cap->ns->mem->free == NULL )
        cap->ns->mem->free = dlsym( ret, "free" );

    if( cap->ns->mem->realloc == NULL )
        cap->ns->mem->realloc = dlsym( ret, "realloc" );

    // =====================================================================
    // TODO: failure in the dlopen fixup phase should probably be fatal:
    if( ret      != NULL && // no errors so far
        wrappers != NULL )  // have a dlopen fixup function
        install_wrappers( ret, wrappers,
                          (const char **)cap->ns->combined_exclude,
                          errcode, error );

    cap->dl_handle = ret;

cleanup:
    ld_libs_finish( &ldlibs );
    return ret;
}

