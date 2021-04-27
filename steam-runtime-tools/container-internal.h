/*<private_header>*/
/*
 * Copyright © 2021 Collabora Ltd.
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

#include "steam-runtime-tools/container.h"

#include <json-glib/json-glib.h>

/*
 * _srt_container_info_new:
 * @type: Type of container
 * @host_directory: (nullable) (type filename): Directory where host files can
 *  be found
 *
 * Inline convenience function to create a new SrtContainerInfo.
 * This is not part of the public API.
 *
 * Returns: (transfer full): A new #SrtContainerInfo
 */
static inline SrtContainerInfo *_srt_container_info_new (SrtContainerType type,
                                                         const gchar *host_directory);

#ifndef __GTK_DOC_IGNORE__
static inline SrtContainerInfo *
_srt_container_info_new (SrtContainerType type,
                         const gchar *host_directory)
{
  return g_object_new (SRT_TYPE_CONTAINER_INFO,
                       "type", type,
                       "host-directory", host_directory,
                       NULL);
}
#endif

SrtContainerInfo *_srt_check_container (int sysroot_fd,
                                        const gchar *sysroot);

SrtContainerInfo *_srt_container_info_get_from_report (JsonObject *json_obj);
