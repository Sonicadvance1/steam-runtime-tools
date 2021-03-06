// Copyright © 2017 Collabora Ltd
// SPDX-License-Identifier: LGPL-2.1-or-later

// libnotgl-helper — A library used by the OpenGL driver, taking the
// role of libstdc++

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

#pragma once

#define HELPER_PUBLIC __attribute__((visibility("default")))

HELPER_PUBLIC
const char *helper_get_implementation( void );
