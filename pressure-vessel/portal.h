/*
 * Adapted from xdg-desktop-portal
 *
 * Copyright © 2017 Red Hat, Inc
 * Copyright © 2020 Collabora Ltd.
 *
 * SPDX-License-Identifier: LGPL-2-or-later
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 */

#pragma once

#define PV_PORTAL_BUS_NAME "com.steampowered.PressureVessel.Portal"

#define PV_PORTAL_INPUT1_PATH "/com/steampowered/PressureVessel/InputPortal1"
#define PV_PORTAL_INPUT1_IFACE "com.steampowered.PressureVessel.InputPortal1"

#define PV_PORTAL_SESSION_IFACE "org.freedesktop.portal.Session"
#define PV_PORTAL_SESSION_PATH_PREFIX "/org/freedesktop/portal/desktop/session/"

#define PV_PORTAL_REQUEST_IFACE "org.freedesktop.portal.Request"
#define PV_PORTAL_REQUEST_PATH_PREFIX "/org/freedesktop/portal/desktop/request/"
