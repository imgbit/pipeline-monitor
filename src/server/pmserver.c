/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pmserver.h"
#include "pmfindings.h"
#include "pmlatency.h"
#include "pmlevels.h"
#include "pmprops.h"
#include "pmmonitor.h"
#include "pmui.h"

#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <string.h>

#define PORT_ATTEMPTS 10
#define UPDATE_INTERVAL_MS 100
#define STATS_INTERVAL_MS 250
#define MAX_COMMAND_SIZE 4096
#define UI_RESOURCE_PATH "/pipeline-monitor/ui/"

struct _PmServer
{
  PmModel *model;
  PmMonitor *monitor;
  PmLatency *latency;
  PmFindings *findings;
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
  /* A page has connected at least once (for wait=true). */
  gboolean had_page;
  /* "bye" was sent: the program is ending. */
  gint bye_sent;
  GError *error;

  /* Server thread only */
  SoupServer *soup;
  GPtrArray *clients;
  gchar *last_graph;
  guint64 log_sent;
  /* Queue levels and pipeline times, sampled every STATS_INTERVAL_MS even
   * without clients. */
  gchar *last_levels;
  /* The findings last sent, to send them again only when they change. */
  gchar *last_findings;
  guint64 ticks;
  /* Watched properties: client -> set of "<element id>\n<property>". */
  GHashTable *watches;

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
set_json_response (SoupServerMessage * msg, gchar * json)
{
  soup_message_headers_replace (soup_server_message_get_response_headers (msg),
      "Cache-Control", "no-store");
  soup_server_message_set_status (msg, SOUP_STATUS_OK, NULL);
  soup_server_message_set_response (msg, "application/json", SOUP_MEMORY_TAKE,
      json, strlen (json));
}

static void
handle_graph (SoupServer * soup, SoupServerMessage * msg, const char *path,
    GHashTable * query, gpointer user_data)
{
  PmServer *self = user_data;

  if (check_request (self, msg))
    set_json_response (msg, pm_model_to_json (self->model));
}

static void
handle_monitors (SoupServer * soup, SoupServerMessage * msg,
    const char *path, GHashTable * query, gpointer user_data)
{
  PmServer *self = user_data;

  if (check_request (self, msg))
    set_json_response (msg, pm_monitor_list_json (self->monitor));
}

static void
handle_levels (SoupServer * soup, SoupServerMessage * msg,
    const char *path, GHashTable * query, gpointer user_data)
{
  PmServer *self = user_data;

  if (check_request (self, msg))
    set_json_response (msg, self->last_levels ? g_strdup (self->last_levels) :
        pm_levels_to_json (self->model));
}

/* How long a request waits for an element's property getters. */
#define PROPERTIES_TIMEOUT_US (300 * G_TIME_SPAN_MILLISECOND)

/* GET /api/properties?element=<id>: all readable properties of an element. */
static void
handle_properties (SoupServer * soup, SoupServerMessage * msg,
    const char *path, GHashTable * query, gpointer user_data)
{
  PmServer *self = user_data;
  const gchar *id = query ? g_hash_table_lookup (query, "element") : NULL;
  guint64 element_id = 0;
  GstElement *element = NULL;

  if (!check_request (self, msg))
    return;
  if (id != NULL)
    g_ascii_string_to_unsigned (id, 10, 1, G_MAXUINT64, &element_id, NULL);
  if (element_id != 0)
    element = pm_model_get_element (self->model, element_id);
  if (element == NULL) {
    soup_server_message_set_status (msg, SOUP_STATUS_NOT_FOUND, NULL);
    return;
  }
  {
    gchar *props = pm_props_read (element, NULL, PROPERTIES_TIMEOUT_US);
    GString *json = g_string_new_len (props, strlen (props) - 1);
    gboolean live;
    GstClockTime min, max;

    /* With the latency the element reports, for its details. */
    if (pm_latency_reported (element, &live, &min, &max)) {
      g_string_append_printf (json, ",\"reported\":{\"live\":%s,\"min\":%"
          G_GUINT64_FORMAT, live ? "true" : "false", min);
      if (GST_CLOCK_TIME_IS_VALID (max))
        g_string_append_printf (json, ",\"max\":%" G_GUINT64_FORMAT, max);
      else
        g_string_append (json, ",\"max\":null");
      g_string_append_c (json, '}');
    }
    g_string_append_c (json, '}');
    g_free (props);
    set_json_response (msg, g_string_free (json, FALSE));
  }
  gst_object_unref (element);
}

static void
handle_findings (SoupServer * soup, SoupServerMessage * msg,
    const char *path, GHashTable * query, gpointer user_data)
{
  PmServer *self = user_data;

  if (check_request (self, msg))
    set_json_response (msg, g_strdup (self->last_findings ?
            self->last_findings : "{\"schema\":1,\"findings\":[]}"));
}

static void
handle_log (SoupServer * soup, SoupServerMessage * msg, const char *path,
    GHashTable * query, gpointer user_data)
{
  PmServer *self = user_data;

  if (check_request (self, msg))
    set_json_response (msg, pm_model_log_to_json (self->model, 0, NULL));
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

static gchar *
log_message (const gchar * log)
{
  return g_strdup_printf ("{\"type\":\"log\",\"log\":%s}", log);
}

static void
send_text (SoupWebsocketConnection * conn, const gchar * text)
{
  if (soup_websocket_connection_get_state (conn) == SOUP_WEBSOCKET_STATE_OPEN)
    soup_websocket_connection_send_text (conn, text);
}

static void
on_ws_message (SoupWebsocketConnection * conn, gint type, GBytes * message,
    PmServer * self);

static void
forget_client (PmServer * self, SoupWebsocketConnection * conn)
{
  g_signal_handlers_disconnect_by_data (conn, self);
  pm_monitor_remove_client (self->monitor, conn);
  pm_latency_remove_client (self->latency, conn);
  g_hash_table_remove (self->watches, conn);
}

/* Property names are made of letters, digits, '-' and '_'. */
static gboolean
valid_property_name (const gchar * name)
{
  const gchar *p;

  if (name == NULL || *name == '\0' || strlen (name) > 128)
    return FALSE;
  for (p = name; *p != '\0'; p++)
    if (!g_ascii_isalnum (*p) && *p != '-' && *p != '_')
      return FALSE;
  return TRUE;
}

static void
watch_property (PmServer * self, SoupWebsocketConnection * conn,
    gint64 element, const gchar * property, gboolean watch)
{
  GHashTable *set = g_hash_table_lookup (self->watches, conn);
  gchar *key = g_strdup_printf ("%" G_GINT64_FORMAT "\n%s", element,
      property);

  if (watch) {
    if (set == NULL) {
      set = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
      g_hash_table_insert (self->watches, conn, set);
    }
    g_hash_table_add (set, key);
  } else {
    if (set != NULL)
      g_hash_table_remove (set, key);
    g_free (key);
  }
}

static void
on_ws_closed (SoupWebsocketConnection * conn, PmServer * self)
{
  forget_client (self, conn);
  if (g_ptr_array_remove (self->clients, conn))
    g_object_unref (conn);
}

static void
send_stats (SoupWebsocketConnection * conn, const gchar * stats)
{
  gchar *text = g_strdup_printf ("{\"type\":\"pad-stats\",\"stats\":%s}",
      stats);

  send_text (conn, text);
  g_free (text);
}

/* Commands from the page: {"type":"monitor","pad":12} starts watching a
 * pad, {"type":"unmonitor","pad":12} stops. Anything else is ignored. */
static void
on_ws_message (SoupWebsocketConnection * conn, gint type, GBytes * message,
    PmServer * self)
{
  JsonParser *parser;
  JsonNode *root, *pad_node;
  JsonObject *object;
  const gchar *command;
  gint64 pad;
  gsize size;
  const gchar *data = g_bytes_get_data (message, &size);

  if (type != SOUP_WEBSOCKET_DATA_TEXT || size > MAX_COMMAND_SIZE)
    return;

  parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, data, size, NULL))
    goto done;
  root = json_parser_get_root (parser);
  if (!JSON_NODE_HOLDS_OBJECT (root))
    goto done;
  object = json_node_get_object (root);
  command = json_object_get_string_member_with_default (object, "type", "");

  /* {"type":"measure-latency","element":12}, {"type":"measure-all"},
   * {"type":"measure-path","from":3,"to":9} and their "un" forms. */
  if (g_str_has_prefix (command, "measure-") ||
      g_str_has_prefix (command, "unmeasure-")) {
    gboolean on = command[0] == 'm';
    const gchar *what = strchr (command, '-') + 1;
    gint64 element = json_object_get_int_member_with_default (object,
        "element", 0);
    gint64 from = json_object_get_int_member_with_default (object, "from", 0);
    gint64 to = json_object_get_int_member_with_default (object, "to", 0);

    if (g_strcmp0 (what, "all") == 0)
      pm_latency_measure_all (self->latency, conn, on);
    else if (g_strcmp0 (what, "latency") == 0 && element > 0)
      pm_latency_measure_element (self->latency, conn, element, on);
    else if (g_strcmp0 (what, "path") == 0 && from > 0 && to > 0)
      pm_latency_measure_path (self->latency, conn, from, to, on);
    goto done;
  }

  /* {"type":"watch-property","element":12,"property":"stats"} */
  if (g_strcmp0 (command, "watch-property") == 0 ||
      g_strcmp0 (command, "unwatch-property") == 0) {
    JsonNode *element = json_object_get_member (object, "element");
    const gchar *property = json_object_get_string_member_with_default
        (object, "property", NULL);

    if (element != NULL && JSON_NODE_HOLDS_VALUE (element) &&
        json_node_get_value_type (element) == G_TYPE_INT64 &&
        json_node_get_int (element) > 0 && valid_property_name (property))
      watch_property (self, conn, json_node_get_int (element), property,
          command[0] == 'w');
    goto done;
  }

  pad_node = json_object_get_member (object, "pad");
  if (pad_node == NULL || !JSON_NODE_HOLDS_VALUE (pad_node) ||
      json_node_get_value_type (pad_node) != G_TYPE_INT64)
    goto done;
  pad = json_node_get_int (pad_node);
  if (pad <= 0)
    goto done;

  if (g_strcmp0 (command, "monitor") == 0) {
    if (!pm_monitor_add (self->monitor, conn, pad)) {
      gchar *gone = g_strdup_printf ("{\"pads\":[{\"pad\":%" G_GINT64_FORMAT
          ",\"gone\":true}]}", pad);
      send_stats (conn, gone);
      g_free (gone);
    }
  } else if (g_strcmp0 (command, "unmonitor") == 0) {
    pm_monitor_remove (self->monitor, conn, pad);
  }

done:
  g_object_unref (parser);
}

static void
add_name (GHashTable * by_element, gint64 id, const gchar * name)
{
  GPtrArray *names = g_hash_table_lookup (by_element, &id);

  if (names == NULL) {
    gint64 *key = g_new (gint64, 1);

    *key = id;
    names = g_ptr_array_new_with_free_func (g_free);
    g_hash_table_insert (by_element, key, names);
  }
  if (!g_ptr_array_find_with_equal_func (names, name, g_str_equal, NULL))
    g_ptr_array_add (names, g_strdup (name));
}

/* Sends one page the elements it watches; results: element id -> JSON
 * from pm_props_read, or NULL if the element is gone (its watches are then
 * dropped). */
static void
send_watched (SoupWebsocketConnection * conn, GHashTable * set,
    GHashTable * results)
{
  GHashTable *sent = g_hash_table_new (g_int64_hash, g_int64_equal);
  GString *json = g_string_new ("{\"type\":\"properties\",\"elements\":[");
  GHashTableIter iter;
  gpointer watch;

  g_hash_table_iter_init (&iter, set);
  while (g_hash_table_iter_next (&iter, &watch, NULL)) {
    gint64 id = g_ascii_strtoll (watch, NULL, 10);
    gpointer id_key, result;

    if (!g_hash_table_lookup_extended (results, &id, &id_key, &result))
      continue;
    if (!g_hash_table_contains (sent, id_key)) {
      g_hash_table_add (sent, id_key);
      if (json->str[json->len - 1] != '[')
        g_string_append_c (json, ',');
      if (result == NULL)
        g_string_append_printf (json, "{\"element\":%" G_GINT64_FORMAT
            ",\"gone\":true}", id);
      else
        g_string_append_printf (json, "{\"element\":%" G_GINT64_FORMAT
            ",\"values\":%s}", id, (gchar *) result);
    }
    if (result == NULL)
      g_hash_table_iter_remove (&iter);
  }
  g_string_append (json, "]}");
  if (g_hash_table_size (sent) > 0)
    send_text (conn, json->str);
  g_string_free (json, TRUE);
  g_hash_table_unref (sent);
}

/* Reads the watched properties, once per element however many pages watch
 * it, and sends each page {"type":"properties","elements":[...]} with its
 * elements; an element that is gone is reported once, then dropped. */
static void
send_properties (PmServer * self)
{
  GHashTable *by_element = g_hash_table_new_full (g_int64_hash,
      g_int64_equal, g_free, (GDestroyNotify) g_ptr_array_unref);
  GHashTable *results = g_hash_table_new_full (g_int64_hash, g_int64_equal,
      NULL, g_free);
  GHashTableIter iter, inner;
  gpointer key, value;

  g_hash_table_iter_init (&iter, self->watches);
  while (g_hash_table_iter_next (&iter, NULL, &value)) {
    g_hash_table_iter_init (&inner, value);
    while (g_hash_table_iter_next (&inner, &key, NULL)) {
      gchar **parts = g_strsplit (key, "\n", 2);

      add_name (by_element, g_ascii_strtoll (parts[0], NULL, 10), parts[1]);
      g_strfreev (parts);
    }
  }

  g_hash_table_iter_init (&iter, by_element);
  while (g_hash_table_iter_next (&iter, &key, &value)) {
    GPtrArray *names = value;
    GstElement *element = pm_model_get_element (self->model,
        *(gint64 *) key);

    if (element == NULL) {
      g_hash_table_insert (results, key, NULL);
      continue;
    }
    g_ptr_array_add (names, NULL);
    g_hash_table_insert (results, key, pm_props_read (element,
            (const gchar * const *) names->pdata, PROPERTIES_TIMEOUT_US));
    gst_object_unref (element);
  }

  g_hash_table_iter_init (&iter, self->watches);
  while (g_hash_table_iter_next (&iter, &key, &value))
    send_watched (key, value, results);
  g_hash_table_unref (results);
  g_hash_table_unref (by_element);
}

static void send_to_all (PmServer * self, const gchar * text);

static void
send_findings (PmServer * self)
{
  gchar *findings = pm_findings_tick (self->findings, PM_STUCK_AFTER_US);
  gchar *text;

  if (g_strcmp0 (findings, self->last_findings) == 0) {
    g_free (findings);
    return;
  }
  text = g_strdup_printf ("{\"type\":\"findings\",\"findings\":%s}",
      findings);
  send_to_all (self, text);
  g_free (text);
  g_free (self->last_findings);
  self->last_findings = findings;
}

static gboolean
send_all_stats (gpointer user_data)
{
  PmServer *self = user_data;
  guint i;

  gchar *levels, *latency;

  pm_monitor_tick (self->monitor);
  g_free (self->last_levels);
  self->last_levels = pm_levels_to_json (self->model);
  levels = g_strdup_printf ("{\"type\":\"levels\",\"levels\":%s}",
      self->last_levels);
  for (i = 0; i < self->clients->len; i++) {
    SoupWebsocketConnection *conn = g_ptr_array_index (self->clients, i);
    gchar *stats = pm_monitor_stats_json (self->monitor, conn);

    send_text (conn, levels);
    if (stats != NULL)
      send_stats (conn, stats);
    g_free (stats);
  }
  g_free (levels);
  send_properties (self);

  /* Findings once per second, sent when they change. */
  if (self->ticks++ % 4 == 0)
    send_findings (self);

  latency = pm_latency_tick (self->latency);
  if (latency != NULL) {
    gchar *text = g_strdup_printf ("{\"type\":\"latency\",\"latency\":%s}",
        latency);

    for (i = 0; i < self->clients->len; i++)
      send_text (g_ptr_array_index (self->clients, i), text);
    g_free (text);
    g_free (latency);
  }
  return G_SOURCE_CONTINUE;
}

static void
handle_ws (SoupServer * soup, SoupServerMessage * msg, const char *path,
    SoupWebsocketConnection * conn, gpointer user_data)
{
  PmServer *self = user_data;
  gchar *graph = pm_model_to_json (self->model);
  gchar *log = pm_model_log_to_json (self->model, 0, NULL);
  gchar *text;

  g_ptr_array_add (self->clients, g_object_ref (conn));
  g_signal_connect (conn, "closed", G_CALLBACK (on_ws_closed), self);
  g_mutex_lock (&self->lock);
  self->had_page = TRUE;
  g_cond_broadcast (&self->cond);
  g_mutex_unlock (&self->lock);
  g_signal_connect (conn, "message", G_CALLBACK (on_ws_message), self);

  /* The whole log once; later updates may repeat some of it, which the
   * page handles by entry id. */
  text = graph_message (graph);
  send_text (conn, text);
  g_free (text);
  text = log_message (log);
  send_text (conn, text);
  g_free (text);
  /* Findings are otherwise only sent when they change. */
  if (self->last_findings != NULL) {
    text = g_strdup_printf ("{\"type\":\"findings\",\"findings\":%s}",
        self->last_findings);
    send_text (conn, text);
    g_free (text);
  }

  g_free (log);
  g_free (graph);
}

static void
send_to_all (PmServer * self, const gchar * text)
{
  guint i;

  for (i = 0; i < self->clients->len; i++)
    send_text (g_ptr_array_index (self->clients, i), text);
}

static gboolean
send_update (gpointer user_data)
{
  PmServer *self = user_data;
  gchar *graph, *log, *text;
  guint64 seq;

  /* Cleared first so changes made while serialising schedule another send. */
  g_atomic_int_set (&self->update_scheduled, 0);

  graph = pm_model_to_json (self->model);
  if (g_strcmp0 (graph, self->last_graph) != 0) {
    text = graph_message (graph);
    send_to_all (self, text);
    g_free (text);
    g_free (self->last_graph);
    self->last_graph = graph;
  } else {
    g_free (graph);
  }

  /* Only entries added or repeated since the last send. */
  log = pm_model_log_to_json (self->model, self->log_sent, &seq);
  if (seq > self->log_sent) {
    text = log_message (log);
    send_to_all (self, text);
    g_free (text);
    self->log_sent = seq;
  }
  g_free (log);

  return G_SOURCE_REMOVE;
}

/* Holds the calling thread until a page has connected (wait=true). */
void
pm_server_wait_for_page (PmServer * self)
{
  g_mutex_lock (&self->lock);
  while (!self->had_page)
    g_cond_wait (&self->cond, &self->lock);
  g_mutex_unlock (&self->lock);
}

/* Shared by the caller and the server thread; freed by the last one. */
typedef struct
{
  gint ref;
  PmServer *server;
  gboolean bye;
  GMutex lock;
  GCond cond;
  gboolean done;
} PmFlush;

static void
flush_unref (PmFlush * flush)
{
  if (!g_atomic_int_dec_and_test (&flush->ref))
    return;
  g_mutex_clear (&flush->lock);
  g_cond_clear (&flush->cond);
  g_free (flush);
}

static gboolean
flush_done (gpointer user_data)
{
  PmFlush *flush = user_data;

  g_mutex_lock (&flush->lock);
  flush->done = TRUE;
  g_cond_signal (&flush->cond);
  g_mutex_unlock (&flush->lock);
  return G_SOURCE_REMOVE;
}

static void send_findings (PmServer * self);

/* On the server thread: everything now, not at the next update. The
 * messages are only queued here; the loop writes them out before
 * flush_done, a little later. */
static gboolean
flush_now (gpointer user_data)
{
  PmFlush *flush = user_data;
  PmServer *self = flush->server;
  GSource *source;

  send_update (self);
  send_findings (self);
  if (flush->bye)
    send_to_all (self, "{\"type\":\"bye\"}");
  source = g_timeout_source_new (100);
  g_source_set_callback (source, flush_done, flush,
      (GDestroyNotify) flush_unref);
  g_source_attach (source, self->context);
  g_source_unref (source);
  return G_SOURCE_REMOVE;
}

/* Sends the current state to the pages at once, and with bye, tells them
 * the program is ending. Waits at most a second: the program may exit
 * right after. Not for the server thread itself. */
void
pm_server_flush (PmServer * self, gboolean bye)
{
  PmFlush *flush;
  gint64 end = g_get_monotonic_time () + G_USEC_PER_SEC;

  if (bye && !g_atomic_int_compare_and_exchange (&self->bye_sent, 0, 1))
    return;
  flush = g_new0 (PmFlush, 1);
  flush->ref = 2;
  flush->server = self;
  flush->bye = bye;
  g_mutex_init (&flush->lock);
  g_cond_init (&flush->cond);
  g_main_context_invoke (self->context, flush_now, flush);
  g_mutex_lock (&flush->lock);
  while (!flush->done && g_cond_wait_until (&flush->cond, &flush->lock,
          end));
  g_mutex_unlock (&flush->lock);
  flush_unref (flush);
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
  GSource *stats_timer;

  g_main_context_push_thread_default (self->context);

  self->soup = soup_server_new ("server-header", "pipeline-monitor ", NULL);
  soup_server_add_handler (self->soup, NULL, handle_ui, self, NULL);
  soup_server_add_handler (self->soup, "/api/graph", handle_graph, self, NULL);
  soup_server_add_handler (self->soup, "/api/log", handle_log, self, NULL);
  soup_server_add_handler (self->soup, "/api/levels", handle_levels, self,
      NULL);
  soup_server_add_handler (self->soup, "/api/findings", handle_findings, self,
      NULL);
  soup_server_add_handler (self->soup, "/api/properties", handle_properties,
      self, NULL);
  soup_server_add_handler (self->soup, "/api/monitors", handle_monitors, self,
      NULL);
  soup_server_add_handler (self->soup, "/ws", handle_ws_request, self, NULL);
  soup_server_add_websocket_handler (self->soup, "/ws", NULL, NULL,
      handle_ws, self, NULL);

  if (!listen (self, &error)) {
    signal_started (self, error);
    goto done;
  }
  signal_started (self, NULL);

  stats_timer = g_timeout_source_new (STATS_INTERVAL_MS);
  g_source_set_callback (stats_timer, send_all_stats, self, NULL);
  g_source_attach (stats_timer, self->context);
  g_main_loop_run (self->loop);
  g_source_destroy (stats_timer);
  g_source_unref (stats_timer);

done:
  while (self->clients->len > 0) {
    SoupWebsocketConnection *conn = g_ptr_array_steal_index (self->clients, 0);

    forget_client (self, conn);
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
  self->monitor = pm_monitor_new (model);
  self->latency = pm_latency_new (model);
  self->findings = pm_findings_new (model);
  /* Lets tests reach the "measure all" limit with a small pipeline. */
  if (g_getenv ("PM_MEASURE_ALL_LIMIT") != NULL)
    pm_latency_set_all_limit (self->latency,
        g_ascii_strtoull (g_getenv ("PM_MEASURE_ALL_LIMIT"), NULL, 10));
  self->ui_dir = g_strdup (ui_dir);
  self->requested_port = port;
  self->context = g_main_context_new ();
  self->loop = g_main_loop_new (self->context, FALSE);
  self->clients = g_ptr_array_new ();
  self->watches = g_hash_table_new_full (NULL, NULL, NULL,
      (GDestroyNotify) g_hash_table_unref);
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
  GSource *source;

  pm_server_flush (self, TRUE);
  source = g_idle_source_new ();

  /* Queued rather than direct so a quit before the loop runs is not lost. */
  g_source_set_callback (source, quit_loop, self->loop, NULL);
  g_source_attach (source, self->context);
  g_source_unref (source);
  g_thread_join (self->thread);

  g_ptr_array_unref (self->clients);
  g_hash_table_unref (self->watches);
  pm_latency_free (self->latency);
  pm_findings_free (self->findings);
  pm_monitor_free (self->monitor);
  g_main_loop_unref (self->loop);
  g_main_context_unref (self->context);
  g_mutex_clear (&self->lock);
  g_cond_clear (&self->cond);
  g_free (self->last_graph);
  g_free (self->ui_dir);
  g_free (self->last_levels);
  g_free (self->last_findings);
  g_free (self->url);
  g_free (self);
}

const gchar *
pm_server_get_url (PmServer * self)
{
  return self->url;
}
