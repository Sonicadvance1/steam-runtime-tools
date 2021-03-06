// Copyright © 2017 Collabora Ltd
// SPDX-License-Identifier: LGPL-2.1-or-later

// This file is part of libcapsule.
//
// libcapsule is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as
// published by the Free Software Foundation; either version 2.1 of the
// License, or (at your option) any later version.
//
// libcapsule is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public
// License along with libcapsule.  If not, see <http://www.gnu.org/licenses/>.

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

#include "notgl.h"

static notgl_extension_function
get_extension( const char *name )
{
    return dlsym( RTLD_DEFAULT, name );
}

int
main ( int argc,
       char **argv )
{
    notgl_extension_function f;

    // Select line-buffering for output, in case we crash
    setvbuf( stdout, NULL, _IOLBF, 0 );

    printf( "NotGL implementation: %s\n", notgl_get_implementation() );
    printf( "NotGL helper implementation: %s\n", notgl_use_helper() );

    f = get_extension( "notgl_extension_both" );

    if( f )
        printf( "notgl_extension_both: %s\n", f() );
    else
        printf( "notgl_extension_both: (not found)\n" );

    f = get_extension( "notgl_extension_red" );

    if( f )
        printf( "notgl_extension_red: %s\n", f() );
    else
        printf( "notgl_extension_red: (not found)\n" );

    f = get_extension( "notgl_extension_green" );

    if( f )
        printf( "notgl_extension_green: %s\n", f() );
    else
        printf( "notgl_extension_green: (not found)\n" );

    return 0;
}
