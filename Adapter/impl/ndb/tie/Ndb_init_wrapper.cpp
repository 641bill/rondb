/*
 Copyright (c) 2012, Oracle and/or its affiliates. All rights
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
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 02110-1301  USA
 */

#include <v8.h>

#include <ndb_init.h>

#include "js_wrapper_macros.h"
#include "NativeCFunctionCall.h"

using namespace v8;

/* int ndb_init(void) 
*/
Handle<Value> Ndb_init_wrapper(const Arguments &args) {
  DEBUG_ENTER();
  HandleScope scope;
  
  REQUIRE_ARGS_LENGTH(0);

  NativeCFunctionCall_0_<int> ncall(args);
  ncall.function = & ndb_init;
  ncall.run();
  DEBUG_TRACE();
  
  return scope.Close(ncall.jsReturnVal());
}


/* void ndb_end(int) 
*/
Handle<Value> Ndb_end_wrapper(const Arguments &args) {
  DEBUG_MARKER();
  HandleScope scope;
  
  REQUIRE_ARGS_LENGTH(1);  
  
  NativeCVoidFunctionCall_1_<int> ncall(args);
  ncall.function = & ndb_end;
  ncall.run();
  
  return scope.Close(ncall.jsReturnVal());
}


void Ndb_init_initOnLoad(Handle<Object> target) {
  DEBUG_MARKER();
  DEFINE_JS_FUNCTION(target, "ndb_init", Ndb_init_wrapper);
  DEFINE_JS_FUNCTION(target, "ndb_end", Ndb_end_wrapper);
}

