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

    Authors:        Aaron Brady, Shopify (insom)
*/

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <pcre.h>
#include <glib.h>
#include <string.h>
#include "config.h"
#include "connection.h"

char *hostname = NULL;
char *username = NULL;
char *password = NULL;
char *socket_path = NULL;
guint port = 0;
gboolean askPassword = FALSE;

GOptionEntry connection_entries[] = {
    {"host", 'h', 0, G_OPTION_ARG_STRING, &hostname, "The host to connect to",
     NULL},
    {"user", 'u', 0, G_OPTION_ARG_STRING, &username,
     "Username with the necessary privileges", NULL},
    {"password", 'p', 0, G_OPTION_ARG_STRING, &password, "User password", NULL},
    {"ask-password", 'a', 0, G_OPTION_ARG_NONE, &askPassword,
     "Prompt For User password", NULL},
    {"port", 'P', 0, G_OPTION_ARG_INT, &port, "TCP/IP port to connect to",
     NULL},
    {"socket", 'S', 0, G_OPTION_ARG_STRING, &socket_path,
     "UNIX domain socket file to use for connection", NULL},
    {NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL}};



extern char *defaults_file;
#ifdef WITH_SSL
extern char *key;
extern char *cert;
extern char *ca;
extern char *capath;
extern char *cipher;
extern char *tls_version;
extern gboolean ssl;
extern gchar *ssl_mode;
#endif
extern guint compress_protocol;

void load_connection_entries(GOptionGroup *main_group){
  g_option_group_add_entries(main_group, connection_entries);
}

void configure_connection(MYSQL *conn, const char *name) {
  if (defaults_file != NULL) {
    mysql_options(conn, MYSQL_READ_DEFAULT_FILE, defaults_file);
  }
  mysql_options(conn, MYSQL_READ_DEFAULT_GROUP, name);
  mysql_options(conn, MYSQL_OPT_LOCAL_INFILE, NULL);
  if (compress_protocol)
    mysql_options(conn, MYSQL_OPT_COMPRESS, NULL);

#ifdef WITH_SSL
#ifdef LIBMARIADB
  my_bool enable= 1;
  if (ssl_mode) {
      if (g_ascii_strncasecmp(ssl_mode, "REQUIRED", 16) == 0) {
        mysql_options(conn, MYSQL_OPT_SSL_ENFORCE, &enable);
      }
      else if (g_ascii_strncasecmp(ssl_mode, "VERIFY_IDENTITY", 16) == 0) {
        mysql_options(conn, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &enable);
      }
      else {
        g_critical("Unsupported ssl-mode specified: %s\n", ssl_mode);
        exit(EXIT_FAILURE);
      }
  }
#else
  unsigned int i;
  if (ssl) {
    i = SSL_MODE_REQUIRED;
    mysql_options(conn, MYSQL_OPT_SSL_MODE, &i);
  } else {
    if (ssl_mode) {
      if (g_ascii_strncasecmp(ssl_mode, "DISABLED", 16) == 0) {
        i = SSL_MODE_DISABLED;
      }
      else if (g_ascii_strncasecmp(ssl_mode, "PREFERRED", 16) == 0) {
        i = SSL_MODE_PREFERRED;
      }
      else if (g_ascii_strncasecmp(ssl_mode, "REQUIRED", 16) == 0) {
        i = SSL_MODE_REQUIRED;
      }
      else if (g_ascii_strncasecmp(ssl_mode, "VERIFY_CA", 16) == 0) {
        i = SSL_MODE_VERIFY_CA;
      }
      else if (g_ascii_strncasecmp(ssl_mode, "VERIFY_IDENTITY", 16) == 0) {
        i = SSL_MODE_VERIFY_IDENTITY;
      }
      else {
        g_critical("Unsupported ssl-mode specified: %s\n", ssl_mode);
        exit(EXIT_FAILURE);
      }
      mysql_options(conn, MYSQL_OPT_SSL_MODE, &i);
    }
  }
#endif // LIBMARIADB
  if (key) {
    mysql_options(conn, MYSQL_OPT_SSL_KEY, key);
  }
  if (cert) {
    mysql_options(conn, MYSQL_OPT_SSL_CERT, cert);
  }
  if (ca) {
    mysql_options(conn, MYSQL_OPT_SSL_CA, ca);
  }
  if (capath) {
    mysql_options(conn, MYSQL_OPT_SSL_CAPATH, capath);
  }
  if (cipher) {
    mysql_options(conn, MYSQL_OPT_SSL_CIPHER, cipher);
  }
  if (tls_version) {
    mysql_options(conn, MYSQL_OPT_TLS_VERSION, tls_version);
  }
#endif
}

void m_connect(MYSQL *conn, const gchar *app, gchar *schema){
  configure_connection(conn, app);
  if (!mysql_real_connect(conn, hostname, username, password, schema, port,
                          socket_path, 0)) {
    g_critical("Error connection to database: %s", mysql_error(conn));
    exit(EXIT_FAILURE);
  }
}

void hide_password(int argc, char *argv[]){
  if (password != NULL){
    int i=1;
    for(i=1; i < argc; i++){
      gchar * p= g_strstr_len(argv[i],-1,password);
      if (p != NULL){
        strncpy(p, "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX", strlen(password));
      }
    }
  }
}

char *passwordPrompt(void) {
  char *p;
  p = getpass("Enter MySQL Password: ");

  return p;
}

void ask_password(){
  // prompt for password if it's NULL
  if (sizeof(password) == 0 || (password == NULL && askPassword)) {
    password = passwordPrompt();
  }
}

