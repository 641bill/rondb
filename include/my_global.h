/*
   Copyright (c) 2001, 2016, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#ifndef MY_GLOBAL_INCLUDED
#define MY_GLOBAL_INCLUDED

/**
  @file include/my_global.h
  This include file used to be included first in every header file.
  Do not include it in new code; instead, include what you use.
*/

#include "my_config.h"

#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stddef.h>
#include <math.h>
#include <limits.h>
#include <float.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>				/* Recommended by debian */
#include <sys/types.h>

#if !defined(_WIN32)
#include <netdb.h>
#endif
#ifdef MY_MSCRT_DEBUG
#include <crtdbg.h>
#endif

/* Include standard definitions of operator new and delete. */
#ifdef __cplusplus
# include <new>
#endif

#include "my_compiler.h"
#include "my_dbug.h"
#include "my_inttypes.h"
#include "my_io.h"
#include "my_macros.h"
#include "my_sharedlib.h"
#include "my_shm_defaults.h"
#include "my_systime.h"
#include "my_table_map.h"

#endif  // MY_GLOBAL_INCLUDED
