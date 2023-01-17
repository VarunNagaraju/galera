/* Copyright (c) 2022 Percona LLC and/or its affiliates. All rights
   reserved.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; version 2 of
   the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

#ifndef __GU_ENC_DEBUG__
#define __GU_ENC_DEBUG__

#include <stddef.h>

namespace gu {

enum DebugLevel {
   NOTE,
   WARNING,
   ERROR
};

#if 0
#define S_DEBUG_N(format, ...) swrite(NOTE, format, ##__VA_ARGS__)
#else
#define S_DEBUG_N(...)
#endif
// always
#define S_DEBUG_A(format, ...) swrite(NOTE, format, ##__VA_ARGS__)
#define S_DEBUG_W(format, ...) swrite(WARNING, format, ##__VA_ARGS__)
#define S_DEBUG_E(format, ...) swrite(ERROR, format, ##__VA_ARGS__)


void swrite(DebugLevel level, const char* format, ...);
void dump_memory(void *ptr, size_t size);

}  // namespace

#endif  // __GU_ENC_DEBUG__
