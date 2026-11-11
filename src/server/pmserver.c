/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pmserver.h"
#include "pmfindings.h"
#include "pmjson.h"
#include "pmlatency.h"
#include "pmlevels.h"
#include "pmprops.h"
#include "pmmonitor.h"
#include "pmrecorder.h"
#include "pmui.h"

#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <string.h>
#ifdef G_OS_WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#define PORT_ATTEMPTS 10
#define UPDATE_INTERVAL_MS 100
#define STATS_INTERVAL_MS 250
#define MAX_COMMAND_SIZE 65536
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
  /* Until then the findings are not updated (monotonic µs). */
  gint64 findings_held_until;
  guint64 ticks;
  /* Watched properties: client -> set of "<element id>\n<property>". */
  GHashTable *watches;

  /* Recordings. tapping: a recording (not a snapshot) is running and gets
   * what is sent; state_thread reads all properties when one starts. */
  PmRecorder *recorder;
  gboolean tapping;
  GThread *state_thread;
  /* Set while state_thread reads; elements whose properties the running
   * recording already has (they are read once, also when added later). */
  gint state_busy;
  GHashTable *recorded;
  /* Whether a recording was running at the last update to the pages. */
  gboolean was_running;
  /* Serving recordings only (the pipeline-monitor command), and the file it
   * was given, if any. */
  gboolean viewer;
  gchar *opened;

  gint update_scheduled;
  /* Set by pm_server_hold_findings, taken by the next flush; and by
   * pm_server_release_findings, taken by the next update. */
  gint hold_findings;
  gint release_findings;
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

/* Sends message (type, body) as the answer; takes body. */
static void
set_message_response (SoupServerMessage * msg, const gchar * type,
    gchar * body)
{
  set_json_response (msg, pm_json_message (type, body));
  g_free (body);
}

/* {"version","gstreamer","program","pid"[,"viewer"]}: what the page talks
 * to. */
static gchar *
hello_body (PmServer * self)
{
  guint major, minor, micro, nano;
  GString *json = g_string_new ("{\"version\":");
  gchar *program;

  gst_version (&major, &minor, &micro, &nano);
  pm_json_append_string (json, VERSION);
  g_string_append_printf (json, ",\"gstreamer\":\"%u.%u.%u\",\"program\":",
      major, minor, micro);
  program = g_get_prgname () ? g_path_get_basename (g_get_prgname ()) : NULL;
  pm_json_append_string (json, program);
  g_free (program);
  g_string_append_printf (json, ",\"pid\":%d%s}", (gint) getpid (),
      self->viewer ? ",\"viewer\":true" : "");
  return g_string_free (json, FALSE);
}

static void
handle_hello (SoupServer * soup, SoupServerMessage * msg, const char *path,
    GHashTable * query, gpointer user_data)
{
  if (check_request (user_data, msg))
    set_message_response (msg, "hello", hello_body (user_data));
}

static void
handle_graph (SoupServer * soup, SoupServerMessage * msg, const char *path,
    GHashTable * query, gpointer user_data)
{
  PmServer *self = user_data;

  if (check_request (self, msg))
    set_message_response (msg, "graph", pm_model_to_json (self->model));
}

static void
handle_watched_pads (SoupServer * soup, SoupServerMessage * msg,
    const char *path, GHashTable * query, gpointer user_data)
{
  PmServer *self = user_data;

  if (check_request (self, msg))
    set_message_response (msg, "watched-pads",
        pm_monitor_list_json (self->monitor));
}

static void
handle_levels (SoupServer * soup, SoupServerMessage * msg,
    const char *path, GHashTable * query, gpointer user_data)
{
  PmServer *self = user_data;

  if (check_request (self, msg))
    set_message_response (msg, "levels", self->last_levels ?
        g_strdup (self->last_levels) : pm_levels_to_json (self->model));
}

/* How long a request waits for an element's property getters. */
#define PROPERTIES_TIMEOUT_US (300 * G_TIME_SPAN_MILLISECOND)

/* GET /api/opened: the file the pipeline-monitor command was given. */
static void
handle_opened (SoupServer * soup, SoupServerMessage * msg, const char *path,
    GHashTable * query, gpointer user_data)
{
  PmServer *self = user_data;
  GMappedFile *mapped;
  GBytes *bytes;

  if (!check_request (self, msg))
    return;
  mapped = self->opened ? g_mapped_file_new (self->opened, FALSE, NULL) :
      NULL;
  if (mapped == NULL) {
    soup_server_message_set_status (msg, SOUP_STATUS_NOT_FOUND, NULL);
    return;
  }
  bytes = g_mapped_file_get_bytes (mapped);
  set_response (msg, "application/jsonl", bytes);
  g_bytes_unref (bytes);
  g_mapped_file_unref (mapped);
}

/* GET /api/recordings: the list; GET /api/recordings/<name>: the file. */
static void
handle_recordings (SoupServer * soup, SoupServerMessage * msg,
    const char *path, GHashTable * query, gpointer user_data)
{
  PmServer *self = user_data;
  const gchar *prefix = "/api/recordings/";
  gchar *file, *disposition;
  GMappedFile *mapped;
  GBytes *bytes;

  if (!check_request (self, msg))
    return;
  if (g_strcmp0 (path, "/api/recordings") == 0) {
    set_message_response (msg, "recordings",
        pm_recorder_list_json (self->recorder));
    return;
  }
  file = g_str_has_prefix (path, prefix) ?
      pm_recorder_path (self->recorder, path + strlen (prefix)) : NULL;
  mapped = file ? g_mapped_file_new (file, FALSE, NULL) : NULL;
  g_free (file);
  if (mapped == NULL) {
    soup_server_message_set_status (msg, SOUP_STATUS_NOT_FOUND, NULL);
    return;
  }
  bytes = g_mapped_file_get_bytes (mapped);
  set_response (msg, "application/jsonl", bytes);
  disposition = g_strdup_printf ("attachment; filename=\"%s\"",
      path + strlen (prefix));
  soup_message_headers_replace (soup_server_message_get_response_headers
      (msg), "Content-Disposition", disposition);
  g_free (disposition);
  g_bytes_unref (bytes);
  g_mapped_file_unref (mapped);
}

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
    GString *json = g_string_new (NULL);
    gboolean live;
    GstClockTime min, max;

    /* As in a "properties" message, with the latency the element reports,
     * for its details. */
    gchar *element_path = pm_model_path (self->model, element_id);

    g_string_append_printf (json, "{\"elements\":[{\"element\":%"
        G_GUINT64_FORMAT ",", element_id);
    pm_json_append_member_string (json, "path", element_path);
    g_free (element_path);
    g_string_append_len (json, props + 1, strlen (props) - 2);
    if (pm_latency_reported (element, &live, &min, &max)) {
      g_string_append_printf (json, ",\"reported\":{\"live\":%s,\"min\":%"
          G_GUINT64_FORMAT, live ? "true" : "false", min);
      if (GST_CLOCK_TIME_IS_VALID (max))
        g_string_append_printf (json, ",\"max\":%" G_GUINT64_FORMAT, max);
      else
        g_string_append (json, ",\"max\":null");
      g_string_append_c (json, '}');
    }
    g_string_append (json, "}]}");
    g_free (props);
    set_message_response (msg, "properties", g_string_free (json, FALSE));
  }
  gst_object_unref (element);
}

static void
handle_findings (SoupServer * soup, SoupServerMessage * msg,
    const char *path, GHashTable * query, gpointer user_data)
{
  PmServer *self = user_data;

  if (check_request (self, msg))
    set_message_response (msg, "findings", g_strdup (self->last_findings ?
            self->last_findings : "{\"findings\":[]}"));
}

static void
handle_log (SoupServer * soup, SoupServerMessage * msg, const char *path,
    GHashTable * query, gpointer user_data)
{
  PmServer *self = user_data;

  if (check_request (self, msg))
    set_message_response (msg, "log", pm_model_log_to_json (self->model, 0,
            NULL));
}

/* Runs before the WebSocket handshake; setting a status rejects it. */
static void
handle_ws_request (SoupServer * soup, SoupServerMessage * msg,
    const char *path, GHashTable * query, gpointer user_data)
{
  check_request (user_data, msg);
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
  gchar *text = pm_json_message ("pads", stats);

  send_text (conn, text);
  g_free (text);
}

/* A member that is a string, or NULL. */
static const gchar *
string_member (JsonObject * object, const gchar * name)
{
  JsonNode *node = json_object_get_member (object, name);

  if (node == NULL || !JSON_NODE_HOLDS_VALUE (node) ||
      json_node_get_value_type (node) != G_TYPE_STRING)
    return NULL;
  return json_node_get_string (node);
}

/* A member that is a positive whole number (an id), or 0. */
static gint64
id_member (JsonObject * object, const gchar * name)
{
  JsonNode *node = json_object_get_member (object, name);

  if (node == NULL || !JSON_NODE_HOLDS_VALUE (node) ||
      json_node_get_value_type (node) != G_TYPE_INT64)
    return 0;
  return MAX (json_node_get_int (node), 0);
}

/* Answers a command that failed: {"type":"error","command","reason"}. */
static void
send_error (SoupWebsocketConnection * conn, const gchar * command,
    const gchar * reason)
{
  GString *body = g_string_new ("{\"command\":");
  gchar *text;

  pm_json_append_string (body, command);
  g_string_append (body, ",\"reason\":");
  pm_json_append_string (body, reason);
  g_string_append_c (body, '}');
  text = pm_json_message ("error", body->str);
  send_text (conn, text);
  g_free (text);
  g_string_free (body, TRUE);
}

#define NOT_AN_ID(name) "\"" name "\" must be a positive whole number."

/* Whether the element with this id exists (and, with property, has it). */
static const gchar *
check_element (PmServer * self, gint64 id, const gchar * property)
{
  GstElement *element = pm_model_get_element (self->model, id);
  const gchar *reason = NULL;

  if (element == NULL)
    return "No element with this id.";
  if (property != NULL &&
      !g_object_class_find_property (G_OBJECT_GET_CLASS (element), property))
    reason = "The element has no such property.";
  gst_object_unref (element);
  return reason;
}

static gboolean
pad_exists (PmServer * self, gint64 id)
{
  GstPad *pad = pm_model_get_pad (self->model, id);

  if (pad == NULL)
    return FALSE;
  gst_object_unref (pad);
  return TRUE;
}

static gboolean start_recording (PmServer * self, const gchar * kind,
    const gchar * view, GError ** error);
static void send_recordings (PmServer * self, SoupWebsocketConnection * conn);
static void send_to_all (PmServer * self, const gchar * text);

/* Runs one command; returns why it failed (to free), or NULL. Stopping
 * something whose element or pad is gone succeeds: it is only forgotten. */
static gchar *
run_command (PmServer * self, SoupWebsocketConnection * conn,
    const gchar * command, JsonObject * object)
{
  gboolean on = !g_str_has_prefix (command, "un");
  const gchar *verb = on ? command : command + 2;

  /* {"type":"measure-all"} */
  if (g_strcmp0 (verb, "measure-all") == 0) {
    pm_latency_measure_all (self->latency, conn, on);
    return NULL;
  }

  /* {"type":"measure-element","element":12} */
  if (g_strcmp0 (verb, "measure-element") == 0) {
    gint64 element = id_member (object, "element");
    const gchar *reason;

    if (element == 0)
      return g_strdup (NOT_AN_ID ("element"));
    if (on && (reason = check_element (self, element, NULL)))
      return g_strdup (reason);
    pm_latency_measure_element (self->latency, conn, element, on);
    return NULL;
  }

  /* {"type":"measure-path","from":3,"to":9} */
  if (g_strcmp0 (verb, "measure-path") == 0) {
    gint64 from = id_member (object, "from");
    gint64 to = id_member (object, "to");

    if (from == 0 || to == 0)
      return g_strdup ("\"from\" and \"to\" must be positive whole "
          "numbers.");
    if (on && (!pad_exists (self, from) || !pad_exists (self, to)))
      return g_strdup ("No pad with this id.");
    pm_latency_measure_path (self->latency, conn, from, to, on);
    return NULL;
  }

  /* {"type":"watch-property","element":12,"property":"stats"} */
  if (g_strcmp0 (verb, "watch-property") == 0) {
    gint64 element = id_member (object, "element");
    const gchar *property = string_member (object, "property");
    const gchar *reason;

    if (element == 0)
      return g_strdup (NOT_AN_ID ("element"));
    if (!valid_property_name (property))
      return g_strdup ("\"property\" must be a name of letters, digits, - "
          "and _.");
    if (on && (reason = check_element (self, element, property)))
      return g_strdup (reason);
    watch_property (self, conn, element, property, on);
    return NULL;
  }

  /* {"type":"watch-pad","pad":12}: a pad that does not exist is answered
   * with "gone" in "pads". */
  if (g_strcmp0 (verb, "watch-pad") == 0) {
    gint64 pad = id_member (object, "pad");

    if (pad == 0)
      return g_strdup (NOT_AN_ID ("pad"));
    if (!on) {
      pm_monitor_remove (self->monitor, conn, pad);
    } else if (!pm_monitor_add (self->monitor, conn, pad)) {
      gchar *gone = g_strdup_printf ("{\"pads\":[{\"pad\":%" G_GINT64_FORMAT
          ",\"gone\":true}]}", pad);

      send_stats (conn, gone);
      g_free (gone);
    }
    return NULL;
  }

  /* {"type":"record-start","view":{...}}, {"type":"snapshot",...} */
  if (g_strcmp0 (command, "record-start") == 0 ||
      g_strcmp0 (command, "snapshot") == 0) {
    JsonNode *view = json_object_get_member (object, "view");
    gchar *text = NULL, *reason = NULL;
    GError *error = NULL;

    if (view != NULL && !JSON_NODE_HOLDS_OBJECT (view) &&
        !JSON_NODE_HOLDS_NULL (view))
      return g_strdup ("\"view\" must be an object.");
    if (view != NULL && JSON_NODE_HOLDS_OBJECT (view))
      text = json_to_string (view, FALSE);
    if (!start_recording (self, command[0] == 'r' ? "recording" : "snapshot",
            text, &error)) {
      reason = g_strdup (error->message);
      g_error_free (error);
    }
    g_free (text);
    send_recordings (self, NULL);
    return reason;
  }

  if (g_strcmp0 (command, "record-stop") == 0) {
    if (!self->tapping || !pm_recorder_running (self->recorder))
      return g_strdup ("No recording is running.");
    self->tapping = FALSE;
    pm_recorder_stop (self->recorder, "stopped");
    send_recordings (self, NULL);
    return NULL;
  }

  /* {"type":"record-delete","name":"pipeline-monitor-...jsonl"} */
  if (g_strcmp0 (command, "record-delete") == 0) {
    if (!pm_recorder_delete (self->recorder, string_member (object, "name")))
      return g_strdup ("No recording with this name, or it is running.");
    send_recordings (self, NULL);
    return NULL;
  }

  return g_strdup ("Unknown command.");
}

/* Commands from the page, {"type":"watch-pad","pad":12} and so on; one that
 * fails is answered with an error. */
static void
on_ws_message (SoupWebsocketConnection * conn, gint type, GBytes * message,
    PmServer * self)
{
  JsonParser *parser;
  JsonNode *root;
  const gchar *command;
  gchar *reason;
  gsize size;
  const gchar *data = g_bytes_get_data (message, &size);

  if (type != SOUP_WEBSOCKET_DATA_TEXT) {
    send_error (conn, NULL, "Commands are JSON text, not binary.");
    return;
  }
  if (size > MAX_COMMAND_SIZE) {
    send_error (conn, NULL, "Commands are at most 64 KiB.");
    return;
  }
  /* An empty message has no data at all; the parser must not get NULL
   * (it then reads an empty document: not a command object). */
  if (data == NULL)
    data = "";

  parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, data, size, NULL)) {
    send_error (conn, NULL, "Not valid JSON.");
  } else if ((root = json_parser_get_root (parser)) == NULL ||
      !JSON_NODE_HOLDS_OBJECT (root)) {
    send_error (conn, NULL, "A command is a JSON object.");
  } else if ((command = string_member (json_node_get_object (root),
              "type")) == NULL) {
    send_error (conn, NULL, "\"type\" is missing or not a string.");
  } else if ((reason = run_command (self, conn, command,
              json_node_get_object (root))) != NULL) {
    send_error (conn, command, reason);
    g_free (reason);
  }
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

/* The values of only the properties one page watches: result (from
 * pm_props_read, parsed) with its "properties" filtered to names. */
static gchar *
watched_values (JsonNode * result, GPtrArray * names)
{
  JsonObject *in = json_node_get_object (result);
  JsonObject *out = json_object_new ();
  GList *members = json_object_get_members (in), *l;
  JsonNode *node;
  gchar *text;

  for (l = members; l != NULL; l = l->next) {
    const gchar *member = l->data;
    JsonNode *value = json_object_get_member (in, member);
    JsonArray *all, *mine;
    guint i;

    if (g_strcmp0 (member, "properties") != 0 ||
        !JSON_NODE_HOLDS_ARRAY (value)) {
      json_object_set_member (out, member, json_node_copy (value));
      continue;
    }
    all = json_node_get_array (value);
    mine = json_array_new ();
    for (i = 0; i < json_array_get_length (all); i++) {
      JsonObject *p = json_array_get_object_element (all, i);
      const gchar *name = json_object_get_string_member_with_default (p,
          "name", "");

      if (g_ptr_array_find_with_equal_func (names, name, g_str_equal, NULL))
        json_array_add_element (mine,
            json_node_copy (json_array_get_element (all, i)));
    }
    json_object_set_array_member (out, member, mine);
  }
  g_list_free (members);
  node = json_node_init_object (json_node_alloc (), out);
  text = json_to_string (node, FALSE);
  json_node_unref (node);
  json_object_unref (out);
  return text;
}

/* Sends one page the properties it watches, and only those; results:
 * element id -> pm_props_read result (parsed), or NULL if the element is
 * gone (its watches are then dropped). */
static void
send_watched (PmServer * self, SoupWebsocketConnection * conn,
    GHashTable * set, GHashTable * results)
{
  GHashTable *mine = g_hash_table_new_full (g_int64_hash, g_int64_equal,
      g_free, (GDestroyNotify) g_ptr_array_unref);
  GString *json = g_string_new ("{\"elements\":[");
  GHashTableIter iter;
  gpointer key, value;

  g_hash_table_iter_init (&iter, set);
  while (g_hash_table_iter_next (&iter, &key, NULL)) {
    gchar **parts = g_strsplit (key, "\n", 2);
    gint64 id = g_ascii_strtoll (parts[0], NULL, 10);
    gpointer result;

    if (g_hash_table_lookup_extended (results, &id, NULL, &result)) {
      add_name (mine, id, parts[1]);
      if (result == NULL)
        g_hash_table_iter_remove (&iter);
    }
    g_strfreev (parts);
  }

  g_hash_table_iter_init (&iter, mine);
  while (g_hash_table_iter_next (&iter, &key, &value)) {
    gint64 id = *(gint64 *) key;
    JsonNode *result = g_hash_table_lookup (results, &id);

    if (json->str[json->len - 1] != '[')
      g_string_append_c (json, ',');
    if (result == NULL) {
      g_string_append_printf (json, "{\"element\":%" G_GINT64_FORMAT
          ",\"gone\":true}", id);
    } else {
      gchar *values = watched_values (result, value);
      gchar *path = pm_model_path (self->model, id);

      /* {"element":12,"path":"...","properties":[...]} */
      g_string_append_printf (json, "{\"element\":%" G_GINT64_FORMAT ",", id);
      pm_json_append_member_string (json, "path", path);
      g_string_append (json, values + 1);
      g_free (path);
      g_free (values);
    }
  }
  g_string_append (json, "]}");
  if (g_hash_table_size (mine) > 0) {
    gchar *text = pm_json_message ("properties", json->str);

    send_text (conn, text);
    g_free (text);
  }
  g_string_free (json, TRUE);
  g_hash_table_unref (mine);
}

static void
result_free (JsonNode * node)
{
  if (node != NULL)
    json_node_unref (node);
}

/* Adds a message to the running recording; it may end at its limit. */
static void
record (PmServer * self, const gchar * text)
{
  if (self->tapping && !pm_recorder_write (self->recorder, text))
    self->tapping = FALSE;
}

/* Elements in their pipeline, and pipelines: one not in a bin yet (made by
 * gst_parse_launch, say) would be kept under the wrong path, so it waits
 * for the next round. */
static gboolean
placed_element (GstElement * element, gboolean top_level)
{
  return !top_level || GST_IS_PIPELINE (element);
}

typedef struct
{
  PmRecorder *recorder;
  GPtrArray *elements;
  gboolean snapshot;
  gint *busy;
} PmStateJob;

/* Reads the properties of the job's elements for a recording and writes
 * them as one "properties" message; ends a snapshot after it. */
static gpointer
read_state (gpointer user_data)
{
  PmStateJob *job = user_data;
  GPtrArray *elements = job->elements;
  GString *json = g_string_new ("{\"elements\":[");
  gchar *text;
  guint i;

  for (i = 0; i < elements->len; i++) {
    PmElementRef *ref = g_ptr_array_index (elements, i);
    gchar *props = pm_props_read (ref->element, NULL, PROPERTIES_TIMEOUT_US);

    if (i > 0)
      g_string_append_c (json, ',');
    g_string_append_printf (json, "{\"element\":%" G_GUINT64_FORMAT ",",
        ref->id);
    pm_json_append_member_string (json, "path", ref->path);
    g_string_append (json, props + 1);
    g_free (props);
  }
  g_string_append (json, "]}");
  text = pm_json_message ("properties", json->str);
  pm_recorder_write (job->recorder, text);
  if (job->snapshot)
    pm_recorder_stop (job->recorder, "snapshot");
  g_free (text);
  g_string_free (json, TRUE);
  g_ptr_array_unref (elements);
  g_atomic_int_set (job->busy, 0);
  g_free (job);
  return NULL;
}

/* Reads, on state_thread, the properties of the elements the recording
 * does not have yet. */
static void
read_new_elements (PmServer * self, gboolean snapshot)
{
  GPtrArray *all = pm_model_find_elements (self->model, placed_element);
  GPtrArray *fresh = g_ptr_array_new_with_free_func ((GDestroyNotify)
      pm_element_ref_free);
  PmStateJob *job;
  guint i;

  for (i = 0; i < all->len; i++) {
    PmElementRef *ref = g_ptr_array_index (all, i);

    if (g_hash_table_add (self->recorded, GSIZE_TO_POINTER (ref->id)))
      g_ptr_array_add (fresh, g_ptr_array_steal_index_fast (all, i--));
  }
  g_ptr_array_unref (all);
  if (fresh->len == 0 && !snapshot) {
    g_ptr_array_unref (fresh);
    return;
  }
  if (self->state_thread != NULL)
    g_clear_pointer (&self->state_thread, g_thread_join);
  job = g_new0 (PmStateJob, 1);
  job->recorder = self->recorder;
  job->elements = fresh;
  job->snapshot = snapshot;
  job->busy = &self->state_busy;
  g_atomic_int_set (&self->state_busy, 1);
  self->state_thread = g_thread_new ("pm-record-state", read_state, job);
}

/* Starts a recording ("recording" or "snapshot") with the state as it is
 * now: graph, the whole log, findings, levels, and every element's
 * properties (read on another thread). view: the page's, or NULL. */
static gboolean
start_recording (PmServer * self, const gchar * kind, const gchar * view,
    GError ** error)
{
  gchar *hello = hello_body (self);
  gchar *parts[4], *text, *program;
  const gchar *types[] = { "graph", "log", "findings", "levels" };
  guint i;

  if (self->state_thread != NULL)
    g_clear_pointer (&self->state_thread, g_thread_join);
  /* The program's name without a folder: it may hold the user's name. */
  program = g_path_get_basename (g_get_prgname () ? g_get_prgname () : "");
  if (!pm_recorder_start (self->recorder, kind, program, hello, view,
          error)) {
    g_free (program);
    g_free (hello);
    return FALSE;
  }
  g_free (program);
  g_free (hello);
  parts[0] = pm_model_to_json (self->model);
  parts[1] = pm_model_log_to_json (self->model, 0, NULL);
  parts[2] = g_strdup (self->last_findings ? self->last_findings :
      "{\"findings\":[]}");
  parts[3] = self->last_levels ? g_strdup (self->last_levels) :
      pm_levels_to_json (self->model);
  for (i = 0; i < G_N_ELEMENTS (parts); i++) {
    text = pm_json_message (types[i], parts[i]);
    pm_recorder_write (self->recorder, text);
    g_free (text);
    g_free (parts[i]);
  }
  self->tapping = g_strcmp0 (kind, "recording") == 0;
  g_hash_table_remove_all (self->recorded);
  read_new_elements (self, !self->tapping);
  return TRUE;
}

/* The recordings in the folder, to one page or (conn NULL) to all. */
static void
send_recordings (PmServer * self, SoupWebsocketConnection * conn)
{
  gchar *list = pm_recorder_list_json (self->recorder);
  gchar *text = pm_json_message ("recordings", list);

  /* The pages hear when it ends, also if that is before the next tick. */
  self->was_running = pm_recorder_running (self->recorder);
  if (conn != NULL)
    send_text (conn, text);
  else
    send_to_all (self, text);
  g_free (text);
  g_free (list);
}

/* The properties any page watches, for a recording. */
static void
record_properties (PmServer * self, GHashTable * by_element,
    GHashTable * results)
{
  GString *json = g_string_new ("{\"elements\":[");
  GHashTableIter iter;
  gpointer key, value;
  gchar *text;

  g_hash_table_iter_init (&iter, by_element);
  while (g_hash_table_iter_next (&iter, &key, &value)) {
    JsonNode *result = g_hash_table_lookup (results, key);
    GPtrArray *names = g_ptr_array_new ();
    gchar *values, *path;
    guint i;

    if (result == NULL) {
      g_ptr_array_unref (names);
      continue;
    }
    for (i = 0; i < ((GPtrArray *) value)->len; i++)
      if (g_ptr_array_index ((GPtrArray *) value, i) != NULL)
        g_ptr_array_add (names, g_ptr_array_index ((GPtrArray *) value, i));
    values = watched_values (result, names);
    path = pm_model_path (self->model, *(gint64 *) key);
    if (json->str[json->len - 1] != '[')
      g_string_append_c (json, ',');
    g_string_append_printf (json, "{\"element\":%" G_GINT64_FORMAT ",",
        *(gint64 *) key);
    pm_json_append_member_string (json, "path", path);
    g_string_append (json, values + 1);
    g_free (path);
    g_free (values);
    g_ptr_array_unref (names);
  }
  g_string_append (json, "]}");
  if (json->str[json->len - 3] != '[') {
    text = pm_json_message ("properties", json->str);
    record (self, text);
    g_free (text);
  }
  g_string_free (json, TRUE);
}

/* Reads the watched properties, once per element however many pages watch
 * it, and sends each page {"type":"properties","elements":[...]} with its
 * elements; an element that is gone is reported once, then dropped. */
static void
send_properties (PmServer * self, gboolean to_recording)
{
  GHashTable *by_element = g_hash_table_new_full (g_int64_hash,
      g_int64_equal, g_free, (GDestroyNotify) g_ptr_array_unref);
  GHashTable *results = g_hash_table_new_full (g_int64_hash, g_int64_equal,
      NULL, (GDestroyNotify) result_free);
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
    gchar *read;

    if (element == NULL) {
      g_hash_table_insert (results, key, NULL);
      continue;
    }
    g_ptr_array_add (names, NULL);
    read = pm_props_read (element, (const gchar * const *) names->pdata,
        PROPERTIES_TIMEOUT_US);
    g_hash_table_insert (results, key, json_from_string (read, NULL));
    g_free (read);
    gst_object_unref (element);
  }

  g_hash_table_iter_init (&iter, self->watches);
  while (g_hash_table_iter_next (&iter, &key, &value))
    send_watched (self, key, value, results);
  if (to_recording)
    record_properties (self, by_element, results);
  g_hash_table_unref (results);
  g_hash_table_unref (by_element);
}

/* After an error the findings are kept as they were for a while: the
 * program usually stops its pipeline right after, and the pads and states
 * the findings are about go away. */
#define FINDINGS_HELD_US (5 * G_USEC_PER_SEC)

static void
send_findings (PmServer * self)
{
  gchar *findings, *text;

  if (g_atomic_int_compare_and_exchange (&self->release_findings, 1, 0))
    self->findings_held_until = 0;
  if (g_get_monotonic_time () < self->findings_held_until)
    return;
  findings = pm_findings_tick (self->findings, PM_STUCK_AFTER_US);

  if (g_strcmp0 (findings, self->last_findings) == 0) {
    g_free (findings);
    return;
  }
  text = pm_json_message ("findings", findings);
  send_to_all (self, text);
  record (self, text);
  g_free (text);
  g_free (self->last_findings);
  self->last_findings = findings;
}

static gboolean send_update (gpointer user_data);

static gboolean
send_all_stats (gpointer user_data)
{
  PmServer *self = user_data;
  /* Recordings keep levels, pads, properties and latency once a second. */
  gboolean to_recording = self->tapping && self->ticks % 4 == 0;
  gchar *levels, *latency;
  guint i;

  /* A finished state thread is joined at once. */
  if (self->state_thread != NULL && !g_atomic_int_get (&self->state_busy))
    g_clear_pointer (&self->state_thread, g_thread_join);
  pm_monitor_tick (self->monitor);
  g_free (self->last_levels);
  self->last_levels = pm_levels_to_json (self->model);
  levels = pm_json_message ("levels", self->last_levels);
  /* Temporary, until one timer sends everything (see the roadmap): a graph
   * change waiting for its timer goes out first, read after the levels, so
   * no levels are newer than the graph before them. */
  if (g_atomic_int_get (&self->update_scheduled))
    send_update (self);
  if (to_recording) {
    gchar *stats = pm_monitor_stats_json (self->monitor, NULL);

    if (!g_atomic_int_get (&self->state_busy))
      read_new_elements (self, FALSE);
    record (self, levels);
    if (stats != NULL) {
      gchar *text = pm_json_message ("pads", stats);

      record (self, text);
      g_free (text);
      g_free (stats);
    }
  }
  for (i = 0; i < self->clients->len; i++) {
    SoupWebsocketConnection *conn = g_ptr_array_index (self->clients, i);
    gchar *stats = pm_monitor_stats_json (self->monitor, conn);

    send_text (conn, levels);
    if (stats != NULL)
      send_stats (conn, stats);
    g_free (stats);
  }
  g_free (levels);
  send_properties (self, to_recording);

  /* Findings once per second, sent when they change; the recordings list
   * once per second while one is running, and once when it ends. */
  if (self->ticks++ % 4 == 0) {
    gboolean running = pm_recorder_running (self->recorder);

    send_findings (self);
    if (running || self->was_running)
      send_recordings (self, NULL);
    self->was_running = running;
  }

  /* Measured once, sent to each page only what it measures. */
  pm_latency_tick (self->latency);
  for (i = 0; i < self->clients->len; i++) {
    SoupWebsocketConnection *conn = g_ptr_array_index (self->clients, i);

    latency = pm_latency_message (self->latency, conn);
    if (latency != NULL) {
      gchar *text = pm_json_message ("latency", latency);

      send_text (conn, text);
      g_free (text);
      g_free (latency);
    }
  }
  if (to_recording && (latency = pm_latency_message_all (self->latency))) {
    gchar *text = pm_json_message ("latency", latency);

    record (self, text);
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
  gchar *hello = hello_body (self);
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
  text = pm_json_message ("hello", hello);
  send_text (conn, text);
  g_free (text);
  text = pm_json_message ("graph", graph);
  send_text (conn, text);
  g_free (text);
  text = pm_json_message ("log", log);
  send_text (conn, text);
  g_free (text);
  send_recordings (self, conn);
  /* Findings are otherwise only sent when they change. */
  if (self->last_findings != NULL) {
    text = pm_json_message ("findings", self->last_findings);
    send_text (conn, text);
    g_free (text);
  }

  g_free (hello);
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
    text = pm_json_message ("graph", graph);
    send_to_all (self, text);
    record (self, text);
    g_free (text);
    g_free (self->last_graph);
    self->last_graph = graph;
  } else {
    g_free (graph);
  }

  /* Only entries added or repeated since the last send. */
  log = pm_model_log_to_json (self->model, self->log_sent, &seq);
  if (seq > self->log_sent) {
    text = pm_json_message ("log", log);
    send_to_all (self, text);
    record (self, text);
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
  if (g_atomic_int_compare_and_exchange (&self->hold_findings, 1, 0))
    self->findings_held_until = g_get_monotonic_time () + FINDINGS_HELD_US;
  if (flush->bye) {
    gchar *bye = pm_json_message ("bye", NULL);

    send_to_all (self, bye);
    g_free (bye);
    self->tapping = FALSE;
    pm_recorder_stop (self->recorder, "program-ended");
  }
  source = g_timeout_source_new (100);
  g_source_set_callback (source, flush_done, flush,
      (GDestroyNotify) flush_unref);
  g_source_attach (source, self->context);
  g_source_unref (source);
  return G_SOURCE_REMOVE;
}

/* The next flush keeps the findings it sends for FINDINGS_HELD_US, or
 * until pm_server_release_findings. */
void
pm_server_hold_findings (PmServer * self)
{
  g_atomic_int_set (&self->hold_findings, 1);
}

/* A pipeline is starting: findings are current again. */
void
pm_server_release_findings (PmServer * self)
{
  g_atomic_int_set (&self->release_findings, 1);
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
  soup_server_add_handler (self->soup, "/api/hello", handle_hello, self, NULL);
  soup_server_add_handler (self->soup, "/api/graph", handle_graph, self, NULL);
  soup_server_add_handler (self->soup, "/api/log", handle_log, self, NULL);
  soup_server_add_handler (self->soup, "/api/levels", handle_levels, self,
      NULL);
  soup_server_add_handler (self->soup, "/api/findings", handle_findings, self,
      NULL);
  soup_server_add_handler (self->soup, "/api/properties", handle_properties,
      self, NULL);
  soup_server_add_handler (self->soup, "/api/recordings", handle_recordings,
      self, NULL);
  soup_server_add_handler (self->soup, "/api/opened", handle_opened, self,
      NULL);
  soup_server_add_handler (self->soup, "/api/pads", handle_watched_pads, self,
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
  self->recorded = g_hash_table_new (NULL, NULL);
  {
    gchar *dir = pm_recorder_default_dir ();

    self->recorder = pm_recorder_new (dir, PM_RECORD_FILE_MAX,
        PM_RECORD_TOTAL_MAX);
    g_free (dir);
  }
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
  if (self->state_thread != NULL)
    g_thread_join (self->state_thread);
  pm_recorder_free (self->recorder);
  g_hash_table_unref (self->recorded);

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
  g_free (self->opened);
  g_free (self->ui_dir);
  g_free (self->last_levels);
  g_free (self->last_findings);
  g_free (self->url);
  g_free (self);
}

typedef struct
{
  PmServer *server;
  PmRecorder *recorder;
  gboolean record;
} PmRecordSetup;

static gboolean
apply_recordings (gpointer user_data)
{
  PmRecordSetup *setup = user_data;
  PmServer *self = setup->server;
  GError *error = NULL;

  pm_recorder_free (self->recorder);
  self->recorder = setup->recorder;
  if (setup->record &&
      !start_recording (self, "recording", NULL, &error)) {
    g_printerr ("pipeline-monitor: cannot record: %s\n", error->message);
    g_error_free (error);
  }
  g_free (setup);
  return G_SOURCE_REMOVE;
}

typedef struct
{
  PmServer *server;
  gchar *opened;
} PmViewerSetup;

static gboolean
apply_viewer (gpointer user_data)
{
  PmViewerSetup *setup = user_data;

  setup->server->viewer = TRUE;
  g_free (setup->server->opened);
  setup->server->opened = setup->opened;
  g_free (setup);
  return G_SOURCE_REMOVE;
}

/* Serves recordings only (no program): pages show the viewer; opened is a
 * file to serve at /api/opened, or NULL. */
void
pm_server_set_viewer (PmServer * self, const gchar * opened)
{
  PmViewerSetup *setup = g_new0 (PmViewerSetup, 1);

  setup->server = self;
  setup->opened = g_strdup (opened);
  g_main_context_invoke (self->context, apply_viewer, setup);
}

/* Sets the recordings folder and limits; with record, starts recording. */
void
pm_server_set_recordings (PmServer * self, const PmRecordOptions * options)
{
  PmRecordSetup *setup = g_new0 (PmRecordSetup, 1);
  gchar *dir = options->dir ? g_strdup (options->dir) :
      pm_recorder_default_dir ();

  setup->server = self;
  setup->recorder = pm_recorder_new (dir, options->file_max,
      options->total_max);
  setup->record = options->record;
  g_main_context_invoke (self->context, apply_recordings, setup);
  g_free (dir);
}

const gchar *
pm_server_get_url (PmServer * self)
{
  return self->url;
}
