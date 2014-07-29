/* Copyright (c) 2014, Oracle and/or its affiliates. All rights reserved.

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

#include "binlog_event.h"
#include "control_events.h"
#include "statement_events.h"
#include "transitional_methods.h"
#include <algorithm>
#include <cstdio>

namespace binary_log
{

/**
  The variable part of the Rotate event contains the name of the next binary
  log file,  and the position of the first event in the next binary log file.

  <pre>
  The buffer layout is as follows:
  +-----------------------------------------------------------------------+
  | common_header | post_header | position og the first event | file name |
  +-----------------------------------------------------------------------+
  </pre>

  @param buf Buffer contain event data in the layout specified above
  @param event_len The length of the event written in the log file
  @param description_event FDE used to extract the post header length, which
                           depends on the binlog version
  @param head Header information of the event
*/
Rotate_event::Rotate_event(const char* buf, unsigned int event_len,
                           const Format_description_event *description_event)
: Binary_log_event(&buf, description_event->binlog_version,
                   description_event->server_version), new_log_ident(0),
  flags(DUP_NAME)
{
  //buf is advanced in Binary_log_event constructor to point to beginning of post-header
  // This will ensure that the event_len is what we have at EVENT_LEN_OFFSET
  size_t header_size= description_event->common_header_len;
  size_t post_header_len= description_event->post_header_len[ROTATE_EVENT - 1];
  unsigned int ident_offset;

  if (event_len < header_size)
    return;


  /**
    By default, an event start immediately after the magic bytes in the binary
    log, which is at offset 4. In case if the slave has to rotate to a
    different event instead of the first one, the binary log offset for that
    event is specified in the post header. Else, the position is set to 4.
  */
  if (post_header_len)
  {
    memcpy(&pos, buf + R_POS_OFFSET, 8);
    pos= le64toh(pos);
  }
  else
    pos= 4;

  ident_len= event_len - (header_size + post_header_len);
  ident_offset= post_header_len;
  if (ident_len > FN_REFLEN - 1)
    ident_len= FN_REFLEN - 1;

  new_log_ident= bapi_strndup(buf + ident_offset, ident_len);
}


/**
   Empty ctor of Start_event_v3 called when we call the
   ctor of FDE which takes binlog_version as the parameter
   It will initialize the server_version by global variable
   server_version
*/
Start_event_v3::Start_event_v3(Log_event_type type_code_arg)
: Binary_log_event(type_code_arg),
  created(0), binlog_version(BINLOG_VERSION),
  dont_set_created(0)
{
}

/**
  Format_description_log_event 1st constructor.

    This constructor can be used to create the event to write to the binary log
    (when the server starts or when FLUSH LOGS), or to create artificial events
    to parse binlogs from MySQL 3.23 or 4.x.
    When in a client, only the 2nd use is possible.

  @param binlog_ver             the binlog version for which we want to build
                                an event. Can be 1 (=MySQL 3.23), 3 (=4.0.x
                                x>=2 and 4.1) or 4 (MySQL 5.0). Note that the
                                old 4.0 (binlog version 2) is not supported;
                                it should not be used for replication with
                                5.0.
  @param server_ver             a string containing the server version.
*/
Format_description_event::Format_description_event(uint8_t binlog_ver,
                                                   const char* server_ver)
: Start_event_v3(FORMAT_DESCRIPTION_EVENT), event_type_permutation(0)
{
  binlog_version= binlog_ver;
  switch (binlog_ver) {
  case 4: /* MySQL 5.0 and above*/
  {
    /*
     As we are copying from a char * it might be the case at times that some part
     of the array server_version remains uninitialized so memset will help
     in getting rid of the valgrind errors.
    */
    memset(server_version, 0, ST_SERVER_VER_LEN);
    bapi_stpcpy(server_version, server_ver);
    if (binary_log_debug::debug_pretend_version_50034_in_binlog)
      bapi_stpcpy(server_version, "5.0.34");
    common_header_len= LOG_EVENT_HEADER_LEN;
    number_of_event_types= LOG_EVENT_TYPES;
    /**
      This will be used to initialze the post_header_len,
      for binlog version 4.
    */
    static uint8_t server_event_header_length[]=
    {
      START_V3_HEADER_LEN,
      QUERY_HEADER_LEN,
      STOP_HEADER_LEN,
      ROTATE_HEADER_LEN,
      INTVAR_HEADER_LEN,
      LOAD_HEADER_LEN,
      /* Unused because the code for Slave log event was removed. (15th Oct. 2010) */
      0,
      CREATE_FILE_HEADER_LEN,
      APPEND_BLOCK_HEADER_LEN,
      EXEC_LOAD_HEADER_LEN,
      DELETE_FILE_HEADER_LEN,
      NEW_LOAD_HEADER_LEN,
      RAND_HEADER_LEN,
      USER_VAR_HEADER_LEN,
      FORMAT_DESCRIPTION_HEADER_LEN,
      XID_HEADER_LEN,
      BEGIN_LOAD_QUERY_HEADER_LEN,
      EXECUTE_LOAD_QUERY_HEADER_LEN,
      TABLE_MAP_HEADER_LEN,
      /*
       The PRE_GA events are never be written to any binlog, but
       their lengths are included in Format_description_log_event.
       Hence, we need to be assign some value here, to avoid reading
       uninitialized memory when the array is written to disk.
      */
      0,                                       /* PRE_GA_WRITE_ROWS_EVENT */
      0,                                       /* PRE_GA_UPDATE_ROWS_EVENT*/
      0,                                       /* PRE_GA_DELETE_ROWS_EVENT*/
      ROWS_HEADER_LEN_V1,                      /* WRITE_ROWS_EVENT_V1*/
      ROWS_HEADER_LEN_V1,                      /* UPDATE_ROWS_EVENT_V1*/
      ROWS_HEADER_LEN_V1,                      /* DELETE_ROWS_EVENT_V1*/
      INCIDENT_HEADER_LEN,
      0,                                       /* HEARTBEAT_LOG_EVENT*/
      IGNORABLE_HEADER_LEN,
      IGNORABLE_HEADER_LEN,
      ROWS_HEADER_LEN_V2,
      ROWS_HEADER_LEN_V2,
      ROWS_HEADER_LEN_V2,
       Gtid_event::POST_HEADER_LENGTH,         /*GTID_EVENT*/
       Gtid_event::POST_HEADER_LENGTH,         /*ANONYMOUS_GTID_EVENT*/
       IGNORABLE_HEADER_LEN
    };
    post_header_len=  new uint8_t [number_of_event_types * sizeof(uint8_t) +
                                   BINLOG_CHECKSUM_ALG_DESC_LEN];

    if (post_header_len)
    {
      /**
         Allows us to sanity-check that all events initialized their
         events (see the end of this 'if' block).
      */
      memset(post_header_len, 255, number_of_event_types * sizeof(uint8_t));
      memcpy(post_header_len, server_event_header_length, number_of_event_types);
      // Sanity-check that all post header lengths are initialized.
      for (int i= 0; i < number_of_event_types; i++)
        assert(post_header_len[i] != 255);
    }
    break;
  }
  case 1: /* 3.23 */
  case 3: /* 4.0.x x >= 2 */
  {
    /*
      We build an artificial (i.e. not sent by the master) event, which
      describes what those old master versions send.
    */
    if (binlog_version == 1)
      bapi_stpcpy(server_version, server_ver ? server_ver : "3.23");
    else
      bapi_stpcpy(server_version, server_ver ? server_ver : "4.0");
    common_header_len= binlog_ver == 1 ? OLD_HEADER_LEN :
      LOG_EVENT_MINIMAL_HEADER_LEN;
    /*
      The first new event in binlog version 4 is Format_desc. So any event type
      after that does not exist in older versions. We use the events known by
      version 3, even if version 1 had only a subset of them (this is not a
      problem: it uses a few bytes for nothing but unifies code; it does not
      make the slave detect less corruptions).
    */
    number_of_event_types= FORMAT_DESCRIPTION_EVENT - 1;
     /**
      This will be used to initialze the post_header_len, for binlog version
      1 and 3
     */
    static uint8_t server_event_header_length_ver_1_3[]=
    {
      START_V3_HEADER_LEN,
      QUERY_HEADER_MINIMAL_LEN,
      STOP_HEADER_LEN,
      uint8_t(binlog_ver == 1 ? 0 : ROTATE_HEADER_LEN),
      INTVAR_HEADER_LEN,
      LOAD_HEADER_LEN,
      0,/* Unused because the code for Slave log event was removed. (15th Oct. 2010) */
      CREATE_FILE_HEADER_LEN,
      APPEND_BLOCK_HEADER_LEN,
      EXEC_LOAD_HEADER_LEN,
      DELETE_FILE_HEADER_LEN,
      NEW_LOAD_HEADER_LEN,
      RAND_HEADER_LEN,
      USER_VAR_HEADER_LEN
    };
    post_header_len= new uint8_t [number_of_event_types * sizeof(uint8_t) +
                                  BINLOG_CHECKSUM_ALG_DESC_LEN];
    if (post_header_len)
    {
      memcpy(post_header_len, server_event_header_length_ver_1_3, number_of_event_types);
    }
    break;
  }
  default: /* Includes binlog version 2 i.e. 4.0.x x<=1 */
    /*
      Will make the mysql-server variable *is_valid* defined in class Log_event
      to be set to false.
    */
    post_header_len= 0;
    break;
  }
  calc_server_version_split();
}

/**
   This method populates the array server_version_split which is then
   used for lookups to find if the server which
   created this event has some known bug.
*/
void Format_description_event::calc_server_version_split()
{
  do_server_version_split(server_version, server_version_split);
}

/**
   This method is used to find out the version of server that originated
   the current FD instance.
   @return the version of server
*/
unsigned long Format_description_event::get_product_version() const
{
  return version_product(server_version_split);
}

/**
   This method checks the MySQL version to determine whether checksums may be
   present in the events contained in the bainry log.

   @retval true  if the event's version is earlier than one that introduced
                 the replication event checksum.
   @retval false otherwise.
*/
bool Format_description_event::is_version_before_checksum() const
{
  return get_product_version() < checksum_version_product;
}


Start_event_v3::Start_event_v3(const char* buf,
                               const Format_description_event *description_event)
  :Binary_log_event(&buf, description_event->binlog_version,
   description_event->server_version)
{
  //buf is advanced in Binary_log_event constructor to point to beginning of post-header
  memcpy(&binlog_version, buf + ST_BINLOG_VER_OFFSET, 2);
  binlog_version= le16toh(binlog_version);
  memcpy(server_version, buf + ST_SERVER_VER_OFFSET,
        ST_SERVER_VER_LEN);
  // prevent overrun if log is corrupted on disk
  server_version[ST_SERVER_VER_LEN - 1]= 0;
  created= 0;
  memcpy(&created, buf + ST_CREATED_OFFSET, 4);
  created= le64toh(created);
  dont_set_created= 1;
}

/**
  The problem with this constructor is that the fixed header may have a
  length different from this version, but we don't know this length as we
  have not read the Format_description_log_event which says it, yet. This
  length is in the post-header of the event, but we don't know where the
  post-header starts.

  So this type of event HAS to:
  - either have the header's length at the beginning (in the header, at a
  fixed position which will never be changed), not in the post-header. That
  would make the header be "shifted" compared to other events.
  - or have a header of size LOG_EVENT_MINIMAL_HEADER_LEN (19), in all future
  versions, so that we know for sure.

  I (Guilhem) chose the 2nd solution. Rotate has the same constraint (because
  it is sent before Format_description_log_event).

  The layout of the event data part  in  Format_description_event
  <pre>
        +=====================================+
        | event  | binlog_version   19 : 2    | = 4
        | data   +----------------------------+
        |        | server_version   21 : 50   |
        |        +----------------------------+
        |        | create_timestamp 71 : 4    |
        |        +----------------------------+
        |        | header_length    75 : 1    |
        |        +----------------------------+
        |        | post-header      76 : n    | = array of n bytes, one byte per
        |        | lengths for all            |   event type that the server knows
        |        | event types                |   about
        +=====================================+
  </pre>
*/
Format_description_event::
Format_description_event(const char* buf, unsigned int event_len,
                         const Format_description_event* description_event)
 :Start_event_v3(buf, description_event), event_type_permutation(0)
{
  unsigned long ver_calc;
  buf+= LOG_EVENT_MINIMAL_HEADER_LEN;
  if ((common_header_len= buf[ST_COMMON_HEADER_LEN_OFFSET]) < OLD_HEADER_LEN)
    return; /* sanity check */
  number_of_event_types=
   event_len - (LOG_EVENT_MINIMAL_HEADER_LEN + ST_COMMON_HEADER_LEN_OFFSET + 1);

  post_header_len=  new uint8_t [number_of_event_types * sizeof(*post_header_len)];
  memcpy(post_header_len, buf + ST_COMMON_HEADER_LEN_OFFSET + 1,
         number_of_event_types * sizeof(*post_header_len));

  calc_server_version_split();
  if ((ver_calc= get_product_version()) >= checksum_version_product)
  {
    /* the last bytes are the checksum alg desc and value (or value's room) */
    number_of_event_types -= BINLOG_CHECKSUM_ALG_DESC_LEN;
    /*
      FD from the checksum-home version server (ver_calc ==
      checksum_version_product) must have
      number_of_event_types == LOG_EVENT_TYPES.
    */
    assert(ver_calc != checksum_version_product ||
                number_of_event_types == LOG_EVENT_TYPES);
    footer()->checksum_alg= (enum_binlog_checksum_alg)
                                  post_header_len[number_of_event_types];
  }
  else
  {
    footer()->checksum_alg=  BINLOG_CHECKSUM_ALG_UNDEF;
  }

  /*
    In some previous versions, the events were given other event type
    id numbers than in the present version. When replicating from such
    a version, we therefore set up an array that maps those id numbers
    to the id numbers of the present server.
    //TODO: Modify the comment
    If post_header_len is null, it means malloc failed, and in the mysql-server
    code the variable *is_valid* will be set to false, so there is no need to do
    anything.

    The trees in which events have wrong id's are:

    mysql-5.1-wl1012.old mysql-5.1-wl2325-5.0-drop6p13-alpha
    mysql-5.1-wl2325-5.0-drop6 mysql-5.1-wl2325-5.0
    mysql-5.1-wl2325-no-dd

    (this was found by grepping for two lines in sequence where the
    first matches "FORMAT_DESCRIPTION_EVENT," and the second matches
    "TABLE_MAP_EVENT," in log_event.h in all trees)

    In these trees, the following server_versions existed since
    TABLE_MAP_EVENT was introduced:
     5.1.1-a_drop5p3   5.1.1-a_drop5p4        5.1.1-alpha
    5.1.2-a_drop5p10  5.1.2-a_drop5p11       5.1.2-a_drop5p12
    5.1.2-a_drop5p13  5.1.2-a_drop5p14       5.1.2-a_drop5p15
    5.1.2-a_drop5p16  5.1.2-a_drop5p16b      5.1.2-a_drop5p16c
    5.1.2-a_drop5p17  5.1.2-a_drop5p4        5.1.2-a_drop5p5
    5.1.2-a_drop5p6   5.1.2-a_drop5p7        5.1.2-a_drop5p8
    5.1.2-a_drop5p9   5.1.3-a_drop5p17       5.1.3-a_drop5p17b
5.1.3-a_drop5p17c 5.1.4-a_drop5p18       5.1.4-a_drop5p19
    5.1.4-a_drop5p20  5.1.4-a_drop6p0        5.1.4-a_drop6p1
    5.1.4-a_drop6p2   5.1.5-a_drop5p20       5.2.0-a_drop6p3
    5.2.0-a_drop6p4   5.2.0-a_drop6p5        5.2.0-a_drop6p6
    5.2.1-a_drop6p10  5.2.1-a_drop6p11       5.2.1-a_drop6p12
    5.2.1-a_drop6p6   5.2.1-a_drop6p7        5.2.1-a_drop6p8
    5.2.2-a_drop6p13  5.2.2-a_drop6p13-alpha 5.2.2-a_drop6p13b
    5.2.2-a_drop6p13c

    (this was found by grepping for "mysql," in all historical
    versions of configure.in in the trees listed above).

    There are 5.1.1-alpha versions that use the new event id's, so we
    do not test that version string.  So replication from 5.1.1-alpha
    with the other event id's to a new version does not work.
    Moreover, we can safely ignore the part after drop[56].  This
    allows us to simplify the big list above to the following regexes:

    5\.1\.[1-5]-a_drop5.*
    5\.1\.4-a_drop6.*
    5\.2\.[0-2]-a_drop6.*

    This is what we test for in the 'if' below.
  */
  if (post_header_len &&
      server_version[0] == '5' && server_version[1] == '.' &&
      server_version[3] == '.' &&
      strncmp(server_version + 5, "-a_drop", 7) == 0 &&
      ((server_version[2] == '1' &&
        server_version[4] >= '1' && server_version[4] <= '5' &&
        server_version[12] == '5') ||
       (server_version[2] == '1' &&
        server_version[4] == '4' &&
        server_version[12] == '6') ||
       (server_version[2] == '2' &&
        server_version[4] >= '0' && server_version[4] <= '2' &&
        server_version[12] == '6')))
  {
    if (number_of_event_types != 22)
    {
      /* this makes is_valid in the server code to be set to false. */
      delete[] post_header_len;
      post_header_len= NULL;
      return;
    }
    static const uint8_t perm[23]=
      {
        UNKNOWN_EVENT, START_EVENT_V3, QUERY_EVENT, STOP_EVENT, ROTATE_EVENT,
        INTVAR_EVENT, LOAD_EVENT, SLAVE_EVENT, CREATE_FILE_EVENT,
        APPEND_BLOCK_EVENT, EXEC_LOAD_EVENT, DELETE_FILE_EVENT,
        NEW_LOAD_EVENT,
        RAND_EVENT, USER_VAR_EVENT,
        FORMAT_DESCRIPTION_EVENT,
        TABLE_MAP_EVENT,
        PRE_GA_WRITE_ROWS_EVENT,
        PRE_GA_UPDATE_ROWS_EVENT,
        PRE_GA_DELETE_ROWS_EVENT,
        XID_EVENT,
        BEGIN_LOAD_QUERY_EVENT,
        EXECUTE_LOAD_QUERY_EVENT,
      };
    event_type_permutation= perm;
    /*
      Since we use (permuted) event id's to index the post_header_len
      array, we need to permute the post_header_len array too.
    */
    uint8_t post_header_len_temp[23];
    for (int i= 1; i < 23; i++)
      post_header_len_temp[perm[i] - 1]= post_header_len[i - 1];
    for (int i= 0; i < 22; i++)
      post_header_len[i] = post_header_len_temp[i];

  }
    return;
}


Format_description_event::~Format_description_event()
{
  if(post_header_len)
    delete[] post_header_len;
}

/**
  Constructor of Incident_event
  The buffer layout is as follows:
  <pre>
  +-----------------------------------------------------+
  | Incident_number | message_length | Incident_message |
  +-----------------------------------------------------+
  </pre>

  Incident number codes are listed in binlog_evnet.h.
  The only code currently used is INCIDENT_LOST_EVENTS, which indicates that
  there may be lost events (a "gap") in the replication stream that requires
  databases to be resynchronized.
*/
Incident_event::Incident_event(const char *buf, unsigned int event_len,
                               const Format_description_event *descr_event)
: Binary_log_event(&buf, descr_event->binlog_version,
                   descr_event->server_version)
{
  //buf is advanced in Binary_log_event constructor to point to beginning of post-header
  uint8_t const common_header_len= descr_event->common_header_len;
  uint8_t const post_header_len= descr_event->post_header_len[INCIDENT_EVENT-1];

  message= NULL;
  message_length= 0;
  uint16_t incident_number;
  memcpy(&incident_number, buf, 2);
  incident_number= le16toh(incident_number);
  if (incident_number >= INCIDENT_COUNT ||
      incident_number <= INCIDENT_NONE)
  {
    /*
      If the incident is not recognized, this binlog event is
      invalid.
    */
    incident= INCIDENT_NONE;

  }

  incident= static_cast<enum_incident>(incident_number);
  char const *ptr= buf + post_header_len;
  char const *const str_end= buf - common_header_len + event_len;
  uint8_t len= 0;                   // Assignment to keep compiler happy
  const char *str= NULL;          // Assignment to keep compiler happy
  read_str_at_most_255_bytes(&ptr, str_end, &str, &len);
  if (!(message= (char*) bapi_malloc(len + 1, MEMORY_LOG_EVENT, 16)))
  {
    /* Mark this event invalid */
    incident= INCIDENT_NONE;
    return;
  }

  bapi_strmake(message, str, len);
  message_length= len;
  return;
}

Xid_event::
Xid_event(const char* buf,
          const Format_description_event *description_event)
  :Binary_log_event(&buf, description_event->binlog_version,
                    description_event->server_version)
{
  //buf is advanced in Binary_log_event constructor to point to beginning of post-header
  /*
   We step to the post-header despite it being empty because it could later be
   filled with something and we have to support that case.
   The Variable Data part begins immediately.
  */
  buf+= description_event->post_header_len[XID_EVENT - 1];
  memcpy((char*) &xid, buf, 8);
}


/**
  Written every time a statement uses the RAND() function; precedes other
  events for the statement. Indicates the seed values to use for generating a
  random number with RAND() in the next statement. This is written only before
  a QUERY_EVENT and is not used with row-based logging
*/
Rand_event::Rand_event(const char* buf,
                       const Format_description_event* description_event)
  :Binary_log_event(&buf, description_event->binlog_version,
                    description_event->server_version)
{
  //buf is advanced in Binary_log_event constructor to point to beginning of post-header
  /*
   We step to the post-header despite it being empty because it could later be
   filled with something and we have to support that case.
   The Variable Data part begins immediately.
  */
  buf+= description_event->post_header_len[RAND_EVENT - 1];
  memcpy(&seed1, buf + RAND_SEED1_OFFSET, 8);
  seed1= le64toh(seed1);
  memcpy(&seed2, buf + RAND_SEED2_OFFSET, 8);
  seed2= le64toh(seed2);
}

/**
    We create an object of Ignorable_log_event for unrecognized sub-class, while
    decoding. So that we just update the position and continue.

    @param buf                Contains the serialized event.
    @param length             Length of the serialized event.
    @param description_event  An FDE event, used to get the following information
                              -binlog_version
                              -server_version
                              -post_header_len
                              -common_header_len
                              The content of this object
                              depends on the binlog-version currently in use.
*/
Ignorable_event::Ignorable_event(const char *buf, const Format_description_event *descr_event)
:Binary_log_event(&buf, descr_event->binlog_version,
                  descr_event->server_version)
{
}


/**
  ctor of Gtid_event
  Each transaction has a coordinate in the form of a pair:
  GTID = (SID, GNO)
  GTID stands for Global Transaction IDentifier, SID for Source Identifier,
  and GNO for Group Number.

  SID is a 128-bit number that identifies the place where the transaction was
  first committed. SID is normally the SERVER_UUID of a server, but may be
  something different if the transaction was generated by something else than
  a MySQL server).

  GNO is a 64-bit sequence number: 1 for the first transaction committed on SID,
  2 for the second transaction, and so on. No transaction can have GNO 0
*/

Gtid_event::Gtid_event(const char *buffer, uint32_t event_len,
                       const Format_description_event *description_event)
 : Binary_log_event(&buffer, description_event->binlog_version,
                    description_event->server_version)
{
  //buf is advanced in Binary_log_event constructor to point to beginning of post-header
  uint8_t const common_header_len= description_event->common_header_len;

/*
  The layout of the buffer is as follows
   +-------------+-------------+------------+------------+--------------+
   | commit flag | ENCODED SID | ENCODED GNO| G_COMMIT_TS| commit_seq_no|
   +-------------+-------------+------------+------------+--------------+
*/
  char const *ptr_buffer= buffer;

  commit_flag= *ptr_buffer != 0;
  ptr_buffer+= ENCODED_FLAG_LENGTH;

  memcpy(Uuid_parent_struct.bytes, (const unsigned char*)ptr_buffer,
          Uuid_parent_struct.BYTE_LENGTH);
  ptr_buffer+= ENCODED_SID_LENGTH;

  // SIDNO is only generated if needed, in get_sidno().
  gtid_info_struct.rpl_gtid_sidno= -1;

  memcpy(&(gtid_info_struct.rpl_gtid_gno), ptr_buffer,
         sizeof(gtid_info_struct.rpl_gtid_gno));
  gtid_info_struct.rpl_gtid_gno= le64toh(gtid_info_struct.rpl_gtid_gno);
  ptr_buffer+= ENCODED_GNO_LENGTH;
    /* fetch the commit timestamp */
  /*Old masters will not have this part, so we should prevent segfaulting */
  if (static_cast<unsigned int>(ptr_buffer - (buffer - common_header_len)) < event_len &&
      *ptr_buffer == G_COMMIT_TS)
  {
    ptr_buffer++;
    memcpy(&commit_seq_no, ptr_buffer, sizeof(commit_seq_no));
    commit_seq_no= (int64_t)le64toh(commit_seq_no);
    ptr_buffer+= COMMIT_SEQ_LEN;
  }
  else
    /* We let coordinator complain when it sees that we have first
       event and the master has not sent us the commit sequence number
       Also, we can be rest assured that this is an old master, because new
       master would have compained of the missing commit seq no while flushing.*/
    commit_seq_no= SEQ_UNINIT;
  return;
}

/**
  Constructor of Previous_gtid_event
  Decodes the gtid_executed in the last binlog file
*/

Previous_gtids_event::
Previous_gtids_event(const char *buffer, unsigned int event_len,
                     const Format_description_event *description_event)
: Binary_log_event(&buffer, description_event->binlog_version,
                     description_event->server_version)
{
  //buf is advanced in Binary_log_event constructor to point to beginning of post-header
  uint8_t const common_header_len= description_event->common_header_len;
  uint8_t const post_header_len=
    description_event->post_header_len[PREVIOUS_GTIDS_LOG_EVENT - 1];

  buf= (const unsigned char *)buffer  + post_header_len;
  buf_size= event_len - common_header_len - post_header_len;
  return;
}



Heartbeat_event::Heartbeat_event(const char* buf, unsigned int event_len,
                                 const Format_description_event*
                                 description_event)
: Binary_log_event(&buf, description_event->binlog_version,
                   description_event->server_version),
  log_ident(buf)
{
  //buf is advanced in Binary_log_event constructor to point to beginning of post-header
  unsigned char header_size= description_event->common_header_len;
  ident_len= event_len - header_size;
  if (ident_len > FN_REFLEN - 1)
    ident_len= FN_REFLEN - 1;
}

#ifndef HAVE_MYSYS
void Rotate_event::print_event_info(std::ostream& info)
{
  info << "Binlog Position: " << pos;
  info << ", Log name: " << new_log_ident;
}

void Rotate_event::print_long_info(std::ostream& info)
{
  info << "Timestamp: " << header()->when.tv_sec;
  info << "\t";
  this->print_event_info(info);
}

void Format_description_event::print_event_info(std::ostream& info)
{
  info << "Server ver: " << server_version;
  info << ", Binlog ver: " << binlog_version;
}

void Format_description_event::print_long_info(std::ostream& info)
{
  this->print_event_info(info);
  info << "\nCreated timestamp: " << created;
  info << "\tCommon Header Length: " << common_header_len;
  info << "\nPost header length for events: \n";
}


void Incident_event::print_event_info(std::ostream& info)
{
  info << get_message();
  info << get_incident_type();
}

void Incident_event::print_long_info(std::ostream& info)
{
  this->print_event_info(info);
}

void Xid_event::print_event_info(std::ostream& info)
{
  info << "Xid ID=" << xid;
}

void Xid_event::print_long_info(std::ostream& info)
{
  info << "Timestamp: " << header()->when.tv_sec;
  info << "\t";
  this->print_event_info(info);
}

void Rand_event::print_event_info(std::ostream& info)
{
  info << " SEED1 is " << seed1;
  info << " SEED2 is " << seed2;
}
void Rand_event::print_long_info(std::ostream& info)
{
  info << "Timestamp: " << header()->when.tv_sec;
  info << "\t";
  this->print_event_info(info);
}
#endif //end HAVE_MYSYS

}// end namespace binary_log


