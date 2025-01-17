/*
   Copyright (c) 2003, 2023, Oracle and/or its affiliates.
    Use is subject to license terms.
   Copyright (c) 2023, 2023, Hopsworks and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/

#include <ndb_global.h>

/* XXH hash function */
#define XXH_INLINE_ALL
#if defined (__AVX2__)
#define XXH_VECTOR_TYPE 2 //AVX2 implementation
#endif
#include <util/ndb_xxhash.h>

Uint64
rondb_xxhash_avx2(const char* keybuf, Uint32 keylen_bytes)
{
  return XXH3_64bits(keybuf, keylen_bytes);
}
