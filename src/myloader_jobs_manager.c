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

        Authors:    David Ducos, Percona (david dot ducos at percona dot com)
*/
#include <mysql.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "myloader_stream.h"
#include "common.h"
#include "myloader.h"
#include "myloader_common.h"
#include "myloader_process.h"
#include "myloader_jobs_manager.h"
#include "myloader_directory.h"
#include "myloader_restore.h"
#include "myloader_restore_job.h"
#include "myloader_control_job.h"
#include "connection.h"
#include <errno.h>

extern gchar *db;
extern gchar *set_names_str;
extern GString *set_session;
extern guint num_threads;
extern gboolean stream;
extern guint max_threads_for_index_creation;

static GMutex *init_mutex=NULL;
static GMutex *index_mutex=NULL;
guint index_threads_counter = 0;
guint sync_threads_remaining;
static GMutex *sync_mutex;

guint sync_threads_remaining2;
static GMutex *sync_mutex2;
GMutex *view_mutex;

void initialize_job(gchar * purge_mode_str){
  initialize_restore_job(purge_mode_str);
  init_mutex = g_mutex_new();
  index_mutex = g_mutex_new();
  sync_threads_remaining=num_threads;
  sync_mutex = g_mutex_new();
  g_mutex_lock(sync_mutex);
  view_mutex=g_mutex_new();

  sync_threads_remaining2=num_threads;
  sync_mutex2 = g_mutex_new();
  g_mutex_lock(sync_mutex2);

  index_threads_counter = 0;
}


gboolean process_index(struct thread_data * td){
  gboolean b=FALSE;
  g_mutex_lock(index_mutex);
  if ( max_threads_for_index_creation > index_threads_counter){
    struct control_job *job=g_async_queue_try_pop(td->conf->index_queue);
    if (job != NULL){
      struct db_table *dbt=job->data.restore_job->dbt;
      index_threads_counter++;
      g_mutex_unlock(index_mutex);
      execute_use_if_needs_to(td, job->use_database, "Restoring index");
      dbt->start_index_time=g_date_time_new_now_local();
//      g_message("restoring index: %s.%s", dbt->database, dbt->table);
      b=process_job(td, job);
      dbt->finish_time=g_date_time_new_now_local();
//      job->data.restore_job->dbt->index_completed=TRUE;
      g_mutex_lock(index_mutex);
      index_threads_counter--;
      b=TRUE;
    }
  }
  g_mutex_unlock(index_mutex);
  return b;
}

void sync_threads(guint *counter, GMutex *mutex){
  if (g_atomic_int_dec_and_test(counter)){
    g_mutex_unlock(mutex);
  }else{
    g_mutex_lock(mutex);
    g_mutex_unlock(mutex);
  }
}

void *loader_thread(struct thread_data *td) {
  struct configuration *conf = td->conf;
  g_mutex_lock(init_mutex);
  td->thrconn = mysql_init(NULL);
  g_mutex_unlock(init_mutex);
  td->current_database=NULL;

  m_connect(td->thrconn, "myloader", NULL);

  mysql_query(td->thrconn, set_names_str);
  mysql_query(td->thrconn, "/*!40101 SET SQL_MODE='NO_AUTO_VALUE_ON_ZERO' */");
  mysql_query(td->thrconn, "/*!40014 SET UNIQUE_CHECKS=0 */");
  mysql_query(td->thrconn, "/*!40014 SET FOREIGN_KEY_CHECKS=0*/");

  execute_gstring(td->thrconn, set_session);
  g_async_queue_push(conf->ready, GINT_TO_POINTER(1));

  if (db){
    td->current_database=db;
    if (execute_use(td, "Initializing thread")){
      g_critical("Changing to database: %s %s", td->current_database,db);
      exit(EXIT_FAILURE);
    }
  }

  g_debug("Thread %d: Starting import", td->thread_id);
  process_stream_queue(td);
  struct control_job *job = NULL;
  gboolean cont=TRUE;

  cont=TRUE;
  while (cont){
    cont=process_index(td);
  }
  sync_threads(&sync_threads_remaining,sync_mutex);
  g_message("Thread %d: Starting post import task over table", td->thread_id);
  cont=TRUE;
  while (cont){
    job = (struct control_job *)g_async_queue_pop(conf->post_table_queue);
    execute_use_if_needs_to(td, job->use_database, "Restoring post table");
    g_mutex_lock(view_mutex);
    cont=process_job(td, job);
    g_mutex_unlock(view_mutex);
  }

//  g_message("Thread %d: Starting post import task: triggers, procedures and triggers", td->thread_id);
  cont=TRUE;
  while (cont){
    job = (struct control_job *)g_async_queue_pop(conf->post_queue);
    execute_use_if_needs_to(td, job->use_database, "Restoring post tasks");
    g_mutex_lock(view_mutex);
    cont=process_job(td, job);
    g_mutex_unlock(view_mutex);
  }
  sync_threads(&sync_threads_remaining2,sync_mutex2);
  cont=TRUE;
  while (cont){
    job = (struct control_job *)g_async_queue_pop(conf->view_queue);
    execute_use_if_needs_to(td, job->use_database, "Restoring view tasks");
    g_mutex_lock(view_mutex);
    cont=process_job(td, job);
    g_mutex_unlock(view_mutex);
  }

  if (td->thrconn)
    mysql_close(td->thrconn);
  mysql_thread_end();
  g_debug("Thread %d: ending", td->thread_id);
  return NULL;
}

GThread **threads = NULL;
struct thread_data *td = NULL;

void initialize_loader_threads(struct configuration *conf){
  guint n=0;
  threads = g_new(GThread *, num_threads);
  td = g_new(struct thread_data, num_threads);
  for (n = 0; n < num_threads; n++) {
    td[n].conf = conf;
    td[n].thread_id = n + 1;
    threads[n] =
        g_thread_create((GThreadFunc)loader_thread, &td[n], TRUE, NULL);
    // Here, the ready queue is being used to serialize the connection to the database.
    // We don't want all the threads try to connect at the same time
    g_async_queue_pop(conf->ready);
  }
}

void wait_loader_threads_to_finish(){
  guint n=0;
  for (n = 0; n < num_threads; n++) {
    g_thread_join(threads[n]);
  }
  restore_job_finish(); 
/*  if (shutdown_triggered)
    g_async_queue_push(file_list_to_do, g_strdup("NO_MORE_FILES"));
    */
}

void free_loader_threads(){
  g_free(td);
  g_free(threads);
}
