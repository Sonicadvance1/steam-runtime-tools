// Copyright © 2017 Collabora Ltd
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

#pragma once

#include <sys/param.h>
#include <sys/mman.h>

typedef struct
{
    char path[PATH_MAX];
    char *start;
    char *end;
    unsigned int protect;
    int invalid;
} mmapinfo;

mmapinfo *load_mmap_info (int *err, const char **errstr);
mmapinfo *find_mmap_info (mmapinfo *maps, void *addr);
void        free_mmap_info (mmapinfo *ptr);

int add_mmap_protection   (mmapinfo *mmap_info, unsigned int flags);
int reset_mmap_protection (mmapinfo *mmap_info);

int mmap_entry_should_be_writable (mmapinfo *mmap_info);

