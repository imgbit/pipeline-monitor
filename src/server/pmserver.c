/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pmserver.h"
#include "pmui.h"

#include <libsoup/soup.h>
#include <string.h>

#define PORT_ATTEMPTS 10
#define UPDATE_INTERVAL_MS 100
#define UI_RESOURCE_PATH "/pipeline-monitor/ui/"

struct _PmServer
{
  PmModel *model;
  gchar *ui_dir;
  guint requested_port;
  guint port;
  gchar *url;

  GThread *thread;
  GMainContext *context;
  GMainLoop *loop;

  GMutex lock;
  GCond cond;
  gboolean started;
  GError *error;

  /* Server thread only */
  SoupServer *soup;
  GPtrArray *clients;
  gchar *last_graph;

  gint update_scheduled;
};

static gboolean
host_allowed (PmServer * self, const gchar * host)
{
  gchar *ip = g_strdup_printf ("127.0.0.1:%u", self->port);
  gchar *name = g_strdup_printf ("localhost:%u", self->port);
  gboolean allowed = host != NULL && (g_ascii_strcasecmp (host, ip) == 0 ||
      g_ascii_strcasecmp (host, name) == 0);

  g_free (ip);
  g_free (name);
  return allowed;
}

/* Only pages served by this server may talk to it. Non-browser clients
 * send no Origin. The Host check stops DNS rebinding. */
static gboolean
request_allowed (PmServer * self, SoupServerMessage * msg)
{
  SoupMessageHeaders *headers = soup_server_message_get_request_headers (msg);
  const gchar *origin = soup_message_headers_get_one (headers, "Origin");

  if (!host_allowed (self, soup_message_headers_get_one (headers, "Host")))
    return FALSE;
  if (origin == NULL)
    return TRUE;
  return g_str_has_prefix (origin, "http://")
      && host_allowed (self, origin + strlen ("http://"));
}

static gboolean
check_request (PmServer * self, SoupServerMessage * msg)
{
  const gchar *method = soup_server_message_get_method (msg);

  if (!request_allowed (self, msg)) {
    soup_server_message_set_status (msg, SOUP_STATUS_FORBIDDEN, NULL);
    return FALSE;
  }
  if (strcmp (method, "GET") != 0 && strcmp (method, "HEAD") != 0) {
    soup_server_message_set_status (msg, SOUP_STATUS_METHOD_NOT_ALLOWED, NULL);
    return FALSE;
  }
  return TRUE;
}

static void
set_response (SoupServerMessage * msg, const gchar * content_type,
    GBytes * body)
{
  gsize size;
  gconstpointer data = g_bytes_get_data (body, &size);

  soup_message_headers_replace (soup_server_message_get_response_headers (msg),
      "Cache-Control", "no-cache");
  soup_server_message_set_status (msg, SOUP_STATUS_OK, NULL);
  soup_server_message_set_response (msg, content_type, SOUP_MEMORY_COPY, data,
      size);
}

static const gchar *
content_type_for (const gchar * path)
{
  if (g_str_has_suffix (path, ".html"))
    return "text/html; charset=utf-8";
  if (g_str_has_suffix (path, ".js"))
    return "text/javascript; charset=utf-8";
  if (g_str_has_suffix (path, ".css"))
    return "text/css; charset=utf-8";
  if (g_str_has_suffix (path, ".svg"))
    return "image/svg+xml";
  return NULL;
}

/* Plain relative paths only: no "..", no hidden files, no empty parts. */
static gboolean
ui_path_valid (const gchar * path)
{
  gchar **parts = g_strsplit (path, "/", -1);
  gboolean valid = TRUE;
  guint i;

  for (i = 0; parts[i] != NULL && valid; i++) {
    const gchar *c;

    valid = parts[i][0] != '\0' && parts[i][0] != '.';
    for (c = parts[i]; *c != '\0' && valid; c++)
      valid = g_ascii_isalnum (*c) || *c == '.' || *c == '-' || *c == '_';
  }
  g_strfreev (parts);
  return valid;
}

static GBytes *
load_ui_file (PmServer * self, const gchar * path)
{
  gchar *contents, *filename, *resource;
  gsize size;
  GBytes *bytes = NULL;

  if (self->ui_dir != NULL) {
    filename = g_build_filename (self->ui_dir, path, NULL);
    if (g_file_get_contents (filename, &contents, &size, NULL))
      bytes = g_bytes_new_take (contents, size);
    g_free (filename);
    return bytes;
  }

  if (pm_ui_get_resource () == NULL)
    return NULL;
  resource = g_strconcat (UI_RESOURCE_PATH, path, NULL);
  bytes = g_resource_lookup_data (pm_ui_get_resource (), resource,
      G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
  g_free (resource);
  return bytes;
}

static void
handle_ui (SoupServer * soup, SoupServerMessage * msg, const char *path,
    GHashTable * query, gpointer user_data)
{
  PmServer *self = user_data;
  const gchar *file = strcmp (path, "/") == 0 ? "index.html" : path + 1;
  const gchar *content_type = content_type_for (file);
  GBytes *body;

  if (!check_request (self, msg))
    return;
  if (content_type == NULL || !ui_path_valid (file)
      || (body = load_ui_file (self, file)) == NULL) {
    soup_server_message_set_status (msg, SOUP_STATUS_NOT_FOUND, NULL);
    return;
  }
  set_response (msg, content_type, body);
  g_bytes_unref (body);
}

static void
handle_graph (SoupServer * soup, SoupServerMessage * msg, const char *path,
    GHashTable * query, gpointer user_data)
{
  PmServer *self = user_data;
  gchar *graph;

  if (!check_request (self, msg))
    return;
  graph = pm_model_to_json (self->model);
  soup_message_headers_replace (soup_server_message_get_response_headers (msg),
      "Cache-Control", "no-store");
  soup_server_message_set_status (msg, SOUP_STATUS_OK, NULL);
  soup_server_message_set_response (msg, "application/json", SOUP_MEMORY_TAKE,
      graph, strlen (graph));
}

/* Runs before the WebSocket handshake; setting a status rejects it. */
static void
handle_ws_request (SoupServer * soup, SoupServerMessage * msg,
    const char *path, GHashTable * query, gpointer user_data)
{
  check_request (user_data, msg);
}

static gchar *
graph_message (const gchar * graph)
{
  return g_strdup_printf ("{\"type\":\"graph\",\"graph\":%s}", graph);
}

static void
send_text (SoupWebsocketConnection * conn, const gchar * text)
{
  if (soup_websocket_connection_get_state (conn) == SOUP_WEBSOCKET_STATE_OPEN)
    soup_websocket_connection_send_text (conn, text);
}

static void
on_ws_closed (SoupWebsocketConnection * conn, PmServer * self)
{
  g_signal_handlers_disconnect_by_func (conn, on_ws_closed, self);
  if (g_ptr_array_remove (self->clients, conn))
    g_object_unref (conn);
}

static void
handle_ws (SoupServer * soup, SoupServerMessage * msg, const char *path,
    SoupWebsocketConnection * conn, gpointer user_data)
{
  PmServer *self = user_data;
  gchar *graph = pm_model_to_json (self->model);
  gchar *text = graph_message (graph);

  g_ptr_array_add (self->clients, g_object_ref (conn));
  g_signal_connect (conn, "closed", G_CALLBACK (on_ws_closed), self);
  send_text (conn, text);

  g_free (text);
  g_free (graph);
}

static gboolean
send_update (gpointer user_data)
{
  PmServer *self = user_data;
  gchar *graph, *text;
  guint i;

  /* Cleared first so changes made while serialising schedule another send. */
  g_atomic_int_set (&self->update_scheduled, 0);

  graph = pm_model_to_json (self->model);
  if (g_strcmp0 (graph, self->last_graph) == 0) {
    g_free (graph);
    return G_SOURCE_REMOVE;
  }

  text = graph_message (graph);
  for (i = 0; i < self->clients->len; i++)
    send_text (g_ptr_array_index (self->clients, i), text);
  g_free (text);

  g_free (self->last_graph);
  self->last_graph = graph;
  return G_SOURCE_REMOVE;
}

void
pm_server_notify (PmServer * self)
{
  GSource *source;

  if (!g_atomic_int_compare_and_exchange (&self->update_scheduled, 0, 1))
    return;

  source = g_timeout_source_new (UPDATE_INTERVAL_MS);
  g_source_set_callback (source, send_update, self, NULL);
  g_source_attach (source, self->context);
  g_source_unref (source);
}

static gboolean
listen (PmServer * self, GError ** error)
{
  guint port = self->requested_port;
  guint last = port == 0 ? 0 : MIN (port + PORT_ATTEMPTS - 1, G_MAXUINT16);
  GError *local = NULL;
  GSList *uris;

  for (; port <= last; port++) {
    g_clear_error (&local);
    if (soup_server_listen_local (self->soup, port,
            SOUP_SERVER_LISTEN_IPV4_ONLY, &local))
      break;
  }
  if (local != NULL) {
    g_propagate_error (error, local);
    return FALSE;
  }

  uris = soup_server_get_uris (self->soup);
  self->port = g_uri_get_port (uris->data);
  g_slist_free_full (uris, (GDestroyNotify) g_uri_unref);
  self->url = g_strdup_printf ("http://127.0.0.1:%u/", self->port);
  return TRUE;
}

static void
signal_started (PmServer * self, GError * error)
{
  g_mutex_lock (&self->lock);
  self->error = error;
  self->started = TRUE;
  g_cond_signal (&self->cond);
  g_mutex_unlock (&self->lock);
}

static gpointer
server_thread (gpointer user_data)
{
  PmServer *self = user_data;
  GError *error = NULL;

  g_main_context_push_thread_default (self->context);

  self->soup = soup_server_new ("server-header", "pipeline-monitor ", NULL);
  soup_server_add_handler (self->soup, NULL, handle_ui, self, NULL);
  soup_server_add_handler (self->soup, "/api/graph", handle_graph, self, NULL);
  soup_server_add_handler (self->soup, "/ws", handle_ws_request, self, NULL);
  soup_server_add_websocket_handler (self->soup, "/ws", NULL, NULL,
      handle_ws, self, NULL);

  if (!listen (self, &error)) {
    signal_started (self, error);
    goto done;
  }
  signal_started (self, NULL);

  g_main_loop_run (self->loop);

done:
  while (self->clients->len > 0) {
    SoupWebsocketConnection *conn = g_ptr_array_steal_index (self->clients, 0);

    g_signal_handlers_disconnect_by_func (conn, on_ws_closed, self);
    g_object_unref (conn);
  }
  soup_server_disconnect (self->soup);
  g_clear_object (&self->soup);
  while (g_main_context_iteration (self->context, FALSE));
  g_main_context_pop_thread_default (self->context);
  return NULL;
}

static gboolean
quit_loop (gpointer user_data)
{
  g_main_loop_quit (user_data);
  return G_SOURCE_REMOVE;
}

PmServer *
pm_server_new (PmModel * model, guint port, const gchar * ui_dir,
    GError ** error)
{
  PmServer *self = g_new0 (PmServer, 1);

  self->model = model;
  self->ui_dir = g_strdup (ui_dir);
  self->requested_port = port;
  self->context = g_main_context_new ();
  self->loop = g_main_loop_new (self->context, FALSE);
  self->clients = g_ptr_array_new ();
  g_mutex_init (&self->lock);
  g_cond_init (&self->cond);

  self->thread = g_thread_new ("pm-server", server_thread, self);

  g_mutex_lock (&self->lock);
  while (!self->started)
    g_cond_wait (&self->cond, &self->lock);
  g_mutex_unlock (&self->lock);

  if (self->error != NULL) {
    g_propagate_error (error, self->error);
    self->error = NULL;
    pm_server_free (self);
    return NULL;
  }
  return self;
}

void
pm_server_free (PmServer * self)
{
  GSource *source = g_idle_source_new ();

  /* Queued rather than direct so a quit before the loop runs is not lost. */
  g_source_set_callback (source, quit_loop, self->loop, NULL);
  g_source_attach (source, self->context);
  g_source_unref (source);
  g_thread_join (self->thread);

  g_ptr_array_unref (self->clients);
  g_main_loop_unref (self->loop);
  g_main_context_unref (self->context);
  g_mutex_clear (&self->lock);
  g_cond_clear (&self->cond);
  g_free (self->last_graph);
  g_free (self->ui_dir);
  g_free (self->url);
  g_free (self);
}

const gchar *
pm_server_get_url (PmServer * self)
{
  return self->url;
}
