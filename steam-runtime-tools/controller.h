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

#if !defined(_SRT_IN_SINGLE_HEADER) && !defined(_SRT_COMPILATION)
#error "Do not include directly, use <steam-runtime-tools/steam-runtime-tools.h>"
#endif

#include <glib.h>

/**
 * SrtControllerIssues:
 * @SRT_CONTROLLER_ISSUES_NONE: There are no problems
 * @SRT_CONTROLLER_ISSUES_INTERNAL_ERROR: An internal error of some kind has occurred
 * @SRT_CONTROLLER_ISSUES_CANNOT_INSPECT: An error occurred trying to inspect the controller
 * @SRT_CONTROLLER_ISSUES_NOT_ENOUGH_PERMISSIONS: The controller permissions are not
 *  what we expected
 * @SRT_CONTROLLER_ISSUES_UNKNOWN_EXPECTATIONS: We are not able to check known controllers
 * permissions because the expectation patterns are not available
 *
 * A bitfield with flags representing problems with Controllers,
 * or %SRT_CONTROLLER_ISSUES_NONE (which is numerically zero)
 * if no problems were detected.
 *
 * In general, more bits set means more problems.
 */
typedef enum
{
  SRT_CONTROLLER_ISSUES_INTERNAL_ERROR = (1 << 0),
  SRT_CONTROLLER_ISSUES_CANNOT_INSPECT = (1 << 1),
  SRT_CONTROLLER_ISSUES_NOT_ENOUGH_PERMISSIONS = (1 << 2),
  SRT_CONTROLLER_ISSUES_UNKNOWN_EXPECTATIONS = (1 << 3),
  SRT_CONTROLLER_ISSUES_NONE = 0
} SrtControllerIssues;

/**
 * SrtUinputIssues:
 * @SRT_UINPUT_ISSUES_NONE: There are no problems
 * @SRT_UINPUT_ISSUES_INTERNAL_ERROR: An internal error of some kind
 *  has occurred
 * @SRT_UINPUT_ISSUES_CANNOT_WRITE: We are not able to open
 *  `/dev/uinput` for writing
 * @SRT_UINPUT_ISSUES_CANNOT_INSPECT: An error occurred trying to
 *  inspect the ACL of `/dev/uinput`
 * @SRT_UINPUT_ISSUES_NOT_ENOUGH_PERMISSIONS: `/dev/uinput` doesn't have
 *  the expected permissions
 * @SRT_UINPUT_ISSUES_MISSING_USER_ACL: The current user is not in the
 *  `/dev/uinput` ACL
 *
 * A bitfield with flags representing problems that are likely to prevent
 * Steam from emulating additional input devices via `/dev/uinput`,
 * or %SRT_UINPUT_ISSUES_NONE (which is numerically zero)
 * if no problems were detected.
 *
 * In general, more bits set means more problems.
 */
typedef enum
{
  SRT_UINPUT_ISSUES_INTERNAL_ERROR = (1 << 0),
  SRT_UINPUT_ISSUES_CANNOT_WRITE = (1 << 1),
  SRT_UINPUT_ISSUES_CANNOT_INSPECT = (1 << 2),
  SRT_UINPUT_ISSUES_NOT_ENOUGH_PERMISSIONS = (1 << 3),
  SRT_UINPUT_ISSUES_MISSING_USER_ACL = (1 << 4),
  SRT_UINPUT_ISSUES_NONE = 0
} SrtUinputIssues;
