/*<private_header>*/
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

#pragma once

#include "steam-runtime-tools/controller.h"

#include <glib.h>
#include <glib-object.h>
#include <json-glib/json-glib.h>

/**
 * PairID:
 * @id_vendor: A pattern matching for the `idVendor` field of udev ATTRS
 * @id_product: A pattern matching for the `idProduct` field of udev ATTRS
 * @skip_read_check: If %TRUE the read permission check on a controller will
 *  be skipped
 * @skip_write_check: If %TRUE the write permission check on a controller will
 *  be skipped
 */
typedef struct
{
  GPatternSpec *id_vendor;
  GPatternSpec *id_product;
  gboolean skip_read_check;
  gboolean skip_write_check;
} PairID;

/**
 * KernelPattern:
 * @pattern: A pattern matching for the `KERNELS` field of udev
 * @skip_read_check: If %TRUE the read permission check on a controller will
 *  be skipped
 * @skip_write_check: If %TRUE the write permission check on a controller will
 *  be skipped
 */
typedef struct
{
  GPatternSpec *pattern;
  gboolean skip_read_check;
  gboolean skip_write_check;
} KernelPattern;

/**
 * ControllerPatterns:
 * @kernels_list: A GList of #Kernel_pattern for udev KERNELS field
 * @hidraw_id_list: A GList of #Pair_id for udev ATTRS field of hidraw
 * @usb_subsystem_id_list: A GList of #Pair_id for udev ATTRS field of USB subsystem
 */
typedef struct
{
  GList *kernels_list;
  GList *hidraw_id_list;
  GList *usb_subsystem_id_list;
} ControllerPatterns;

G_GNUC_INTERNAL
SrtUinputIssues _srt_check_uinput (void);
SrtControllerIssues _srt_controller_check_permissions (ControllerPatterns *patterns);
ControllerPatterns * _srt_controller_initialize_patterns (const gchar *filename);
void _free_controller_patterns (ControllerPatterns *patterns);
