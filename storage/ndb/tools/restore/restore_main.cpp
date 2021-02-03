/*
   Copyright (c) 2003, 2021, Oracle and/or its affiliates. All rights reserved.

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
#include <ndb_opts.h>
#include <Vector.hpp>
#include <Properties.hpp>
#include <ndb_limits.h>
#include <NdbTCP.h>
#include <NdbOut.hpp>
#include <OutputStream.hpp>
#include <Logger.hpp>

#include "consumer_restore.hpp"
#include "consumer_printer.hpp"
#include "../src/ndbapi/NdbDictionaryImpl.hpp"

#define TMP_TABLE_PREFIX "#sql"
#define TMP_TABLE_PREFIX_LEN 4

extern FilteredNdbOut err;
extern FilteredNdbOut info;
extern FilteredNdbOut debug;

static Uint32 g_tableCompabilityMask = 0;
static int ga_nodeId = 0;
static int ga_nParallelism = 128;
static int ga_backupId = 0;
bool ga_dont_ignore_systab_0 = false;
static bool ga_no_upgrade = false;
static bool ga_promote_attributes = false;
static bool ga_demote_attributes = false;
static Vector<class BackupConsumer *> g_consumers;
static BackupPrinter* g_printer = NULL;

static const char* default_backupPath = "." DIR_SEPARATOR;
static const char* ga_backupPath = default_backupPath;

static const char *opt_nodegroup_map_str= 0;
static unsigned opt_nodegroup_map_len= 0;
static NODE_GROUP_MAP opt_nodegroup_map[MAX_NODE_GROUP_MAPS];
#define OPT_NDB_NODEGROUP_MAP 'z'

const char *opt_ndb_database= NULL;
const char *opt_ndb_table= NULL;
unsigned int opt_verbose;
unsigned int opt_hex_format;
unsigned int opt_progress_frequency;
NDB_TICKS g_report_prev;
Vector<BaseString> g_databases;
Vector<BaseString> g_tables;
Vector<BaseString> g_include_tables, g_exclude_tables;
Vector<BaseString> g_include_databases, g_exclude_databases;
Properties g_rewrite_databases;
NdbRecordPrintFormat g_ndbrecord_print_format;
unsigned int opt_no_binlog;

class RestoreOption
{
public:
  virtual ~RestoreOption() { }
  int optid;
  BaseString argument;
};

Vector<class RestoreOption *> g_include_exclude;
static void save_include_exclude(int optid, char * argument);

static inline void parse_rewrite_database(char * argument);

/**
 * print and restore flags
 */
static bool ga_restore_epoch = false;
static bool ga_restore = false;
static bool ga_print = false;
static bool ga_skip_table_check = false;
static bool ga_exclude_missing_columns = false;
static bool ga_exclude_missing_tables = false;
static bool opt_exclude_intermediate_sql_tables = true;
#ifdef ERROR_INSERT 
static unsigned int _error_insert = 0;
#endif
static int _print = 0;
static int _print_meta = 0;
static int _print_data = 0;
static int _print_log = 0;
static int _print_sql_log = 0;
static int _restore_data = 0;
static int _restore_meta = 0;
static int _no_restore_disk = 0;
static bool _preserve_trailing_spaces = false;
static bool ga_disable_indexes = false;
static bool ga_rebuild_indexes = false;
bool ga_skip_unknown_objects = false;
bool ga_skip_broken_objects = false;
bool ga_allow_pk_changes = false;
bool ga_ignore_extended_pk_updates = false;
BaseString g_options("ndb_restore");
static int ga_num_slices = 1;
static int ga_slice_id = 0;

const char *load_default_groups[]= { "mysql_cluster","ndb_restore",0 };

enum ndb_restore_options {
  OPT_VERBOSE = NDB_STD_OPTIONS_LAST,
  OPT_INCLUDE_TABLES,
  OPT_EXCLUDE_TABLES,
  OPT_INCLUDE_DATABASES,
  OPT_EXCLUDE_DATABASES,
  OPT_REWRITE_DATABASE
#ifdef ERROR_INSERT
  ,OPT_ERROR_INSERT
#endif
  ,OPT_REMAP_COLUMN = 'x'
};
static const char *opt_fields_enclosed_by= NULL;
static const char *opt_fields_terminated_by= NULL;
static const char *opt_fields_optionally_enclosed_by= NULL;
static const char *opt_lines_terminated_by= NULL;

static const char *tab_path= NULL;
static int opt_append;
static const char *opt_exclude_tables= NULL;
static const char *opt_include_tables= NULL;
static const char *opt_exclude_databases= NULL;
static const char *opt_include_databases= NULL;
static const char *opt_rewrite_database= NULL;
static const char *opt_one_remap_col_arg= NULL;
static bool opt_restore_privilege_tables = false;

/**
 * ExtraTableInfo
 *
 * Container for information from user about how
 * table should be restored
 */
class ExtraTableInfo
{
public:
  ExtraTableInfo(const char* db_name,
                 const char* table_name):
    m_dbName(db_name),
    m_tableName(table_name)
  {
  }

  ~ExtraTableInfo() {};

  const BaseString m_dbName;
  const BaseString m_tableName;

  /* Arguments related to column remappings */
  Vector<BaseString> m_remapColumnArgs;
};

/**
 * ExtraRestoreInfo
 *
 * Container for information from user about
 * how to restore
 */
class ExtraRestoreInfo
{
public:
  ExtraRestoreInfo()
  {};
  ~ExtraRestoreInfo()
  {
    for (Uint32 i=0; i<m_tables.size(); i++)
    {
      delete m_tables[i];
      m_tables[i] = NULL;
    }
  };

  /**
   * findTable
   *
   * Lookup extra restore info for named table
   */
  ExtraTableInfo* findTable(const char* db_name,
                            const char* table_name)
  {
    for (Uint32 i=0; i<m_tables.size(); i++)
    {
      ExtraTableInfo* tab = m_tables[i];
      if ((strcmp(db_name, tab->m_dbName.c_str()) == 0) &&
          (strcmp(table_name, tab->m_tableName.c_str()) == 0))
      {
        return tab;
      }
    }
    return NULL;
  }

  /**
   * findOrAddTable
   *
   * Lookup or Add empty extra restore info for named table
   */
  ExtraTableInfo* findOrAddTable(const char* db_name,
                                 const char* table_name)
  {
    ExtraTableInfo* tab = findTable(db_name, table_name);
    if (tab != NULL)
    {
      return tab;
    }

    tab = new ExtraTableInfo(db_name, table_name);
    m_tables.push_back(tab);
    return tab;
  }

  Vector<ExtraTableInfo*> m_tables;
};

static ExtraRestoreInfo g_extra_restore_info;

static struct my_option my_long_options[] =
{
  NDB_STD_OPTS("ndb_restore"),
  { "connect", 'c', "same as --connect-string",
    (uchar**) &opt_ndb_connectstring, (uchar**) &opt_ndb_connectstring, 0,
    GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0 },
  { "nodeid", 'n', "Backup files from node with id",
    (uchar**) &ga_nodeId, (uchar**) &ga_nodeId, 0,
    GET_INT, REQUIRED_ARG, 0, 0, 0, 0, 0, 0 },
  { "backupid", 'b', "Backup id",
    (uchar**) &ga_backupId, (uchar**) &ga_backupId, 0,
    GET_INT, REQUIRED_ARG, 0, 0, 0, 0, 0, 0 },
  { "restore_data", 'r', 
    "Restore table data/logs into NDB Cluster using NDBAPI", 
    (uchar**) &_restore_data, (uchar**) &_restore_data,  0,
    GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0 },
  { "restore_meta", 'm',
    "Restore meta data into NDB Cluster using NDBAPI",
    (uchar**) &_restore_meta, (uchar**) &_restore_meta,  0,
    GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0 },
  { "no-upgrade", 'u',
    "Don't upgrade array type for var attributes, which don't resize VAR data and don't change column attributes",
    (uchar**) &ga_no_upgrade, (uchar**) &ga_no_upgrade, 0,
    GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0 },
  { "promote-attributes", 'A',
    "Allow attributes to be promoted when restoring data from backup",
    (uchar**) &ga_promote_attributes, (uchar**) &ga_promote_attributes, 0,
    GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0 },
  { "lossy-conversions", 'L',
    "Allow lossy conversions for attributes (type demotions or integral"
    " signed/unsigned type changes) when restoring data from backup",
    (uchar**) &ga_demote_attributes, (uchar**) &ga_demote_attributes, 0,
    GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0 },
  { "preserve-trailing-spaces", 'P',
    "Allow to preserve the tailing spaces (including paddings) When char->varchar or binary->varbinary is promoted",
    (uchar**) &_preserve_trailing_spaces, (uchar**)_preserve_trailing_spaces , 0,
    GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0 },
  { "no-restore-disk-objects", 'd',
    "Dont restore disk objects (tablespace/logfilegroups etc)",
    (uchar**) &_no_restore_disk, (uchar**) &_no_restore_disk,  0,
    GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0 },
  { "restore_epoch", 'e', 
    "Restore epoch info into the status table. Convenient on a MySQL Cluster "
    "replication slave, for starting replication. The row in "
    NDB_REP_DB "." NDB_APPLY_TABLE " with id 0 will be updated/inserted.", 
    (uchar**) &ga_restore_epoch, (uchar**) &ga_restore_epoch,  0,
    GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0 },
  { "skip-table-check", 's', "Skip table structure check during restore of data",
   (uchar**) &ga_skip_table_check, (uchar**) &ga_skip_table_check, 0,
   GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0 },
  { "parallelism", 'p',
    "No of parallel transactions during restore of data."
    "(parallelism can be 1 to 1024)", 
    (uchar**) &ga_nParallelism, (uchar**) &ga_nParallelism, 0,
    GET_INT, REQUIRED_ARG, 128, 1, 1024, 0, 1, 0 },
  { "print", NDB_OPT_NOSHORT, "Print metadata, data and log to stdout",
    (uchar**) &_print, (uchar**) &_print, 0,
    GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0 },
  { "print_data", NDB_OPT_NOSHORT, "Print data to stdout",
    (uchar**) &_print_data, (uchar**) &_print_data, 0,
    GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0 },
  { "print_meta", NDB_OPT_NOSHORT, "Print meta data to stdout",
    (uchar**) &_print_meta, (uchar**) &_print_meta,  0,
    GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0 },
  { "print_log", NDB_OPT_NOSHORT, "Print log to stdout",
    (uchar**) &_print_log, (uchar**) &_print_log,  0,
    GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0 },
  { "print_sql_log", NDB_OPT_NOSHORT, "Print SQL log to stdout",
    (uchar**) &_print_sql_log, (uchar**) &_print_sql_log,  0,
    GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0 },
  { "backup_path", NDB_OPT_NOSHORT, "Path to backup files",
    (uchar**) &ga_backupPath, (uchar**) &ga_backupPath, 0,
    GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0 },
  { "dont_ignore_systab_0", 'f',
    "Do not ignore system table during --print-data.", 
    (uchar**) &ga_dont_ignore_systab_0, (uchar**) &ga_dont_ignore_systab_0, 0,
    GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0 },
  { "ndb-nodegroup-map", OPT_NDB_NODEGROUP_MAP,
    "Nodegroup map for ndbcluster. Syntax: list of (source_ng, dest_ng)",
    (uchar**) &opt_nodegroup_map_str,
    (uchar**) &opt_nodegroup_map_str,
    0,
    GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0 },
  { "fields-enclosed-by", NDB_OPT_NOSHORT,
    "Fields are enclosed by ...",
    (uchar**) &opt_fields_enclosed_by, (uchar**) &opt_fields_enclosed_by, 0,
    GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0 },
  { "fields-terminated-by", NDB_OPT_NOSHORT,
    "Fields are terminated by ...",
    (uchar**) &opt_fields_terminated_by,
    (uchar**) &opt_fields_terminated_by, 0,
    GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0 },
  { "fields-optionally-enclosed-by", NDB_OPT_NOSHORT,
    "Fields are optionally enclosed by ...",
    (uchar**) &opt_fields_optionally_enclosed_by,
    (uchar**) &opt_fields_optionally_enclosed_by, 0,
    GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0 },
  { "hex", NDB_OPT_NOSHORT, "print binary types in hex format", 
    (uchar**) &opt_hex_format, (uchar**) &opt_hex_format, 0,
    GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0 },
  { "tab", 'T', "Creates tab separated textfile for each table to "
    "given path. (creates .txt files)",
   (uchar**) &tab_path, (uchar**) &tab_path, 0,
    GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  { "append", NDB_OPT_NOSHORT, "for --tab append data to file", 
    (uchar**) &opt_append, (uchar**) &opt_append, 0,
    GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0 },
  { "lines-terminated-by", NDB_OPT_NOSHORT, "",
    (uchar**) &opt_lines_terminated_by, (uchar**) &opt_lines_terminated_by, 0,
    GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0 },
  { "progress-frequency", NDB_OPT_NOSHORT,
    "Print status uf restore periodically in given seconds", 
    (uchar**) &opt_progress_frequency, (uchar**) &opt_progress_frequency, 0,
    GET_INT, REQUIRED_ARG, 0, 0, 65535, 0, 0, 0 },
  { "no-binlog", NDB_OPT_NOSHORT,
    "If a mysqld is connected and has binary log, do not log the restored data", 
    (uchar**) &opt_no_binlog, (uchar**) &opt_no_binlog, 0,
    GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0 },
  { "verbose", OPT_VERBOSE,
    "verbosity", 
    (uchar**) &opt_verbose, (uchar**) &opt_verbose, 0,
    GET_INT, REQUIRED_ARG, 1, 0, 255, 0, 0, 0 },
  { "include-databases", OPT_INCLUDE_DATABASES,
    "Comma separated list of databases to restore. Example: db1,db3",
    (uchar**) &opt_include_databases, (uchar**) &opt_include_databases, 0,
    GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0 },
  { "exclude-databases", OPT_EXCLUDE_DATABASES,
    "Comma separated list of databases to not restore. Example: db1,db3",
    (uchar**) &opt_exclude_databases, (uchar**) &opt_exclude_databases, 0,
    GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0 },
  { "rewrite-database", OPT_REWRITE_DATABASE,
    "A pair 'source,dest' of database names from/into which to restore. "
    "Example: --rewrite-database=oldDb,newDb",
    (uchar**) &opt_rewrite_database, (uchar**) &opt_rewrite_database, 0,
    GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0 },
  { "include-tables", OPT_INCLUDE_TABLES, "Comma separated list of tables to "
    "restore. Table name should include database name. Example: db1.t1,db3.t1", 
    (uchar**) &opt_include_tables, (uchar**) &opt_include_tables, 0,
    GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0 },
  { "exclude-tables", OPT_EXCLUDE_TABLES, "Comma separated list of tables to "
    "not restore. Table name should include database name. "
    "Example: db1.t1,db3.t1",
    (uchar**) &opt_exclude_tables, (uchar**) &opt_exclude_tables, 0,
    GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0 },
  { "restore-privilege-tables", NDB_OPT_NOSHORT,
    "Restore privilege tables (after they have been moved to ndb)",
    (uchar**) &opt_restore_privilege_tables,
    (uchar**) &opt_restore_privilege_tables, 0,
    GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0 },
  { "exclude-missing-columns", NDB_OPT_NOSHORT,
    "Ignore columns present in backup but not in database",
    (uchar**) &ga_exclude_missing_columns,
    (uchar**) &ga_exclude_missing_columns, 0,
    GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0 },
  { "exclude-missing-tables", NDB_OPT_NOSHORT,
    "Ignore tables present in backup but not in database",
    (uchar**) &ga_exclude_missing_tables,
    (uchar**) &ga_exclude_missing_tables, 0,
    GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0 },
  { "exclude-intermediate-sql-tables", NDB_OPT_NOSHORT,
    "Do not restore intermediate tables with #sql-prefixed names",
    (uchar**) &opt_exclude_intermediate_sql_tables,
    (uchar**) &opt_exclude_intermediate_sql_tables, 0,
    GET_BOOL, NO_ARG, 1, 0, 0, 0, 0, 0 },
  { "disable-indexes", NDB_OPT_NOSHORT,
    "Disable indexes and foreign keys",
    (uchar**) &ga_disable_indexes,
    (uchar**) &ga_disable_indexes, 0,
    GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0 },
  { "rebuild-indexes", NDB_OPT_NOSHORT,
    "Rebuild indexes",
    (uchar**) &ga_rebuild_indexes,
    (uchar**) &ga_rebuild_indexes, 0,
    GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0 },
  { "skip-unknown-objects", 256, "Skip unknown object when parsing backup",
    (uchar**) &ga_skip_unknown_objects, (uchar**) &ga_skip_unknown_objects, 0,
    GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0 },
  { "skip-broken-objects", 256, "Skip broken object when parsing backup",
    (uchar**) &ga_skip_broken_objects, (uchar**) &ga_skip_broken_objects, 0,
    GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0 },
  { "allow-pk-changes", NDB_OPT_NOSHORT,
    "Allow changes to the set of columns making up a table's primary key.",
    (uchar**) &ga_allow_pk_changes, (uchar**) &ga_allow_pk_changes, 0,
    GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0 },
  { "remap-column", OPT_REMAP_COLUMN, "Remap content for column while "
    "restoring, format <database>.<table>.<column>:<function>:<function_args>."
    "  <database> is remapped name, remapping applied before other conversions.",
    (uchar**) &opt_one_remap_col_arg,
    (uchar**) &opt_one_remap_col_arg, 0,
    GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

#ifdef ERROR_INSERT
  { "error-insert", OPT_ERROR_INSERT,
    "Insert errors (testing option)",
    (uchar **)&_error_insert, (uchar **)&_error_insert, 0,
    GET_INT, REQUIRED_ARG, 0, 0, 0, 0, 0, 0 },
#endif
  { "num_slices", NDB_OPT_NOSHORT,
    "How many slices are being applied",
    (uchar**) &ga_num_slices,
    (uchar**) &ga_num_slices,
    0,
    GET_UINT,
    REQUIRED_ARG,
    1, /* default */
    1, /* min */
    1024, /* max */
    0,
    0,
    0 },
  { "slice_id", NDB_OPT_NOSHORT,
    "My slice id",
    (uchar**) &ga_slice_id,
    (uchar**) &ga_slice_id,
    0,
    GET_INT,
    REQUIRED_ARG,
    0, /* default */
    0, /* min */
    1023, /* max */
    0,
    0,
    0 },
  { "ignore-extended-pk-updates", NDB_OPT_NOSHORT,
    "Ignore log entries containing updates to columns now included in an "
    "extended primary key.",
    (uchar**) &ga_ignore_extended_pk_updates,
    (uchar**) &ga_ignore_extended_pk_updates,
    0,
    GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0 },
  { 0, 0, 0, 0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0}
};


static char* analyse_one_map(char *map_str, uint16 *source, uint16 *dest)
{
  char *end_ptr;
  int number;
  DBUG_ENTER("analyse_one_map");
  /*
    Search for pattern ( source_ng , dest_ng )
  */

  while (isspace(*map_str)) map_str++;

  if (*map_str != '(')
  {
    DBUG_RETURN(NULL);
  }
  map_str++;

  while (isspace(*map_str)) map_str++;

  number= strtol(map_str, &end_ptr, 10);
  if (!end_ptr || number < 0 || number >= MAX_NODE_GROUP_MAPS)
  {
    DBUG_RETURN(NULL);
  }
  *source= (uint16)number;
  map_str= end_ptr;

  while (isspace(*map_str)) map_str++;

  if (*map_str != ',')
  {
    DBUG_RETURN(NULL);
  }
  map_str++;

  number= strtol(map_str, &end_ptr, 10);
  if (!end_ptr || number < 0 || number >= NDB_UNDEF_NODEGROUP)
  {
    DBUG_RETURN(NULL);
  }
  *dest= (uint16)number;
  map_str= end_ptr;

  if (*map_str != ')')
  {
    DBUG_RETURN(NULL);
  }
  map_str++;

  while (isspace(*map_str)) map_str++;
  DBUG_RETURN(map_str);
}

static bool insert_ng_map(NODE_GROUP_MAP *ng_map,
                          uint16 source_ng, uint16 dest_ng)
{
  uint index= source_ng;
  uint ng_index= ng_map[index].no_maps;

  opt_nodegroup_map_len++;
  if (ng_index >= MAX_MAPS_PER_NODE_GROUP)
    return true;
  ng_map[index].no_maps++;
  ng_map[index].map_array[ng_index]= dest_ng;
  return false;
}

static void init_nodegroup_map()
{
  uint i,j;
  NODE_GROUP_MAP *ng_map = &opt_nodegroup_map[0];

  for (i = 0; i < MAX_NODE_GROUP_MAPS; i++)
  {
    ng_map[i].no_maps= 0;
    for (j= 0; j < MAX_MAPS_PER_NODE_GROUP; j++)
      ng_map[i].map_array[j]= NDB_UNDEF_NODEGROUP;
  }
}

static bool analyse_nodegroup_map(const char *ng_map_str,
                                  NODE_GROUP_MAP *ng_map)
{
  uint16 source_ng, dest_ng;
  char *local_str= (char*)ng_map_str;
  DBUG_ENTER("analyse_nodegroup_map");

  do
  {
    if (!local_str)
    {
      DBUG_RETURN(TRUE);
    }
    local_str= analyse_one_map(local_str, &source_ng, &dest_ng);
    if (!local_str)
    {
      DBUG_RETURN(TRUE);
    }
    if (insert_ng_map(ng_map, source_ng, dest_ng))
    {
      DBUG_RETURN(TRUE);
    }
    if (!(*local_str))
      break;
  } while (TRUE);
  DBUG_RETURN(FALSE);
}

static bool parse_remap_option(const BaseString option,
                               BaseString& db_name,
                               BaseString& tab_name,
                               BaseString& col_name,
                               BaseString& func_name,
                               BaseString& func_args,
                               BaseString& error_msg)
{
  const char* expectedFormat = "<db>.<table>.<column>:function[:args]";

  Vector<BaseString> optParts;
  const int numOptParts = option.split(optParts,
                                       BaseString(":"),
                                       3);

  if (numOptParts < 2)
  {
    error_msg.assfmt("remap-column : Badly formed option : %s.  "
                     "Expected format : %s.",
                     option.c_str(),
                     expectedFormat);
    return false;
  }

  Vector<BaseString> nameParts;
  const int numNameParts = optParts[0].split(nameParts,
                                             BaseString("."));
  if (numNameParts != 3)
  {
    error_msg.assfmt("remap-column : Badly formed column specifier : %s "
                     "in option %s.  "
                     "Expected format : %s.",
                     optParts[0].c_str(),
                     option.c_str(),
                     expectedFormat);
    return false;
  }

  /* Copy out substrings */
  db_name.assign(nameParts[0]);
  tab_name.assign(nameParts[1]);
  col_name.assign(nameParts[2]);
  func_name.assign(optParts[1]);

  if (numOptParts == 3)
  {
    func_args.assign(optParts[2]);
  }
  else
  {
    func_args.assign("");
  }

  return true;
};


static bool parse_remap_column(const char* argument)
{
  BaseString option(argument);
  BaseString db, tab, col, func, args, error_msg;

  if (!parse_remap_option(option,
                          db,
                          tab,
                          col,
                          func,
                          args,
                          error_msg))
  {
    info << error_msg << endl;
    return false;
  }

  /* Store this remapping + arguments against the db+table name */
  ExtraTableInfo* eti = g_extra_restore_info.findOrAddTable(db.c_str(),
                                                            tab.c_str());

  /* We store the whole argument string to assist error reporting later */
  eti->m_remapColumnArgs.push_back(option);

  return true;
}

static void short_usage_sub(void)
{
  ndb_short_usage_sub("[<path to backup files>]");
}
static void usage()
{
  ndb_usage(short_usage_sub, load_default_groups, my_long_options);
}

static my_bool
get_one_option(int optid, const struct my_option *opt MY_ATTRIBUTE((unused)),
	       char *argument)
{
#ifndef NDEBUG
  opt_debug= "d:t:O,/tmp/ndb_restore.trace";
#endif
  ndb_std_get_one_option(optid, opt, argument);
  switch (optid) {
  case OPT_VERBOSE:
    info.setThreshold(255-opt_verbose);
    break;
  case 'n':
    if (ga_nodeId == 0)
    {
      err << "Error in --nodeid,-n setting, see --help";
      exit(NdbRestoreStatus::WrongArgs);
    }
    info.setLevel(254);
    info << "Nodeid = " << ga_nodeId << endl;
    break;
  case 'b':
    if (ga_backupId == 0)
    {
      err << "Error in --backupid,-b setting, see --help";
      exit(NdbRestoreStatus::WrongArgs);
    }
    info.setLevel(254);
    info << "Backup Id = " << ga_backupId << endl;
    break;
  case OPT_NDB_NODEGROUP_MAP:
    /*
      This option is used to set a map from nodegroup in original cluster
      to nodegroup in new cluster.
    */
    opt_nodegroup_map_len= 0;

    info.setLevel(254);
    info << "Analyse node group map" << endl;
    if (analyse_nodegroup_map(opt_nodegroup_map_str,
                              &opt_nodegroup_map[0]))
    {
      exit(NdbRestoreStatus::WrongArgs);
    }
    break;
  case OPT_INCLUDE_DATABASES:
  case OPT_EXCLUDE_DATABASES:
  case OPT_INCLUDE_TABLES:
  case OPT_EXCLUDE_TABLES:
    save_include_exclude(optid, argument);
    break;
  case OPT_REWRITE_DATABASE:
    parse_rewrite_database(argument);
    break;
  case OPT_REMAP_COLUMN:
    return (parse_remap_column(argument) ? 0 : 1);
  }
  return 0;
}

static const char* SCHEMA_NAME="/def/";
static const int SCHEMA_NAME_SIZE= 5;

int
makeInternalTableName(const BaseString &externalName, 
                      BaseString& internalName)
{
  // Make dbname.table1 into dbname/def/table1
  Vector<BaseString> parts;

  // Must contain a dot
  if (externalName.indexOf('.') == -1)
    return -1;
  externalName.split(parts,".");
  // .. and only 1 dot
  if (parts.size() != 2)
    return -1;
  internalName.clear();
  internalName.append(parts[0]); // db name
  internalName.append(SCHEMA_NAME); // /def/
  internalName.append(parts[1]); // table name
  return 0;
}

void
processTableList(const char* str, Vector<BaseString> &lst)
{
  // Process tables list like db1.t1,db2.t1 and exits when
  // it finds problems.
  Vector<BaseString> tmp;
  unsigned int i;
  /* Split passed string on comma into 2 BaseStrings in the vector */
  BaseString(str).split(tmp,",");
  for (i=0; i < tmp.size(); i++)
  {
    BaseString internalName;
    if (makeInternalTableName(tmp[i], internalName))
    {
      info << "`" << tmp[i] << "` is not a valid tablename!" << endl;
      exit(NdbRestoreStatus::WrongArgs);
    }
    lst.push_back(internalName);
  }
}

BaseString
makeExternalTableName(const BaseString &internalName)
{
   // Make dbname/def/table1 into dbname.table1
  BaseString externalName;
  
  ssize_t idx = internalName.indexOf('/');
  externalName = internalName.substr(0,idx);
  externalName.append(".");
  externalName.append(internalName.substr(idx + SCHEMA_NAME_SIZE,
                                          internalName.length()));
  return externalName;
}

#include "../../../../sql/ndb_dist_priv_util.h"

// Exclude privilege tables unless explicitely included
void
exclude_privilege_tables()
{
  const char* table_name;
  Ndb_dist_priv_util dist_priv;
  while((table_name= dist_priv.iter_next_table()))
  {
    BaseString priv_tab;
    priv_tab.assfmt("%s.%s", dist_priv.database(), table_name);
    g_exclude_tables.push_back(priv_tab);
    save_include_exclude(OPT_EXCLUDE_TABLES, (char *)priv_tab.c_str());
  }
}


bool
readArguments(int *pargc, char*** pargv) 
{
  Uint32 i;
  BaseString tmp;
  debug << "Load defaults" << endl;
  const char *load_default_groups[]= { "mysql_cluster","ndb_restore",0 };

  init_nodegroup_map();
  ndb_load_defaults(NULL,load_default_groups,pargc,pargv);
  debug << "handle_options" << endl;

  ndb_opt_set_usage_funcs(short_usage_sub, usage);

  if (handle_options(pargc, pargv, my_long_options, get_one_option))
  {
    exit(NdbRestoreStatus::WrongArgs);
  }
  if (ga_nodeId == 0)
  {
    err << "Backup file node ID not specified, please provide --nodeid" << endl;
    exit(NdbRestoreStatus::WrongArgs);
  }
  if (ga_backupId == 0)
  {
    err << "Backup ID not specified, please provide --backupid" << endl;
    exit(NdbRestoreStatus::WrongArgs);
  }


  for (i = 0; i < MAX_NODE_GROUP_MAPS; i++)
    opt_nodegroup_map[i].curr_index = 0;

#if 0
  /*
    Test code written t{
o verify nodegroup mapping
  */
  printf("Handled options successfully\n");
  Uint16 map_ng[16];
  Uint32 j;
  for (j = 0; j < 4; j++)
  {
  for (i = 0; i < 4 ; i++)
    map_ng[i] = i;
  map_nodegroups(&map_ng[0], (Uint32)4);
  for (i = 0; i < 4 ; i++)
    printf("NG %u mapped to %u \n", i, map_ng[i]);
  }
  for (j = 0; j < 4; j++)
  {
  for (i = 0; i < 8 ; i++)
    map_ng[i] = i >> 1;
  map_nodegroups(&map_ng[0], (Uint32)8);
  for (i = 0; i < 8 ; i++)
    printf("NG %u mapped to %u \n", i >> 1, map_ng[i]);
  }
  exit(NdbRestoreStatus::WrongArgs);
#endif

  /* Slices */
  if (ga_num_slices < 1)
  {
    printf("Too few slices\n");
    exit(NdbRestoreStatus::WrongArgs);
  }
  if ((ga_slice_id < 0) ||
      (ga_slice_id >= ga_num_slices))
  {
    printf("Slice id %d out of range (0-%d)\n",
           ga_slice_id,
           ga_num_slices);
    exit(NdbRestoreStatus::WrongArgs);
  }
  else
  {
    if (ga_num_slices > 1)
    {
      printf("ndb_restore slice %d/%d\n",
             ga_slice_id,
             ga_num_slices);
    }
  }

  g_printer = new BackupPrinter(opt_nodegroup_map,
                                opt_nodegroup_map_len);
  if (g_printer == NULL)
    return false;

  char restore_id[20];
  BaseString::snprintf(restore_id, sizeof(restore_id),
                       "%d-%d", ga_nodeId, ga_slice_id);

  BackupRestore* restore = new BackupRestore(opt_ndb_connectstring,
                                             opt_ndb_nodeid,
                                             opt_nodegroup_map,
                                             opt_nodegroup_map_len,
                                             restore_id,
                                             ga_nParallelism,
                                             opt_connect_retry_delay,
                                             opt_connect_retries);
  if (restore == NULL) 
  {
    delete g_printer;
    g_printer = NULL;
    return false;
  }

  if (_print) 
  {
    ga_print = true;
    ga_restore = true;
    g_printer->m_print = true;
  } 
  if (_print_meta) 
  {
    ga_print = true;
    g_printer->m_print_meta = true;
  }
  if (_print_data) 
  {
    ga_print = true;
    g_printer->m_print_data = true;
  }
  if (_print_log) 
  {
    ga_print = true;
    g_printer->m_print_log = true;
  }
  if (_print_sql_log)
    {
      ga_print = true;
      g_printer->m_print_sql_log = true;
    }

  if (_restore_data)
  {
    ga_restore = true;
    restore->m_restore = true; 
  }

  if (_restore_meta)
  {
    //    ga_restore = true;
    restore->m_restore_meta = true;
    if(ga_exclude_missing_tables)
    {
      //conflict in options
      err << "Conflicting arguments found : "
          << "Cannot use `restore-meta` and "
          << "`exclude-missing-tables` together. Exiting..." << endl;
      return false;
    }
  }

  if (_no_restore_disk)
  {
    restore->m_no_restore_disk = true;
  }
  
  if (ga_no_upgrade)
  {
     restore->m_no_upgrade = true;
  }

  if (_preserve_trailing_spaces)
  {
     restore->m_preserve_trailing_spaces = true;
  }

  if (ga_restore_epoch)
  {
    restore->m_restore_epoch = true;
  }

  if (ga_disable_indexes)
  {
    restore->m_disable_indexes = true;
  }

  if (ga_rebuild_indexes)
  {
    restore->m_rebuild_indexes = true;
  }

  {
    BackupConsumer * c = g_printer;
    g_consumers.push_back(c);
  }
  {
    BackupConsumer * c = restore;
    g_consumers.push_back(c);
  }
  for (;;)
  {
    int i= 0;
    if (ga_backupPath == default_backupPath)
    {
      // Set backup file path
      if ((*pargv)[i] == NULL)
        break;
      ga_backupPath = (*pargv)[i++];
    }
    if ((*pargv)[i] == NULL)
      break;
    g_databases.push_back((*pargv)[i++]);
    while ((*pargv)[i] != NULL)
    {
      g_tables.push_back((*pargv)[i++]);
    }
    break;
  }
  info.setLevel(254);
  info << "backup path = " << ga_backupPath << endl;
  if (g_databases.size() > 0)
  {
    info << "WARNING! Using deprecated syntax for selective object restoration." << endl;
    info << "Please use --include-*/--exclude-* options in future." << endl;
    info << "Restoring only from database " << g_databases[0].c_str() << endl;
    if (g_tables.size() > 0)
    {
        info << "Restoring tables:";
    }
    for (unsigned i= 0; i < g_tables.size(); i++)
    {
      info << " " << g_tables[i].c_str();
    }
    if (g_tables.size() > 0)
      info << endl;
  }

  if (ga_restore)
  {
    // Exclude privilege tables unless explicitely included
    if (!opt_restore_privilege_tables)
      exclude_privilege_tables();

    // Move over old style arguments to include/exclude lists
    if (g_databases.size() > 0)
    {
      BaseString tab_prefix, tab;
      tab_prefix.append(g_databases[0].c_str());
      tab_prefix.append(".");
      if (g_tables.size() == 0)
      {
        g_include_databases.push_back(g_databases[0]);
        save_include_exclude(OPT_INCLUDE_DATABASES,
                             (char *)g_databases[0].c_str());
      }
      for (unsigned i= 0; i < g_tables.size(); i++)
      {
        tab.assign(tab_prefix);
        tab.append(g_tables[i]);
        g_include_tables.push_back(tab);
        save_include_exclude(OPT_INCLUDE_TABLES, (char *)tab.c_str());
      }
    }
  }

  if (opt_include_databases)
  {
    tmp = BaseString(opt_include_databases);
    tmp.split(g_include_databases,",");
    info << "Including Databases: ";
    for (i= 0; i < g_include_databases.size(); i++)
    {
      info << g_include_databases[i] << " ";
    }
    info << endl;
  }
  
  if (opt_exclude_databases)
  {
    tmp = BaseString(opt_exclude_databases);
    tmp.split(g_exclude_databases,",");
    info << "Excluding databases: ";
    for (i= 0; i < g_exclude_databases.size(); i++)
    {
      info << g_exclude_databases[i] << " ";
    }
    info << endl;
  }
  
  if (opt_rewrite_database)
  {
    info << "Rewriting databases:";
    Properties::Iterator it(&g_rewrite_databases);
    const char * src;
    for (src = it.first(); src != NULL; src = it.next()) {
      const char * dst = NULL;
      bool r = g_rewrite_databases.get(src, &dst);
      require(r && (dst != NULL));
      info << " (" << src << "->" << dst << ")";
    }
    info << endl;
  }

  if (opt_include_tables)
  {
    processTableList(opt_include_tables, g_include_tables);
    info << "Including tables: ";
    for (i= 0; i < g_include_tables.size(); i++)
    {
      info << makeExternalTableName(g_include_tables[i]).c_str() << " ";
    }
    info << endl;
  }
  
  if (opt_exclude_tables)
  {
    processTableList(opt_exclude_tables, g_exclude_tables);
    info << "Excluding tables: ";
    for (i= 0; i < g_exclude_tables.size(); i++)
    {
      info << makeExternalTableName(g_exclude_tables[i]).c_str() << " ";
    }
    info << endl;
  }

  /*
    the below formatting follows the formatting from mysqldump
    do not change unless to adopt to changes in mysqldump
  */
  g_ndbrecord_print_format.fields_enclosed_by=
    opt_fields_enclosed_by ? opt_fields_enclosed_by : "";
  g_ndbrecord_print_format.fields_terminated_by=
    opt_fields_terminated_by ? opt_fields_terminated_by : "\t";
  g_ndbrecord_print_format.fields_optionally_enclosed_by=
    opt_fields_optionally_enclosed_by ? opt_fields_optionally_enclosed_by : "";
  g_ndbrecord_print_format.lines_terminated_by=
    opt_lines_terminated_by ? opt_lines_terminated_by : "\n";
  if (g_ndbrecord_print_format.fields_optionally_enclosed_by[0] == '\0')
    g_ndbrecord_print_format.null_string= "\\N";
  else
    g_ndbrecord_print_format.null_string= "";
  g_ndbrecord_print_format.hex_prefix= "";
  g_ndbrecord_print_format.hex_format= opt_hex_format;

  if (ga_skip_table_check)
  {
    g_tableCompabilityMask = ~(Uint32)0;
    ga_skip_unknown_objects = true;
  }

  if (ga_promote_attributes)
  {
    g_tableCompabilityMask |= TCM_ATTRIBUTE_PROMOTION;
  }

  if (ga_demote_attributes)
  {
    g_tableCompabilityMask |= TCM_ATTRIBUTE_DEMOTION;
  }

  if (ga_exclude_missing_columns)
  {
    g_tableCompabilityMask |= TCM_EXCLUDE_MISSING_COLUMNS;
  }

  if (ga_allow_pk_changes)
  {
    g_tableCompabilityMask |= TCM_ALLOW_PK_CHANGES;
  }
  if(ga_ignore_extended_pk_updates)
  {
    g_tableCompabilityMask |= TCM_IGNORE_EXTENDED_PK_UPDATES;
  }

  return true;
}

void
clearConsumers()
{
  for(Uint32 i= 0; i<g_consumers.size(); i++)
    delete g_consumers[i];
  g_consumers.clear();
}

static inline bool
checkSysTable(const TableS* table)
{
  return ! table->getSysTable();
}

static inline bool
checkSysTable(const RestoreMetaData& metaData, uint i)
{
  assert(i < metaData.getNoOfTables());
  return checkSysTable(metaData[i]);
}

static inline bool
isBlobTable(const TableS* table)
{
  return table->getMainTable() != NULL;
}

static inline bool
isIndex(const TableS* table)
{
  const NdbTableImpl & tmptab = NdbTableImpl::getImpl(* table->m_dictTable);
  return (int) tmptab.m_indexType != (int) NdbDictionary::Index::Undefined;
}

static inline bool
isSYSTAB_0(const TableS* table)
{
  return table->isSYSTAB_0();
}

const char*
getTableName(const TableS* table)
{
  const char *table_name;
  if (isBlobTable(table))
    table_name= table->getMainTable()->getTableName();
  else if (isIndex(table))
    table_name=
      NdbTableImpl::getImpl(*table->m_dictTable).m_primaryTable.c_str();
  else
    table_name= table->getTableName();
    
  return table_name;
}

static void parse_rewrite_database(char * argument)
{
  const BaseString arg(argument);
  Vector<BaseString> args;
  unsigned int n = arg.split(args, ",");
  if ((n == 2)
      && (args[0].length() > 0)
      && (args[1].length() > 0)) {
    const BaseString src = args[0];
    const BaseString dst = args[1];
    const bool replace = true;
    bool r = g_rewrite_databases.put(src.c_str(), dst.c_str(), replace);
    require(r);
    return; // ok
  }

  info << "argument `" << arg.c_str()
       << "` is not a pair 'a,b' of non-empty names." << endl;
  exit(NdbRestoreStatus::WrongArgs);
}

static void save_include_exclude(int optid, char * argument)
{
  BaseString arg = argument;
  Vector<BaseString> args;
  arg.split(args, ",");
  for (uint i = 0; i < args.size(); i++)
  {
    RestoreOption * option = new RestoreOption();
    BaseString arg;
    
    option->optid = optid;
    switch (optid) {
    case OPT_INCLUDE_TABLES:
    case OPT_EXCLUDE_TABLES:
      if (makeInternalTableName(args[i], arg))
      {
        info << "`" << args[i] << "` is not a valid tablename!" << endl;
        exit(NdbRestoreStatus::WrongArgs);
      }
      break;
    default:
      arg = args[i];
      break;
    }
    option->argument = arg;
    g_include_exclude.push_back(option);
  }
}
static bool check_include_exclude(BaseString database, BaseString table)
{
  const char * db = database.c_str();
  const char * tbl = table.c_str();
  bool do_include = true;

  if (g_include_databases.size() != 0 ||
      g_include_tables.size() != 0)
  {
    /*
      User has explicitly specified what databases
      and/or tables should be restored. If no match is
      found then DON'T restore table.
     */
    do_include = false;
  }
  if (do_include &&
      (g_exclude_databases.size() != 0 ||
       g_exclude_tables.size() != 0))
  {
    /*
      User has not explicitly specified what databases
      and/or tables should be restored.
      User has explicitly specified what databases
      and/or tables should NOT be restored. If no match is
      found then DO restore table.
     */
    do_include = true;
  }

  if (g_include_exclude.size() != 0)
  {
    /*
      Scan include exclude arguments in reverse.
      First matching include causes table to be restored.
      first matching exclude causes table NOT to be restored.      
     */
    for(uint i = g_include_exclude.size(); i > 0; i--)
    {
      RestoreOption *option = g_include_exclude[i-1];
      switch (option->optid) {
      case OPT_INCLUDE_TABLES:
        if (strcmp(tbl, option->argument.c_str()) == 0)
          return true; // do include
        break;
      case OPT_EXCLUDE_TABLES:
        if (strcmp(tbl, option->argument.c_str()) == 0)
          return false; // don't include
        break;
      case OPT_INCLUDE_DATABASES:
        if (strcmp(db, option->argument.c_str()) == 0)
          return true; // do include
        break;
      case OPT_EXCLUDE_DATABASES:
        if (strcmp(db, option->argument.c_str()) == 0)
          return false; // don't include
        break;
      default:
        continue;
      }
    }
  }

  return do_include;
}

static bool
check_intermediate_sql_table(const char *table_name)
{
  BaseString tbl(table_name);
  Vector<BaseString> fields;
  tbl.split(fields, "/");
  if((fields.size() == 3) && !fields[2].empty() && strncmp(fields[2].c_str(), TMP_TABLE_PREFIX, TMP_TABLE_PREFIX_LEN) == 0) 
    return true;  
  return false;
}
  
static inline bool
checkDoRestore(const TableS* table)
{
  bool ret = true;
  BaseString db, tbl;
  
  tbl.assign(getTableName(table));
  ssize_t idx = tbl.indexOf('/');
  db = tbl.substr(0, idx);
  
  /*
    Include/exclude flags are evaluated right
    to left, and first match overrides any other
    matches. Non-overlapping arguments are accumulative.
    If no include flags are specified this means all databases/tables
    except any excluded are restored.
    If include flags are specified than only those databases
    or tables specified are restored.
   */
  ret = check_include_exclude(db, tbl);
  return ret;
}

static inline bool
checkDbAndTableName(const TableS* table)
{
  if (table->isBroken())
    return false;

  const char *table_name = getTableName(table);
  if(opt_exclude_intermediate_sql_tables && (check_intermediate_sql_table(table_name) == true)) {
    return false;
  }

  // If new options are given, ignore the old format
  if (opt_include_tables || g_exclude_tables.size() > 0 ||
      opt_include_databases || opt_exclude_databases ) {
    return (checkDoRestore(table));
  }
  
  if (g_tables.size() == 0 && g_databases.size() == 0)
    return true;

  if (g_databases.size() == 0)
    g_databases.push_back("TEST_DB");

  // Filter on the main table name for indexes and blobs
  unsigned i;
  for (i= 0; i < g_databases.size(); i++)
  {
    if (strncmp(table_name, g_databases[i].c_str(),
                g_databases[i].length()) == 0 &&
        table_name[g_databases[i].length()] == '/')
    {
      // we have a match
      if (g_databases.size() > 1 || g_tables.size() == 0)
        return true;
      break;
    }
  }
  if (i == g_databases.size())
    return false; // no match found

  while (*table_name != '/') table_name++;
  table_name++;
  while (*table_name != '/') table_name++;
  table_name++;

  // Check if table should be restored
  for (i= 0; i < g_tables.size(); i++)
  {
    if (strcmp(table_name, g_tables[i].c_str()) == 0)
      return true;
  }
  return false;
}

static void
exclude_missing_tables(const RestoreMetaData& metaData)
{
  Uint32 i, j;
  bool isMissing;
  Vector<BaseString> missingTables;
  for(i = 0; i < metaData.getNoOfTables(); i++)
  {
    const TableS *table= metaData[i];
    isMissing = false;
    for(j = 0; j < g_consumers.size(); j++)
      isMissing |= g_consumers[j]->isMissingTable(*table);
    if( isMissing )
    {
      /* add missing tables to exclude list */
      g_exclude_tables.push_back(table->getTableName());
      BaseString tableName = makeExternalTableName(table->getTableName());
      save_include_exclude(OPT_EXCLUDE_TABLES, (char*)tableName.c_str());
      missingTables.push_back(tableName);
    }
  }

  if(missingTables.size() > 0){
    info << "Excluded Missing tables: ";
    for (i=0 ; i < missingTables.size(); i++)
    {
      info << missingTables[i] << " ";
    }
    info << endl;
  }
}

static TableS*
find_table_spec(RestoreMetaData& metaData,
                const char* searchDbName,
                const char* searchTableName,
                bool rewrite_backup_db)
{
  for(Uint32 m = 0; m < metaData.getNoOfTables(); m++)
  {
    TableS *tableSpec= metaData[m];

    BaseString externalName = makeExternalTableName(tableSpec->getTableName());
    BaseString dbName, tabName;
    {
      Vector<BaseString> components;
      if (externalName.split(components,
                             BaseString(".")) != 2)
      {
        info << "Error processing table name from backup " << externalName
             << " from " << tableSpec->getTableName() << endl;
        return NULL;
      }
      dbName = components[0];
      tabName = components[1];

      if (rewrite_backup_db)
      {
        /* Check for rewrite db, as args are specified wrt new db names */
        const char* rewrite_dbname;
        if (g_rewrite_databases.get(dbName.c_str(), &rewrite_dbname))
        {
          dbName.assign(rewrite_dbname);
        }
      }
    }

    if (dbName == searchDbName &&
        tabName == searchTableName)
    {
      return tableSpec;
    }
  }

  return NULL;
}

class OffsetTransform : public ColumnTransform
{
public:
  static
  OffsetTransform* parse(const NdbDictionary::Column* col,
                         const BaseString& func_name,
                         const BaseString& func_args,
                         BaseString& error_msg)
  {
    bool sig = true;
    Uint64 bits = 0;
    switch (col->getType())
    {
    case NdbDictionary::Column::Bigint:
      sig = true;
      bits = 64;
      break;
    case NdbDictionary::Column::Bigunsigned:
      sig = false;
      bits = 64;
      break;
    case NdbDictionary::Column::Int:
      sig = true;
      bits = 32;
      break;
    case NdbDictionary::Column::Unsigned:
      sig = false;
      bits = 32;
      break;
    default:
      error_msg.assfmt("Column does not have supported integer type");
      return NULL;
    }

    /* Todo : Use ndb type traits */
    const Uint64 shift = bits - 1;
    const Uint64 max_uval = ((Uint64(1) << shift) -1) | (Uint64(1) << shift);
    const Int64 min_sval = 0 - (Uint64(1) << shift);
    const Int64 max_sval = (Uint64(1) << shift) - 1;

    Int64 offset_val;

    int cnt = sscanf(func_args.c_str(), "%lld", &offset_val);
    if (cnt != 1)
    {
      error_msg.assfmt("offset argument invalid");
      return NULL;
    }

    {
      /* Re-convert back to check for silent-saturation in sscanf */
      char numbuf[22];
      BaseString::snprintf(numbuf, sizeof(numbuf),
                           "%lld", offset_val);
      if (strncmp(func_args.c_str(), numbuf, sizeof(numbuf)) != 0)
      {
        error_msg.assfmt("Offset %s unreadable - out of range for type?",
                         func_args.c_str());
        return NULL;
      }
    }

    if (offset_val < min_sval ||
        offset_val > max_sval)
    {
      error_msg.assfmt("Offset %lld is out of range for type.",
                       offset_val);
      return NULL;
    }

    return new OffsetTransform(offset_val,
                               sig,
                               bits,
                               min_sval,
                               max_sval,
                               max_uval);
  };

private:
  Int64 m_offset_val;
  Int64 m_sig_bound;
  Uint64 m_unsig_bound;
  bool m_offset_positive;
  bool m_sig;
  Uint32 m_bits;

  OffsetTransform(Int64 offset_val,
                  bool sig,
                  Uint32 bits,
                  Int64 min_sval,
                  Int64 max_sval,
                  Uint64 max_uval):
    m_offset_val(offset_val),
    m_sig(sig),
    m_bits(bits)
  {
    m_offset_positive = offset_val >= 0;
    if (sig)
    {
      if (m_offset_positive)
      {
        m_sig_bound = max_sval - offset_val;
      }
      else
      {
        m_sig_bound = min_sval - offset_val; // - - = +
      }
    }
    else
    {
      if (m_offset_positive)
      {
        m_unsig_bound = max_uval - offset_val;
      }
      else
      {
        m_unsig_bound = (0 - offset_val);
      }
    }
  };

  ~OffsetTransform() {};

  static Uint64 readIntoU64(const void* src, Uint32 bits)
  {
    switch(bits)
    {
    case 64:
      Uint64 dst;
      memcpy(&dst, src, 8);
      return dst;
    case 32:
    {
      Uint32 u32;
      memcpy(&u32, src, 4);
      return u32;
    }
    default:
      abort();
    }
    return 0;
  }

  static void writeFromU64(Uint64 src, void* dst, Uint32 bits)
  {
    switch(bits)
    {
    case 64:
      memcpy(dst, &src, 8);
      return;
    case 32:
    {
      Uint32 u32 = (Uint32) src;
      memcpy(dst, &u32, 4);
      return;
    }
    default:
      abort();
    }
  }

  static Int64 readIntoS64(const void* src, Uint32 bits)
  {
    switch(bits)
    {
    case 64:
      Int64 dst;
      memcpy(&dst, src, 8);
      return dst;
    case 32:
    {
      Int32 i32;
      memcpy(&i32, src, 4);
      return i32;
    }
    default:
      abort();
    }
    return 0;
  }

  static void writeFromS64(Int64 src, void* dst, Uint32 bits)
  {
    switch(bits)
    {
    case 64:
      memcpy(dst, &src, 8);
      return;
    case 32:
    {
      Int32 i32 = (Int32) src;
      memcpy(dst, &i32, 4);
      return;
    }
    default:
      abort();
    }
  }
      
  virtual bool apply(const NdbDictionary::Column* col,
                     const void* src_data,
                     void** dst_data)
  {
    if (src_data == NULL)
    {
      /* Offset(NULL, *) -> NULL */
      *dst_data = NULL;
      return true;
    }

    if (m_sig)
    {
      Int64 src_val = readIntoS64(src_data, m_bits);

      bool src_in_bounds = true;
      if (m_offset_positive)
      {
        src_in_bounds = (src_val <= m_sig_bound);
      }
      else
      {
        src_in_bounds = (src_val >= m_sig_bound);
      }

      if (unlikely(!src_in_bounds))
      {
        fprintf(stderr, "Offset : Source value out of bounds : adding %lld to %lld "
                "gives an out of bounds value\n",
                m_offset_val,
                src_val);

        return false;
      }

      src_val += m_offset_val;

      writeFromS64(src_val, *dst_data, m_bits);
    }
    else
    {
      /* Unsigned */
      Uint64 src_val = readIntoU64(src_data, m_bits);

      bool src_in_bounds = true;
      if (m_offset_positive)
      {
        src_in_bounds = (src_val <= m_unsig_bound);
      }
      else
      {
        src_in_bounds = (src_val >= m_unsig_bound);
      }

      if (unlikely(!src_in_bounds))
      {
        fprintf(stderr, "Offset : Source value out of bounds : adding %lld to %llu "
                "gives an out of bounds value\n",
                m_offset_val,
                src_val);

        return false;
      }

      src_val+= m_offset_val;

      writeFromU64(src_val, *dst_data, m_bits);
    }

    return true;
  }
};


static ColumnTransform*
create_column_transform(const NdbDictionary::Column* col,
                        const BaseString& func_name,
                        const BaseString& func_args,
                        BaseString& error_msg)
{
  if (strcasecmp("offset", func_name.c_str()) == 0)
  {
    return OffsetTransform::parse(col, func_name, func_args, error_msg);
  }
  error_msg.assfmt("Function %s not defined", func_name.c_str());
  return NULL;
}

static bool
setup_one_remapping(TableS* tableSpec,
                    const BaseString& col_name,
                    const BaseString& func_name,
                    const BaseString& func_args,
                    BaseString& error_msg)
{
  const NdbDictionary::Column* col =
    tableSpec->m_dictTable->getColumn(col_name.c_str());

  if (!col)
  {
    error_msg.assfmt("Failed to find column %s in table",
                     col_name.c_str());
    return false;
  }

  AttributeDesc* ad = tableSpec->getAttributeDesc(col->getColumnNo());

  if (ad->transform != NULL)
  {
    error_msg.assfmt("Duplicate remappings on column %s",
                     col_name.c_str());
    return false;
  }

  debug << "Initialising remap function \""
        << func_name.c_str() << ":" << func_args.c_str()
        << "\" on column "
        << tableSpec->m_dictTable->getName()
        << "."
        << col_name.c_str()
        << endl;

  ColumnTransform* ct = create_column_transform(col,
                                                func_name,
                                                func_args,
                                                error_msg);

  if (ct == NULL)
  {
    return false;
  }

  ad->transform = ct;

  return true;
}

static bool
setup_column_remappings(RestoreMetaData& metaData)
{
  for (Uint32 t = 0; t < g_extra_restore_info.m_tables.size(); t++)
  {
    const ExtraTableInfo* eti = g_extra_restore_info.m_tables[t];

    TableS* tableSpec = find_table_spec(metaData,
                                        eti->m_dbName.c_str(),
                                        eti->m_tableName.c_str(),
                                        true); // Rewrite database
    if (tableSpec)
    {
      const Vector<TableS*> blobTables = tableSpec->getBlobTables();
      const bool haveBlobPartTables = blobTables.size() > 0;

      for (Uint32 a=0; a < eti->m_remapColumnArgs.size(); a++)
      {
        BaseString db_name, tab_name, col_name, func_name, func_args, error_msg;
        if (!parse_remap_option(eti->m_remapColumnArgs[a],
                                db_name,
                                tab_name,
                                col_name,
                                func_name,
                                func_args,
                                error_msg))
        {
          /* Should never happen as arg parsed on initial read */
          info << "Unexpected - parse failed : \""
               << eti->m_remapColumnArgs[a].c_str()
               << "\"" << endl;
          return false;
        }

        if (!setup_one_remapping(tableSpec,
                                 col_name,
                                 func_name,
                                 func_args,
                                 error_msg))
        {
          info << "remap_column : Failed with \""
               << error_msg.c_str()
               << "\" while processing option : \""
               << eti->m_remapColumnArgs[a].c_str()
               << "\"" << endl;
          return false;
        }

        const bool col_in_pk =
          tableSpec->m_dictTable->getColumn(col_name.c_str())->getPrimaryKey();

        if (col_in_pk &&
            haveBlobPartTables)
        {
          /* This transform should be added on the Blob part table(s) */
          for (Uint32 b=0; b < blobTables.size(); b++)
          {
            TableS* blobPartTableSpec = blobTables[b];
            const NdbDictionary::Column* mainTabBlobCol =
              tableSpec->m_dictTable->getColumn(blobPartTableSpec->getMainColumnId());

            if (unlikely(mainTabBlobCol->getBlobVersion() == NDB_BLOB_V1))
            {
              info << "remap_column : Failed as table has v1 Blob column "
                   << mainTabBlobCol->getName()
                   << " when processing option "
                   << eti->m_remapColumnArgs[a].c_str()
                   << endl;
              return false;
            }

            if (!setup_one_remapping(blobPartTableSpec,
                                     col_name.c_str(),
                                     func_name,
                                     func_args,
                                     error_msg))
            {
              info << "remap_column : Failed with error "
                   << error_msg
                   << " while applying remapping to blob "
                   << "parts table "
                   << blobPartTableSpec->m_dictTable->getName()
                   << " from option : "
                   <<  eti->m_remapColumnArgs[a].c_str()
                   << endl;
              return false;
            }
          }
        }
      }
    }
    else
    {
      info << "remap_column : Failed to find table in Backup "
           << "matching option : \""
           << eti->m_remapColumnArgs[0].c_str()
           << "\"" << endl;
      return false;
    }
  }

  return true;
}

static void
free_data_callback()
{
  for(Uint32 i= 0; i < g_consumers.size(); i++) 
    g_consumers[i]->tuple_free();
}

static void exitHandler(int code)
{
  if (opt_core)
    abort();
  else
    exit(code);
}

static void init_progress()
{
  g_report_prev = NdbTick_getCurrentTicks();
}

static int check_progress()
{
  if (!opt_progress_frequency)
    return 0;

  const NDB_TICKS now = NdbTick_getCurrentTicks();
  
  if (NdbTick_Elapsed(g_report_prev, now).seconds() >= opt_progress_frequency)
  {
    g_report_prev = now;
    return 1;
  }
  return 0;
}

static void report_progress(const char *prefix, const BackupFile &f)
{
  info.setLevel(255);
  if (f.get_file_size())
    info << prefix << (f.get_file_pos() * 100 + f.get_file_size()-1) / f.get_file_size() 
         << "%(" << f.get_file_pos() << " bytes)\n";
  else
    info << prefix << f.get_file_pos() << " bytes\n";
}

/**
 * Reports, clears information on columns where data truncation was detected.
 */
static void
check_data_truncations(const TableS * table)
{
  assert(table);
  const char * tname = table->getTableName();
  const int n = table->getNoOfAttributes();
  for (int i = 0; i < n; i++) {
    AttributeDesc * desc = table->getAttributeDesc(i);
    if (desc->truncation_detected) {
      const char * cname = desc->m_column->getName();
      info.setLevel(254);
      info << "Data truncation(s) detected for attribute: "
           << tname << "." << cname << endl;
      desc->truncation_detected = false;
    }
  }
}

/**
 * Check whether we should skip this table fragment due to
 * operating in slice mode
 */
static bool
determine_slice_skip_fragment(TableS * table, Uint32 fragmentId, Uint32& fragmentCount)
{
  if (ga_num_slices == 1)
  {
    /* No slicing */
    return false;
  }

  /* Should we restore this fragment? */
  int fragmentRestoreSlice = 0;
  if (table->isBlobRelated())
  {
    /**
     * v2 blobs + staging tables
     * Staging tables need complete blobs restored
     * at end of slice restore
     * That requires that we restore matching main and
     * parts table fragments
     * So we must ensure that we slice deterministically
     * across main and parts tables for Blobs tables.
     * The id of the 'main' table is used to give some
     * offsetting
     */
    const Uint32 mainId = table->getMainTable() ?
      table->getMainTable()->getTableId() :  // Parts table
      table->getTableId();                   // Main table

    fragmentRestoreSlice = (mainId + fragmentId) % ga_num_slices;
  }
  else
  {
    /* For non-Blob tables we use round-robin so
     * that we can balance across a number of slices
     * different to the number of fragments
     */
    fragmentRestoreSlice = fragmentCount ++ % ga_num_slices;
  }

  debug << "Table : " << table->m_dictTable->getName()
        << " blobRelated : " << table->isBlobRelated()
        << " frag id : " << fragmentId
        << " slice id : " << ga_slice_id
        << " fragmentRestoreSlice : " << fragmentRestoreSlice
        << " apply : " << (fragmentRestoreSlice == ga_slice_id)
        << endl;

  /* If it's not for this slice, skip it */
  const bool skip_fragment = (fragmentRestoreSlice != ga_slice_id);

  /* Remember for later lookup */
  table->setSliceSkipFlag(fragmentId, skip_fragment);

  return skip_fragment;
}

static
bool check_slice_skip_fragment(const TableS* table, Uint32 fragmentId)
{
  if (ga_num_slices == 1)
  {
    /* No slicing */
    return false;
  }

  return table->getSliceSkipFlag(fragmentId);
}


int
main(int argc, char** argv)
{
  NDB_INIT(argv[0]);

  debug << "Start readArguments" << endl;
  if (!readArguments(&argc, &argv))
  {
    exitHandler(NdbRestoreStatus::Failed);
  }

  g_options.appfmt(" -b %u", ga_backupId);
  g_options.appfmt(" -n %d", ga_nodeId);
  if (_restore_meta)
    g_options.appfmt(" -m");
  if (ga_no_upgrade)
    g_options.appfmt(" -u");
  if (ga_promote_attributes)
    g_options.appfmt(" -A");
  if (ga_demote_attributes)
    g_options.appfmt(" -L");
  if (_preserve_trailing_spaces)
    g_options.appfmt(" -P");
  if (ga_skip_table_check)
    g_options.appfmt(" -s");
  if (_restore_data)
    g_options.appfmt(" -r");
  if (ga_restore_epoch)
    g_options.appfmt(" -e");
  if (_no_restore_disk)
    g_options.appfmt(" -d");
  if (ga_exclude_missing_columns)
    g_options.append(" --exclude-missing-columns");
  if (ga_exclude_missing_tables)
    g_options.append(" --exclude-missing-tables");
  if (ga_disable_indexes)
    g_options.append(" --disable-indexes");
  if (ga_rebuild_indexes)
    g_options.append(" --rebuild-indexes");
  g_options.appfmt(" -p %d", ga_nParallelism);
  if (ga_skip_unknown_objects)
    g_options.append(" --skip-unknown-objects");
  if (ga_skip_broken_objects)
    g_options.append(" --skip-broken-objects");
  if (ga_num_slices > 1)
  {
    g_options.appfmt(" --num-slices=%u --slice-id=%u",
                     ga_num_slices,
                     ga_slice_id);
  }
  if (ga_allow_pk_changes)
    g_options.append(" --allow-pk-changes");
  if (ga_ignore_extended_pk_updates)
    g_options.append(" --ignore-extended-pk-updates");

  init_progress();

  char timestamp[64];

  /**
   * we must always load meta data, even if we will only print it to stdout
   */

  debug << "Start restoring meta data" << endl;

  RestoreMetaData metaData(ga_backupPath, ga_nodeId, ga_backupId);
#ifdef ERROR_INSERT
  if(_error_insert > 0)
  {
    metaData.error_insert(_error_insert);
  }
#endif 

  Logger::format_timestamp(time(NULL), timestamp, sizeof(timestamp));
  info << timestamp << " [restore_metadata]" << " Read meta data file header" << endl;

  if (!metaData.readHeader())
  {
    err << "Failed to read " << metaData.getFilename() << endl << endl;
    exitHandler(NdbRestoreStatus::Failed);
  }

  const BackupFormat::FileHeader & tmp = metaData.getFileHeader();
  const Uint32 version = tmp.BackupVersion;
  
  char buf[NDB_VERSION_STRING_BUF_SZ];
  info.setLevel(254);
  info << "Backup version in files: " 
       <<  ndbGetVersionString(version, 0, 
                               isDrop6(version) ? "-drop6" : 0, 
                               buf, sizeof(buf));
  if (version >= NDBD_RAW_LCP)
  {
    info << " ndb version: "
         << ndbGetVersionString(tmp.NdbVersion, tmp.MySQLVersion, 0, 
                                buf, sizeof(buf));
  }

  info << endl;

  /**
   * check wheater we can restore the backup (right version).
   */
  // in these versions there was an error in how replica info was
  // stored on disk
  if (version >= MAKE_VERSION(5,1,3) && version <= MAKE_VERSION(5,1,9))
  {
    err << "Restore program incompatible with backup versions between "
        << ndbGetVersionString(MAKE_VERSION(5,1,3), 0, 0, buf, sizeof(buf))
        << " and "
        << ndbGetVersionString(MAKE_VERSION(5,1,9), 0, 0, buf, sizeof(buf))
        << endl;
    exitHandler(NdbRestoreStatus::Failed);
  }

  if (version > NDB_VERSION)
  {
    err << "Restore program older than backup version. Not supported. "
        << "Use new restore program" << endl;
    exitHandler(NdbRestoreStatus::Failed);
  }

  debug << "Load content" << endl;
  Logger::format_timestamp(time(NULL), timestamp, sizeof(timestamp));
  info << timestamp << " [restore_metadata]" << " Load content" << endl;

  int res  = metaData.loadContent();

  info << "Start GCP of Backup: " << metaData.getStartGCP() << endl;
  info << "Stop GCP of Backup: " << metaData.getStopGCP() << endl;
  
  if (res == 0)
  {
    err << "Restore: Failed to load content" << endl;
    exitHandler(NdbRestoreStatus::Failed);
  }
  debug << "Get number of Tables" << endl;
  Logger::format_timestamp(time(NULL), timestamp, sizeof(timestamp));
  info << timestamp << " [restore_metadata]" << " Get number of Tables" << endl;
  if (metaData.getNoOfTables() == 0) 
  {
    err << "The backup contains no tables" << endl;
    exitHandler(NdbRestoreStatus::Failed);
  }

  if(_print_sql_log && _print_log)
  {
    debug << "Check to ensure that both print-sql-log and print-log "
          << "options are not passed" << endl;
    err << "Both print-sql-log and print-log options passed. Exiting..."
        << endl;
    exitHandler(NdbRestoreStatus::Failed);
  }

  if (_print_sql_log)
  {
    debug << "Check for tables with hidden PKs or column of type blob "
          << "when print-sql-log option is passed" << endl;
    for(Uint32 i = 0; i < metaData.getNoOfTables(); i++)
    {
      const TableS *table = metaData[i];
      if (!(checkSysTable(table) && checkDbAndTableName(table)))
        continue;
      /* Blobs are stored as separate tables with names prefixed
       * with NDB$BLOB. This can be used to check if there are
       * any columns of type blob in the tables being restored */
      BaseString tableName(table->getTableName());
      Vector<BaseString> tableNameParts;
      tableName.split(tableNameParts, "/");
      if (tableNameParts[2].substr(0,8) == "NDB$BLOB")
      {
        err << "Found column of type blob with print-sql-log option set. "
            << "Exiting..." << endl;
        exitHandler(NdbRestoreStatus::Failed);
      }
      /* Hidden PKs are stored with the name $PK */
      int noOfPK = table->m_dictTable->getNoOfPrimaryKeys();
      for(int j = 0; j < noOfPK; j++)
      {
        const char* pkName = table->m_dictTable->getPrimaryKey(j);
        if(strcmp(pkName,"$PK") == 0)
        {
          err << "Found hidden primary key with print-sql-log option set. "
              << "Exiting..." << endl;
          exitHandler(NdbRestoreStatus::Failed);
        }
      }
    }
  }

  debug << "Validate Footer" << endl;
  Logger::format_timestamp(time(NULL), timestamp, sizeof(timestamp));
  info << timestamp << " [restore_metadata]" << " Validate Footer" << endl;

  if (!metaData.validateFooter()) 
  {
    err << "Restore: Failed to validate footer." << endl;
    exitHandler(NdbRestoreStatus::Failed);
  }
  debug << "Init Backup objects" << endl;
  Uint32 i;
  for(i= 0; i < g_consumers.size(); i++)
  {
    if (!g_consumers[i]->init(g_tableCompabilityMask))
    {
      clearConsumers();
      err << "Failed to initialize consumers" << endl;
      exitHandler(NdbRestoreStatus::Failed);
    }

  }

  if(ga_exclude_missing_tables)
    exclude_missing_tables(metaData);

 if (!setup_column_remappings(metaData))
  {
    exitHandler(NdbRestoreStatus::Failed);
  }

  /* report to clusterlog if applicable */
  for (i = 0; i < g_consumers.size(); i++)
    g_consumers[i]->report_started(ga_backupId, ga_nodeId);

  debug << "Restore objects (tablespaces, ..)" << endl;
  Logger::format_timestamp(time(NULL), timestamp, sizeof(timestamp));
  info << timestamp << " [restore_metadata]" << " Restore objects (tablespaces, ..)" << endl;
  for(i = 0; i<metaData.getNoOfObjects(); i++)
  {
    for(Uint32 j= 0; j < g_consumers.size(); j++)
      if (!g_consumers[j]->object(metaData.getObjType(i),
				  metaData.getObjPtr(i)))
      {
	err << "Restore: Failed to restore table: ";
        err << metaData[i]->getTableName() << " ... Exiting " << endl;
	exitHandler(NdbRestoreStatus::Failed);
      } 
    if (check_progress())
    {
      info.setLevel(255);
      info << "Object create progress: "
           << i+1 << " objects out of "
           << metaData.getNoOfObjects() << endl;
    }
  }

  Vector<OutputStream *> table_output(metaData.getNoOfTables());
  debug << "Restoring tables" << endl;
  Logger::format_timestamp(time(NULL), timestamp, sizeof(timestamp));
  info << timestamp << " [restore_metadata]" << " Restoring tables" << endl;

  for(i = 0; i<metaData.getNoOfTables(); i++)
  {
    const TableS *table= metaData[i];
    table_output.push_back(NULL);
    if (!checkDbAndTableName(table))
      continue;
    if (isSYSTAB_0(table))
    {
      table_output[i]= ndbout.m_out;
    }
    if (checkSysTable(table))
    {
      if (!tab_path || isBlobTable(table) || isIndex(table))
      {
        table_output[i]= ndbout.m_out;
      }
      else
      {
        FILE* res;
        char filename[FN_REFLEN], tmp_path[FN_REFLEN];
        const char *table_name;
        table_name= table->getTableName();
        while (*table_name != '/') table_name++;
        table_name++;
        while (*table_name != '/') table_name++;
        table_name++;
        convert_dirname(tmp_path, tab_path, NullS);
        res= my_fopen(fn_format(filename, table_name, tmp_path, ".txt", 4),
                      opt_append ?
                      O_WRONLY|O_APPEND|O_CREAT :
                      O_WRONLY|O_TRUNC|O_CREAT,
                      MYF(MY_WME));
        if (res == 0)
        {
          exitHandler(NdbRestoreStatus::Failed);
        }
        FileOutputStream *f= new FileOutputStream(res);
        table_output[i]= f;
      }
      for(Uint32 j= 0; j < g_consumers.size(); j++)
	if (!g_consumers[j]->table(* table))
	{
	  err << "Restore: Failed to restore table: `";
          err << table->getTableName() << "` ... Exiting " << endl;
	  exitHandler(NdbRestoreStatus::Failed);
	} 
    } else {
      for(Uint32 j= 0; j < g_consumers.size(); j++)
        if (!g_consumers[j]->createSystable(* table))
        {
          err << "Restore: Failed to restore system table: ";
          err << table->getTableName() << " ... Exiting " << endl;
          exitHandler(NdbRestoreStatus::Failed);
        }
    }
    if (check_progress())
    {
      info.setLevel(255);
      info << "Table create progress: "
           << i+1 << " tables out of "
           << metaData.getNoOfTables() << endl;
    }
  }

  debug << "Save foreign key info" << endl;
  Logger::format_timestamp(time(NULL), timestamp, sizeof(timestamp));
  info << timestamp << " [restore_metadata]" << " Save foreign key info" << endl;
  for(i = 0; i<metaData.getNoOfObjects(); i++)
  {
    for(Uint32 j= 0; j < g_consumers.size(); j++)
      if (!g_consumers[j]->fk(metaData.getObjType(i),
			      metaData.getObjPtr(i)))
      {
        exitHandler(NdbRestoreStatus::Failed);
      } 
  }

  debug << "Close tables" << endl; 
  for(i= 0; i < g_consumers.size(); i++)
  {
    if (!g_consumers[i]->endOfTables())
    {
      err << "Restore: Failed while closing tables" << endl;
      exitHandler(NdbRestoreStatus::Failed);
    } 
    if (!ga_disable_indexes && !ga_rebuild_indexes)
    {
      if (!g_consumers[i]->endOfTablesFK())
      {
        err << "Restore: Failed while closing tables FKs" << endl;
        exitHandler(NdbRestoreStatus::Failed);
      } 
    }
  }
  Uint32 fragmentsTotal = 0;
  Uint32 fragmentsRestored = 0;
  /* report to clusterlog if applicable */
  for(i= 0; i < g_consumers.size(); i++)
  {
    g_consumers[i]->report_meta_data(ga_backupId, ga_nodeId);
  }
  debug << "Iterate over data" << endl;
  Logger::format_timestamp(time(NULL), timestamp, sizeof(timestamp));
  info << timestamp << " [restore_data]" << " Start restoring table data" << endl;
  if (ga_restore || ga_print) 
  {
    if(_restore_data || _print_data)
    {
      // Check table compatibility
      for (i=0; i < metaData.getNoOfTables(); i++){
        if (checkSysTable(metaData, i) &&
            checkDbAndTableName(metaData[i]))
        {
          TableS & tableS = *metaData[i]; // not const
          for(Uint32 j= 0; j < g_consumers.size(); j++)
          {
            if (!g_consumers[j]->table_compatible_check(tableS))
            {
              err << "Restore: Failed to restore data, ";
              err << tableS.getTableName() << " table structure incompatible with backup's ... Exiting " << endl;
              exitHandler(NdbRestoreStatus::Failed);
            } 
            if (tableS.m_staging &&
                !g_consumers[j]->prepare_staging(tableS))
            {
              err << "Restore: Failed to restore data, ";
              err << tableS.getTableName() << " failed to prepare staging table for data conversion ... Exiting " << endl;
              exitHandler(NdbRestoreStatus::Failed);
            }
          } 
        }
      }
      for (i=0; i < metaData.getNoOfTables(); i++)
      {
        if (checkSysTable(metaData, i) &&
            checkDbAndTableName(metaData[i]))
        {
          // blob table checks use data which is populated by table compatibility checks
          TableS & tableS = *metaData[i];
          if(isBlobTable(&tableS))
          {
            for(Uint32 j= 0; j < g_consumers.size(); j++)
            {
              if (!g_consumers[j]->check_blobs(tableS))
              {
                  err << "Restore: Failed to restore data, ";
                  err << tableS.getTableName() << " table's blobs incompatible with backup's ... Exiting " << endl;
                  exitHandler(NdbRestoreStatus::Failed);
              }
            }
          }
        }
      }
        
      RestoreDataIterator dataIter(metaData, &free_data_callback);

      if (!dataIter.validateBackupFile())
      {
          err << "Unable to allocate memory for BackupFile constructor" << endl;
          exitHandler(NdbRestoreStatus::Failed);
      }


      if (!dataIter.validateRestoreDataIterator())
      {
          err << "Unable to allocate memory for RestoreDataIterator constructor" << endl;
          exitHandler(NdbRestoreStatus::Failed);
      }
      
      Logger::format_timestamp(time(NULL), timestamp, sizeof(timestamp));
      info << timestamp << " [restore_data]" << " Read data file header" << endl;

      // Read data file header
      if (!dataIter.readHeader())
      {
	err << "Failed to read header of data file. Exiting..." << endl;
	exitHandler(NdbRestoreStatus::Failed);
      }
      
      Logger::format_timestamp(time(NULL), timestamp, sizeof(timestamp));
      info << timestamp << " [restore_data]" << " Restore fragments" << endl;

      Uint32 fragmentCount = 0;
      Uint32 fragmentId;
      while (dataIter.readFragmentHeader(res= 0, &fragmentId))
      {
        TableS* table = dataIter.getCurrentTable();
        OutputStream *output = table_output[table->getLocalId()];

        /**
         * Check whether we should skip the entire fragment
         */
        bool skipFragment = true;
        if (output == NULL)
        {
          info << "  Skipping fragment" << endl;
        }
        else
        {
          fragmentsTotal++;
          skipFragment = determine_slice_skip_fragment(table,
                                                       fragmentId,
                                                       fragmentCount);
          if (skipFragment)
          {
            info << " Skipping fragment on this slice" << endl;
          }
          else
          {
            fragmentsRestored++;
          }
        }

        /**
         * Iterate over all rows stored in the data file for
         * this fragment
         */
        const TupleS* tuple;
#ifdef ERROR_INSERT
        Uint64 rowCount = 0;
#endif
	while ((tuple = dataIter.getNextTuple(res= 1, skipFragment)) != 0)
        {
          assert(output && !skipFragment);
#ifdef ERROR_INSERT
          if ((_error_insert == NDB_RESTORE_ERROR_INSERT_SKIP_ROWS) &&
              ((++rowCount % 3) == 0))
          {
            info << "Skipping row on error insertion" << endl;
            continue;
          }
#endif
          OutputStream *tmp = ndbout.m_out;
          ndbout.m_out = output;
          for(Uint32 j= 0; j < g_consumers.size(); j++) 
            g_consumers[j]->tuple(* tuple, fragmentId);
          ndbout.m_out =  tmp;
          if (check_progress())
            report_progress("Data file progress: ", dataIter);
	} // while (tuple != NULL);
	
	if (res < 0)
	{
	  err <<" Restore: An error occured while restoring data. Exiting...";
          err << endl;
	  exitHandler(NdbRestoreStatus::Failed);
	}
	if (!dataIter.validateFragmentFooter()) {
	  err << "Restore: Error validating fragment footer. ";
          err << "Exiting..." << endl;
	  exitHandler(NdbRestoreStatus::Failed);
	}
      } // while (dataIter.readFragmentHeader(res))
      
      if (res < 0)
      {
	err << "Restore: An error occured while restoring data. Exiting... "
	    << "res= " << res << endl;
	exitHandler(NdbRestoreStatus::Failed);
      }
      
      
      dataIter.validateFooter(); //not implemented
      
      for (i= 0; i < g_consumers.size(); i++)
	g_consumers[i]->endOfTuples();

      /* report to clusterlog if applicable */
      for(i= 0; i < g_consumers.size(); i++)
      {
        g_consumers[i]->report_data(ga_backupId, ga_nodeId);
      }
    }

    if(_restore_data || _print_log || _print_sql_log)
    {
      RestoreLogIterator logIter(metaData);

      Logger::format_timestamp(time(NULL), timestamp, sizeof(timestamp));
      info << timestamp << " [restore_log]" << " Read log file header" << endl;

      if (!logIter.readHeader())
      {
	err << "Failed to read header of data file. Exiting..." << endl;
	exitHandler(NdbRestoreStatus::Failed);
      }
      
      const LogEntry * logEntry = 0;

      Logger::format_timestamp(time(NULL), timestamp, sizeof(timestamp));
      info << timestamp << " [restore_log]" << " Restore log entries" << endl;

      while ((logEntry = logIter.getNextLogEntry(res= 0)) != 0)
      {
        const TableS* table = logEntry->m_table;
        OutputStream *output = table_output[table->getLocalId()];
        if (!output)
          continue;
        if (check_slice_skip_fragment(table, logEntry->m_frag_id))
          continue;
        for(Uint32 j= 0; j < g_consumers.size(); j++)
          g_consumers[j]->logEntry(* logEntry);

        if (check_progress())
          report_progress("Log file progress: ", logIter);
      }
      if (res < 0)
      {
	err << "Restore: An restoring the data log. Exiting... res=" 
	    << res << endl;
	exitHandler(NdbRestoreStatus::Failed);
      }
      logIter.validateFooter(); //not implemented
      for (i= 0; i < g_consumers.size(); i++)
	g_consumers[i]->endOfLogEntrys();

      /* report to clusterlog if applicable */
      for(i= 0; i < g_consumers.size(); i++)
      {
        g_consumers[i]->report_log(ga_backupId, ga_nodeId);
      }
    }
    
    /* move data from staging table to real table */
    if(_restore_data)
    {
      for (i = 0; i < metaData.getNoOfTables(); i++)
      {
        const TableS* table = metaData[i];
        if (table->m_staging)
        {
          for(Uint32 j= 0; j < g_consumers.size(); j++)
          {
            if (!g_consumers[j]->finalize_staging(*table))
            {
              err << "Restore: Failed staging data to table: ";
              err << table->getTableName() << ". ";
              err << "Exiting... " << endl;
              exitHandler(NdbRestoreStatus::Failed);
            }
          }
        }
      }
    }

    if(_restore_data)
    {
      for(i = 0; i < metaData.getNoOfTables(); i++)
      {
        const TableS* table = metaData[i];
        check_data_truncations(table);
        OutputStream *output = table_output[table->getLocalId()];
        if (!output)
          continue;
        for(Uint32 j= 0; j < g_consumers.size(); j++)
          if (!g_consumers[j]->finalize_table(*table))
          {
            err << "Restore: Failed to finalize restore table: %s. ";
            err << "Exiting... " << metaData[i]->getTableName() << endl;
            exitHandler(NdbRestoreStatus::Failed);
          }
      }

      if (ga_num_slices != 1)
      {
        info << "Restore: Slice id "
             << ga_slice_id << "/"
             << ga_num_slices
             << " restored "
             << fragmentsRestored
             << "/"
             << fragmentsTotal
             << " fragments."
             << endl;
      };
    }
  }
  if (ga_restore_epoch)
  {
    Logger::format_timestamp(time(NULL), timestamp, sizeof(timestamp));
    info << timestamp << " [restore_epoch]" << " Restoring epoch" << endl;
    RestoreLogIterator logIter(metaData);

    if (!logIter.readHeader())
    {
      err << "Failed to read snapshot info from log file. Exiting..." << endl;
      return NdbRestoreStatus::Failed;
    }
    bool snapshotstart = logIter.isSnapshotstartBackup();
    for (i= 0; i < g_consumers.size(); i++)
      if (!g_consumers[i]->update_apply_status(metaData, snapshotstart))
      {
        err << "Restore: Failed to restore epoch" << endl;
        return -1;
      }
  }

  unsigned j;
  for(j= 0; j < g_consumers.size(); j++) 
  {
    if (g_consumers[j]->has_temp_error())
    {
      clearConsumers();
      ndbout_c("\nRestore successful, but encountered temporary error, "
               "please look at configuration.");
    }               
  }

  if (ga_rebuild_indexes)
  {
    debug << "Rebuilding indexes" << endl;
    Logger::format_timestamp(time(NULL), timestamp, sizeof(timestamp));
    info << timestamp << " [rebuild_indexes]" << " Rebuilding indexes" << endl;

    for(i = 0; i<metaData.getNoOfTables(); i++)
    {
      const TableS *table= metaData[i];
      if (! (checkSysTable(table) && checkDbAndTableName(table)))
        continue;
      if (isBlobTable(table) || isIndex(table))
        continue;
      for(Uint32 j= 0; j < g_consumers.size(); j++)
      {
        if (!g_consumers[j]->rebuild_indexes(* table))
          return -1;
      }
    }
    for(Uint32 j= 0; j < g_consumers.size(); j++)
    {
      if (!g_consumers[j]->endOfTablesFK())
        return -1;
    }
  }

  /* report to clusterlog if applicable */
  for (i = 0; i < g_consumers.size(); i++)
    g_consumers[i]->report_completed(ga_backupId, ga_nodeId);

  clearConsumers();

  for(i = 0; i < metaData.getNoOfTables(); i++)
  {
    if (table_output[i] &&
        table_output[i] != ndbout.m_out)
    {
      my_fclose(((FileOutputStream *)table_output[i])->getFile(), MYF(MY_WME));
      delete table_output[i];
      table_output[i] = NULL;
    }
  }
  return NdbRestoreStatus::Ok;
} // main

template class Vector<BackupConsumer*>;
template class Vector<OutputStream*>;
template class Vector<RestoreOption *>;
