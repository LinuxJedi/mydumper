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

void load_chunks_entries(GOptionGroup *main_group);
GList *get_chunks_for_table(MYSQL *conn, struct db_table * dbt,
                            struct configuration *conf);
guint64 estimate_count(MYSQL *conn, char *database, char *table, char *field,
                       char *from, char *to);

char *get_field_for_dbt(MYSQL *conn, struct db_table * dbt, struct configuration *conf);
void set_chunk_strategy_for_dbt(MYSQL *conn, struct db_table *dbt);
void free_char_step(union chunk_step * cs);
void free_integer_step(union chunk_step * cs);
union chunk_step *get_next_chunk(struct db_table *dbt);
gchar * get_max_char( MYSQL *conn, struct db_table *dbt, char *field, gchar min);
GList * get_partitions_for_table(MYSQL *conn, char *database, char *table);
void *chunk_builder_thread(struct configuration *conf);
void initialize_chunk();

void give_me_another_non_innodb_chunk_step();
void give_me_another_innodb_chunk_step();
gboolean get_new_minmax (struct thread_data *td, struct db_table *dbt, union chunk_step *cs);
gchar* update_cursor (MYSQL *conn, struct table_job *tj);
void next_chunk_in_char_step(union chunk_step * cs);
void update_integer_min(MYSQL *conn, struct table_job *tj);
void update_integer_max(MYSQL *conn, struct table_job *tj);
