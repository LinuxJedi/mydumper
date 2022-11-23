/*
    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

        Authors:    Domas Mituzas, Facebook ( domas at fb dot com )
                    Mark Leith, Oracle Corporation (mark dot leith at oracle dot com)
                    Andrew Hutchings, MariaDB Foundation (andrew at mariadb dot org)
                    Max Bubenick, Percona RDBA (max dot bubenick at percona dot com)
                    David Ducos, Percona (david dot ducos at percona dot com)
*/
#include <mysql.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include "mydumper_start_dump.h"
#include "server_detect.h"
#include "common.h"
#include "regex.h"
#include "mydumper_common.h"
#include "mydumper_jobs.h"
#include "mydumper_database.h"
#include "mydumper_working_thread.h"
#include "mydumper_write.h"
#include "mydumper_chunks.h"

extern guint char_chunk;
extern gboolean load_data;
extern gchar *where_option;
extern gboolean success_on_1146;
extern int detected_server;
extern FILE * (*m_open)(const char *filename, const char *);
extern int (*m_close)(void *file);
extern guint errors;
extern guint statement_size;
extern int skip_tz;
extern gchar *set_names_str;
extern GAsyncQueue *stream_queue;
extern gboolean stream;
extern gboolean dump_routines;
extern gboolean dump_events;
extern gboolean use_savepoints;
extern gint database_counter;
extern GMutex *ready_database_dump_mutex;
//extern gint table_counter;
extern guint rows_per_file;
//extern gint non_innodb_table_counter;
extern GMutex *ready_table_dump_mutex;
gboolean dump_triggers = FALSE;
gboolean order_by_primary_key = FALSE;
gboolean ignore_generated_fields = FALSE;

gchar *exec_per_thread = NULL;
gchar *exec_per_thread_extension = NULL;
gboolean use_fifo = FALSE;
gchar **exec_per_thread_cmd=NULL;

extern gboolean schema_checksums;
extern gboolean routine_checksums;
extern gboolean use_fifo;


gboolean skip_definer = FALSE;

static GOptionEntry dump_into_file_entries[] = {
    {"triggers", 'G', 0, G_OPTION_ARG_NONE, &dump_triggers, "Dump triggers. By default, it do not dump triggers",
     NULL},
    { "no-check-generated-fields", 0, 0, G_OPTION_ARG_NONE, &ignore_generated_fields,
      "Queries related to generated fields are not going to be executed."
      "It will lead to restoration issues if you have generated columns", NULL },
    {"order-by-primary", 0, 0, G_OPTION_ARG_NONE, &order_by_primary_key,
     "Sort the data by Primary Key or Unique key if no primary key exists",
     NULL},
    {"exec-per-thread",0, 0, G_OPTION_ARG_STRING, &exec_per_thread,
     "Set the command that will receive by STDIN and write in the STDOUT into the output file", NULL},
    {"exec-per-thread-extension",0, 0, G_OPTION_ARG_STRING, &exec_per_thread_extension,
     "Set the extension for the STDOUT file when --exec-per-thread is used", NULL},
    {"skip-definer", 0, 0, G_OPTION_ARG_NONE, &skip_definer,
     "Removes DEFINER from the CREATE statement. By default, statements are not modified", NULL},
    {NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL}};

void load_dump_into_file_entries(GOptionGroup *main_group){
  g_option_group_add_entries(main_group, dump_into_file_entries);
}

void initialize_jobs(){
  initialize_database();
  if (ignore_generated_fields)
    g_warning("Queries related to generated fields are not going to be executed. It will lead to restoration issues if you have generated columns");

  if (exec_per_thread_extension != NULL){
    if(exec_per_thread == NULL)
      g_error("--exec-per-thread needs to be set when --exec-per-thread-extension is used");
  }

  if (exec_per_thread!=NULL){
    use_fifo=TRUE;
    exec_per_thread_cmd=g_strsplit(exec_per_thread, " ", 0);
  }
}

void write_checksum_into_file(MYSQL *conn, char *database, char *table, char *filename, gchar *fun()) {
  int errn=0;
  gchar * checksum=fun(conn, database, table, &errn);

  if (errn != 0 && !(success_on_1146 && errn == 1146)) {
    errors++;
    return;
  }

  if (checksum == NULL)
    checksum = g_strdup("0");

  void *outfile = NULL;

  outfile = g_fopen(filename, "w");

  if (!outfile) {
    g_critical("Error: DB: %s TABLE: %s Could not create output file %s (%d)",
               database, table, filename, errno);
    errors++;
    return;
  }

  fprintf(outfile, "%s\n", checksum);
  fclose(outfile);

  if (stream) g_async_queue_push(stream_queue, g_strdup(filename));
  g_free(checksum);

  return;
}

void write_my_data_into_file(const char *filename, gchar * str){
  FILE *table_meta = g_fopen(filename, "w");
  write_file(table_meta, str, strlen(str));
  fclose(table_meta);
}

void write_table_metadata_into_file(struct db_table * dbt){
  char *filename = build_meta_filename(dbt->database->filename, dbt->table_filename, "metadata");
  FILE *table_meta = g_fopen(filename, "w");
  if (!table_meta) {
    g_critical("Couldn't write table metadata file %s (%d)", filename, errno);
    exit(EXIT_FAILURE);
  }
  fprintf(table_meta, "%"G_GUINT64_FORMAT"\n", dbt->rows);
  fclose(table_meta);
  if (stream) g_async_queue_push(stream_queue, g_strdup(filename));
  g_free(filename);
}

gchar * get_tablespace_query(){
  if ( get_product() == SERVER_TYPE_PERCONA || get_product() == SERVER_TYPE_MYSQL){
    if ( get_major() == 5 && get_secondary() == 7)
      return g_strdup("select NAME, PATH, FS_BLOCK_SIZE from information_schema.INNODB_SYS_TABLESPACES join information_schema.INNODB_SYS_DATAFILES using (space) where SPACE_TYPE='General' and NAME != 'mysql';");
    if ( get_major() == 8 )
      return g_strdup("select NAME,PATH,FS_BLOCK_SIZE,ENCRYPTION from information_schema.INNODB_TABLESPACES join information_schema.INNODB_DATAFILES using (space) where SPACE_TYPE='General' and NAME != 'mysql';");
  }
  return NULL;
}

void write_tablespace_definition_into_file(MYSQL *conn,char *filename){
  void *outfile = NULL;
  char *query = NULL;
  MYSQL_RES *result = NULL;
  MYSQL_ROW row;
  outfile = m_open(filename,"w");
  if (!outfile) {
    g_critical("Error: Could not create output file %s (%d)",
               filename, errno);
    errors++;
    return;
  }
  query=get_tablespace_query();
  if (query == NULL ){
    g_warning("Tablespace resquested, but not possible due to server version not supported");
    return;
  }
  if (mysql_query(conn, query) || !(result = mysql_use_result(conn))) {
    if (success_on_1146 && mysql_errno(conn) == 1146) {
      g_warning("Error dumping create tablespace: %s",
                mysql_error(conn));
    } else {
      g_critical("Error dumping create tablespace: %s",
                 mysql_error(conn));
      errors++;
    }
    g_free(query);
    return;
  }
  GString *statement = g_string_sized_new(statement_size);
  while ((row = mysql_fetch_row(result))) {
    g_string_printf(statement, "CREATE TABLESPACE `%s` ADD DATAFILE '%s' FILE_BLOCK_SIZE = %s ENGINE=INNODB;\n", row[0],row[1],row[2]);
    if (!write_data((FILE *)outfile, statement)) {
      g_critical("Could not write tablespace data for %s", row[0]);
      errors++;
      return;
    }
    g_string_set_size(statement, 0);
  }
}

void write_schema_definition_into_file(MYSQL *conn, char *database, char *filename, char *checksum_filename) {
  void *outfile = NULL;
  char *query = NULL;
  MYSQL_RES *result = NULL;
  MYSQL_ROW row;

  outfile = m_open(filename,"w");

  if (!outfile) {
    g_critical("Error: DB: %s Could not create output file %s (%d)", database,
               filename, errno);
    errors++;
    return;
  }

  GString *statement = g_string_sized_new(statement_size);

  query = g_strdup_printf("SHOW CREATE DATABASE IF NOT EXISTS `%s`", database);
  if (mysql_query(conn, query) || !(result = mysql_use_result(conn))) {
    if (success_on_1146 && mysql_errno(conn) == 1146) {
      g_warning("Error dumping create database (%s): %s", database,
                mysql_error(conn));
    } else {
      g_critical("Error dumping create database (%s): %s", database,
                 mysql_error(conn));
      errors++;
    }
    g_free(query);
    return;
  }

  /* There should never be more than one row */
  row = mysql_fetch_row(result);
  g_string_append(statement, row[1]);
  g_string_append(statement, ";\n");
  if (!write_data((FILE *)outfile, statement)) {
    g_critical("Could not write create database for %s", database);
    errors++;
  }
  g_free(query);

  m_close(outfile);
  if (stream) g_async_queue_push(stream_queue, g_strdup(filename));
  g_string_free(statement, TRUE);
  if (result)
    mysql_free_result(result);


  if (schema_checksums)
    write_checksum_into_file(conn, database, NULL, checksum_filename, checksum_database_defaults);
  return;
}

void write_table_definition_into_file(MYSQL *conn, char *database, char *table,
                      char *filename, char *checksum_filename, char *checksum_index_filename) {
  void *outfile;
  char *query = NULL;
  MYSQL_RES *result = NULL;
  MYSQL_ROW row;
  outfile = m_open(filename,"w");

  if (!outfile) {
    g_critical("Error: DB: %s Could not create output file %s (%d)", database,
               filename, errno);
    errors++;
    return;
  }

  GString *statement = g_string_sized_new(statement_size);

  if (detected_server == SERVER_TYPE_MYSQL) {
    if (set_names_str)
      g_string_printf(statement,"%s;\n",set_names_str);
    g_string_append(statement, "/*!40014 SET FOREIGN_KEY_CHECKS=0*/;\n\n");
    if (!skip_tz) {
      g_string_append(statement, "/*!40103 SET TIME_ZONE='+00:00' */;\n");
    }
  } else if (detected_server == SERVER_TYPE_TIDB) {
    if (!skip_tz) {
      g_string_printf(statement, "/*!40103 SET TIME_ZONE='+00:00' */;\n");
    }
  } else {
    g_string_printf(statement, "SET FOREIGN_KEY_CHECKS=0;\n");
  }

  if (!write_data((FILE *)outfile, statement)) {
    g_critical("Could not write schema data for %s.%s", database, table);
    errors++;
    return;
  }

  query = g_strdup_printf("SHOW CREATE TABLE `%s`.`%s`", database, table);
  if (mysql_query(conn, query) || !(result = mysql_use_result(conn))) {
    if (success_on_1146 && mysql_errno(conn) == 1146) {
      g_warning("Error dumping schemas (%s.%s): %s", database, table,
                mysql_error(conn));
    } else {
      g_critical("Error dumping schemas (%s.%s): %s", database, table,
                 mysql_error(conn));
      errors++;
    }
    g_free(query);
    return;
  }

  g_string_set_size(statement, 0);

  /* There should never be more than one row */
  row = mysql_fetch_row(result);
  g_string_append(statement, row[1]);
  g_string_append(statement, ";\n");
  if (!write_data((FILE *)outfile, statement)) {
    g_critical("Could not write schema for %s.%s", database, table);
    errors++;
  }
  g_free(query);

  m_close(outfile);
  if (stream) g_async_queue_push(stream_queue, g_strdup(filename));
  g_string_free(statement, TRUE);
  if (result)
    mysql_free_result(result);

  if (checksum_filename)
    write_checksum_into_file(conn, database, table, checksum_filename, checksum_table_structure);
  if (checksum_index_filename)
    write_checksum_into_file(conn, database, table, checksum_index_filename, checksum_table_indexes);
  return;
}

void write_triggers_definition_into_file(MYSQL *conn, char *database, char *table, char *filename, char *checksum_filename) {
  void *outfile;
  char *query = NULL;
  MYSQL_RES *result = NULL;
  MYSQL_RES *result2 = NULL;
  MYSQL_ROW row;
  MYSQL_ROW row2;
  gchar **splited_st = NULL;

  outfile = m_open(filename,"w");

  if (!outfile) {
    g_critical("Error: DB: %s Could not create output file %s (%d)", database,
               filename, errno);
    errors++;
    return;
  }

  GString *statement = g_string_sized_new(statement_size);

  // get triggers
  query = g_strdup_printf("SHOW TRIGGERS FROM `%s` LIKE '%s'", database, table);
  if (mysql_query(conn, query) || !(result = mysql_store_result(conn))) {
    if (success_on_1146 && mysql_errno(conn) == 1146) {
      g_warning("Error dumping triggers (%s.%s): %s", database, table,
                mysql_error(conn));
    } else {
      g_critical("Error dumping triggers (%s.%s): %s", database, table,
                 mysql_error(conn));
      errors++;
    }
    g_free(query);
    return;
  }

  while ((row = mysql_fetch_row(result))) {
    set_charset(statement, row[8], row[9]);
    if (!write_data((FILE *)outfile, statement)) {
      g_critical("Could not write triggers data for %s.%s", database, table);
      errors++;
      return;
    }
    g_string_set_size(statement, 0);
    query = g_strdup_printf("SHOW CREATE TRIGGER `%s`.`%s`", database, row[0]);
    mysql_query(conn, query);
    result2 = mysql_store_result(conn);
    row2 = mysql_fetch_row(result2);
    g_string_append_printf(statement, "%s", row2[2]);
    splited_st = g_strsplit(statement->str, ";\n", 0);
    g_string_printf(statement, "%s", g_strjoinv("; \n", splited_st));
    g_string_append(statement, ";\n");
    restore_charset(statement);
    if (!write_data((FILE *)outfile, statement)) {
      g_critical("Could not write triggers data for %s.%s", database, table);
      errors++;
      return;
    }
    g_string_set_size(statement, 0);
  }
  g_free(query);
  m_close(outfile);
  if (stream) g_async_queue_push(stream_queue, g_strdup(filename));
  g_string_free(statement, TRUE);
  g_strfreev(splited_st);
  if (result)
    mysql_free_result(result);
  if (result2)
    mysql_free_result(result2);
  if (checksum_filename)
    write_checksum_into_file(conn, database, table, checksum_filename, checksum_trigger_structure);
  return;
}

void write_view_definition_into_file(MYSQL *conn, char *database, char *table, char *filename, char *filename2, char *checksum_filename) {
  void *outfile, *outfile2;
  char *query = NULL;
  MYSQL_RES *result = NULL;
  MYSQL_ROW row;
  GString *statement = g_string_sized_new(statement_size);

  mysql_select_db(conn, database);

  outfile = m_open(filename,"w");
  outfile2 = m_open(filename2,"w");

  if (!outfile || !outfile2) {
    g_critical("Error: DB: %s Could not create output file (%d)", database,
               errno);
    errors++;
    return;
  }

  if (detected_server == SERVER_TYPE_MYSQL && set_names_str) {
    g_string_printf(statement,"%s;\n",set_names_str);
  }

  if (!write_data((FILE *)outfile, statement)) {
    g_critical("Could not write schema data for %s.%s", database, table);
    errors++;
    return;
  }

  g_string_append_printf(statement, "DROP TABLE IF EXISTS `%s`;\n", table);
  g_string_append_printf(statement, "DROP VIEW IF EXISTS `%s`;\n", table);

  if (!write_data((FILE *)outfile2, statement)) {
    g_critical("Could not write schema data for %s.%s", database, table);
    errors++;
    return;
  }

  // we create tables as workaround
  // for view dependencies
  query = g_strdup_printf("SHOW FIELDS FROM `%s`.`%s`", database, table);
  if (mysql_query(conn, query) || !(result = mysql_use_result(conn))) {
    if (success_on_1146 && mysql_errno(conn) == 1146) {
      g_warning("Error dumping schemas (%s.%s): %s", database, table,
                mysql_error(conn));
    } else {
      g_critical("Error dumping schemas (%s.%s): %s", database, table,
                 mysql_error(conn));
      errors++;
    }
    g_free(query);
    return;
  }
  g_free(query);
  g_string_set_size(statement, 0);
  g_string_append_printf(statement, "CREATE TABLE IF NOT EXISTS `%s`(\n", table);
  row = mysql_fetch_row(result);
  g_string_append_printf(statement, "`%s` int", row[0]);
  while ((row = mysql_fetch_row(result))) {
    g_string_append(statement, ",\n");
    g_string_append_printf(statement, "`%s` int", row[0]);
  }
  g_string_append(statement, "\n);\n");

  if (result)
    mysql_free_result(result);

  if (!write_data((FILE *)outfile, statement)) {
    g_critical("Could not write view schema for %s.%s", database, table);
    errors++;
  }

  // real view
  query = g_strdup_printf("SHOW CREATE VIEW `%s`.`%s`", database, table);
  if (mysql_query(conn, query) || !(result = mysql_use_result(conn))) {
    if (success_on_1146 && mysql_errno(conn) == 1146) {
      g_warning("Error dumping schemas (%s.%s): %s", database, table,
                mysql_error(conn));
    } else {
      g_critical("Error dumping schemas (%s.%s): %s", database, table,
                 mysql_error(conn));
      errors++;
    }
    g_free(query);
    return;
  }
  g_string_set_size(statement, 0);

  /* There should never be more than one row */
  row = mysql_fetch_row(result);
  set_charset(statement, row[2], row[3]);
  if ( skip_definer && g_str_has_prefix(row[1],"CREATE")){
    remove_definer_from_gchar(row[1]);
  }
  g_string_append(statement, row[1]);
  g_string_append(statement, ";\n");
  restore_charset(statement);
  if (!write_data((FILE *)outfile2, statement)) {
    g_critical("Could not write schema for %s.%s", database, table);
    errors++;
  }
  g_free(query);
  m_close(outfile);

  if (stream) g_async_queue_push(stream_queue, g_strdup(filename));
  m_close(outfile2);
  if (stream) g_async_queue_push(stream_queue, g_strdup(filename2));
  g_string_free(statement, TRUE);
  if (result)
    mysql_free_result(result);

  if (checksum_filename)
    // build_meta_filename(database,table,"schema-view-checksum"),
    write_checksum_into_file(conn, database, table, checksum_filename, checksum_view_structure);
  return;
}

// Routines, Functions and Events
// TODO: We need to split it in 3 functions 
void write_routines_definition_into_file(MYSQL *conn, struct database *database, char *filename, char *checksum_filename) {
  void *outfile;
  char *query = NULL;
  MYSQL_RES *result = NULL;
  MYSQL_RES *result2 = NULL;
  MYSQL_ROW row;
  MYSQL_ROW row2;
  gchar **splited_st = NULL;

  outfile = m_open(filename,"w");

  if (!outfile) {
    g_critical("Error: DB: %s Could not create output file %s (%d)", database->name,
               filename, errno);
    errors++;
    return;
  }

  GString *statement = g_string_sized_new(statement_size);

  if (dump_routines) {
    // get functions
    query = g_strdup_printf("SHOW FUNCTION STATUS WHERE CAST(Db AS BINARY) = '%s'", database->escaped);
    if (mysql_query(conn, query) || !(result = mysql_store_result(conn))) {
      if (success_on_1146 && mysql_errno(conn) == 1146) {
        g_warning("Error dumping functions from %s: %s", database->name,
                  mysql_error(conn));
      } else {
        g_critical("Error dumping functions from %s: %s", database->name,
                   mysql_error(conn));
        errors++;
      }
      g_free(query);
      return;
    }

    while ((row = mysql_fetch_row(result))) {
      set_charset(statement, row[8], row[9]);
      g_string_append_printf(statement, "DROP FUNCTION IF EXISTS `%s`;\n",
                             row[1]);
      if (!write_data((FILE *)outfile, statement)) {
        g_critical("Could not write stored procedure data for %s.%s", database->name,
                   row[1]);
        errors++;
        return;
      }
      g_string_set_size(statement, 0);
      query =
          g_strdup_printf("SHOW CREATE FUNCTION `%s`.`%s`", database->name, row[1]);
      mysql_query(conn, query);
      result2 = mysql_store_result(conn);
      row2 = mysql_fetch_row(result2);
      g_string_printf(statement, "%s", row2[2]);
      if ( skip_definer && g_str_has_prefix(statement->str,"CREATE")){
        remove_definer(statement);
      }
      splited_st = g_strsplit(statement->str, ";\n", 0);
      g_string_printf(statement, "%s", g_strjoinv("; \n", splited_st));
      g_string_append(statement, ";\n");
      restore_charset(statement);
      if (!write_data((FILE *)outfile, statement)) {
        g_critical("Could not write function data for %s.%s", database->name, row[1]);
        errors++;
        return;
      }
      g_string_set_size(statement, 0);
    }

    // get sp
    query = g_strdup_printf("SHOW PROCEDURE STATUS WHERE CAST(Db AS BINARY) = '%s'", database->escaped);
    if (mysql_query(conn, query) || !(result = mysql_store_result(conn))) {
      if (success_on_1146 && mysql_errno(conn) == 1146) {
        g_warning("Error dumping stored procedures from %s: %s", database->name,
                  mysql_error(conn));
      } else {
        g_critical("Error dumping stored procedures from %s: %s", database->name,
                   mysql_error(conn));
        errors++;
      }
      g_free(query);
      return;
    }

    while ((row = mysql_fetch_row(result))) {
      set_charset(statement, row[8], row[9]);
      g_string_append_printf(statement, "DROP PROCEDURE IF EXISTS `%s`;\n",
                             row[1]);
      if (!write_data((FILE *)outfile, statement)) {
        g_critical("Could not write stored procedure data for %s.%s", database->name,
                   row[1]);
        errors++;
        return;
      }
      g_string_set_size(statement, 0);
      query =
          g_strdup_printf("SHOW CREATE PROCEDURE `%s`.`%s`", database->name, row[1]);
      mysql_query(conn, query);
      result2 = mysql_store_result(conn);
      row2 = mysql_fetch_row(result2);
      g_string_printf(statement, "%s", row2[2]);
      if ( skip_definer && g_str_has_prefix(statement->str,"CREATE")){
        remove_definer(statement);
      }
      splited_st = g_strsplit(statement->str, ";\n", 0);
      g_string_printf(statement, "%s", g_strjoinv("; \n", splited_st));
      g_string_append(statement, ";\n");
      restore_charset(statement);
      if (!write_data((FILE *)outfile, statement)) {
        g_critical("Could not write stored procedure data for %s.%s", database->name,
                   row[1]);
        errors++;
        return;
      }
      g_string_set_size(statement, 0);
    }
    if (checksum_filename)
      write_checksum_into_file(conn, database->name, NULL, checksum_filename, checksum_process_structure);
  }

  // get events
  if (dump_events) {
    query = g_strdup_printf("SHOW EVENTS FROM `%s`", database->name);
    if (mysql_query(conn, query) || !(result = mysql_store_result(conn))) {
      if (success_on_1146 && mysql_errno(conn) == 1146) {
        g_warning("Error dumping events from %s: %s", database->name,
                  mysql_error(conn));
      } else {
        g_critical("Error dumping events from %s: %s", database->name,
                   mysql_error(conn));
        errors++;
      }
      g_free(query);
      return;
    }

    while ((row = mysql_fetch_row(result))) {
      set_charset(statement, row[12], row[13]);
      g_string_append_printf(statement, "DROP EVENT IF EXISTS `%s`;\n", row[1]);
      if (!write_data((FILE *)outfile, statement)) {
        g_critical("Could not write stored procedure data for %s.%s", database->name,
                   row[1]);
        errors++;
        return;
      }
      query = g_strdup_printf("SHOW CREATE EVENT `%s`.`%s`", database->name, row[1]);
      mysql_query(conn, query);
      result2 = mysql_store_result(conn);
      // DROP EVENT IF EXISTS event_name
      row2 = mysql_fetch_row(result2);
      g_string_printf(statement, "%s", row2[3]);
      if ( skip_definer && g_str_has_prefix(statement->str,"CREATE")){
        remove_definer(statement);
      }
      splited_st = g_strsplit(statement->str, ";\n", 0);
      g_string_printf(statement, "%s", g_strjoinv("; \n", splited_st));
      g_string_append(statement, ";\n");
      restore_charset(statement);
      if (!write_data((FILE *)outfile, statement)) {
        g_critical("Could not write event data for %s.%s", database->name, row[1]);
        errors++;
        return;
      }
      g_string_set_size(statement, 0);
    }
  }

  g_free(query);
  m_close(outfile);
  if (stream) g_async_queue_push(stream_queue, g_strdup(filename));
  g_string_free(statement, TRUE);
  g_strfreev(splited_st);
  if (result)
    mysql_free_result(result);
  if (result2)
    mysql_free_result(result2);

  return;
}

void free_schema_job(struct schema_job *sj){
  if (sj->table){
    g_free(sj->table);
    sj->table=NULL;
  }
  if (sj->filename){
    g_free(sj->filename);
    sj->filename=NULL;
  }
  g_free(sj);
}

void free_view_job(struct view_job *vj){
  if (vj->table)
    g_free(vj->table);
  if (vj->filename)
    g_free(vj->filename);
  if (vj->filename2)
    g_free(vj->filename2);
//  g_free(vj);
}

void free_schema_post_job(struct schema_post_job *sp){
  if (sp->filename)
    g_free(sp->filename);
//  g_free(sp);
}
void free_create_database_job(struct create_database_job * cdj){
  if (cdj->filename)
    g_free(cdj->filename);
  if (cdj->database)
    g_free(cdj->database);
  g_free(cdj);
}

void free_create_tablespace_job(struct create_tablespace_job * ctj){
  if (ctj->filename)
    g_free(ctj->filename);
//  g_free(cdj);
}

void free_table_checksum_job(struct table_checksum_job*tcj){
      if (tcj->table)
        g_free(tcj->table);
      if (tcj->filename)
        g_free(tcj->filename);
      g_free(tcj);
}

void do_JOB_CREATE_DATABASE(struct thread_data *td, struct job *job){
  struct create_database_job * cdj = (struct create_database_job *)job->job_data;
  g_message("Thread %d dumping schema create for `%s`", td->thread_id,
            cdj->database);
  write_schema_definition_into_file(td->thrconn, cdj->database, cdj->filename, cdj->checksum_filename);
  free_create_database_job(cdj);
  g_free(job);
}

void do_JOB_CREATE_TABLESPACE(struct thread_data *td, struct job *job){
  struct create_tablespace_job * ctj = (struct create_tablespace_job *)job->job_data;
  g_message("Thread %d dumping create tablespace if any", td->thread_id);
  write_tablespace_definition_into_file(td->thrconn, ctj->filename);
  free_create_tablespace_job(ctj);
  g_free(job);
}

void do_JOB_SCHEMA_POST(struct thread_data *td, struct job *job){
  struct schema_post_job * sp = (struct schema_post_job *)job->job_data;
  g_message("Thread %d dumping SP and VIEWs for `%s`", td->thread_id,
            sp->database->name);
  write_routines_definition_into_file(td->thrconn, sp->database, sp->filename, sp->checksum_filename);
  free_schema_post_job(sp);
  g_free(job);
}

void do_JOB_VIEW(struct thread_data *td, struct job *job){
  struct view_job * vj = (struct view_job *)job->job_data;
  g_message("Thread %d dumping view for `%s`.`%s`", td->thread_id,
            vj->database, vj->table);
  write_view_definition_into_file(td->thrconn, vj->database, vj->table, vj->filename,
                 vj->filename2, vj->checksum_filename);
//  free_view_job(vj);
  g_free(job);
}

void do_JOB_SCHEMA(struct thread_data *td, struct job *job){
  struct schema_job *sj = (struct schema_job *)job->job_data;
  g_message("Thread %d dumping schema for `%s`.`%s`", td->thread_id,
            sj->database, sj->table);
  write_table_definition_into_file(td->thrconn, sj->database, sj->table, sj->filename, sj->checksum_filename, sj->checksum_index_filename);
//  free_schema_job(sj);
  g_free(job);
//  if (g_atomic_int_dec_and_test(&table_counter)) {
//    g_message("Unlocing ready_table_dump_mutex");
//    g_mutex_unlock(ready_table_dump_mutex);
//  }
}

void do_JOB_TRIGGERS(struct thread_data *td, struct job *job){
  struct schema_job * sj = (struct schema_job *)job->job_data;
  g_message("Thread %d dumping triggers for `%s`.`%s`", td->thread_id,
            sj->database, sj->table);
  write_triggers_definition_into_file(td->thrconn, sj->database, sj->table, sj->filename, sj->checksum_filename);
  free_schema_job(sj);
  g_free(job);
}


void do_JOB_CHECKSUM(struct thread_data *td, struct job *job){
  struct table_checksum_job *tcj = (struct table_checksum_job *)job->job_data;
  g_message("Thread %d dumping checksum for `%s`.`%s`", td->thread_id,
            tcj->database, tcj->table);
  if (use_savepoints && mysql_query(td->thrconn, "SAVEPOINT mydumper")) {
    g_critical("Savepoint failed: %s", mysql_error(td->thrconn));
  }
  write_checksum_into_file(td->thrconn, tcj->database, tcj->table, tcj->filename, checksum_table);
  if (use_savepoints &&
      mysql_query(td->thrconn, "ROLLBACK TO SAVEPOINT mydumper")) {
    g_critical("Rollback to savepoint failed: %s", mysql_error(td->thrconn));
  }
  free_table_checksum_job(tcj);
  g_free(job);
}


void create_job_to_dump_metadata(struct configuration *conf, FILE *mdfile){
  struct job *j = g_new0(struct job, 1);
  j->job_data = (void *)mdfile;
//  j->conf = conf;
  j->type = JOB_WRITE_MASTER_STATUS;
  g_async_queue_push(conf->schema_queue, j);
}

void create_job_to_dump_tablespaces(struct configuration *conf){
  struct job *j = g_new0(struct job, 1);
  struct create_tablespace_job *ctj = g_new0(struct create_tablespace_job, 1);
  j->job_data = (void *)ctj;
//  j->conf = conf;
  j->type = JOB_CREATE_TABLESPACE;
  ctj->filename = build_tablespace_filename();
  g_async_queue_push(conf->schema_queue, j);
}

void create_job_to_dump_schema(char *database, struct configuration *conf) {
  struct job *j = g_new0(struct job, 1);
  struct create_database_job *cdj = g_new0(struct create_database_job, 1);
  j->job_data = (void *)cdj;
  gchar *d=get_ref_table(database);
  cdj->database = g_strdup(database);
//  j->conf = conf;
  j->type = JOB_CREATE_DATABASE;
  cdj->filename = build_schema_filename(d, "schema-create");
  if (schema_checksums)
    cdj->checksum_filename = build_meta_filename(database,NULL,"schema-create-checksum"); 
  g_async_queue_push(conf->schema_queue, j);
  return;
}

void create_job_to_dump_triggers(MYSQL *conn, struct db_table *dbt, struct configuration *conf) {
  if (dump_triggers) {
    char *query = NULL;
    MYSQL_RES *result = NULL;

    query =
        g_strdup_printf("SHOW TRIGGERS FROM `%s` LIKE '%s'", dbt->database->name, dbt->escaped_table);
    if (mysql_query(conn, query) || !(result = mysql_store_result(conn))) {
      g_critical("Error Checking triggers for %s.%s. Err: %s St: %s", dbt->database->name, dbt->table,
                 mysql_error(conn),query);
      errors++;
    } else {
      if (mysql_num_rows(result)) {
        struct job *t = g_new0(struct job, 1);
        struct schema_job *st = g_new0(struct schema_job, 1);
        t->job_data = (void *)st;
        st->database = dbt->database->name;
        st->table = g_strdup(dbt->table);
//        t->conf = conf;
        t->type = JOB_TRIGGERS;
        st->filename = build_schema_table_filename(dbt->database->filename, dbt->table_filename, "schema-triggers");
        if ( routine_checksums )
          st->checksum_filename=build_meta_filename(dbt->database->filename,dbt->table_filename,"schema-triggers-checksum");
        g_async_queue_push(conf->post_data_queue, t);
      }
    }
    g_free(query);
    if (result) {
      mysql_free_result(result);
    }
  }

}

void create_job_to_dump_table_schema(struct db_table *dbt, struct configuration *conf) {
  struct job *j = g_new0(struct job, 1);
  struct schema_job *sj = g_new0(struct schema_job, 1);
  j->job_data = (void *)sj;
  sj->database = dbt->database->name;
  sj->table = g_strdup(dbt->table);
//  j->conf = conf;
  j->type = JOB_SCHEMA;
  sj->filename = build_schema_table_filename(dbt->database->filename, dbt->table_filename, "schema");
  if ( schema_checksums ){
    sj->checksum_filename=build_meta_filename(dbt->database->filename,dbt->table_filename,"schema-checksum");
    sj->checksum_index_filename = build_meta_filename(dbt->database->filename,dbt->table_filename,"schema-indexes-checksum");
  }
  g_async_queue_push(conf->schema_queue, j);
}

void create_job_to_dump_view(struct db_table *dbt, struct configuration *conf) {
  struct job *j = g_new0(struct job, 1);
  struct view_job *vj = g_new0(struct view_job, 1);
  j->job_data = (void *)vj;
  vj->database = dbt->database->name;
  vj->table = g_strdup(dbt->table);
//  j->conf = conf;
  j->type = JOB_VIEW;
  vj->filename  = build_schema_table_filename(dbt->database->filename, dbt->table_filename, "schema");
  vj->filename2 = build_schema_table_filename(dbt->database->filename, dbt->table_filename, "schema-view");
  if ( schema_checksums )
    vj->checksum_filename = build_meta_filename(dbt->database->filename, dbt->table_filename, "schema-view-checksum");
  g_async_queue_push(conf->post_data_queue, j);
  return;
}

void create_job_to_dump_post(struct database *database, struct configuration *conf) {
  struct job *j = g_new0(struct job, 1);
  struct schema_post_job *sp = g_new0(struct schema_post_job, 1);
  j->job_data = (void *)sp;
  sp->database = database;
//  j->conf = conf;
  j->type = JOB_SCHEMA_POST;
  sp->filename = build_schema_filename(sp->database->filename,"schema-post");
  if ( routine_checksums )
    sp->checksum_filename = build_meta_filename(sp->database->filename, NULL, "schema-post-checksum");
  g_async_queue_push(conf->post_data_queue, j);
  return;
}

void create_job_to_dump_checksum(struct db_table * dbt, struct configuration *conf) {
  struct job *j = g_new0(struct job, 1);
  struct table_checksum_job *tcj = g_new0(struct table_checksum_job, 1);
  j->job_data = (void *)tcj;
  tcj->database = dbt->database->name;
  tcj->table = g_strdup(dbt->table);
//  j->conf = conf;
  j->type = JOB_CHECKSUM;
  tcj->filename = build_meta_filename(dbt->database->filename, dbt->table_filename,"checksum");
  g_async_queue_push(conf->post_data_queue, j);
  return;
}

void execute_file_per_thread( gchar *sql_fn, gchar *sql_fn3){
  int childpid=fork();
  if(!childpid){
    FILE *sql_file2 = m_open(sql_fn,"r");
    FILE *sql_file3 = m_open(sql_fn3,"w");
    dup2(fileno(sql_file2), STDIN_FILENO);
    dup2(fileno(sql_file3), STDOUT_FILENO);
    execv(exec_per_thread_cmd[0],exec_per_thread_cmd);
    m_close(sql_file2);
    m_close(sql_file3);
  }
}

void initialize_fn(gchar ** sql_filename, struct db_table * dbt, FILE ** sql_file, guint fn, guint sub_part, const gchar *extension, gchar * f()){
  gchar *stdout_fn=NULL;
/*  if (*sql_filename != NULL){
    remove(*sql_filename);
    g_free(*sql_filename);
  }
*/
  if (use_fifo){
    *sql_filename = build_fifo_filename(dbt->database->filename, dbt->table_filename, fn, sub_part, extension);
    mkfifo(*sql_filename,0666);
    stdout_fn = build_stdout_filename(dbt->database->filename, dbt->table_filename, fn, sub_part, extension, exec_per_thread_extension);
    execute_file_per_thread(*sql_filename,stdout_fn);
  }else{
    *sql_filename = f(dbt->database->filename, dbt->table_filename, fn, sub_part);
  }
  *sql_file = m_open(*sql_filename,"w");
}

void initialize_sql_fn(struct table_job * tj){
  initialize_fn(&(tj->sql_filename),tj->dbt,&(tj->sql_file), tj->nchunk, tj->sub_part,"sql", &build_data_filename);
}

void initialize_load_data_fn(struct table_job * tj){
  initialize_fn(&(tj->dat_filename),tj->dbt,&(tj->dat_file), tj->nchunk, tj->sub_part,"dat", &build_load_data_filename);
}

void update_files_on_table_job(struct table_job *tj){
  if (tj->sql_file == NULL){
     if (load_data){
       initialize_load_data_fn(tj);
       tj->sql_filename = build_data_filename(tj->dbt->database->filename, tj->dbt->table_filename, tj->nchunk, tj->sub_part);
       tj->sql_file = m_open(tj->sql_filename,"w");
     }else{
       initialize_sql_fn(tj);
     }
//     write_load_data_statement(tj, fields, num_fields);
  }

}


struct table_job * new_table_job(struct db_table *dbt, char *partition, guint nchunk, char *order_by, union chunk_step *chunk_step, gboolean update_where){
  struct table_job *tj = g_new0(struct table_job, 1);
// begin Refactoring: We should review this, as dbt->database should not be free, so it might be no need to g_strdup.
  // from the ref table?? TODO
//  tj->database=dbt->database->name;
//  tj->table=g_strdup(dbt->table);
// end
  tj->partition=g_strdup(partition);
  tj->chunk_step = chunk_step;
  tj->where=NULL;
  tj->order_by=g_strdup(order_by);
  tj->nchunk=nchunk;
  tj->sub_part = 0;
  tj->dat_file = NULL;
  tj->dat_filename = NULL;
  tj->sql_file = NULL;
  tj->sql_filename = NULL;
  tj->dbt=dbt;
  tj->st_in_file=0;
  tj->filesize=0;
  tj->char_chunk_part=char_chunk;
  if (update_where)
    update_where_on_table_job(NULL, tj);
  update_files_on_table_job(tj);
  return tj;
}

struct job * create_job_to_dump_chunk_without_enqueuing(struct db_table *dbt, char *partition, guint nchunk, char *order_by, union chunk_step *chunk_step, gboolean update_where){
  struct job *j = g_new0(struct job,1);
  struct table_job *tj = new_table_job(dbt, partition, nchunk, order_by, chunk_step, update_where);
  j->job_data=(void*) tj;
//  j->conf=conf;
  j->type= dbt->is_innodb ? JOB_DUMP : JOB_DUMP_NON_INNODB;
  j->job_data = (void *)tj;
  return j;
}

void create_job_to_dump_chunk(struct db_table *dbt, char *partition, guint nchunk, char *order_by, union chunk_step *chunk_step, void f(), GAsyncQueue *queue, gboolean update_where){
  struct job *j = g_new0(struct job,1);
  struct table_job *tj = new_table_job(dbt, partition, nchunk, order_by, chunk_step, update_where);
  j->job_data=(void*) tj;
  j->type= dbt->is_innodb ? JOB_DUMP : JOB_DUMP_NON_INNODB;
  f(queue,j);
}

void create_job_to_determine_chunk_type(struct db_table *dbt, void f(), GAsyncQueue *queue){
  struct job *j = g_new0(struct job,1);
  j->type = JOB_DETERMINE_CHUNK_TYPE;
  j->job_data=(void*) dbt;
  f(queue,j);
}

void create_job_to_dump_all_databases(struct configuration *conf) {
  g_atomic_int_inc(&database_counter);
  struct job *j = g_new0(struct job, 1);
  j->job_data = NULL;
//  j->conf = conf;
  j->type = JOB_DUMP_ALL_DATABASES;
  g_async_queue_push(conf->initial_queue, j);
  return;
}

void create_job_to_dump_table_list(gchar **table_list, struct configuration *conf) {
  g_atomic_int_inc(&database_counter);
  struct job *j = g_new0(struct job, 1);
  struct dump_table_list_job *dtlj = g_new0(struct dump_table_list_job, 1);
  j->job_data = (void *)dtlj;
  dtlj->table_list = table_list;
  j->type = JOB_DUMP_TABLE_LIST;
  g_async_queue_push(conf->initial_queue, j);
  return;
}

void create_job_to_dump_database(struct database *database, struct configuration *conf) {
  g_atomic_int_inc(&database_counter);
  struct job *j = g_new0(struct job, 1);
  struct dump_database_job *ddj = g_new0(struct dump_database_job, 1);
  j->job_data = (void *)ddj;
  ddj->database = database;
//  j->conf = conf;
  j->type = JOB_DUMP_DATABASE;
  g_async_queue_push(conf->initial_queue, j);
  return;
}

gchar *get_primary_key_string(MYSQL *conn, char *database, char *table) {
  if (!order_by_primary_key) return NULL;

  MYSQL_RES *res = NULL;
  MYSQL_ROW row;

  GString *field_list = g_string_new("");

  gchar *query =
          g_strdup_printf("SELECT k.COLUMN_NAME, ORDINAL_POSITION "
                          "FROM information_schema.table_constraints t "
                          "LEFT JOIN information_schema.key_column_usage k "
                          "USING(constraint_name,table_schema,table_name) "
                          "WHERE t.constraint_type IN ('PRIMARY KEY', 'UNIQUE') "
                          "AND t.table_schema='%s' "
                          "AND t.table_name='%s' "
                          "ORDER BY t.constraint_type, ORDINAL_POSITION; ",
                          database, table);
  mysql_query(conn, query);
  g_free(query);

  res = mysql_store_result(conn);
  gboolean first = TRUE;
  while ((row = mysql_fetch_row(res))) {
    if (first) {
      first = FALSE;
    } else if (atoi(row[1]) > 1) {
      g_string_append(field_list, ",");
    } else {
      break;
    }

    gchar *tb = g_strdup_printf("`%s`", row[0]);
    g_string_append(field_list, tb);
    g_free(tb);
  }
  mysql_free_result(res);
  // Return NULL if we never found a PRIMARY or UNIQUE key
  if (first) {
    g_string_free(field_list, TRUE);
    return NULL;
  } else {
    return g_string_free(field_list, FALSE);
  }
}
