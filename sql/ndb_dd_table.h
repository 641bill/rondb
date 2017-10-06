/*
   Copyright (c) 2017, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/

#ifndef NDB_DD_TABLE_H
#define NDB_DD_TABLE_H

namespace dd {
  class Table;
}


/* Functions operating on dd::Table*, prefixed with ndb_dd_table_ */

/*
   Save the tables object id and version in table definition
*/
void ndb_dd_table_set_object_id_and_version(dd::Table* table_def,
                                            int object_id, int object_version);

/*
  Return table definitions object id and version
*/
bool
ndb_dd_table_get_object_id_and_version(const dd::Table* table_def,
                                       int& object_id, int& object_version);


/*
   Mark the table as being hidden, thus avoiding that it shows
   up in SHOW TABLES and information_schema queries.
*/
void ndb_dd_table_mark_as_hidden(dd::Table* table_def);

#endif
