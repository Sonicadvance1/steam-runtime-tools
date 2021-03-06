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

#include <dlfcn.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <link.h>
#include "utils/utils.h"
#include "mmap-info.h"

// these macros are secretly the same for elf32 & elf64:
#define ELFW_ST_TYPE(a)       ELF32_ST_TYPE(a)
#define ELFW_ST_BIND(a)       ELF32_ST_BIND(a)
#define ELFW_ST_VISIBILITY(a) ELF32_ST_VISIBILITY(a)

typedef enum
{
    RELOCATION_FLAGS_NONE = 0,
    RELOCATION_FLAGS_AVOID_LIBC = (1 << 0),
} relocation_flags;

typedef struct
{
    capsule_item *relocs;
    struct { int success; int failure; } count;
    int debug;
    char *error;
    mmapinfo *mmap_info;
    relocation_flags flags;
    ptr_list *seen;
} relocation_data;

/*
 * relocate_rela_cb:
 * @start: array of relocation entries
 * @relasz: number of bytes (not number of structs!) following @start
 * @strtab: string table, a series of 0-terminated strings concatenated
 * @strsz: length of string table in bytes
 * @symtab: symbol table, an array of ElfW(Sym) structs
 * @symsz: length of symbol table in bytes
 * @base: base address of the shared object
 * @data: the same data that was passed to process_pt_dynamic()
 *
 * Callback used to iterate over relocations.
 */
typedef int (*relocate_rela_cb)(const ElfW(Rela) *start,
                                  const size_t relasz,
                                  const char *strtab,
                                  const size_t strsz,
                                  const ElfW(Sym) *symtab,
                                  const size_t symsz,
                                  void *base,
                                  void *data);

/*
 * relocate_rel_cb:
 * @start: beginning of an array of relocation entries
 * @relasz: number of bytes (not number of structs!) following @start
 * @strtab: string table, a series of 0-terminated strings concatenated
 * @strsz: length of string table in bytes
 * @symtab: symbol table, an array of ElfW(Sym) structs
 * @symsz: length of symbol table in bytes
 * @base: base address of the shared object
 * @data: the same data that was passed to process_pt_dynamic()
 *
 * Callback used to iterate over relocations.
 */
typedef int (*relocate_rel_cb)(const ElfW(Rel) *start,
                                 const size_t relasz,
                                 const char *strtab,
                                 const size_t strsz,
                                 const ElfW(Sym) *symtab,
                                 const size_t symsz,
                                 void *base,
                                 void *data);

int process_dt_rela (const ElfW(Rela) *start,
                     const size_t relasz,
                     const char *strtab,
                     const size_t strsz,
                     const ElfW(Sym) *symtab,
                     const size_t symsz,
                     void *base,
                     void *data);

int process_dt_rel  (const ElfW(Rel) *start,
                     const size_t relasz,
                     const char *strtab,
                     const size_t strsz,
                     const ElfW(Sym) *symtab,
                     const size_t symsz,
                     void *base,
                     void *data);

int process_pt_dynamic (ElfW(Addr) start,
                        size_t size,
                        void *base,
                        relocate_rela_cb process_rela,
                        relocate_rel_cb process_rel,
                        void *data);
