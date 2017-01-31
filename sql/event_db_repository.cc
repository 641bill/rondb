/* Copyright (c) 2006, 2017 Oracle and/or its affiliates. All rights reserved.

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

#include "event_db_repository.h"

#include <vector>

#include "auth_acls.h"
#include "dd/cache/dictionary_client.h" // fetch_schema_components
#include "dd/dd_event.h"
#include "dd/dd_schema.h"
#include "dd/string_type.h"
#include "dd/types/event.h"
#include "dd/types/schema.h"
#include "derror.h"
#include "event_data_objects.h"
#include "event_parse_data.h"
#include "my_dbug.h"
#include "my_sys.h"
#include "mysqld_error.h"
#include "sp_head.h"
#include "sql_class.h"
#include "sql_error.h"
#include "sql_lex.h"
#include "sql_security_ctx.h"
#include "sql_show.h"
#include "table.h"
#include "template_utils.h"
#include "transaction.h"
#include "tztime.h"                     // struct Time_zone

/**
  @addtogroup Event_Scheduler
  @{
*/


/**
  Creates an event object and persist to Data Dictionary.

  @pre All semantic checks must be performed outside.

  @param[in,out] thd                   THD
  @param[in]     parse_data            Parsed event definition
  @param[in]     create_if_not         true if IF NOT EXISTS clause was provided
                                       to CREATE EVENT statement
  @param[out]    event_already_exists  When method is completed successfully
                                       set to true if event already exists else
                                       set to false
  @retval false  Success
  @retval true   Error
*/

bool
Event_db_repository::create_event(THD *thd, Event_parse_data *parse_data,
                                  bool create_if_not,
                                  bool *event_already_exists)
{
  DBUG_ENTER("Event_db_repository::create_event");
  sp_head *sp= thd->lex->sphead;
  // Turn off autocommit.
  Disable_autocommit_guard autocommit_guard(thd);

  DBUG_ASSERT(sp);

  if (dd::event_exists(thd->dd_client(), parse_data->dbname.str,
                       parse_data->name.str, event_already_exists))
    DBUG_RETURN(true);

  if (*event_already_exists)
  {
    if (create_if_not)
    {
      push_warning_printf(thd, Sql_condition::SL_NOTE,
                          ER_EVENT_ALREADY_EXISTS,
                          ER_THD(thd, ER_EVENT_ALREADY_EXISTS),
                          parse_data->name.str);
      DBUG_RETURN(false);
    }
    my_error(ER_EVENT_ALREADY_EXISTS, MYF(0), parse_data->name.str);
    DBUG_RETURN(true);
  }

  DBUG_RETURN(dd::create_event(thd, parse_data->dbname.str,
                               parse_data->name.str,
                               sp->m_body.str, sp->m_body_utf8.str,
                               thd->lex->definer,
                               parse_data));
}


/**
  Used to execute ALTER EVENT. Pendant to Events::update_event().

  @param[in]      thd         THD context
  @param[in]      parse_data  parsed event definition
  @param[in]      new_dbname  not NULL if ALTER EVENT RENAME
                              points at a new database name
  @param[in]      new_name    not NULL if ALTER EVENT RENAME
                              points at a new event name

  @pre All semantic checks are performed outside this function.

  @retval false Success
  @retval true Error (reported)
*/

bool
Event_db_repository::update_event(THD *thd, Event_parse_data *parse_data,
                                  LEX_STRING *new_dbname, LEX_STRING *new_name)
{
  DBUG_ENTER("Event_db_repository::update_event");
  sp_head *sp= thd->lex->sphead;
  // Turn off autocommit.
  Disable_autocommit_guard autocommit_guard(thd);

  /* None or both must be set */
  DBUG_ASSERT((new_dbname && new_name) || new_dbname == new_name);

  DBUG_PRINT("info", ("dbname: %s", parse_data->dbname.str));
  DBUG_PRINT("info", ("name: %s", parse_data->name.str));
  DBUG_PRINT("info", ("user: %s", parse_data->definer.str));

  dd::cache::Dictionary_client::Auto_releaser releaser(thd->dd_client());

  /* first look whether we overwrite */
  if (new_name)
  {
    DBUG_PRINT("info", ("rename to: %s@%s", new_dbname->str, new_name->str));

    bool exists= false;
    if (dd::event_exists(thd->dd_client(), new_dbname->str,
                         new_name->str, &exists))
      DBUG_RETURN(true);

    if (exists)
    {
      my_error(ER_EVENT_ALREADY_EXISTS, MYF(0), new_name->str);
      DBUG_RETURN(true);
    }
  }

  const dd::Event *event= nullptr;
  if (thd->dd_client()->acquire(parse_data->dbname.str,
                                parse_data->name.str, &event))
    DBUG_RETURN(true);

  if (event == nullptr)
  {
    my_error(ER_EVENT_DOES_NOT_EXIST, MYF(0), parse_data->name.str);
    DBUG_RETURN(true);
  }

  // Update Event in the data dictionary with altered event object attributes.
  if (dd::update_event(thd, event, new_dbname != nullptr ? new_dbname->str : "",
                       new_name != nullptr ? new_name->str : "",
                       (parse_data->body_changed) ? sp->m_body.str : event->definition(),
                       (parse_data->body_changed) ? sp->m_body_utf8.str :
                                                    event->definition_utf8(),
                       thd->lex->definer, parse_data))
  {
    DBUG_RETURN(true);
  }
  DBUG_RETURN(false);
}


/**
  Delete event.

  @param[in]     thd            THD context
  @param[in]     db             Database name
  @param[in]     name           Event name
  @param[in]     drop_if_exists DROP IF EXISTS clause was specified.
                                If set, and the event does not exist,
                                the error is downgraded to a warning.

  @retval false success
  @retval true error (reported)
*/

bool
Event_db_repository::drop_event(THD *thd, LEX_STRING db, LEX_STRING name,
                                bool drop_if_exists)
{
  DBUG_ENTER("Event_db_repository::drop_event");
  // Turn off autocommit.
  Disable_autocommit_guard autocommit_guard(thd);
  /*
    Turn off row binlogging of this statement and use statement-based
    so that all supporting tables are updated for CREATE EVENT command.
    When we are going out of the function scope, the original binary
    format state will be restored.
  */
  Save_and_Restore_binlog_format_state binlog_format_state(thd);

  DBUG_PRINT("enter", ("%s@%s", db.str, name.str));

  const dd::Event *event_ptr= nullptr;
  dd::cache::Dictionary_client::Auto_releaser releaser(thd->dd_client());
  if (thd->dd_client()->acquire(db.str, name.str, &event_ptr))
  {
    // Error is reported by the dictionary subsystem.
    DBUG_RETURN(true);
  }

  if (event_ptr != nullptr)
    DBUG_RETURN(dd::drop_event(thd, event_ptr));

  // Event not found
  if (!drop_if_exists)
  {
    my_error(ER_EVENT_DOES_NOT_EXIST, MYF(0), name.str);
    DBUG_RETURN(true);
  }

  push_warning_printf(thd, Sql_condition::SL_NOTE,
                      ER_SP_DOES_NOT_EXIST, ER_THD(thd, ER_SP_DOES_NOT_EXIST),
                      "Event", name.str);
  DBUG_RETURN(false);
}


/**
  Drops all events in the selected database.

  @param      thd     THD context
  @param      schema  The database under which events are to be dropped.

  @returns true on error, false on success.
*/

bool
Event_db_repository::drop_schema_events(THD *thd, LEX_STRING schema)
{
  DBUG_ENTER("Event_db_repository::drop_schema_events");
  DBUG_PRINT("enter", ("schema=%s", schema.str));

  dd::cache::Dictionary_client::Auto_releaser releaser(thd->dd_client());
  const dd::Schema *sch_obj= nullptr;

  if (thd->dd_client()->acquire(schema.str, &sch_obj))
    DBUG_RETURN(true);
  if (sch_obj == nullptr)
  {
    my_error(ER_BAD_DB_ERROR, MYF(0), schema.str);
    DBUG_RETURN(true);
  }

  std::vector<const dd::Event*> events;
  if (thd->dd_client()->fetch_schema_components(sch_obj, &events))
    DBUG_RETURN(true);

  for (const dd::Event *event_obj : events)
  {
    if (thd->dd_client()->drop(event_obj))
    {
      my_error(ER_SP_DROP_FAILED, MYF(0),
               "Drop failed for Event: %s", event_obj->name().c_str());
      DBUG_RETURN(true);
    }
  }

  DBUG_RETURN(false);
}


/**
  Looks for a named event in the Data Dictionary and load it.

  @pre The given thread does not have open tables.

  @retval false  success
  @retval true   error
*/

bool
Event_db_repository::load_named_event(THD *thd, LEX_STRING dbname,
                                      LEX_STRING name, Event_basic *etn)
{
  const dd::Event *event_obj= nullptr;

  DBUG_ENTER("Event_db_repository::load_named_event");
  DBUG_PRINT("enter",("thd: %p  name: %*s", thd,
                      (int) name.length, name.str));

  dd::cache::Dictionary_client::Auto_releaser releaser(thd->dd_client());

  if (thd->dd_client()->acquire(dbname.str, name.str, &event_obj))
  {
    // Error is reported by the dictionary subsystem.
    DBUG_RETURN(true);
  }

  if (event_obj == nullptr)
  {
    my_error(ER_EVENT_DOES_NOT_EXIST, MYF(0), name.str);
    DBUG_RETURN(true);
  }

  if (etn->fill_event_info(thd, *event_obj, dbname.str))
  {
    my_error(ER_CANNOT_LOAD_FROM_TABLE_V2, MYF(0), "mysql", "events");
    DBUG_RETURN(true);
  }

  DBUG_RETURN(false);
}


/**
   Update the event in Data Dictionary with changed status
   and/or last execution time.
*/

bool
Event_db_repository::
update_timing_fields_for_event(THD *thd, LEX_STRING event_db_name,
                               LEX_STRING event_name, my_time_t last_executed,
                               ulonglong status)
{
  DBUG_ENTER("Event_db_repository::update_timing_fields_for_event");
  // Turn off autocommit.
  Disable_autocommit_guard autocommit_guard(thd);

  /*
    Turn off row binlogging of this statement and use statement-based
    so that all supporting tables are updated for CREATE EVENT command.
    When we are going out of the function scope, the original binary
    format state will be restored.
  */
  Save_and_Restore_binlog_format_state binlog_format_state(thd);

  DBUG_ASSERT(thd->security_context()->check_access(SUPER_ACL));

  const dd::Event *event_ptr= nullptr;
  dd::cache::Dictionary_client::Auto_releaser releaser(thd->dd_client());
  if (thd->dd_client()->acquire(event_db_name.str, event_name.str, &event_ptr))
    DBUG_RETURN(true);
  if (event_ptr == nullptr)
    DBUG_RETURN(true);

  bool res= dd::update_event_time_and_status(thd, event_ptr, last_executed,
                                        status);

  DBUG_RETURN(res);
}

/**
  @} (End of group Event_Scheduler)
*/
// XXX:
