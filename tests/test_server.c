/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <gst/gst.h>
#include <gst/gsttracer.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <stdlib.h>
#include <string.h>

#include "pmserver.h"

#define EMPTY_GRAPH "{\"pipelines\":[],\"links\":[]}"

static gchar *
tracer_string (const gchar * property)
{
  GList *tracers, *l;
  gchar *value = NULL;

  tracers = gst_tracing_get_active_tracers ();
  for (l = tracers; l != NULL; l = l->next) {
    if (g_strcmp0 (G_OBJECT_TYPE_NAME (l->data),
            "GstPipelineMonitorTracer") == 0)
      g_object_get (l->data, property, &value, NULL);
  }
  g_list_free_full (tracers, gst_object_unref);
  g_assert_nonnull (value);
  return value;
}

static gchar *
server_url (const gchar * path)
{
  gchar *base = tracer_string ("url");
  gchar *url = g_strconcat (base, path, NULL);

  g_free (base);
  return url;
}

static GstElement *
start_pipeline (const gchar * description)
{
  GError *error = NULL;
  GstElement *pipeline = gst_parse_launch (description, &error);

  g_assert_no_error (error);
  g_assert_cmpint (gst_element_set_state (pipeline, GST_STATE_PLAYING), !=,
      GST_STATE_CHANGE_FAILURE);
  g_assert_cmpint (gst_element_get_state (pipeline, NULL, NULL,
          5 * GST_SECOND), ==, GST_STATE_CHANGE_SUCCESS);
  return pipeline;
}

static void
stop_pipeline (GstElement * pipeline)
{
  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (pipeline);
}

/* Runs the default main context until *done is set or the timeout expires. */
static gboolean
wait_for (gboolean * done)
{
  gint64 deadline = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;

  while (!*done && g_get_monotonic_time () < deadline)
    if (!g_main_context_iteration (NULL, FALSE))
      g_usleep (1000);
  return *done;
}

static guint
get_status (const gchar * path, const gchar * method, const gchar * host,
    gchar ** body, gchar ** content_type)
{
  SoupSession *session = soup_session_new ();
  gchar *url = server_url (path);
  SoupMessage *msg = soup_message_new (method, url);
  GBytes *bytes;
  GError *error = NULL;
  guint status;

  if (host != NULL)
    soup_message_headers_replace (soup_message_get_request_headers (msg),
        "Host", host);
  bytes = soup_session_send_and_read (session, msg, NULL, &error);
  g_assert_no_error (error);
  status = soup_message_get_status (msg);
  if (body != NULL)
    *body = g_strndup (g_bytes_get_data (bytes, NULL), g_bytes_get_size (bytes));
  if (content_type != NULL)
    *content_type = g_strdup (soup_message_headers_get_content_type
        (soup_message_get_response_headers (msg), NULL));

  g_bytes_unref (bytes);
  g_object_unref (msg);
  g_free (url);
  g_object_unref (session);
  return status;
}

/* Sends a request line as is; HTTP clients would normalise "..". */
static guint
raw_status (const gchar * base_url, const gchar * path)
{
  GUri *uri = g_uri_parse (base_url, G_URI_FLAGS_NONE, NULL);
  GSocketClient *client = g_socket_client_new ();
  GSocketConnection *conn;
  GDataInputStream *in;
  gchar *request, *line;
  guint status = 0;

  conn = g_socket_client_connect_to_host (client, "127.0.0.1",
      g_uri_get_port (uri), NULL, NULL);
  g_assert_nonnull (conn);
  request = g_strdup_printf ("GET %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n"
      "Connection: close\r\n\r\n", path, g_uri_get_port (uri));
  g_output_stream_write_all (g_io_stream_get_output_stream (G_IO_STREAM
          (conn)), request, strlen (request), NULL, NULL, NULL);
  in = g_data_input_stream_new (g_io_stream_get_input_stream (G_IO_STREAM
          (conn)));
  line = g_data_input_stream_read_line (in, NULL, NULL, NULL);
  if (line != NULL && g_str_has_prefix (line, "HTTP/1.1 "))
    status = atoi (line + strlen ("HTTP/1.1 "));

  g_free (line);
  g_object_unref (in);
  g_free (request);
  g_object_unref (conn);
  g_object_unref (client);
  g_uri_unref (uri);
  return status;
}

static void
test_url (void)
{
  gchar *url = tracer_string ("url");

  g_assert_true (g_str_has_prefix (url, "http://127.0.0.1:"));
  g_assert_true (g_str_has_suffix (url, "/"));
  g_assert_cmpstr (url, !=, "http://127.0.0.1:0/");
  g_free (url);
}

static void
test_http (void)
{
  GstElement *pipeline;
  gchar *body, *type;

  g_assert_cmpuint (get_status ("", "GET", NULL, &body, &type), ==, 200);
  g_assert_cmpstr (type, ==, "text/html");
  g_assert_nonnull (strstr (body, "<script type=\"module\" src=\"app.js\">"));
  g_free (body);
  g_free (type);

  g_assert_cmpuint (get_status ("api/graph", "GET", NULL, &body, &type), ==,
      200);
  g_assert_cmpstr (type, ==, "application/json");
  g_assert_cmpstr (body, ==, EMPTY_GRAPH);
  g_free (body);
  g_free (type);

  pipeline = start_pipeline ("fakesrc ! queue ! fakesink name=http-sink");
  g_assert_cmpuint (get_status ("api/graph", "GET", NULL, &body, NULL), ==,
      200);
  g_assert_nonnull (strstr (body, "\"name\":\"http-sink\""));
  g_free (body);
  stop_pipeline (pipeline);

  g_assert_cmpuint (get_status ("nope", "GET", NULL, NULL, NULL), ==, 404);
  g_assert_cmpuint (get_status ("api/graph", "POST", NULL, NULL, NULL), ==,
      405);
}

static void
test_ui_files (void)
{
  const gchar *scripts[] = { "app.js", "layout.js", "render.js", "panel.js",
    "log.js", "caps.js", "tooltip.js",
    "vendor/elkjs/elk-api.js", "vendor/elkjs/elk-worker.min.js"
  };
  gchar *body, *type, *url;
  guint i;

  for (i = 0; i < G_N_ELEMENTS (scripts); i++) {
    g_assert_cmpuint (get_status (scripts[i], "GET", NULL, &body, &type), ==,
        200);
    g_assert_cmpstr (type, ==, "text/javascript");
    g_assert_cmpuint (strlen (body), >, 100);
    g_free (body);
    g_free (type);
  }
  g_assert_cmpuint (get_status ("style.css", "GET", NULL, NULL, &type), ==,
      200);
  g_assert_cmpstr (type, ==, "text/css");
  g_free (type);

  g_assert_cmpuint (get_status ("missing.js", "GET", NULL, NULL, NULL), ==,
      404);
  g_assert_cmpuint (get_status ("index.html.bak", "GET", NULL, NULL, NULL),
      ==, 404);

  url = tracer_string ("url");
  g_assert_cmpuint (raw_status (url, "/../meson.build"), ==, 404);
  /* libsoup removes dot segments, so this is just /app.js */
  g_assert_cmpuint (raw_status (url, "/vendor/../app.js"), ==, 200);
  g_assert_cmpuint (raw_status (url, "/.hidden.js"), ==, 404);
  g_assert_cmpuint (raw_status (url, "//app.js"), ==, 404);
  g_free (url);
}

static void
test_ui_dir (void)
{
  gchar *dir = g_dir_make_tmp ("pm-ui-XXXXXX", NULL);
  gchar *parent_file = g_build_filename (dir, "..", "pm-secret.js", NULL);
  gchar *file = g_build_filename (dir, "app.js", NULL);
  PmModel *model = pm_model_new ();
  PmServer *server;
  SoupSession *session = soup_session_new ();
  SoupMessage *msg;
  GBytes *bytes;
  gchar *url;

  g_assert_true (g_file_set_contents (file, "console.log('dev')", -1, NULL));
  g_assert_true (g_file_set_contents (parent_file, "secret", -1, NULL));
  server = pm_server_new (model, 0, dir, NULL);

  url = g_strconcat (pm_server_get_url (server), "app.js", NULL);
  msg = soup_message_new ("GET", url);
  bytes = soup_session_send_and_read (session, msg, NULL, NULL);
  g_assert_cmpuint (soup_message_get_status (msg), ==, 200);
  g_assert_cmpmem (g_bytes_get_data (bytes, NULL), g_bytes_get_size (bytes),
      "console.log('dev')", 18);
  g_bytes_unref (bytes);
  g_object_unref (msg);
  g_free (url);

  g_assert_cmpuint (raw_status (pm_server_get_url (server),
          "/../pm-secret.js"), ==, 404);

  pm_server_free (server);
  pm_model_free (model);
  g_object_unref (session);
  g_unlink (parent_file);
  g_unlink (file);
  g_rmdir (dir);
  g_free (parent_file);
  g_free (file);
  g_free (dir);
}

static void
test_host_check (void)
{
  gchar *url = tracer_string ("url");
  const gchar *port = strrchr (url, ':');
  gchar *localhost = g_strdup_printf ("localhost%.*s",
      (int) strlen (port) - 1, port);

  g_assert_cmpuint (get_status ("api/graph", "GET", "evil.example", NULL,
          NULL), ==, 403);
  g_assert_cmpuint (get_status ("api/graph", "GET", "127.0.0.1", NULL,
          NULL), ==, 403);
  g_assert_cmpuint (get_status ("api/graph", "GET", localhost, NULL,
          NULL), ==, 200);
  g_free (localhost);
  g_free (url);
}

typedef struct
{
  SoupWebsocketConnection *conn;
  GError *error;
  gboolean connected;
  GQueue messages;
  gboolean got_message;
} WsClient;

static void
on_message (SoupWebsocketConnection * conn, gint type, GBytes * message,
    WsClient * client)
{
  g_queue_push_tail (&client->messages, g_strndup (g_bytes_get_data (message,
              NULL), g_bytes_get_size (message)));
  client->got_message = TRUE;
}

static void
on_connected (GObject * session, GAsyncResult * result, gpointer user_data)
{
  WsClient *client = user_data;

  client->conn = soup_session_websocket_connect_finish (SOUP_SESSION (session),
      result, &client->error);
  if (client->conn != NULL)
    g_signal_connect (client->conn, "message", G_CALLBACK (on_message),
        client);
  client->connected = TRUE;
}

static void
ws_connect (WsClient * client, SoupSession * session, const gchar * url,
    const gchar * origin)
{
  SoupMessage *msg = soup_message_new ("GET", url);

  memset (client, 0, sizeof (*client));
  g_queue_init (&client->messages);
  soup_session_websocket_connect_async (session, msg, origin, NULL,
      G_PRIORITY_DEFAULT, NULL, on_connected, client);
  g_assert_true (wait_for (&client->connected));
  g_object_unref (msg);
}

static void
ws_disconnect (WsClient * client)
{
  g_clear_object (&client->conn);
  g_clear_error (&client->error);
  g_queue_clear_full (&client->messages, g_free);
}

/* The next message of the given type; other messages are skipped. */
static gchar *
ws_next (WsClient * client, const gchar * type)
{
  gchar *prefix = g_strdup_printf ("{\"type\":\"%s\",", type);
  gchar *message = NULL;

  while (message == NULL) {
    if (g_queue_is_empty (&client->messages)) {
      client->got_message = FALSE;
      g_assert_true (wait_for (&client->got_message));
    }
    message = g_queue_pop_head (&client->messages);
    if (!g_str_has_prefix (message, prefix))
      g_clear_pointer (&message, g_free);
  }
  g_free (prefix);
  return message;
}

/* Waits for the next graph message and checks whether it mentions needle. */
static gboolean
ws_next_graph_has (WsClient * client, const gchar * needle)
{
  gchar *message = ws_next (client, "graph");
  gboolean found;

  g_assert_true (g_str_has_prefix (message, "{\"type\":\"graph\",\"graph\":{"));
  found = strstr (message, needle) != NULL;
  g_free (message);
  return found;
}

static void
post_warning (GstElement * element, const gchar * text)
{
  GError *error = g_error_new_literal (GST_CORE_ERROR, GST_CORE_ERROR_FAILED,
      text);

  gst_element_post_message (element,
      gst_message_new_warning (GST_OBJECT (element), error, NULL));
  g_error_free (error);
}

/* After connecting, only new or repeated entries are sent. */
static void
test_websocket_log (void)
{
  SoupSession *session = soup_session_new ();
  gchar *url = server_url ("ws");
  GstElement *pipeline = gst_pipeline_new (NULL);
  GstElement *element = gst_element_factory_make ("identity", "ws-log");
  GstBus *bus = gst_element_get_bus (pipeline);
  WsClient client;
  gchar *message;

  gst_bus_set_flushing (bus, TRUE);
  gst_bin_add (GST_BIN (pipeline), element);

  ws_connect (&client, session, url, NULL);
  g_assert_no_error (client.error);
  message = ws_next (&client, "log");
  g_assert_true (g_str_has_prefix (message,
          "{\"type\":\"log\",\"log\":{\"enabled\":true,"));
  g_free (message);

  post_warning (element, "ws first");
  message = ws_next (&client, "log");
  g_assert_nonnull (strstr (message, "\"text\":\"ws first\""));
  g_assert_nonnull (strstr (message, "\"count\":1,"));
  g_free (message);

  post_warning (element, "ws second");
  message = ws_next (&client, "log");
  g_assert_nonnull (strstr (message, "\"text\":\"ws second\""));
  g_assert_null (strstr (message, "ws first"));
  g_free (message);

  post_warning (element, "ws first");
  message = ws_next (&client, "log");
  g_assert_nonnull (strstr (message, "\"text\":\"ws first\""));
  g_assert_nonnull (strstr (message, "\"count\":2,"));
  g_assert_null (strstr (message, "ws second"));
  g_free (message);

  ws_disconnect (&client);
  gst_object_unref (bus);
  gst_object_unref (pipeline);
  g_free (url);
  g_object_unref (session);
}

static void
test_websocket (void)
{
  SoupSession *session = soup_session_new ();
  gchar *url = server_url ("ws");
  GstElement *pipeline;
  WsClient client;

  ws_connect (&client, session, url, NULL);
  g_assert_no_error (client.error);
  g_assert_false (ws_next_graph_has (&client, "ws-sink"));

  pipeline = start_pipeline ("fakesrc ! queue ! fakesink name=ws-sink");
  while (!ws_next_graph_has (&client, "\"ws-sink\""));

  stop_pipeline (pipeline);
  while (ws_next_graph_has (&client, "\"ws-sink\""));

  ws_disconnect (&client);
  g_free (url);
  g_object_unref (session);
}

static void
test_origin_check (void)
{
  SoupSession *session = soup_session_new ();
  gchar *url = server_url ("ws");
  gchar *base = tracer_string ("url");
  gchar *own = g_strndup (base, strlen (base) - 1);
  WsClient client;

  ws_connect (&client, session, url, "http://evil.example");
  g_assert_null (client.conn);
  g_assert_nonnull (client.error);
  ws_disconnect (&client);

  ws_connect (&client, session, url, own);
  g_assert_no_error (client.error);
  g_assert_nonnull (client.conn);
  ws_disconnect (&client);

  g_free (own);
  g_free (base);
  g_free (url);
  g_object_unref (session);
}

static void
test_port_fallback (void)
{
  GSocketListener *listener = g_socket_listener_new ();
  GInetAddress *loopback = g_inet_address_new_loopback (G_SOCKET_FAMILY_IPV4);
  GSocketAddress *address = g_inet_socket_address_new (loopback, 0);
  GSocketAddress *bound = NULL;
  PmModel *model = pm_model_new ();
  PmServer *server;
  GError *error = NULL;
  guint busy;
  gchar *expected;

  g_socket_listener_add_address (listener, address, G_SOCKET_TYPE_STREAM,
      G_SOCKET_PROTOCOL_TCP, NULL, (GSocketAddress **) & bound, &error);
  g_assert_no_error (error);
  busy = g_inet_socket_address_get_port (G_INET_SOCKET_ADDRESS (bound));

  server = pm_server_new (model, busy, NULL, &error);
  g_assert_no_error (error);
  expected = g_strdup_printf ("http://127.0.0.1:%u/", busy);
  g_assert_cmpstr (pm_server_get_url (server), !=, expected);
  g_free (expected);

  pm_server_free (server);
  pm_model_free (model);
  g_object_unref (bound);
  g_object_unref (address);
  g_object_unref (loopback);
  g_socket_listener_close (listener);
  g_object_unref (listener);
}

/* The fallback must stop at the last port and report the failure. */
static void
test_no_free_port (void)
{
  GSocketListener *listener = g_socket_listener_new ();
  GInetAddress *loopback = g_inet_address_new_loopback (G_SOCKET_FAMILY_IPV4);
  GSocketAddress *address = g_inet_socket_address_new (loopback, G_MAXUINT16);
  PmModel *model = pm_model_new ();
  PmServer *server;
  GError *error = NULL;

  /* Fails harmlessly if another program already holds the port. */
  g_socket_listener_add_address (listener, address, G_SOCKET_TYPE_STREAM,
      G_SOCKET_PROTOCOL_TCP, NULL, NULL, NULL);

  server = pm_server_new (model, G_MAXUINT16, NULL, &error);
  g_assert_null (server);
  g_assert_nonnull (error);
  g_error_free (error);

  pm_model_free (model);
  g_object_unref (address);
  g_object_unref (loopback);
  g_socket_listener_close (listener);
  g_object_unref (listener);
}

static void
test_shutdown_with_client (void)
{
  SoupSession *session = soup_session_new ();
  PmModel *model = pm_model_new ();
  PmServer *server = pm_server_new (model, 0, NULL, NULL);
  gchar *url = g_strconcat (pm_server_get_url (server), "ws", NULL);
  WsClient client;

  ws_connect (&client, session, url, NULL);
  g_assert_no_error (client.error);
  g_assert_true (ws_next_graph_has (&client, EMPTY_GRAPH));

  pm_server_free (server);
  pm_model_free (model);

  ws_disconnect (&client);
  g_free (url);
  g_object_unref (session);
}

/* Pad monitoring */

static JsonNode *
parse (const gchar * text)
{
  JsonParser *parser = json_parser_new ();
  JsonNode *root;

  g_assert_true (json_parser_load_from_data (parser, text, -1, NULL));
  root = json_node_copy (json_parser_get_root (parser));
  g_object_unref (parser);
  return root;
}

static gint64
find_pad (JsonObject * element, const gchar * name, const gchar * pad)
{
  JsonArray *list = json_object_get_array_member (element, "pads");
  guint i;

  if (g_strcmp0 (json_object_get_string_member (element, "name"), name) == 0)
    for (i = 0; i < json_array_get_length (list); i++) {
      JsonObject *p = json_array_get_object_element (list, i);
      if (g_strcmp0 (json_object_get_string_member (p, "name"), pad) == 0)
        return json_object_get_int_member (p, "id");
    }
  list = json_object_get_array_member (element, "children");
  for (i = 0; i < json_array_get_length (list); i++) {
    gint64 id = find_pad (json_array_get_object_element (list, i), name, pad);
    if (id > 0)
      return id;
  }
  return 0;
}

/* The id the page would use for element:pad. */
static gint64
pad_id (const gchar * name, const gchar * pad)
{
  gchar *graph;
  JsonNode *root;
  JsonArray *pipelines;
  gint64 id = 0;
  guint i;

  get_status ("api/graph", "GET", NULL, &graph, NULL);
  root = parse (graph);
  pipelines = json_object_get_array_member (json_node_get_object (root),
      "pipelines");
  for (i = 0; i < json_array_get_length (pipelines) && id == 0; i++)
    id = find_pad (json_array_get_object_element (pipelines, i), name, pad);
  json_node_unref (root);
  g_free (graph);
  g_assert_cmpint (id, >, 0);
  return id;
}

static void
ws_send (WsClient * client, const gchar * format, ...)
{
  va_list args;
  gchar *text;

  va_start (args, format);
  text = g_strdup_vprintf (format, args);
  va_end (args);
  soup_websocket_connection_send_text (client->conn, text);
  g_free (text);
}

/* The next pad-stats entry for pad: buffers, or -1 if it is gone. */
static gint64
next_stats (WsClient * client, gint64 pad)
{
  while (TRUE) {
    gchar *message = ws_next (client, "pad-stats");
    JsonNode *root = parse (message);
    JsonArray *pads = json_object_get_array_member (json_object_get_object_member
        (json_node_get_object (root), "stats"), "pads");
    gint64 result = -2;
    guint i;

    for (i = 0; i < json_array_get_length (pads); i++) {
      JsonObject *p = json_array_get_object_element (pads, i);
      if (json_object_get_int_member (p, "pad") != pad)
        continue;
      result = json_object_get_boolean_member_with_default (p, "gone", FALSE)
          ? -1 : json_object_get_int_member (p, "buffers");
    }
    json_node_unref (root);
    g_free (message);
    if (result != -2)
      return result;
  }
}

/* Number of viewers of pad according to /api/monitors, 0 if not watched. */
static gint64
viewers (gint64 pad)
{
  gchar *text;
  JsonNode *root;
  JsonArray *pads;
  gint64 n = 0;
  guint i;

  get_status ("api/monitors", "GET", NULL, &text, NULL);
  root = parse (text);
  pads = json_object_get_array_member (json_node_get_object (root), "pads");
  for (i = 0; i < json_array_get_length (pads); i++) {
    JsonObject *p = json_array_get_object_element (pads, i);
    if (json_object_get_int_member (p, "pad") == pad)
      n = json_object_get_int_member (p, "viewers");
  }
  json_node_unref (root);
  g_free (text);
  return n;
}

/* Waits up to 2 s for pad to have the given number of viewers. */
static gboolean
wait_viewers (gint64 pad, gint64 expected)
{
  gint tries;

  for (tries = 0; tries < 200; tries++) {
    if (viewers (pad) == expected)
      return TRUE;
    g_main_context_iteration (NULL, FALSE);
    g_usleep (10 * G_TIME_SPAN_MILLISECOND);
  }
  return FALSE;
}

static void
test_monitor (void)
{
  SoupSession *session = soup_session_new ();
  gchar *url = server_url ("ws");
  GstElement *pipeline = start_pipeline ("videotestsrc is-live=true ! "
      "fakesink name=mon-sink");
  gint64 pad = pad_id ("mon-sink", "sink");
  WsClient a, b;
  gint64 first, second;

  ws_connect (&a, session, url, NULL);
  ws_connect (&b, session, url, NULL);

  ws_send (&a, "{\"type\":\"monitor\",\"pad\":%" G_GINT64_FORMAT "}", pad);
  first = next_stats (&a, pad);
  second = next_stats (&a, pad);
  g_assert_cmpint (first, >=, 0);
  g_assert_cmpint (second, >, first);
  g_assert_true (wait_viewers (pad, 1));

  /* A second viewer shares the probe. */
  ws_send (&b, "{\"type\":\"monitor\",\"pad\":%" G_GINT64_FORMAT "}", pad);
  g_assert_cmpint (next_stats (&b, pad), >=, 0);
  g_assert_true (wait_viewers (pad, 2));

  ws_send (&a, "{\"type\":\"unmonitor\",\"pad\":%" G_GINT64_FORMAT "}",
      pad);
  g_assert_true (wait_viewers (pad, 1));
  g_assert_cmpint (next_stats (&b, pad), >, 0);

  /* Closing the page drops its pads. */
  ws_disconnect (&b);
  g_assert_true (wait_viewers (pad, 0));

  ws_disconnect (&a);
  stop_pipeline (pipeline);
  g_free (url);
  g_object_unref (session);
}

static void
test_monitor_bad_commands (void)
{
  SoupSession *session = soup_session_new ();
  gchar *url = server_url ("ws");
  GstElement *pipeline = start_pipeline ("videotestsrc is-live=true ! "
      "fakesink name=bad-sink");
  gint64 pad = pad_id ("bad-sink", "sink");
  WsClient client;

  ws_connect (&client, session, url, NULL);
  ws_send (&client, "not json");
  ws_send (&client, "[1, 2]");
  ws_send (&client, "{\"type\":\"monitor\",\"pad\":\"%" G_GINT64_FORMAT
      "\"}", pad);
  ws_send (&client, "{\"type\":\"monitor\",\"pad\":-1}");
  ws_send (&client, "{\"type\":\"explode\",\"pad\":%" G_GINT64_FORMAT "}",
      pad);
  /* Unknown pads are answered with "gone". */
  ws_send (&client, "{\"type\":\"monitor\",\"pad\":999999999}");
  g_assert_cmpint (next_stats (&client, 999999999), ==, -1);
  g_assert_cmpint (viewers (pad), ==, 0);

  ws_disconnect (&client);
  stop_pipeline (pipeline);
  g_free (url);
  g_object_unref (session);
}

/* The next pad-stats entry for pad, which must not be gone. */
static JsonNode *
next_entry (WsClient * client, gint64 pad)
{
  while (TRUE) {
    gchar *message = ws_next (client, "pad-stats");
    JsonNode *root = parse (message);
    JsonArray *pads = json_object_get_array_member (json_object_get_object_member
        (json_node_get_object (root), "stats"), "pads");
    JsonNode *entry = NULL;
    guint i;

    for (i = 0; i < json_array_get_length (pads); i++)
      if (json_object_get_int_member (json_array_get_object_element (pads, i),
              "pad") == pad)
        entry = json_node_copy (json_array_get_element (pads, i));
    json_node_unref (root);
    g_free (message);
    if (entry != NULL)
      return entry;
  }
}

static void
test_monitor_values (void)
{
  SoupSession *session = soup_session_new ();
  gchar *url = server_url ("ws");
  GstElement *pipeline = start_pipeline ("videotestsrc is-live=true ! "
      "video/x-raw,framerate=30/1 ! fakesink name=values-sink");
  gint64 pad = pad_id ("values-sink", "sink");
  WsClient client;
  JsonNode *node = NULL;
  JsonObject *entry, *last, *window;
  gint tries;

  ws_connect (&client, session, url, NULL);
  ws_send (&client, "{\"type\":\"monitor\",\"pad\":%" G_GINT64_FORMAT "}",
      pad);
  /* Wait for a full window. */
  for (tries = 0; tries < 8; tries++) {
    g_clear_pointer (&node, json_node_unref);
    node = next_entry (&client, pad);
  }
  entry = json_node_get_object (node);

  g_assert_cmpint (json_object_get_int_member (entry, "bytes"), >, 0);
  last = json_object_get_object_member (entry, "last");
  g_assert_false (json_object_get_null_member (last, "pts"));
  /* Frame durations are rounded to whole ns. */
  g_assert_cmpint (ABS (json_object_get_int_member (last, "duration") -
          GST_SECOND / 30), <=, 1);
  /* Live, so on time within a frame or two. */
  g_assert_cmpint (ABS (json_object_get_int_member (last, "delay")), <,
      GST_SECOND);
  g_assert_cmpstr (json_object_get_string_member
      (json_object_get_object_member (entry, "segment"), "format"), ==,
      "time");
  window = json_object_get_object_member (entry, "window");
  g_assert_cmpfloat (json_object_get_double_member (window, "buffers_per_s"),
      >, 20);
  g_assert_cmpfloat (json_object_get_double_member (window, "buffers_per_s"),
      <, 40);
  g_assert_cmpint (json_object_get_int_member (window, "gap_avg"), >,
      GST_SECOND / 60);
  json_node_unref (node);

  ws_disconnect (&client);
  stop_pipeline (pipeline);
  g_free (url);
  g_object_unref (session);
}

/* Queue levels and pipeline times */

static GstPadProbeReturn
hold (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  return GST_PAD_PROBE_OK;
}

/* The entry of /api/levels' "queues" whose path ends with name, or NULL. */
static JsonObject *
find_queue (JsonNode * root, const gchar * name)
{
  JsonArray *queues = json_object_get_array_member (json_node_get_object
      (root), "queues");
  guint i;

  for (i = 0; i < json_array_get_length (queues); i++) {
    JsonObject *q = json_array_get_object_element (queues, i);

    if (g_str_has_suffix (json_object_get_string_member (q, "path"), name))
      return q;
  }
  return NULL;
}

/* Polls /api/levels for up to 5 s until the queue has the given number of
 * buffers and at least one overrun. */
static JsonNode *
wait_full (const gchar * name, gint64 buffers)
{
  gint tries;

  for (tries = 0; tries < 50; tries++) {
    gchar *text;
    JsonNode *root;
    JsonObject *q;

    get_status ("api/levels", "GET", NULL, &text, NULL);
    root = parse (text);
    q = find_queue (root, name);
    if (q != NULL && json_object_get_int_member (q, "overruns") > 0 &&
        json_object_get_int_member (json_array_get_object_element
            (json_object_get_array_member (q, "levels"), 0), "buffers") ==
        buffers) {
      g_free (text);
      return root;
    }
    g_free (text);
    json_node_unref (root);
    g_usleep (100 * G_TIME_SPAN_MILLISECOND);
  }
  return NULL;
}

static void
test_levels (void)
{
  GstElement *pipeline = gst_parse_launch ("videotestsrc is-live=true ! "
      "queue name=lv-queue leaky=downstream max-size-buffers=5 ! "
      "fakesink async=false", NULL);
  GstElement *queue = gst_bin_get_by_name (GST_BIN (pipeline), "lv-queue");
  GstPad *src = gst_element_get_static_pad (queue, "src");
  JsonNode *root;
  JsonObject *q, *limits, *p = NULL;
  JsonArray *pipelines;
  guint i;

  /* The queue's thread waits on its first buffer (events and queries still
   * pass, so the source can negotiate); the queue fills up and then drops
   * the oldest (leaky) instead of blocking. */
  gst_pad_add_probe (src, GST_PAD_PROBE_TYPE_BLOCK |
      GST_PAD_PROBE_TYPE_BUFFER, hold, NULL, NULL);
  gst_element_set_state (pipeline, GST_STATE_PLAYING);

  root = wait_full ("/lv-queue", 5);
  g_assert_nonnull (root);
  g_assert_cmpint (json_object_get_int_member (json_node_get_object (root),
          "schema"), ==, 1);
  q = find_queue (root, "/lv-queue");
  g_assert_cmpstr (json_object_get_string_member (q, "kind"), ==, "queue");
  g_assert_cmpstr (json_object_get_string_member (q, "leaky"), ==,
      "downstream");
  limits = json_object_get_object_member (q, "limits");
  g_assert_cmpint (json_object_get_int_member (limits, "buffers"), ==, 5);

  pipelines = json_object_get_array_member (json_node_get_object (root),
      "pipelines");
  for (i = 0; i < json_array_get_length (pipelines); i++) {
    JsonObject *candidate = json_array_get_object_element (pipelines, i);

    if (json_object_get_int_member (candidate, "id") > 0 &&
        g_strcmp0 (json_object_get_string_member (candidate, "path"),
            GST_OBJECT_NAME (pipeline)) == 0)
      p = candidate;
  }
  g_assert_nonnull (p);
  g_assert_cmpstr (json_object_get_string_member (p, "state"), ==,
      "PLAYING");
  g_assert_nonnull (json_object_get_string_member (p, "clock"));
  g_assert_cmpint (json_object_get_int_member (p, "running_time"), >, 0);
  json_node_unref (root);

  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (src);
  gst_object_unref (queue);
  gst_object_unref (pipeline);
}

/* multiqueue reports one level per sink pad, with the pad's id. */
static void
test_levels_multiqueue (void)
{
  GstElement *pipeline = start_pipeline ("videotestsrc is-live=true ! "
      "multiqueue name=lv-mq ! fakesink async=false");
  gint64 pad = pad_id ("lv-mq", "sink_0");
  gboolean found = FALSE;
  gint tries;

  for (tries = 0; tries < 20 && !found; tries++) {
    gchar *text;
    JsonNode *root;
    JsonObject *q;

    get_status ("api/levels", "GET", NULL, &text, NULL);
    root = parse (text);
    g_free (text);
    q = find_queue (root, "/lv-mq");
    if (q != NULL) {
      JsonArray *levels = json_object_get_array_member (q, "levels");

      g_assert_cmpstr (json_object_get_string_member (q, "kind"), ==,
          "multiqueue");
      g_assert_cmpuint (json_array_get_length (levels), ==, 1);
      g_assert_cmpint (json_object_get_int_member
          (json_array_get_object_element (levels, 0), "pad"), ==, pad);
      found = TRUE;
    }
    json_node_unref (root);
    if (!found)
      g_usleep (100 * G_TIME_SPAN_MILLISECOND);
  }
  g_assert_true (found);
  stop_pipeline (pipeline);
}

/* Levels are also sent to every page, every 250 ms. */
static void
test_levels_websocket (void)
{
  SoupSession *session = soup_session_new ();
  gchar *url = server_url ("ws");
  WsClient client;
  gchar *message;

  ws_connect (&client, session, url, NULL);
  message = ws_next (&client, "levels");
  g_assert_nonnull (strstr (message, "\"schema\":1"));
  g_assert_nonnull (strstr (message, "\"queues\":["));
  g_free (message);
  ws_disconnect (&client);
  g_free (url);
  g_object_unref (session);
}

/* Element properties */

/* The id the page would use for an element with this name. */
static gint64
element_id (const gchar * name)
{
  gchar *graph;
  gchar *needle = g_strdup_printf ("\"name\":\"%s\"", name);
  gchar *at;
  gint64 id = 0;

  get_status ("api/graph", "GET", NULL, &graph, NULL);
  /* Elements are written as {"id":N,"name":"...",...}. */
  at = strstr (graph, needle);
  if (at != NULL) {
    while (at > graph && !g_str_has_prefix (at, "{\"id\":"))
      at--;
    id = g_ascii_strtoll (at + strlen ("{\"id\":"), NULL, 10);
  }
  g_free (needle);
  g_free (graph);
  g_assert_cmpint (id, >, 0);
  return id;
}

static JsonObject *
property (JsonNode * root, const gchar * name)
{
  JsonArray *props = json_object_get_array_member (json_node_get_object
      (root), "properties");
  guint i;

  for (i = 0; i < json_array_get_length (props); i++) {
    JsonObject *p = json_array_get_object_element (props, i);

    if (g_strcmp0 (json_object_get_string_member (p, "name"), name) == 0)
      return p;
  }
  return NULL;
}

static JsonNode *
get_properties (gint64 id)
{
  gchar *path = g_strdup_printf ("api/properties?element=%" G_GINT64_FORMAT,
      id);
  gchar *text;
  JsonNode *root;

  g_assert_cmpuint (get_status (path, "GET", NULL, &text, NULL), ==, 200);
  root = parse (text);
  g_free (text);
  g_free (path);
  return root;
}

static void
test_properties (void)
{
  GstElement *pipeline = start_pipeline ("videotestsrc is-live=true ! "
      "queue name=props-queue max-size-buffers=7 leaky=upstream ! "
      "fakesink name=props-sink async=false");
  JsonNode *root;
  JsonObject *p;

  root = get_properties (element_id ("props-queue"));
  p = property (root, "max-size-buffers");
  g_assert_cmpint (json_object_get_int_member (p, "value"), ==, 7);
  g_assert_cmpint (json_object_get_int_member (p, "default"), ==, 200);
  g_assert_true (json_object_get_boolean_member (p, "changed"));
  g_assert_true (json_object_has_member (p, "min"));
  p = property (root, "leaky");
  g_assert_cmpstr (json_object_get_string_member
      (json_object_get_object_member (p, "value"), "nick"), ==, "upstream");
  g_assert_cmpuint (json_array_get_length (json_object_get_array_member (p,
              "choices")), ==, 3);
  json_node_unref (root);

  /* basesink "stats" is a structure with counters. */
  root = get_properties (element_id ("props-sink"));
  p = json_object_get_object_member (property (root, "stats"), "value");
  g_assert_cmpstr (json_object_get_string_member (p, "structure"), ==,
      "application/x-gst-base-sink-stats");
  g_assert_true (json_object_has_member (json_object_get_object_member (p,
              "fields"), "rendered"));
  json_node_unref (root);

  g_assert_cmpuint (get_status ("api/properties?element=999999", "GET", NULL,
          NULL, NULL), ==, 404);
  g_assert_cmpuint (get_status ("api/properties?element=x", "GET", NULL,
          NULL, NULL), ==, 404);
  g_assert_cmpuint (get_status ("api/properties", "GET", NULL, NULL, NULL),
      ==, 404);
  stop_pipeline (pipeline);
}

/* An element whose "slow" property blocks until the test releases it. */
typedef struct
{
  GstElement parent;
} SlowElement;

typedef struct
{
  GstElementClass parent_class;
} SlowElementClass;

G_DEFINE_TYPE (SlowElement, slow_element, GST_TYPE_ELEMENT);

static GMutex slow_lock;
static GCond slow_cond;
static gboolean slow_released;

static void
slow_element_get_property (GObject * object, guint id, GValue * value,
    GParamSpec * pspec)
{
  if (id == 1) {
    g_mutex_lock (&slow_lock);
    while (!slow_released)
      g_cond_wait (&slow_cond, &slow_lock);
    g_mutex_unlock (&slow_lock);
  }
  g_value_set_int (value, id == 1 ? 1 : 7);
}

static void
slow_element_class_init (SlowElementClass * klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = slow_element_get_property;
  g_object_class_install_property (object_class, 1, g_param_spec_int ("slow",
          "slow", "blocks", 0, 10, 0, G_PARAM_READABLE));
  g_object_class_install_property (object_class, 2, g_param_spec_int ("quick",
          "quick", "answers", 0, 10, 0, G_PARAM_READABLE));
}

static void
slow_element_init (SlowElement * self)
{
}

/* A getter that blocks makes the request answer in time, says which
 * property did not answer, and the property is skipped while it blocks;
 * once the getter returns, the property is read again. */
static void
test_properties_not_answering (void)
{
  GstElement *pipeline = gst_pipeline_new ("slow-pipeline");
  GstElement *slow = g_object_new (slow_element_get_type (), "name",
      "slow-element", NULL);
  gint64 id, start;
  JsonNode *root;

  gst_bin_add (GST_BIN (pipeline), slow);
  id = element_id ("slow-element");

  start = g_get_monotonic_time ();
  root = get_properties (id);
  g_assert_cmpint (g_get_monotonic_time () - start, <, 3 * G_USEC_PER_SEC);
  g_assert_cmpstr (json_object_get_string_member (json_node_get_object (root),
          "not_answering"), ==, "slow");
  json_node_unref (root);

  root = get_properties (id);
  g_assert_true (json_object_get_boolean_member (property (root, "slow"),
          "not_answering"));
  g_assert_cmpint (json_object_get_int_member (property (root, "quick"),
          "value"), ==, 7);
  json_node_unref (root);

  g_mutex_lock (&slow_lock);
  slow_released = TRUE;
  g_cond_broadcast (&slow_cond);
  g_mutex_unlock (&slow_lock);

  for (start = 0; start < 50; start++) {
    gboolean back;

    root = get_properties (id);
    back = json_object_has_member (json_node_get_object (root),
        "properties") && !json_object_get_boolean_member_with_default
        (property (root, "slow"), "not_answering", FALSE);
    if (back)
      g_assert_cmpint (json_object_get_int_member (property (root, "slow"),
              "value"), ==, 1);
    json_node_unref (root);
    if (back)
      break;
    g_usleep (20 * G_TIME_SPAN_MILLISECOND);
  }
  g_assert_cmpint (start, <, 50);
  gst_object_unref (pipeline);
}

/* The next "properties" entry for element: rendered count of its stats,
 * -1 if gone, or -2 when the message does not mention the element. */
static gint64
next_rendered (WsClient * client, gint64 element)
{
  gchar *message = ws_next (client, "properties");
  JsonNode *root = parse (message);
  JsonArray *elements = json_object_get_array_member (json_node_get_object
      (root), "elements");
  gint64 result = -2;
  guint i;

  for (i = 0; i < json_array_get_length (elements); i++) {
    JsonObject *e = json_array_get_object_element (elements, i);
    JsonArray *props;

    if (json_object_get_int_member (e, "element") != element)
      continue;
    if (json_object_get_boolean_member_with_default (e, "gone", FALSE)) {
      result = -1;
      break;
    }
    props = json_object_get_array_member (json_object_get_object_member (e,
            "values"), "properties");
    g_assert_cmpuint (json_array_get_length (props), ==, 1);
    result = json_object_get_int_member (json_object_get_object_member
        (json_object_get_object_member (json_array_get_object_element (props,
                    0), "value"), "fields"), "rendered");
  }
  json_node_unref (root);
  g_free (message);
  return result;
}

static void
test_watch_property (void)
{
  SoupSession *session = soup_session_new ();
  gchar *url = server_url ("ws");
  GstElement *pipeline = start_pipeline ("videotestsrc is-live=true ! "
      "fakesink name=watch-sink async=false");
  gint64 id = element_id ("watch-sink");
  WsClient client;
  gint64 first, second;
  gint tries;

  ws_connect (&client, session, url, NULL);
  /* Ignored: bad names and ids. */
  ws_send (&client, "{\"type\":\"watch-property\",\"element\":%"
      G_GINT64_FORMAT ",\"property\":\"a b\"}", id);
  ws_send (&client, "{\"type\":\"watch-property\",\"element\":-1,"
      "\"property\":\"stats\"}");
  ws_send (&client, "{\"type\":\"watch-property\",\"element\":%"
      G_GINT64_FORMAT ",\"property\":\"stats\"}", id);

  first = next_rendered (&client, id);
  second = next_rendered (&client, id);
  g_assert_cmpint (first, >=, 0);
  g_assert_cmpint (second, >=, first);

  /* After unwatching, the element is no longer sent (the message may still
   * come for one tick). */
  ws_send (&client, "{\"type\":\"unwatch-property\",\"element\":%"
      G_GINT64_FORMAT ",\"property\":\"stats\"}", id);
  ws_send (&client, "{\"type\":\"watch-property\",\"element\":%"
      G_GINT64_FORMAT ",\"property\":\"sync\"}", id);
  for (tries = 0; tries < 3; tries++) {
    gchar *message = ws_next (&client, "properties");
    gboolean has_stats = strstr (message, "\"stats\"") != NULL;

    g_free (message);
    if (!has_stats)
      break;
  }
  g_assert_cmpint (tries, <, 3);

  /* An element that goes away is reported once. */
  stop_pipeline (pipeline);
  for (tries = 0; tries < 20; tries++) {
    gchar *message = ws_next (&client, "properties");
    gboolean gone = strstr (message, "\"gone\":true") != NULL;

    g_free (message);
    if (gone)
      break;
  }
  g_assert_cmpint (tries, <, 20);

  ws_disconnect (&client);
  g_free (url);
  g_object_unref (session);
}

/* Latency */

/* The next "latency" message's entry for element, or NULL. */
static JsonNode *
next_latency (WsClient * client, gint64 element, JsonNode ** all)
{
  gchar *message = ws_next (client, "latency");
  JsonNode *root = parse (message);
  JsonObject *latency = json_object_get_object_member (json_node_get_object
      (root), "latency");
  JsonArray *elements = json_object_get_array_member (latency, "elements");
  JsonNode *found = NULL;
  guint i;

  for (i = 0; i < json_array_get_length (elements); i++)
    if (json_object_get_int_member (json_array_get_object_element (elements,
                i), "element") == element)
      found = json_node_copy (json_array_get_element (elements, i));
  if (all != NULL)
    *all = json_node_copy (json_object_get_member (latency, "all"));
  json_node_unref (root);
  g_free (message);
  return found;
}

/* Waits for a full window and returns its average for element, in ns. */
static gint64
measured_avg (WsClient * client, gint64 element)
{
  gint64 avg = -1;
  gint tries;

  for (tries = 0; tries < 40 && avg < 0; tries++) {
    JsonNode *node = next_latency (client, element, NULL);
    JsonObject *m;

    if (node == NULL)
      continue;
    m = json_object_get_object_member (json_node_get_object (node),
        "measured");
    if (tries >= 6 && !json_object_get_null_member (m, "avg"))
      avg = json_object_get_int_member (m, "avg");
    json_node_unref (node);
  }
  return avg;
}

static void
test_latency_element (void)
{
  SoupSession *session = soup_session_new ();
  gchar *url = server_url ("ws");
  /* The queue holds what the late sink has not played yet (~200 ms);
   * identity sleeps 10 ms per 20 ms buffer, so it keeps up. */
  GstElement *pipeline = start_pipeline ("audiotestsrc is-live=true "
      "samplesperbuffer=960 ! audio/x-raw,rate=48000 ! "
      "identity name=lat-sleep sleep-time=10000 ! "
      "queue name=lat-queue max-size-time=500000000 ! fakesink sync=true "
      "ts-offset=200000000 async=false");
  gint64 sleep = element_id ("lat-sleep");
  gint64 queue = element_id ("lat-queue");
  WsClient client;
  gint64 avg;

  ws_connect (&client, session, url, NULL);
  ws_send (&client, "{\"type\":\"measure-latency\",\"element\":%"
      G_GINT64_FORMAT "}", sleep);
  ws_send (&client, "{\"type\":\"measure-latency\",\"element\":%"
      G_GINT64_FORMAT "}", queue);

  avg = measured_avg (&client, sleep);
  g_assert_cmpint (avg, >=, 8 * GST_MSECOND);
  g_assert_cmpint (avg, <, 100 * GST_MSECOND);
  avg = measured_avg (&client, queue);
  g_assert_cmpint (avg, >=, 120 * GST_MSECOND);
  g_assert_cmpint (avg, <, 450 * GST_MSECOND);

  ws_disconnect (&client);
  stop_pipeline (pipeline);
  g_free (url);
  g_object_unref (session);
}

/* "measure all" stops itself above its limit (50 buffers/s in this test,
 * see main). */
static void
test_latency_all_limit (void)
{
  SoupSession *session = soup_session_new ();
  gchar *url = server_url ("ws");
  GstElement *pipeline = start_pipeline ("videotestsrc is-live=true ! "
      "video/x-raw,framerate=60/1 ! queue ! identity ! queue ! "
      "fakesink async=false");
  WsClient client;
  gboolean stopped = FALSE;
  gint tries;

  ws_connect (&client, session, url, NULL);
  ws_send (&client, "{\"type\":\"measure-all\"}");
  for (tries = 0; tries < 20 && !stopped; tries++) {
    JsonNode *all = NULL;
    JsonNode *entry = next_latency (&client, 0, &all);
    JsonObject *a = json_node_get_object (all);

    if (entry != NULL)
      json_node_unref (entry);
    stopped = !json_object_get_boolean_member (a, "on") &&
        !json_object_get_null_member (a, "stopped_at");
    if (stopped)
      g_assert_cmpint (json_object_get_int_member (a, "stopped_at"), >, 50);
    json_node_unref (all);
  }
  g_assert_true (stopped);

  ws_disconnect (&client);
  stop_pipeline (pipeline);
  g_free (url);
  g_object_unref (session);
}

/* A watched pad that goes away is reported once, then dropped. */
static void
test_monitor_pad_gone (void)
{
  SoupSession *session = soup_session_new ();
  gchar *url = server_url ("ws");
  GstElement *pipeline = start_pipeline ("videotestsrc is-live=true ! "
      "fakesink name=gone-sink");
  gint64 pad = pad_id ("gone-sink", "sink");
  WsClient client;

  ws_connect (&client, session, url, NULL);
  ws_send (&client, "{\"type\":\"monitor\",\"pad\":%" G_GINT64_FORMAT "}",
      pad);
  g_assert_cmpint (next_stats (&client, pad), >=, 0);

  stop_pipeline (pipeline);
  g_assert_cmpint (next_stats (&client, pad), ==, -1);
  g_assert_true (wait_viewers (pad, 0));

  ws_disconnect (&client);
  g_free (url);
  g_object_unref (session);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  /* Read when the tracer starts its server, in gst_init. */
  g_setenv ("PM_MEASURE_ALL_LIMIT", "50", TRUE);
  gst_init (&argc, &argv);

  g_test_add_func ("/server/url", test_url);
  g_test_add_func ("/server/http", test_http);
  g_test_add_func ("/server/ui-files", test_ui_files);
  g_test_add_func ("/server/ui-dir", test_ui_dir);
  g_test_add_func ("/server/host-check", test_host_check);
  g_test_add_func ("/server/websocket", test_websocket);
  g_test_add_func ("/server/websocket-log", test_websocket_log);
  g_test_add_func ("/server/monitor", test_monitor);
  g_test_add_func ("/server/monitor-values", test_monitor_values);
  g_test_add_func ("/server/monitor-bad-commands", test_monitor_bad_commands);
  g_test_add_func ("/server/monitor-pad-gone", test_monitor_pad_gone);
  g_test_add_func ("/server/levels", test_levels);
  g_test_add_func ("/server/levels-multiqueue", test_levels_multiqueue);
  g_test_add_func ("/server/levels-websocket", test_levels_websocket);
  g_test_add_func ("/server/properties", test_properties);
  g_test_add_func ("/server/properties-not-answering",
      test_properties_not_answering);
  g_test_add_func ("/server/watch-property", test_watch_property);
  g_test_add_func ("/server/latency-element", test_latency_element);
  g_test_add_func ("/server/latency-all-limit", test_latency_all_limit);
  g_test_add_func ("/server/origin-check", test_origin_check);
  g_test_add_func ("/server/port-fallback", test_port_fallback);
  g_test_add_func ("/server/no-free-port", test_no_free_port);
  g_test_add_func ("/server/shutdown-with-client", test_shutdown_with_client);

  return g_test_run ();
}
