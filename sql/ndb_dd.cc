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

// Implements the functions defined in ndb_dd.h
#include "sql/ndb_dd.h"

// Using
#include "sql/ndb_dd_client.h"
#include "sql/ndb_dd_table.h"
#include "sql/ndb_dd_sdi.h"

#include "sql/sql_class.h"

#include "sql/dd/dd.h"
#include "sql/dd/types/table.h"

bool ndb_sdi_serialize(THD *thd,
                       const dd::Table *table_def,
                       const char* schema_name,
                       dd::sdi_t& sdi)
{
  // Require the table to be visible or else have temporary name
  DBUG_ASSERT(table_def->hidden() == dd::Abstract_table::HT_VISIBLE ||
              is_prefix(table_def->name().c_str(), tmp_file_prefix));

  // Make a copy of the table definition to allow it to
  // be modified before serialization
  std::unique_ptr<dd::Table> table_def_clone(
        dynamic_cast<dd::Table*>(table_def->clone()));

  // Don't include the se_private_id in the serialized table def.
  table_def_clone->set_se_private_id(dd::INVALID_OBJECT_ID);

  // Don't include any se_private_data properties in the
  // serialized table def.
  table_def_clone->se_private_data().clear();

  sdi = ndb_dd_sdi_serialize(thd, *table_def_clone,
                             dd::String_type(schema_name));
  if (sdi.empty())
    return false; // Failed to serialize

  return true; // OK
}


/*
  Workaround for BUG#25657041

  During inplace alter table, the table has a temporary
  tablename and is also marked as hidden. Since the temporary
  name and hidden status is part of the serialized table
  definition, there's a mismatch down the line when this is
  stored as extra metadata in the NDB dictionary.

  The workaround for now involves setting the table as a user
  visible table and restoring the original table name
*/

void ndb_dd_fix_inplace_alter_table_def(dd::Table* table_def,
                                        const char* proper_table_name)
{
  DBUG_ENTER("ndb_dd_fix_inplace_alter_table_def");
  DBUG_PRINT("enter", ("table_name: %s", table_def->name().c_str()));
  DBUG_PRINT("enter", ("proper_table_name: %s", proper_table_name));

  // Check that the proper_table_name is not a temporary name
  DBUG_ASSERT(!is_prefix(proper_table_name, tmp_file_prefix));

  table_def->set_name(proper_table_name);
  table_def->set_hidden(dd::Abstract_table::HT_VISIBLE);

  DBUG_VOID_RETURN;
}


bool ndb_dd_does_table_exist(class THD *thd,
                             const char* schema_name,
                             const char* table_name,
                             int& table_id,
                             int& table_version,
                             dd::String_type* engine)

{
  DBUG_ENTER("ndb_dd_does_table_exist");

  Ndb_dd_client dd_client(thd);

  // First acquire MDL locks on schema and table
  if (!dd_client.mdl_lock_table(schema_name, table_name))
  {
    DBUG_RETURN(false);
  }

  if (!dd_client.check_table_exists(schema_name, table_name,
                                    table_id, table_version, engine))
  {
    DBUG_RETURN(false);
  }

  dd_client.commit();

  DBUG_RETURN(true); // OK!
}


bool
ndb_dd_install_table(class THD *thd,
                     const char* schema_name,
                     const char* table_name,
                     const dd::sdi_t &sdi,
                     int ndb_table_id, int ndb_table_version,
                     bool force_overwrite)
{

  DBUG_ENTER("ndb_dd_install_table");

  Ndb_dd_client dd_client(thd);

  // First acquire exclusive MDL locks on schema and table
  if (!dd_client.mdl_locks_acquire_exclusive(schema_name, table_name))
  {
    DBUG_RETURN(false);
  }

  if (!dd_client.install_table(schema_name, table_name,
                               sdi, ndb_table_id, ndb_table_version,
                               force_overwrite))
  {
    DBUG_RETURN(false);
  }

  dd_client.commit();

  DBUG_RETURN(true); // OK!
}


bool
ndb_dd_drop_table(THD *thd,
                  const char *schema_name, const char *table_name)
{
  DBUG_ENTER("ndb_dd_drop_table");

  Ndb_dd_client dd_client(thd);

  if (!dd_client.drop_table(schema_name, table_name))
  {
    DBUG_RETURN(false);
  }

  dd_client.commit();

  DBUG_RETURN(true); // OK
}


bool
ndb_dd_rename_table(THD *thd,
                    const char *old_schema_name, const char *old_table_name,
                    const char *new_schema_name, const char *new_table_name)
{
  DBUG_ENTER("ndb_dd_rename_table");
  DBUG_PRINT("enter", ("old: '%s'.'%s'  new: '%s'.'%s'",
                       old_schema_name, old_table_name,
                       new_schema_name, new_table_name));

  Ndb_dd_client dd_client(thd);

  if (!dd_client.rename_table(old_schema_name, old_table_name,
                              new_schema_name, new_table_name))
  {
    DBUG_RETURN(false);
  }

  dd_client.commit();

  DBUG_RETURN(true); // OK
}


bool
ndb_dd_table_get_engine(THD *thd,
                        const char *schema_name,
                        const char *table_name,
                        dd::String_type* engine)
{
  DBUG_ENTER("ndb_dd_table_get_engine");

  Ndb_dd_client dd_client(thd);

  if (!dd_client.get_engine(schema_name, table_name, engine))
  {
    DBUG_RETURN(false);
  }

  DBUG_PRINT("info", (("table '%s.%s' exists in DD, engine: '%s'"),
                      schema_name, table_name, engine->c_str()));

  dd_client.commit();

  DBUG_RETURN(true); // OK
}

