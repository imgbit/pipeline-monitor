/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <gst/gst.h>
#include <gst/gsttracer.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <stdlib.h>
#include <string.h>
#ifdef G_OS_WIN32
#include <sys/utime.h>
#else
#include <utime.h>
#endif
#ifdef G_OS_UNIX
#include <signal.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

#include "envelope.h"
#include "pmrecorder.h"
#include "pmserver.h"

static gchar *pad_finding (const gchar * kind, const gchar * element,
    const gchar * pad);
static gchar *element_finding (const gchar * kind, const gchar * element);
static JsonNode *parse (const gchar * text);

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
  body = without_envelope (body, "graph");
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
  g_assert_cmpuint (get_status ("icon.svg", "GET", NULL, NULL, &type), ==,
      200);
  g_assert_cmpstr (type, ==, "image/svg+xml");
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
  /* Shown in the latency log. */
  const gchar *name;
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
  /* Other messages keep coming: give up after a while, do not hang. */
  gint64 end = g_get_monotonic_time () + 10 * G_USEC_PER_SEC;

  while (message == NULL) {
    if (g_get_monotonic_time () > end)
      g_error ("no \"%s\" message", type);
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
  gchar *message = without_envelope (ws_next (client, "graph"), "graph");
  gboolean found;

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
  JsonNode *root;

  gst_bus_set_flushing (bus, TRUE);
  gst_bin_add (GST_BIN (pipeline), element);

  ws_connect (&client, session, url, NULL);
  g_assert_no_error (client.error);
  message = without_envelope (ws_next (&client, "log"), "log");
  g_assert_true (g_str_has_prefix (message, "{\"enabled\":true,"));
  g_free (message);

  post_warning (element, "ws first");
  message = ws_next (&client, "log");
  g_assert_nonnull (strstr (message, "\"text\":\"ws first\""));
  g_assert_nonnull (strstr (message, "\"count\":1,"));
  /* Times on the wall clock are µs since 1970 (2026: about 1.8e15). */
  root = parse (message);
  g_assert_cmpint (json_object_get_int_member (json_array_get_object_element
          (json_object_get_array_member (json_node_get_object (root),
                  "entries"), 0), "first"), >, G_GINT64_CONSTANT (1000000000000000));
  json_node_unref (root);
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

/* Text that is not valid UTF-8 (GStreamer passes it on) still reaches the
 * page, with replacement characters. */
static void
test_invalid_utf8 (void)
{
  SoupSession *session = soup_session_new ();
  gchar *url = server_url ("ws");
  GstElement *pipeline = gst_pipeline_new (NULL);
  GstElement *element = gst_element_factory_make ("identity", "utf8-log");
  GstBus *bus = gst_element_get_bus (pipeline);
  WsClient client;
  gchar *message, *body;
  JsonNode *root;

  gst_bus_set_flushing (bus, TRUE);
  gst_bin_add (GST_BIN (pipeline), element);

  ws_connect (&client, session, url, NULL);
  g_free (ws_next (&client, "log"));
  post_warning (element, "bad \xff text");
  message = ws_next (&client, "log");
  g_assert_true (g_utf8_validate (message, -1, NULL));
  g_assert_nonnull (strstr (message, "\"text\":\"bad \xef\xbf\xbd text\""));
  g_free (message);

  g_assert_cmpuint (get_status ("api/log", "GET", NULL, &body, NULL), ==,
      SOUP_STATUS_OK);
  g_assert_true (g_utf8_validate (body, -1, NULL));
  root = parse (body);
  json_node_unref (root);
  g_free (body);

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

/* The path the graph gives element name, or its pad if pad is not NULL. */
static gchar *
find_path (JsonObject * element, const gchar * name, const gchar * pad)
{
  JsonArray *list = json_object_get_array_member (element, "pads");
  gchar *path = NULL;
  guint i;

  if (g_strcmp0 (json_object_get_string_member (element, "name"), name) == 0) {
    if (pad == NULL)
      return g_strdup (json_object_get_string_member (element, "path"));
    for (i = 0; i < json_array_get_length (list); i++) {
      JsonObject *p = json_array_get_object_element (list, i);
      if (g_strcmp0 (json_object_get_string_member (p, "name"), pad) == 0)
        return g_strdup (json_object_get_string_member (p, "path"));
    }
  }
  list = json_object_get_array_member (element, "children");
  for (i = 0; i < json_array_get_length (list) && path == NULL; i++)
    path = find_path (json_array_get_object_element (list, i), name, pad);
  return path;
}

static gchar *
graph_path (const gchar * name, const gchar * pad)
{
  gchar *graph, *path = NULL;
  JsonNode *root;
  JsonArray *pipelines;
  guint i;

  get_status ("api/graph", "GET", NULL, &graph, NULL);
  root = parse (graph);
  pipelines = json_object_get_array_member (json_node_get_object (root),
      "pipelines");
  for (i = 0; i < json_array_get_length (pipelines) && path == NULL; i++)
    path = find_path (json_array_get_object_element (pipelines, i), name, pad);
  json_node_unref (root);
  g_free (graph);
  g_assert_nonnull (path);
  return path;
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

/* The next pads entry for pad: buffers, or -1 if it is gone. */
static gint64
next_stats (WsClient * client, gint64 pad)
{
  while (TRUE) {
    gchar *message = ws_next (client, "pads");
    JsonNode *root = parse (message);
    JsonArray *pads = json_object_get_array_member (json_node_get_object
        (root), "pads");
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

/* Number of viewers of pad according to /api/pads, 0 if not watched. */
static gint64
viewers (gint64 pad)
{
  gchar *text;
  JsonNode *root;
  JsonArray *pads;
  gint64 n = 0;
  guint i;

  get_status ("api/pads", "GET", NULL, &text, NULL);
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

  ws_send (&a, "{\"type\":\"watch-pad\",\"pad\":%" G_GINT64_FORMAT "}", pad);
  first = next_stats (&a, pad);
  second = next_stats (&a, pad);
  g_assert_cmpint (first, >=, 0);
  g_assert_cmpint (second, >, first);
  g_assert_true (wait_viewers (pad, 1));

  /* A second viewer shares the probe. */
  ws_send (&b, "{\"type\":\"watch-pad\",\"pad\":%" G_GINT64_FORMAT "}", pad);
  g_assert_cmpint (next_stats (&b, pad), >=, 0);
  g_assert_true (wait_viewers (pad, 2));

  ws_send (&a, "{\"type\":\"unwatch-pad\",\"pad\":%" G_GINT64_FORMAT "}",
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

static guint count_messages (WsClient * client, const gchar * type, gint ms);
static gint64 element_id (const gchar * name);

/* The next error answer, as an object. */
static JsonObject *
next_error (WsClient * client)
{
  gchar *message = ws_next (client, "error");
  JsonNode *root = parse (message);
  JsonObject *error = json_object_ref (json_node_get_object (root));

  json_node_unref (root);
  g_free (message);
  return error;
}

/* Recordings */

static gchar *
read_file (const gchar * dir, const gchar * name)
{
  gchar *path = g_build_filename (dir, name, NULL);
  gchar *text = NULL;

  g_assert_true (g_file_get_contents (path, &text, NULL, NULL));
  g_free (path);
  return text;
}

/* The names in a recordings list, newest first, comma separated. */
static gchar *
recording_names (PmRecorder * recorder)
{
  gchar *list = pm_recorder_list_json (recorder);
  JsonNode *root = parse (list);
  JsonArray *items = json_object_get_array_member (json_node_get_object
      (root), "recordings");
  GString *names = g_string_new (NULL);
  guint i;

  for (i = 0; i < json_array_get_length (items); i++)
    g_string_append_printf (names, "%s%s", i ? "," : "",
        json_object_get_string_member (json_array_get_object_element (items,
                i), "name"));
  json_node_unref (root);
  g_free (list);
  return g_string_free (names, FALSE);
}

/* Passwords in URIs and secret properties are hidden; the rest is not. */
static void
test_recorder_secrets (void)
{
  gchar *out = pm_recorder_hide_secrets ("{\"type\":\"log\",\"text\":"
      "\"cannot open rtsp://admin:s3cret@cam/1 now\",\"properties\":["
      "{\"name\":\"password\",\"value\":\"pw\"},"
      "{\"name\":\"srtp-key\",\"value\":\"00ff\",\"default\":\"aa\"},"
      "{\"name\":\"key-int-max\",\"value\":30},"
      "{\"name\":\"location\",\"value\":\"file:///tmp/a.mp4\"}]}");

  g_assert_null (strstr (out, "s3cret"));
  g_assert_nonnull (strstr (out, "rtsp://(hidden)@cam/1"));
  g_assert_null (strstr (out, "\"pw\""));
  g_assert_null (strstr (out, "00ff"));
  g_assert_null (strstr (out, "\"aa\""));
  g_assert_nonnull (strstr (out, "\"key-int-max\",\"value\":30"));
  g_assert_nonnull (strstr (out, "file:///tmp/a.mp4"));
  g_free (out);

  /* A number too large to read stays JSON (found by fuzzing: it was
   * written back as "inf", and the file could not be read). */
  out = pm_recorder_hide_secrets ("{\"view\":{\"x\":1e999,\"y\":2}}");
  g_assert_cmpstr (out, ==, "{\"view\":{\"x\":null,\"y\":2}}");
  g_free (out);
}

/* A recording: header, messages, end line; listed, downloadable by name,
 * deletable when it is not running. */
static void
test_recorder_files (void)
{
  gchar *dir = g_dir_make_tmp ("pm-rec-XXXXXX", NULL);
  PmRecorder *recorder = pm_recorder_new (dir, 1024 * 1024, 1024 * 1024);
  gchar *names, *text, *path;
  gchar **lines;

  g_assert_true (pm_recorder_start (recorder, "recording", "my prog",
          "{\"version\":\"x\"}", "{\"pins\":[]}", NULL));
  g_assert_false (pm_recorder_start (recorder, "snapshot", "p", NULL, NULL,
          NULL));
  g_assert_true (pm_recorder_write (recorder, "{\"type\":\"levels\"}"));
  names = recording_names (recorder);
  g_assert_true (g_str_has_prefix (names, "pipeline-monitor-my_prog-"));
  g_assert_true (g_str_has_suffix (names, ".jsonl"));
  g_assert_false (pm_recorder_delete (recorder, names));
  pm_recorder_stop (recorder, "stopped");
  g_assert_false (pm_recorder_write (recorder, "{\"type\":\"levels\"}"));

  text = read_file (dir, names);
  lines = g_strsplit (text, "\n", -1);
  g_assert_cmpuint (g_strv_length (lines), ==, 4);
  g_assert_true (g_str_has_prefix (lines[0], "{\"format\":"
          "\"pipeline-monitor-recording\",\"version\":1,\"protocol\":1,"
          "\"kind\":\"recording\",\"started\":"));
  g_assert_nonnull (strstr (lines[0], "\"view\":{\"pins\":[]}"));
  g_assert_cmpstr (lines[1], ==, "{\"type\":\"levels\"}");
  g_assert_true (g_str_has_prefix (lines[2], "{\"type\":\"end\","));
  g_assert_nonnull (strstr (lines[2], "\"reason\":\"stopped\""));
  g_assert_cmpstr (lines[3], ==, "");
  g_strfreev (lines);
  g_free (text);

  path = pm_recorder_path (recorder, names);
  g_assert_nonnull (path);
  g_free (path);
  g_assert_null (pm_recorder_path (recorder, "../pipeline-monitor-x.jsonl"));
  g_assert_null (pm_recorder_path (recorder, "other.jsonl"));
  g_assert_true (pm_recorder_delete (recorder, names));
  g_free (names);
  names = recording_names (recorder);
  g_assert_cmpstr (names, ==, "");
  g_free (names);

  pm_recorder_free (recorder);
  g_rmdir (dir);
  g_free (dir);
}

/* A folder that cannot be made: the start fails and says why. */
static void
test_recorder_no_folder (void)
{
  gchar *dir = g_dir_make_tmp ("pm-rec-XXXXXX", NULL);
  gchar *file = g_build_filename (dir, "not-a-folder", NULL);
  PmRecorder *recorder;
  GError *error = NULL;

  g_assert_true (g_file_set_contents (file, "", 0, NULL));
  recorder = pm_recorder_new (file, 1024, 1024);
  g_assert_false (pm_recorder_start (recorder, "recording", "p", NULL, NULL,
          &error));
  g_assert_nonnull (error);
  g_assert_true (g_str_has_prefix (error->message, "Cannot create "));
  g_assert_false (pm_recorder_running (recorder));
  g_clear_error (&error);
  pm_recorder_free (recorder);
  g_unlink (file);
  g_rmdir (dir);
  g_free (file);
  g_free (dir);
}

static void remove_folder (const gchar * dir);

/* Many recordings: their headers are read once, not on every list (found
 * with 1,000 recordings: every list read every header again); a changed
 * file is read again, a deleted one forgotten. */
static void
test_recorder_many_files (void)
{
  gchar *dir = g_dir_make_tmp ("pm-rec-XXXXXX", NULL);
  PmRecorder *recorder = pm_recorder_new (dir, 1 << 30, (guint64) 1 << 40);
  gchar *view = g_strnfill (60000, 'v');
  gchar *text, *path, *list;
  gint64 start, first, again = G_MAXINT64;
  gint i;

  for (i = 0; i < 400; i++) {
    path = g_strdup_printf ("%s/pipeline-monitor-many-%03d.jsonl", dir, i);
    text = g_strdup_printf ("{\"format\":\"pipeline-monitor-recording\","
        "\"version\":1,\"kind\":\"snapshot\",\"started\":%d,"
        "\"view\":{\"x\":\"%s\"}}\n{\"type\":\"end\"}\n", i + 1, view);
    g_assert_true (g_file_set_contents (path, text, -1, NULL));
    g_free (text);
    g_free (path);
  }

  start = g_get_monotonic_time ();
  list = pm_recorder_list_json (recorder);
  first = g_get_monotonic_time () - start;
  g_assert_nonnull (strstr (list, "pipeline-monitor-many-399.jsonl"));
  g_assert_null (strstr (list, "\"kind\":null"));
  g_free (list);
  for (i = 0; i < 3; i++) {
    start = g_get_monotonic_time ();
    g_free (pm_recorder_list_json (recorder));
    again = MIN (again, g_get_monotonic_time () - start);
  }
  /* For information: how much faster depends on the system (listing the
   * folder itself costs more on Windows). */
  g_test_message ("listing 400 recordings: %" G_GINT64_FORMAT " us first, "
      "%" G_GINT64_FORMAT " us after", first, again);

  /* Not read again while it looks the same: another header of the same
   * size, with the file's time put back, still lists the first one. */
  {
    GStatBuf st;
    struct utimbuf times;
    gchar *before, *after;

    path = g_strdup_printf ("%s/pipeline-monitor-many-003.jsonl", dir);
    g_assert_cmpint (g_stat (path, &st), ==, 0);
    g_assert_true (g_file_get_contents (path, &before, NULL, NULL));
    after = g_strdup (before);
    memcpy (strstr (after, "snapshot"), "SNAPSHOT", 8);
    g_assert_true (g_file_set_contents (path, after, -1, NULL));
    times.actime = st.st_atime;
    times.modtime = st.st_mtime;
    g_assert_cmpint (g_utime (path, &times), ==, 0);
    list = pm_recorder_list_json (recorder);
    g_assert_null (strstr (list, "SNAPSHOT"));
    g_free (list);
    g_free (before);
    g_free (after);
    g_free (path);
  }

  /* Rewritten (another size): read again. */
  path = g_strdup_printf ("%s/pipeline-monitor-many-007.jsonl", dir);
  g_assert_true (g_file_set_contents (path, "{\"format\":"
          "\"pipeline-monitor-recording\",\"version\":1,\"kind\":"
          "\"recording\",\"started\":5}\n", -1, NULL));
  list = pm_recorder_list_json (recorder);
  g_assert_nonnull (strstr (list, "\"name\":\"pipeline-monitor-many-007.jsonl"
          "\",\"kind\":\"recording\""));
  g_free (list);
  /* Deleted: gone from the list. */
  g_assert_cmpint (g_unlink (path), ==, 0);
  g_free (path);
  list = pm_recorder_list_json (recorder);
  g_assert_null (strstr (list, "pipeline-monitor-many-007.jsonl"));
  g_free (list);

  pm_recorder_free (recorder);
  remove_folder (dir);
  g_free (dir);
  g_free (view);
}

/* A file ends at its limit; the oldest files go when the folder is full. */
static void
test_recorder_limits (void)
{
  gchar *dir = g_dir_make_tmp ("pm-rec-XXXXXX", NULL);
  PmRecorder *recorder = pm_recorder_new (dir, 2000, 5000);
  gchar *line = g_strdup_printf ("{\"type\":\"levels\",\"pad\":\"%s\"}",
      "0123456789012345678901234567890123456789");
  gchar *names, *text, **list;
  gint i, written = 0;

  g_assert_true (pm_recorder_start (recorder, "recording", "p", NULL, NULL,
          NULL));
  for (i = 0; i < 100 && pm_recorder_write (recorder, line); i++)
    written++;
  g_assert_cmpint (written, >, 10);
  g_assert_cmpint (written, <, 60);
  g_assert_false (pm_recorder_running (recorder));
  names = recording_names (recorder);
  text = read_file (dir, names);
  g_assert_nonnull (strstr (text, "\"reason\":\"limit\""));
  g_free (text);
  g_free (names);

  /* Each about 2 KB: with 5 KB in total, the oldest are removed. */
  for (i = 0; i < 4; i++) {
    g_usleep (1100 * G_TIME_SPAN_MILLISECOND);
    g_assert_true (pm_recorder_start (recorder, "recording", "p", NULL,
            NULL, NULL));
    while (pm_recorder_write (recorder, line));
  }
  names = recording_names (recorder);
  list = g_strsplit (names, ",", -1);
  g_assert_cmpuint (g_strv_length (list), <=, 3);
  g_strfreev (list);
  g_free (names);

  for (names = recording_names (recorder), list = g_strsplit (names, ",", -1),
      i = 0; list[i] != NULL && *list[i] != '\0'; i++)
    g_assert_true (pm_recorder_delete (recorder, list[i]));
  g_strfreev (list);
  g_free (names);
  pm_recorder_free (recorder);
  g_rmdir (dir);
  g_free (dir);
  g_free (line);
}

/* Removes the files in a test folder, then the folder. */
static void
remove_folder (const gchar * dir)
{
  GDir *d = g_dir_open (dir, 0, NULL);
  const gchar *name;

  while (d != NULL && (name = g_dir_read_name (d)) != NULL) {
    gchar *path = g_build_filename (dir, name, NULL);

    g_unlink (path);
    g_free (path);
  }
  if (d != NULL)
    g_dir_close (d);
  g_rmdir (dir);
}

static JsonObject *
recordings_list (PmRecorder * recorder)
{
  gchar *text = pm_recorder_list_json (recorder);
  JsonNode *root = parse (text);
  JsonObject *list = json_object_ref (json_node_get_object (root));

  json_node_unref (root);
  g_free (text);
  return list;
}

/* Files in the folder that are not (whole) recordings are listed without a
 * kind and can be deleted; the folder's limits keep a file in. */
static void
test_recorder_broken_files (void)
{
  gchar *dir = g_dir_make_tmp ("pm-rec-XXXXXX", NULL);
  const gchar *contents[] = { "", "not json\n", "[1,2]\n",
    "{\"format\":\"pipeline-monitor-recording\"}\n"
  };
  PmRecorder *recorder = pm_recorder_new (dir, 5000, 1000);
  JsonObject *list;
  JsonArray *items;
  guint i;

  for (i = 0; i < G_N_ELEMENTS (contents); i++) {
    gchar *name = g_strdup_printf ("pipeline-monitor-broken-%u.jsonl", i);
    gchar *path = g_build_filename (dir, name, NULL);

    g_assert_true (g_file_set_contents (path, contents[i], -1, NULL));
    g_free (path);
    g_free (name);
  }
  list = recordings_list (recorder);
  g_assert_cmpint (json_object_get_int_member (list, "total_max"), ==, 5000);
  g_assert_true (json_object_get_null_member (list, "failure"));
  items = json_object_get_array_member (list, "recordings");
  g_assert_cmpuint (json_array_get_length (items), ==, 4);
  for (i = 0; i < 4; i++) {
    JsonObject *item = json_array_get_object_element (items, i);

    g_assert_true (json_object_get_null_member (item, "kind"));
    g_assert_cmpint (json_object_get_int_member (item, "started"), ==, 0);
    g_assert_true (pm_recorder_delete (recorder,
            json_object_get_string_member (item, "name")));
  }
  json_object_unref (list);
  pm_recorder_free (recorder);
  remove_folder (dir);
  g_free (dir);
}

#ifdef G_OS_UNIX
/* Recordings are only for the user to read. */
static void
test_recorder_private (void)
{
  gchar *dir = g_dir_make_tmp ("pm-rec-XXXXXX", NULL);
  PmRecorder *recorder = pm_recorder_new (dir, 5000, 5000);
  gchar *names, *path;
  GStatBuf st;

  g_assert_true (pm_recorder_start (recorder, "snapshot", "p", NULL, NULL,
          NULL));
  pm_recorder_stop (recorder, "snapshot");
  names = recording_names (recorder);
  path = g_build_filename (dir, names, NULL);
  g_assert_cmpint (g_stat (path, &st), ==, 0);
  g_assert_cmpint (st.st_mode & 0777, ==, 0600);
  g_free (path);
  g_free (names);
  pm_recorder_free (recorder);
  remove_folder (dir);
  g_free (dir);
}

/* A folder the user may not write in: the start fails and says why. */
static void
test_recorder_read_only (void)
{
  gchar *dir = g_dir_make_tmp ("pm-rec-XXXXXX", NULL);
  PmRecorder *recorder = pm_recorder_new (dir, 5000, 5000);
  GError *error = NULL;
  gchar *names;

  if (geteuid () == 0) {
    g_test_skip ("root may write anywhere");
    pm_recorder_free (recorder);
    g_rmdir (dir);
    g_free (dir);
    return;
  }
  g_assert_cmpint (g_chmod (dir, 0500), ==, 0);
  g_assert_false (pm_recorder_start (recorder, "recording", "p", NULL, NULL,
          &error));
  g_assert_nonnull (error);
  g_assert_true (g_str_has_prefix (error->message, "Cannot write in "));
  g_clear_error (&error);
  g_assert_false (pm_recorder_running (recorder));
  g_assert_false (pm_recorder_write (recorder, "{\"type\":\"levels\"}"));
  names = recording_names (recorder);
  g_assert_cmpstr (names, ==, "");
  g_free (names);
  g_chmod (dir, 0700);
  pm_recorder_free (recorder);
  remove_folder (dir);
  g_free (dir);
}

/* A full disk (here: a file size limit for the process). A recording that
 * cannot write its header does not start and leaves no file; one that
 * cannot write a message ends, and the list says why. */
static void
test_recorder_disk_full (void)
{
  gchar *dir = g_dir_make_tmp ("pm-rec-XXXXXX", NULL);
  PmRecorder *recorder = pm_recorder_new (dir, 1000000, 1000000);
  struct rlimit saved, small;
  void (*handler) (int) = signal (SIGXFSZ, SIG_IGN);
  GError *error = NULL;
  JsonObject *list;
  gchar *names;
  gint i;

  g_assert_cmpint (getrlimit (RLIMIT_FSIZE, &saved), ==, 0);
  small = saved;
  small.rlim_cur = 0;
  g_assert_cmpint (setrlimit (RLIMIT_FSIZE, &small), ==, 0);
  g_assert_false (pm_recorder_start (recorder, "recording", "p", NULL, NULL,
          &error));
  g_assert_nonnull (error);
  g_clear_error (&error);
  names = recording_names (recorder);
  g_assert_cmpstr (names, ==, "");
  g_free (names);

  small.rlim_cur = 2000;
  g_assert_cmpint (setrlimit (RLIMIT_FSIZE, &small), ==, 0);
  g_assert_true (pm_recorder_start (recorder, "recording", "p", NULL, NULL,
          NULL));
  for (i = 0; i < 1000 && pm_recorder_write (recorder,
          "{\"type\":\"levels\",\"queues\":[]}"); i++);
  g_assert_cmpint (setrlimit (RLIMIT_FSIZE, &saved), ==, 0);
  signal (SIGXFSZ, handler);
  g_assert_cmpint (i, <, 1000);
  g_assert_false (pm_recorder_running (recorder));
  list = recordings_list (recorder);
  g_assert_nonnull (strstr (json_object_get_string_member (list, "failure"),
          "ended"));
  json_object_unref (list);

  /* The next recording starts afresh. */
  g_assert_true (pm_recorder_start (recorder, "snapshot", "p", NULL, NULL,
          NULL));
  pm_recorder_stop (recorder, "snapshot");
  list = recordings_list (recorder);
  g_assert_true (json_object_get_null_member (list, "failure"));
  json_object_unref (list);
  pm_recorder_free (recorder);
  remove_folder (dir);
  g_free (dir);
}
#endif

static gchar *check_message (const gchar * text);

/* The newest recording in the next "recordings" message: name (to free),
 * kind and whether it runs; FALSE if the list is empty. */
static gboolean
next_recording (WsClient * client, gchar ** name, gchar ** kind,
    gboolean * running)
{
  gchar *message = ws_next (client, "recordings");
  JsonNode *root = parse (message);
  JsonArray *list = json_object_get_array_member (json_node_get_object (root),
      "recordings");
  gboolean any = json_array_get_length (list) > 0;

  if (any) {
    JsonObject *r = json_array_get_object_element (list, 0);

    *name = g_strdup (json_object_get_string_member (r, "name"));
    if (kind != NULL)
      *kind = g_strdup (json_object_get_string_member (r, "kind"));
    *running = json_object_get_boolean_member (r, "running");
  }
  json_node_unref (root);
  g_free (message);
  return any;
}

/* Record, stop, snapshot, download and delete, as the page does. */
static void
test_recordings (void)
{
  SoupSession *session = soup_session_new ();
  gchar *url = server_url ("ws");
  GstElement *pipeline = start_pipeline ("videotestsrc is-live=true ! "
      "queue name=rc-queue ! fakesink name=rc-sink");
  gchar *name = NULL, *kind = NULL, *path, *body, *queue_path, *member;
  gboolean running = FALSE;
  JsonObject *error;
  WsClient client;

  ws_connect (&client, session, url, NULL);
  ws_send (&client, "{\"type\":\"watch-property\",\"element\":%"
      G_GINT64_FORMAT ",\"property\":\"stats\"}", element_id ("rc-sink"));
  ws_send (&client, "{\"type\":\"watch-pad\",\"pad\":%" G_GINT64_FORMAT
      "}", pad_id ("rc-queue", "src"));
  g_assert_cmpuint (count_messages (&client, "recordings", 300), >=, 1);

  ws_send (&client, "{\"type\":\"record-start\",\"view\":"
      "{\"pins\":[\"pipeline0/rc-queue:src\"]}}");
  do {
    g_clear_pointer (&name, g_free);
    g_assert_true (next_recording (&client, &name, NULL, &running));
  } while (!running);
  ws_send (&client, "{\"type\":\"record-start\"}");
  error = next_error (&client);
  g_assert_cmpstr (json_object_get_string_member (error, "reason"), ==,
      "A recording is already running.");
  json_object_unref (error);
  count_messages (&client, "levels", 2200);
  ws_send (&client, "{\"type\":\"record-stop\"}");
  do {
    g_clear_pointer (&name, g_free);
    g_assert_true (next_recording (&client, &name, NULL, &running));
  } while (running);
  ws_send (&client, "{\"type\":\"record-stop\"}");
  error = next_error (&client);
  g_assert_cmpstr (json_object_get_string_member (error, "reason"), ==,
      "No recording is running.");
  json_object_unref (error);

  path = g_strconcat ("api/recordings/", name, NULL);
  g_assert_cmpuint (get_status (path, "GET", NULL, &body, NULL), ==,
      SOUP_STATUS_OK);
  g_assert_true (g_str_has_prefix (body, "{\"format\":"
          "\"pipeline-monitor-recording\""));
  g_assert_nonnull (strstr (body, "\"view\":{\"pins\":"
          "[\"pipeline0/rc-queue:src\"]}"));
  g_assert_nonnull (strstr (body, "\n{\"type\":\"graph\","));
  g_assert_nonnull (strstr (body, "\n{\"type\":\"pads\","));
  g_assert_nonnull (strstr (body, "\"name\":\"stats\""));
  g_assert_true (g_str_has_suffix (body, "\"reason\":\"stopped\"}\n"));
  g_free (body);
  g_free (path);

  /* A snapshot: the state of one moment, every element's properties. */
  ws_send (&client, "{\"type\":\"snapshot\"}");
  do {
    g_clear_pointer (&kind, g_free);
    g_free (name);
    g_assert_true (next_recording (&client, &name, &kind, &running));
  } while (running || g_strcmp0 (kind, "snapshot") != 0);
  path = g_strconcat ("api/recordings/", name, NULL);
  g_assert_cmpuint (get_status (path, "GET", NULL, &body, NULL), ==,
      SOUP_STATUS_OK);
  g_assert_nonnull (strstr (body, "\"kind\":\"snapshot\""));
  queue_path = graph_path ("rc-queue", NULL);
  member = g_strdup_printf ("\"path\":\"%s\"", queue_path);
  g_assert_nonnull (strstr (body, member));
  g_free (member);
  g_free (queue_path);
  g_assert_true (g_str_has_suffix (body, "\"reason\":\"snapshot\"}\n"));
  g_free (body);
  g_free (path);

  g_assert_cmpuint (get_status ("api/recordings/other.jsonl", "GET", NULL,
          NULL, NULL), ==, SOUP_STATUS_NOT_FOUND);
  g_assert_cmpuint (get_status ("api/recordings", "GET", NULL, &body, NULL),
      ==, SOUP_STATUS_OK);
  g_free (check_message (body));
  g_free (body);

  /* Delete both (each answer lists what is left); a name that is not there
   * is an error. */
  do {
    ws_send (&client, "{\"type\":\"record-delete\",\"name\":\"%s\"}",
        name);
    g_free (name);
  } while (next_recording (&client, &name, NULL, &running));
  ws_send (&client, "{\"type\":\"record-delete\",\"name\":\"x.jsonl\"}");
  error = next_error (&client);
  g_assert_cmpstr (json_object_get_string_member (error, "command"), ==,
      "record-delete");
  json_object_unref (error);

  g_free (kind);
  ws_disconnect (&client);
  stop_pipeline (pipeline);
  g_free (url);
  g_object_unref (session);
}

/* The rules every message follows (docs/protocol.md). */
static const gchar *message_types[] = { "hello", "graph", "log", "findings",
  "levels", "pads", "properties", "latency", "error", "bye", "watched-pads",
  "recordings", NULL
};

/* Each reference to an element or pad has its path next to its id. */
static void
check_references (JsonNode * node)
{
  static const gchar *pairs[][2] = { {"element", "path"}, {"pad", "pad_path"},
  {"from", "from_path"}, {"to", "to_path"}
  };
  GList *members, *l;
  guint i;

  if (JSON_NODE_HOLDS_ARRAY (node)) {
    JsonArray *array = json_node_get_array (node);

    for (i = 0; i < json_array_get_length (array); i++)
      check_references (json_array_get_element (array, i));
    return;
  }
  if (!JSON_NODE_HOLDS_OBJECT (node))
    return;
  if (!json_object_get_boolean_member_with_default (json_node_get_object
          (node), "gone", FALSE))
    for (i = 0; i < G_N_ELEMENTS (pairs); i++)
      if (json_object_has_member (json_node_get_object (node), pairs[i][0]) &&
          !json_object_has_member (json_node_get_object (node), pairs[i][1]))
        g_error ("\"%s\" without \"%s\"", pairs[i][0], pairs[i][1]);
  members = json_object_get_members (json_node_get_object (node));
  for (l = members; l != NULL; l = l->next)
    /* Property values are the element's own data. */
    if (g_strcmp0 (l->data, "value") != 0 && g_strcmp0 (l->data,
            "default") != 0)
      check_references (json_object_get_member (json_node_get_object (node),
              l->data));
  g_list_free (members);
}

/* Checks a message; returns its type. */
static gchar *
check_message (const gchar * text)
{
  JsonNode *root = parse (text);
  JsonObject *o = json_node_get_object (root);
  gchar *type = g_strdup (json_object_get_string_member (o, "type"));

  g_assert_true (g_strv_contains (message_types, type));
  g_assert_cmpint (json_object_get_int_member (o, "protocol"), ==, 1);
  g_assert_cmpint (json_object_get_int_member (o, "time"), >,
      G_GINT64_CONSTANT (1000000000000000));
  check_references (root);
  json_node_unref (root);
  return type;
}

/* Every message and answer follows the rules, and each type shows up. */
static void
test_protocol (void)
{
  const gchar *expected[] = { "hello", "graph", "log", "findings", "levels",
    "pads", "properties", "latency", "error", NULL
  };
  const gchar *endpoints[] = { "api/hello", "api/graph", "api/log",
    "api/findings", "api/levels", "api/pads", NULL
  };
  SoupSession *session = soup_session_new ();
  gchar *url = server_url ("ws");
  /* The second branch pushes into a tee without outputs: a finding. */
  GstElement *pipeline = start_pipeline ("videotestsrc is-live=true ! "
      "tee name=pr-tee ! queue name=pr-queue ! fakesink name=pr-sink "
      "pr-tee. ! queue name=pr-dead ! tee");
  GstElement *queue = gst_bin_get_by_name (GST_BIN (pipeline), "pr-queue");
  GHashTable *seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
      NULL);
  gint64 end = g_get_monotonic_time () + 8 * G_USEC_PER_SEC;
  WsClient client;
  gchar *body, *path;
  guint i;

  post_warning (queue, "protocol");
  ws_connect (&client, session, url, NULL);
  ws_send (&client, "{\"type\":\"watch-pad\",\"pad\":%" G_GINT64_FORMAT
      "}", pad_id ("pr-queue", "src"));
  ws_send (&client, "{\"type\":\"measure-element\",\"element\":%"
      G_GINT64_FORMAT "}", element_id ("pr-queue"));
  ws_send (&client, "{\"type\":\"measure-path\",\"from\":%"
      G_GINT64_FORMAT ",\"to\":%" G_GINT64_FORMAT "}",
      pad_id ("pr-queue", "sink"), pad_id ("pr-queue", "src"));
  ws_send (&client, "{\"type\":\"watch-property\",\"element\":%"
      G_GINT64_FORMAT ",\"property\":\"stats\"}", element_id ("pr-sink"));
  ws_send (&client, "{\"type\":\"explode\"}");

  while (g_get_monotonic_time () < end &&
      g_hash_table_size (seen) < g_strv_length ((gchar **) expected)) {
    if (!g_main_context_iteration (NULL, FALSE))
      g_usleep (1000);
    while (!g_queue_is_empty (&client.messages)) {
      gchar *message = g_queue_pop_head (&client.messages);
      gchar *type = check_message (message);

      if (g_strv_contains (expected, type))
        g_hash_table_add (seen, type);
      else
        g_free (type);
      g_free (message);
    }
  }
  for (i = 0; expected[i] != NULL; i++)
    if (!g_hash_table_contains (seen, expected[i]))
      g_error ("no \"%s\" message", expected[i]);

  for (i = 0; endpoints[i] != NULL; i++) {
    g_assert_cmpuint (get_status (endpoints[i], "GET", NULL, &body, NULL), ==,
        SOUP_STATUS_OK);
    g_free (check_message (body));
    g_free (body);
  }
  path = g_strdup_printf ("api/properties?element=%" G_GINT64_FORMAT,
      element_id ("pr-sink"));
  g_assert_cmpuint (get_status (path, "GET", NULL, &body, NULL), ==,
      SOUP_STATUS_OK);
  g_free (check_message (body));
  g_free (body);
  g_free (path);

  ws_disconnect (&client);
  g_hash_table_unref (seen);
  gst_object_unref (queue);
  stop_pipeline (pipeline);
  g_free (url);
  g_object_unref (session);
}

/* Every connection starts with hello; the same over HTTP. */
static void
test_hello (void)
{
  SoupSession *session = soup_session_new ();
  gchar *url = server_url ("ws");
  gchar *body, *expected;
  WsClient client;
  JsonNode *root;
  JsonObject *hello;

  ws_connect (&client, session, url, NULL);
  client.got_message = FALSE;
  g_assert_true (g_queue_get_length (&client.messages) > 0 ||
      wait_for (&client.got_message));
  root = parse (g_queue_peek_head (&client.messages));
  hello = json_node_get_object (root);
  g_assert_cmpstr (json_object_get_string_member (hello, "type"), ==,
      "hello");
  g_assert_cmpint (json_object_get_int_member (hello, "protocol"), ==, 1);
  g_assert_cmpstr (json_object_get_string_member (hello, "version"), ==,
      VERSION);
  g_assert_true (g_str_has_prefix (json_object_get_string_member (hello,
              "gstreamer"), "1."));
  g_assert_cmpint (json_object_get_int_member (hello, "pid"), >, 0);
  json_node_unref (root);
  ws_disconnect (&client);

  g_assert_cmpuint (get_status ("api/hello", "GET", NULL, &body, NULL), ==,
      SOUP_STATUS_OK);
  body = without_envelope (body, "hello");
  expected = g_strdup_printf ("{\"version\":\"%s\",", VERSION);
  g_assert_true (g_str_has_prefix (body, expected));
  g_free (expected);
  g_free (body);
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
  JsonObject *error;
  gchar *long_command;

  ws_connect (&client, session, url, NULL);
  ws_send (&client, "not json");
  ws_send (&client, "[1, 2]");
  ws_send (&client, "{\"type\":\"watch-pad\",\"pad\":\"%" G_GINT64_FORMAT
      "\"}", pad);
  ws_send (&client, "{\"type\":\"watch-pad\",\"pad\":-1}");
  ws_send (&client, "{\"type\":\"explode\",\"pad\":%" G_GINT64_FORMAT "}",
      pad);
  /* Fields of the wrong type, for every command. */
  ws_send (&client, "{\"type\":5}");
  ws_send (&client, "{\"type\":null}");
  ws_send (&client, "{\"type\":[\"monitor\"],\"pad\":%" G_GINT64_FORMAT
      "}", pad);
  ws_send (&client, "{\"type\":\"watch-pad\",\"pad\":%" G_GINT64_FORMAT
      ".5}", pad);
  ws_send (&client, "{\"type\":\"measure-element\",\"element\":\"1\"}");
  ws_send (&client, "{\"type\":\"measure-element\",\"element\":{}}");
  ws_send (&client, "{\"type\":\"measure-element\",\"element\":1.5}");
  ws_send (&client, "{\"type\":\"measure-element\",\"element\":true}");
  ws_send (&client, "{\"type\":\"measure-path\",\"from\":\"1\",\"to\":2}");
  ws_send (&client, "{\"type\":\"measure-path\",\"from\":1,\"to\":[2]}");
  ws_send (&client, "{\"type\":\"measure-\"}");
  ws_send (&client, "{\"type\":\"unmeasure-element\"}");
  ws_send (&client, "{\"type\":\"watch-property\",\"element\":1,"
      "\"property\":5}");
  ws_send (&client, "{\"type\":\"watch-property\",\"element\":\"1\","
      "\"property\":\"name\"}");
  ws_send (&client, "{\"type\":\"watch-property\",\"element\":1,"
      "\"property\":\"a b\"}");
  /* Things that do not exist. */
  ws_send (&client, "{\"type\":\"measure-element\",\"element\":999999}");
  ws_send (&client, "{\"type\":\"watch-property\",\"element\":%"
      G_GINT64_FORMAT ",\"property\":\"no-such-property\"}",
      element_id ("bad-sink"));
  ws_send (&client, "{\"type\":\"measure-path\",\"from\":999998,"
      "\"to\":999999}");
  long_command = g_strnfill (5000, ' ');
  ws_send (&client, "%s", long_command);
  g_free (long_command);
  /* Each is answered with an error, and none started anything. */
  g_assert_cmpuint (count_messages (&client, "error", 600), ==, 24);
  g_assert_cmpuint (count_messages (&client, "latency", 600), ==, 0);
  g_assert_cmpuint (count_messages (&client, "properties", 600), ==, 0);

  ws_send (&client, "not json");
  error = next_error (&client);
  g_assert_true (json_object_get_null_member (error, "command"));
  g_assert_cmpstr (json_object_get_string_member (error, "reason"), ==,
      "Not valid JSON.");
  json_object_unref (error);
  /* An empty message (found by fuzzing: it reached the JSON parser as
   * NULL, a GLib critical in the program). */
  ws_send (&client, "%s", "");
  error = next_error (&client);
  g_assert_cmpstr (json_object_get_string_member (error, "reason"), ==,
      "A command is a JSON object.");
  json_object_unref (error);
  ws_send (&client, "{\"type\":\"watch-pad\",\"pad\":\"x\"}");
  error = next_error (&client);
  g_assert_cmpstr (json_object_get_string_member (error, "command"), ==,
      "watch-pad");
  g_assert_cmpstr (json_object_get_string_member (error, "reason"), ==,
      "\"pad\" must be a positive whole number.");
  json_object_unref (error);

  /* Unknown pads are answered with "gone", not an error. */
  ws_send (&client, "{\"type\":\"watch-pad\",\"pad\":999999999}");
  g_assert_cmpint (next_stats (&client, 999999999), ==, -1);
  g_assert_cmpint (viewers (pad), ==, 0);
  g_assert_cmpuint (count_messages (&client, "error", 300), ==, 0);

  ws_disconnect (&client);
  stop_pipeline (pipeline);
  g_free (url);
  g_object_unref (session);
}

/* The next pads entry for pad, which must not be gone. */
static JsonNode *
next_entry (WsClient * client, gint64 pad)
{
  while (TRUE) {
    gchar *message = ws_next (client, "pads");
    JsonNode *root = parse (message);
    JsonArray *pads = json_object_get_array_member (json_node_get_object
        (root), "pads");
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
  ws_send (&client, "{\"type\":\"watch-pad\",\"pad\":%" G_GINT64_FORMAT "}",
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
          "protocol"), ==, 1);
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

    if (json_object_get_int_member (candidate, "element") > 0 &&
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

/* An element without pads that gives the pipeline its clock, as an audio
 * sink does. */
typedef GstElement PmTestClock;
typedef GstElementClass PmTestClockClass;
G_DEFINE_TYPE (PmTestClock, pm_test_clock, GST_TYPE_ELEMENT);

static GstClock *
pm_test_clock_provide (GstElement * element)
{
  return gst_system_clock_obtain ();
}

static void
pm_test_clock_class_init (PmTestClockClass * klass)
{
  klass->provide_clock = pm_test_clock_provide;
}

static void
pm_test_clock_init (PmTestClock * element)
{
  GST_OBJECT_FLAG_SET (element, GST_ELEMENT_FLAG_PROVIDE_CLOCK);
}

/* queue2 reports how full its buffer is; the clock's provider is named. */
static void
test_levels_buffering_clock (void)
{
  GstElement *pipeline = start_pipeline ("audiotestsrc is-live=true ! "
      "queue2 name=bf-queue use-buffering=true ! fakesink async=false");
  GstElement *clock = g_object_new (pm_test_clock_get_type (), "name",
      "bf-clock", NULL);
  gboolean buffering = FALSE, provider = FALSE;
  gint tries;

  gst_element_set_state (pipeline, GST_STATE_PAUSED);
  gst_bin_add (GST_BIN (pipeline), clock);
  gst_element_set_state (pipeline, GST_STATE_PLAYING);

  for (tries = 0; tries < 50 && !(buffering && provider); tries++) {
    gchar *text;
    JsonNode *root;
    JsonObject *q;
    JsonArray *pipelines;
    guint i;

    g_usleep (100 * G_TIME_SPAN_MILLISECOND);
    get_status ("api/levels", "GET", NULL, &text, NULL);
    root = parse (text);
    q = find_queue (root, "/bf-queue");
    if (q != NULL && json_object_has_member (q, "buffering")) {
      gint64 percent = json_object_get_int_member (q, "buffering");

      g_assert_cmpint (percent, >=, 0);
      g_assert_cmpint (percent, <=, 100);
      buffering = TRUE;
    }
    pipelines = json_object_get_array_member (json_node_get_object (root),
        "pipelines");
    for (i = 0; i < json_array_get_length (pipelines); i++) {
      JsonObject *p = json_array_get_object_element (pipelines, i);
      const gchar *path = json_object_get_string_member_with_default (p,
          "clock_provider", NULL);

      if (path != NULL && g_str_has_suffix (path, "/bf-clock"))
        provider = TRUE;
    }
    json_node_unref (root);
    g_free (text);
  }
  g_assert_true (buffering);
  g_assert_true (provider);
  stop_pipeline (pipeline);
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
  g_assert_nonnull (strstr (message, "\"protocol\":1"));
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
  JsonObject *element = json_array_get_object_element
      (json_object_get_array_member (json_node_get_object (root), "elements"),
      0);
  JsonArray *props = json_object_get_array_member (element, "properties");
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
  g_assert_cmpstr (json_object_get_string_member (json_array_get_object_element
          (json_object_get_array_member (json_node_get_object (root),
                  "elements"), 0), "not_answering"), ==, "slow");
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
    back = json_object_has_member (json_array_get_object_element
        (json_object_get_array_member (json_node_get_object (root),
                "elements"), 0), "properties") &&
        !json_object_get_boolean_member_with_default
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
    props = json_object_get_array_member (e, "properties");
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
/* The latency messages a test read, printed when one of its checks fails:
 * which page, the server's time, when it arrived here, "all" and the
 * elements in it. */
static GString *latency_log;

static void
note_latency (WsClient * client, const gchar * message)
{
  JsonNode *root = json_from_string (message, NULL);
  JsonObject *latency;
  JsonArray *elements;
  gchar *all;
  guint i;

  if (latency_log == NULL || root == NULL) {
    if (root != NULL)
      json_node_unref (root);
    return;
  }
  latency = json_node_get_object (root);
  elements = json_object_get_array_member (latency, "elements");
  all = json_to_string (json_object_get_member (latency, "all"), FALSE);
  g_string_append_printf (latency_log, "%s time=%" G_GINT64_FORMAT
      " here=%" G_GINT64_FORMAT " all=%s elements=[", client->name ?
      client->name : "?", json_object_get_int_member (latency, "time"),
      g_get_monotonic_time () / 1000, all);
  for (i = 0; elements && i < json_array_get_length (elements); i++)
    g_string_append_printf (latency_log, "%s%" G_GINT64_FORMAT, i ? "," : "",
        json_object_get_int_member (json_array_get_object_element (elements,
                i), "element"));
  g_string_append (latency_log, "]\n");
  g_free (all);
  json_node_unref (root);
}

static void
print_latency_log (const gchar * check)
{
  g_printerr ("%s failed; latency messages read (here: ms):\n%s", check,
      latency_log ? latency_log->str : "(none)\n");
}

/* For a check that fails: the log first, then the usual assertion. */
#define LATENCY_CHECK(expr) G_STMT_START { \
  gboolean ok_ = (expr); \
  if (!ok_) \
    print_latency_log (#expr); \
  g_assert_true (ok_); \
} G_STMT_END

static JsonNode *
next_latency (WsClient * client, gint64 element, JsonNode ** all)
{
  gchar *message = ws_next (client, "latency");
  JsonNode *root = parse (message);
  JsonObject *latency = json_node_get_object (root);

  note_latency (client, message);
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

/* Like measured_avg, for the path that starts at pad from. */
static gint64
path_avg (WsClient * client, gint64 from)
{
  gint64 avg = -1;
  gint tries;

  for (tries = 0; tries < 40 && avg < 0; tries++) {
    gchar *message = ws_next (client, "latency");
    JsonNode *root = parse (message);
    JsonObject *latency = json_node_get_object (root);
    JsonArray *paths = json_object_get_array_member (latency, "paths");
    guint i;

    for (i = 0; paths != NULL && i < json_array_get_length (paths); i++) {
      JsonObject *p = json_array_get_object_element (paths, i);
      JsonObject *m = json_object_get_object_member (p, "measured");

      if (json_object_get_int_member (p, "from") == from && tries >= 6 &&
          !json_object_get_null_member (m, "avg"))
        avg = json_object_get_int_member (m, "avg");
    }
    json_node_unref (root);
    g_free (message);
  }
  return avg;
}

static void
test_latency_element (void)
{
  SoupSession *session = soup_session_new ();
  gchar *url = server_url ("ws");
  /* identity sleeps 5 ms per 20 ms buffer: a quarter of real time, so it
   * keeps up even on a slow machine. */
  GstElement *pipeline = start_pipeline ("audiotestsrc is-live=true "
      "samplesperbuffer=960 ! audio/x-raw,rate=48000 ! "
      "identity name=lat-sleep sleep-time=5000 ! fakesink async=false");
  gint64 id = element_id ("lat-sleep");
  WsClient client;
  gint64 avg;

  ws_connect (&client, session, url, NULL);
  ws_send (&client, "{\"type\":\"measure-element\",\"element\":%"
      G_GINT64_FORMAT "}", id);
  avg = measured_avg (&client, id);
  g_assert_cmpint (avg, >=, 4 * GST_MSECOND);
  g_assert_cmpint (avg, <, 100 * GST_MSECOND);

  /* The same from its sink pad to its src pad, as pinned pads do. */
  ws_send (&client, "{\"type\":\"measure-path\",\"from\":%"
      G_GINT64_FORMAT ",\"to\":%" G_GINT64_FORMAT "}",
      pad_id ("lat-sleep", "sink"), pad_id ("lat-sleep", "src"));
  avg = path_avg (&client, pad_id ("lat-sleep", "sink"));
  g_assert_cmpint (avg, >=, 4 * GST_MSECOND);
  g_assert_cmpint (avg, <, 100 * GST_MSECOND);
  ws_send (&client, "{\"type\":\"unmeasure-path\",\"from\":%"
      G_GINT64_FORMAT ",\"to\":%" G_GINT64_FORMAT "}",
      pad_id ("lat-sleep", "sink"), pad_id ("lat-sleep", "src"));
  stop_pipeline (pipeline);

  /* The queue holds what the late sink has not played yet (~200 ms); its
   * own pipeline, so nothing slow upstream keeps it from filling. */
  pipeline = start_pipeline ("audiotestsrc is-live=true ! "
      "queue name=lat-queue max-size-time=500000000 ! fakesink sync=true "
      "ts-offset=200000000 async=false");
  id = element_id ("lat-queue");
  ws_send (&client, "{\"type\":\"measure-element\",\"element\":%"
      G_GINT64_FORMAT "}", id);
  avg = measured_avg (&client, id);
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
        !json_object_get_null_member (a, "stopped_rate");
    if (stopped)
      g_assert_cmpint (json_object_get_int_member (a, "stopped_rate"), >, 50);
    json_node_unref (all);
  }
  g_assert_true (stopped);

  ws_disconnect (&client);
  stop_pipeline (pipeline);
  g_free (url);
  g_object_unref (session);
}

/* Independent pages */

/* Runs the main loop for ms, then drops the messages client got and
 * returns how many were of this type. */
static guint
count_messages (WsClient * client, const gchar * type, gint ms)
{
  gchar *prefix = g_strdup_printf ("{\"type\":\"%s\",", type);
  gint64 end = g_get_monotonic_time () + ms * G_TIME_SPAN_MILLISECOND;
  guint n = 0;

  while (g_get_monotonic_time () < end)
    if (!g_main_context_iteration (NULL, FALSE))
      g_usleep (1000);
  while (!g_queue_is_empty (&client->messages)) {
    gchar *message = g_queue_pop_head (&client->messages);

    if (g_str_has_prefix (message, prefix))
      n++;
    if (g_str_has_prefix (message, "{\"type\":\"latency\","))
      note_latency (client, message);
    g_free (message);
  }
  g_free (prefix);
  return n;
}

/* Whether the next "latency" message for client has element; on: its
 * "measure all" state. */
static gboolean
latency_has (WsClient * client, gint64 element, gboolean * on)
{
  JsonNode *all = NULL;
  JsonNode *entry = next_latency (client, element, &all);
  gboolean found = entry != NULL;

  if (on != NULL)
    *on = json_object_get_boolean_member (json_node_get_object (all), "on");
  if (entry != NULL)
    json_node_unref (entry);
  json_node_unref (all);
  return found;
}

/* Each page gets only the latency it measures. */
static void
test_pages_latency (void)
{
  SoupSession *session = soup_session_new ();
  gchar *url = server_url ("ws");
  /* 5 buffers/s through three elements (with the capsfilter): below the
   * "measure all" limit of 50 calls/s used in these tests (see main). */
  GstElement *pipeline = start_pipeline ("audiotestsrc is-live=true "
      "samplesperbuffer=9600 ! audio/x-raw,rate=48000 ! identity name=pg-a ! "
      "identity name=pg-b ! fakesink async=false");
  gint64 a = element_id ("pg-a");
  gint64 b = element_id ("pg-b");
  WsClient one, two;
  gboolean on;
  gint tries;

  latency_log = g_string_new (NULL);
  g_string_append_printf (latency_log, "a=%" G_GINT64_FORMAT " b=%"
      G_GINT64_FORMAT "\n", a, b);
  ws_connect (&one, session, url, NULL);
  ws_connect (&two, session, url, NULL);
  one.name = "one";
  two.name = "two";

  /* One measures a, two nothing: two gets no latency at all. */
  ws_send (&one, "{\"type\":\"measure-element\",\"element\":%"
      G_GINT64_FORMAT "}", a);
  LATENCY_CHECK (latency_has (&one, a, NULL));
  LATENCY_CHECK (count_messages (&two, "latency", 1500) == 0);

  /* Two measures b: each gets only its own. */
  ws_send (&two, "{\"type\":\"measure-element\",\"element\":%"
      G_GINT64_FORMAT "}", b);
  count_messages (&one, "latency", 600);
  count_messages (&two, "latency", 0);
  LATENCY_CHECK (latency_has (&two, b, NULL));
  LATENCY_CHECK (!latency_has (&two, a, NULL));
  LATENCY_CHECK (latency_has (&one, a, NULL));
  LATENCY_CHECK (!latency_has (&one, b, NULL));

  /* Both measure a; one stops: two still gets it, one gets an empty
   * message, then nothing. */
  ws_send (&two, "{\"type\":\"measure-element\",\"element\":%"
      G_GINT64_FORMAT "}", a);
  ws_send (&one, "{\"type\":\"unmeasure-element\",\"element\":%"
      G_GINT64_FORMAT "}", a);
  for (tries = 0; tries < 5 && latency_has (&one, a, NULL); tries++);
  LATENCY_CHECK (tries < 5);
  LATENCY_CHECK (count_messages (&one, "latency", 1500) == 0);
  count_messages (&two, "latency", 0);
  for (tries = 0; tries < 5 && !latency_has (&two, a, NULL); tries++);
  LATENCY_CHECK (tries < 5);

  /* Measure all in one: one sees it on and gets every element; two keeps
   * only its own, with measure all off. */
  ws_send (&one, "{\"type\":\"measure-all\"}");
  count_messages (&one, "latency", 600);
  for (tries = 0; tries < 5 && !latency_has (&one, b, &on); tries++);
  LATENCY_CHECK (tries < 5);
  LATENCY_CHECK (on);
  count_messages (&two, "latency", 0);
  latency_has (&two, b, &on);
  LATENCY_CHECK (!on);

  ws_disconnect (&one);
  ws_disconnect (&two);
  stop_pipeline (pipeline);
  g_string_free (latency_log, TRUE);
  latency_log = NULL;
  g_free (url);
  g_object_unref (session);
}

/* The "measure all" safety stop is reported only to the page that had it
 * on (limit 50 buffers/s in this test, see main). */
static void
test_pages_all_stopped (void)
{
  SoupSession *session = soup_session_new ();
  gchar *url = server_url ("ws");
  GstElement *pipeline = start_pipeline ("videotestsrc is-live=true ! "
      "video/x-raw,framerate=60/1 ! queue ! identity name=pg-stop ! queue ! "
      "fakesink async=false");
  gint64 id = element_id ("pg-stop");
  WsClient one, two;
  gboolean stopped = FALSE;
  gint tries;

  ws_connect (&one, session, url, NULL);
  ws_connect (&two, session, url, NULL);
  ws_send (&two, "{\"type\":\"measure-element\",\"element\":%"
      G_GINT64_FORMAT "}", id);
  ws_send (&one, "{\"type\":\"measure-all\"}");
  for (tries = 0; tries < 20 && !stopped; tries++) {
    JsonNode *all = NULL;
    JsonNode *entry = next_latency (&one, 0, &all);

    stopped = !json_object_get_null_member (json_node_get_object (all),
        "stopped_rate");
    if (entry != NULL)
      json_node_unref (entry);
    json_node_unref (all);
  }
  g_assert_true (stopped);

  count_messages (&two, "latency", 0);
  for (tries = 0; tries < 3; tries++) {
    JsonNode *all = NULL;
    JsonNode *entry = next_latency (&two, id, &all);

    g_assert_nonnull (entry);
    g_assert_true (json_object_get_null_member (json_node_get_object (all),
            "stopped_rate"));
    json_node_unref (entry);
    json_node_unref (all);
  }

  ws_disconnect (&one);
  ws_disconnect (&two);
  stop_pipeline (pipeline);
  g_free (url);
  g_object_unref (session);
}

/* The names of the properties in the next "properties" message for
 * element, joined with commas. */
static gchar *
next_property_names (WsClient * client, gint64 element)
{
  GString *names = g_string_new (NULL);

  while (names->len == 0) {
    gchar *message = ws_next (client, "properties");
    JsonNode *root = parse (message);
    JsonArray *elements = json_object_get_array_member (json_node_get_object
        (root), "elements");
    guint i, j;

    for (i = 0; i < json_array_get_length (elements); i++) {
      JsonObject *e = json_array_get_object_element (elements, i);
      JsonArray *props;

      if (json_object_get_int_member (e, "element") != element ||
          !json_object_has_member (e, "properties"))
        continue;
      props = json_object_get_array_member (e, "properties");
      for (j = 0; j < json_array_get_length (props); j++)
        g_string_append_printf (names, "%s%s", names->len ? "," : "",
            json_object_get_string_member (json_array_get_object_element
                (props, j), "name"));
    }
    json_node_unref (root);
    g_free (message);
  }
  return g_string_free (names, FALSE);
}

/* Each page gets only the properties it watches. */
static void
test_pages_properties (void)
{
  SoupSession *session = soup_session_new ();
  gchar *url = server_url ("ws");
  GstElement *pipeline = start_pipeline ("videotestsrc is-live=true ! "
      "fakesink name=pg-sink async=false");
  gint64 id = element_id ("pg-sink");
  WsClient one, two;
  gchar *names;

  ws_connect (&one, session, url, NULL);
  ws_connect (&two, session, url, NULL);
  ws_send (&one, "{\"type\":\"watch-property\",\"element\":%"
      G_GINT64_FORMAT ",\"property\":\"stats\"}", id);
  ws_send (&two, "{\"type\":\"watch-property\",\"element\":%"
      G_GINT64_FORMAT ",\"property\":\"sync\"}", id);
  count_messages (&one, "properties", 600);
  count_messages (&two, "properties", 0);

  names = next_property_names (&one, id);
  g_assert_cmpstr (names, ==, "stats");
  g_free (names);
  names = next_property_names (&two, id);
  g_assert_cmpstr (names, ==, "sync");
  g_free (names);

  ws_disconnect (&one);
  ws_disconnect (&two);
  stop_pipeline (pipeline);
  g_free (url);
  g_object_unref (session);
}

/* A pad pinned in two pages keeps its stats for the other when one
 * unpins it. */
static void
test_pages_pads (void)
{
  SoupSession *session = soup_session_new ();
  gchar *url = server_url ("ws");
  GstElement *pipeline = start_pipeline ("videotestsrc is-live=true ! "
      "fakesink name=pg-pad async=false");
  gint64 pad = pad_id ("pg-pad", "sink");
  WsClient one, two;

  ws_connect (&one, session, url, NULL);
  ws_connect (&two, session, url, NULL);
  ws_send (&one, "{\"type\":\"watch-pad\",\"pad\":%" G_GINT64_FORMAT "}", pad);
  ws_send (&two, "{\"type\":\"watch-pad\",\"pad\":%" G_GINT64_FORMAT "}", pad);
  g_assert_cmpint (next_stats (&one, pad), >=, 0);
  g_assert_cmpint (next_stats (&two, pad), >=, 0);

  ws_send (&one, "{\"type\":\"unwatch-pad\",\"pad\":%" G_GINT64_FORMAT "}",
      pad);
  count_messages (&one, "pads", 600);
  g_assert_cmpuint (count_messages (&one, "pads", 1500), ==, 0);
  count_messages (&two, "pads", 0);
  g_assert_cmpint (next_stats (&two, pad), >=, 0);

  ws_disconnect (&one);
  ws_disconnect (&two);
  stop_pipeline (pipeline);
  g_free (url);
  g_object_unref (session);
}

/* Findings */

/* Polls /api/findings for up to timeout_s until one has this id; returns
 * a copy of it, or NULL. */
static JsonNode *
wait_finding (const gchar * id, gint timeout_s)
{
  gint64 end = g_get_monotonic_time () + timeout_s * G_USEC_PER_SEC;

  while (g_get_monotonic_time () < end) {
    gchar *text;
    JsonNode *root, *found = NULL;
    JsonArray *list;
    guint i;

    get_status ("api/findings", "GET", NULL, &text, NULL);
    root = parse (text);
    g_free (text);
    list = json_object_get_array_member (json_node_get_object (root),
        "findings");
    for (i = 0; i < json_array_get_length (list); i++)
      if (g_strcmp0 (json_object_get_string_member
              (json_array_get_object_element (list, i), "id"), id) == 0)
        found = json_node_copy (json_array_get_element (list, i));
    json_node_unref (root);
    if (found != NULL)
      return found;
    g_usleep (100 * G_TIME_SPAN_MILLISECOND);
  }
  return NULL;
}

/* Whether the finding with this id is gone within timeout_s. */
static gboolean
finding_gone (const gchar * id, gint timeout_s)
{
  gint64 end = g_get_monotonic_time () + timeout_s * G_USEC_PER_SEC;

  while (g_get_monotonic_time () < end) {
    gchar *text;
    JsonNode *root;
    JsonArray *list;
    gboolean present = FALSE;
    guint i;

    get_status ("api/findings", "GET", NULL, &text, NULL);
    root = parse (text);
    g_free (text);
    list = json_object_get_array_member (json_node_get_object (root),
        "findings");
    for (i = 0; i < json_array_get_length (list); i++)
      present |= g_strcmp0 (json_object_get_string_member
          (json_array_get_object_element (list, i), "id"), id) == 0;
    json_node_unref (root);
    if (!present)
      return TRUE;
    g_usleep (100 * G_TIME_SPAN_MILLISECOND);
  }
  return FALSE;
}

/* A tee without outputs: the queue's pushes fail with not-linked. */
static void
test_findings_flow (void)
{
  GstElement *pipeline = start_pipeline ("videotestsrc ! "
      "queue name=flow-queue ! tee name=flow-tee");
  gchar *pad_path = graph_path ("flow-queue", "src");
  gchar *path = graph_path ("flow-queue", NULL);
  gchar *id = g_strdup_printf ("flow:%s", pad_path);
  JsonNode *node = wait_finding (id, 5);
  JsonObject *f;

  g_assert_nonnull (node);
  f = json_node_get_object (node);
  /* Its place by id and by path. */
  g_assert_cmpstr (json_object_get_string_member (f, "path"), ==, path);
  g_assert_cmpstr (json_object_get_string_member (f, "pad_path"), ==,
      pad_path);
  g_assert_cmpint (json_object_get_int_member (f, "pad"), ==,
      pad_id ("flow-queue", "src"));
  g_free (path);
  g_free (pad_path);
  g_assert_cmpstr (json_object_get_string_member (f, "severity"), ==,
      "warning");
  g_assert_nonnull (strstr (json_object_get_string_member (f, "text"),
          "not-linked"));
  g_assert_cmpint (json_object_get_int_member (f, "count"), >=, 1);
  json_node_unref (node);
  stop_pipeline (pipeline);
  g_free (id);
}

/* A sink that never gets data keeps the pipeline from PAUSED. */
static void
test_findings_stuck (void)
{
  GstElement *pipeline = gst_parse_launch ("appsrc name=stuck-src ! "
      "fakesink name=stuck-sink", NULL);
  gchar *id;
  JsonNode *node;

  gst_element_set_state (pipeline, GST_STATE_PLAYING);
  id = element_finding ("stuck", GST_OBJECT_NAME (pipeline));
  node = wait_finding (id, 8);
  g_assert_nonnull (node);
  g_assert_nonnull (strstr (json_object_get_string_member
          (json_node_get_object (node), "text"), "has not reached"));
  json_node_unref (node);
  g_free (id);

  /* And the sink that got no data, with no source reaching it. */
  id = pad_finding ("starved", "stuck-sink", "sink");
  node = wait_finding (id, 3);
  g_assert_nonnull (node);
  g_assert_nonnull (strstr (json_object_get_string_member
          (json_node_get_object (node), "hint"), "No pad before it"));
  json_node_unref (node);

  /* Data comes: the pipeline reaches PLAYING and both findings go. */
  {
    GstElement *src = gst_bin_get_by_name (GST_BIN (pipeline), "stuck-src");
    GstBuffer *buffer = gst_buffer_new_allocate (NULL, 16, NULL);
    GstFlowReturn ret;
    gchar *stuck = element_finding ("stuck", GST_OBJECT_NAME (pipeline));

    g_signal_emit_by_name (src, "push-buffer", buffer, &ret);
    g_assert_cmpint (ret, ==, GST_FLOW_OK);
    g_assert_cmpint (gst_element_get_state (pipeline, NULL, NULL,
            5 * GST_SECOND), ==, GST_STATE_CHANGE_SUCCESS);
    g_assert_true (finding_gone (stuck, 5));
    g_assert_true (finding_gone (id, 5));
    gst_buffer_unref (buffer);
    gst_object_unref (src);
    g_free (stuck);
  }
  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (pipeline);
  g_free (id);
}

/* Starts a pipeline without waiting for its state: the checks look at
 * pipelines that fail or hang. */
static GstElement *
run_pipeline (const gchar * description)
{
  GError *error = NULL;
  GstElement *pipeline = gst_parse_launch (description, &error);

  g_assert_no_error (error);
  gst_element_set_state (pipeline, GST_STATE_PLAYING);
  return pipeline;
}

static gchar *
pad_finding (const gchar * kind, const gchar * element, const gchar * pad)
{
  gchar *path = graph_path (element, pad);
  gchar *id = g_strdup_printf ("%s:%s", kind, path);

  g_free (path);
  return id;
}

static gchar *
element_finding (const gchar * kind, const gchar * element)
{
  gchar *path = graph_path (element, NULL);
  gchar *id = g_strdup_printf ("%s:%s", kind, path);

  g_free (path);
  return id;
}

/* Expects the finding within timeout_s; returns it for more checks. */
static JsonObject *
expect_finding (const gchar * id, gint timeout_s, JsonNode ** node)
{
  *node = wait_finding (id, timeout_s);
  if (*node == NULL)
    g_error ("no finding %s", id);
  return json_node_get_object (*node);
}

/* The source cannot agree on a format: its error is turned into a finding
 * at its src pad, with what each side can handle. */
static void
test_check_not_negotiated (void)
{
  /* Linked first, the impossible caps set afterwards: negotiation fails
   * when the source starts. */
  GstElement *pipeline = gst_parse_launch ("videotestsrc name=nn-src ! "
      "capsfilter name=nn-filter ! fakesink", NULL);
  GstElement *filter = gst_bin_get_by_name (GST_BIN (pipeline), "nn-filter");
  GstCaps *audio = gst_caps_from_string ("audio/x-raw");
  gchar *id;
  JsonNode *node;
  JsonObject *f, *caps;

  g_object_set (filter, "caps", audio, NULL);
  gst_caps_unref (audio);
  gst_object_unref (filter);
  gst_element_set_state (pipeline, GST_STATE_PLAYING);
  id = pad_finding ("flow", "nn-src", "src");
  f = expect_finding (id, 5, &node);

  g_assert_cmpstr (json_object_get_string_member (f, "severity"), ==,
      "error");
  caps = json_object_get_object_member (f, "caps");
  g_assert_nonnull (caps);
  g_assert_false (json_object_get_boolean_member (caps, "common"));
  g_assert_true (g_str_has_prefix (json_object_get_string_member (caps,
              "upstream"), "video/x-raw"));
  json_node_unref (node);
  stop_pipeline (pipeline);
  g_free (id);
}

static void
test_check_failed_link (void)
{
  GstElement *pipeline = gst_pipeline_new ("fl-pipeline");
  GstElement *src = gst_element_factory_make ("videotestsrc", "fl-src");
  GstElement *conv = gst_element_factory_make ("audioconvert", "fl-conv");
  JsonNode *node;
  JsonObject *f, *caps;
  GstPad *src_pad, *sink_pad;


  /* As a pad-added handler would do it; gst_element_link checks the
   * formats itself and never tries. */
  gst_bin_add_many (GST_BIN (pipeline), src, conv, NULL);
  src_pad = gst_element_get_static_pad (src, "src");
  sink_pad = gst_element_get_static_pad (conv, "sink");
  g_assert_cmpint (gst_pad_link (src_pad, sink_pad), ==,
      GST_PAD_LINK_NOFORMAT);
  gst_object_unref (src_pad);
  gst_object_unref (sink_pad);
  /* The previous test ends with an error: findings can be held 5 s. */
  f = expect_finding ("link:fl-pipeline/fl-src:src>fl-pipeline/fl-conv:sink",
      8, &node);
  caps = json_object_get_object_member (f, "caps");
  g_assert_true (g_str_has_prefix (json_object_get_string_member (caps,
              "upstream"), "video/x-raw"));
  g_assert_true (g_str_has_prefix (json_object_get_string_member (caps,
              "downstream"), "audio/x-raw"));
  json_node_unref (node);
  gst_object_unref (pipeline);
}



/* A link refused for another reason than formats: both sides are shown,
 * and here they do share a format. */
static void
test_check_link_was_linked (void)
{
  GstElement *pipeline = gst_parse_launch ("videotestsrc name=wl-src ! "
      "fakesink name=wl-a", NULL);
  GstElement *src = gst_bin_get_by_name (GST_BIN (pipeline), "wl-src");
  GstElement *other = gst_element_factory_make ("fakesink", "wl-b");
  GstPad *src_pad, *sink_pad;
  JsonNode *node;
  JsonObject *f;
  gchar *src_path, *sink_path, *id;

  gst_bin_add (GST_BIN (pipeline), other);
  src_pad = gst_element_get_static_pad (src, "src");
  sink_pad = gst_element_get_static_pad (other, "sink");
  g_assert_cmpint (gst_pad_link (src_pad, sink_pad), ==,
      GST_PAD_LINK_WAS_LINKED);
  src_path = graph_path ("wl-src", "src");
  sink_path = graph_path ("wl-b", "sink");
  id = g_strdup_printf ("link:%s>%s", src_path, sink_path);
  f = expect_finding (id, 8, &node);
  g_free (id);
  g_free (src_path);
  g_free (sink_path);
  g_assert_nonnull (strstr (json_object_get_string_member (f, "text"),
          gst_pad_link_get_name (GST_PAD_LINK_WAS_LINKED)));
  g_assert_true (json_object_get_boolean_member (json_object_get_object_member
          (f, "caps"), "common"));
  json_node_unref (node);
  gst_object_unref (src_pad);
  gst_object_unref (sink_pad);
  gst_object_unref (src);
  gst_object_unref (pipeline);
}

/* Whether element:pad in the graph has a flow result. */
static gboolean
has_flow (JsonObject * element, const gchar * name, const gchar * pad)
{
  JsonArray *list = json_object_get_array_member (element, "pads");
  guint i;

  if (g_strcmp0 (json_object_get_string_member (element, "name"), name) == 0)
    for (i = 0; i < json_array_get_length (list); i++) {
      JsonObject *p = json_array_get_object_element (list, i);
      if (g_strcmp0 (json_object_get_string_member (p, "name"), pad) == 0)
        return json_object_has_member (p, "flow");
    }
  list = json_object_get_array_member (element, "children");
  for (i = 0; i < json_array_get_length (list); i++)
    if (has_flow (json_array_get_object_element (list, i), name, pad))
      return TRUE;
  return FALSE;
}

/* Runs a pipeline whose element "file" gets location, until it ends. */
static GstElement *
run_to_end (const gchar * description, const gchar * location)
{
  GstElement *pipeline = gst_parse_launch (description, NULL);
  GstElement *file = gst_bin_get_by_name (GST_BIN (pipeline), "file");
  GstBus *bus = gst_element_get_bus (pipeline);
  GstMessage *message;

  g_object_set (file, "location", location, NULL);
  gst_object_unref (file);
  gst_element_set_state (pipeline, GST_STATE_PLAYING);
  message = gst_bus_timed_pop_filtered (bus, 10 * GST_SECOND,
      GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
  g_assert_nonnull (message);
  g_assert_cmpint (GST_MESSAGE_TYPE (message), ==, GST_MESSAGE_EOS);
  gst_message_unref (message);
  gst_object_unref (bus);
  return pipeline;
}

/* A demuxer pulling a file reaches its end: data did not stop there. */
static void
test_check_pull_end (void)
{
  gchar *dir = g_dir_make_tmp ("pm-pull-XXXXXX", NULL);
  gchar *path = g_build_filename (dir, "clip.mp4", NULL);
  GstElement *pipeline;
  gchar *body;
  JsonNode *root;
  JsonArray *pipelines;
  guint i;
  gboolean flow = FALSE;

  stop_pipeline (run_to_end ("videotestsrc num-buffers=5 ! jpegenc ! qtmux ! "
          "filesink name=file", path));
  pipeline = run_to_end ("filesrc name=file ! qtdemux name=pe-demux ! "
      "fakesink", path);

  g_assert_cmpuint (get_status ("api/graph", "GET", NULL, &body, NULL), ==,
      SOUP_STATUS_OK);
  root = parse (body);
  pipelines = json_object_get_array_member (json_node_get_object (root),
      "pipelines");
  g_assert_cmpuint (json_array_get_length (pipelines), >, 0);
  for (i = 0; i < json_array_get_length (pipelines); i++)
    flow |= has_flow (json_array_get_object_element (pipelines, i),
        "pe-demux", "sink");
  g_assert_false (flow);

  json_node_unref (root);
  g_free (body);
  stop_pipeline (pipeline);
  g_unlink (path);
  g_rmdir (dir);
  g_free (path);
  g_free (dir);
}

static void
test_check_missing_plugin (void)
{
  /* Pushed, so decodebin takes the caps as they are and looks for a
   * decoder (pulled, its typefinder would fail on the bytes first). */
  GstElement *pipeline = run_pipeline ("fakesrc can-activate-pull=false "
      "num-buffers=1 sizetype=fixed sizemax=16 ! "
      "capsfilter caps=application/x-pm-unknown ! decodebin name=mp-dec ! "
      "fakesink");
  gchar *id = element_finding ("missing", "mp-dec");
  JsonNode *node;
  JsonObject *f = expect_finding (id, 5, &node);

  g_assert_nonnull (strstr (json_object_get_string_member (f, "text"),
          "missing"));
  json_node_unref (node);
  stop_pipeline (pipeline);
  g_free (id);
}

/* Tee branches without queues, not live: the first sink prerolls and
 * holds the tee's thread, the second never gets data. The push into the
 * first sink is where it is blocked. */
static void
test_check_blocked (void)
{
  GstElement *pipeline = run_pipeline ("videotestsrc ! tee name=bt ! "
      "fakesink name=bt-a bt. ! fakesink name=bt-b");
  gchar *id = pad_finding ("blocked", "bt", "src_0");
  JsonNode *node;
  JsonObject *f = expect_finding (id, 8, &node);

  g_assert_nonnull (strstr (json_object_get_string_member (f, "text"),
          "bt:src_0 → "));
  g_assert_nonnull (strstr (json_object_get_string_member (f, "hint"),
          "has not returned"));
  g_assert_nonnull (strstr (json_object_get_string_member (f, "hint"),
          "is in PAUSED"));
  json_node_unref (node);
  stop_pipeline (pipeline);
  g_free (id);
}

/* An element's preroll in the graph, waiting up to 5 s for it to be
 * expected (NULL: none). */
static void
expect_preroll (const gchar * name, const gchar * expected)
{
  gchar *needle = g_strdup_printf ("\"name\":\"%s\"", name);
  gchar *want = g_strdup_printf ("\"preroll\":\"%s\"", expected);
  gint64 end = g_get_monotonic_time () + 5 * G_USEC_PER_SEC;
  gchar *found = NULL;

  while (g_get_monotonic_time () < end) {
    gchar *graph, *at, *pads;

    get_status ("api/graph", "GET", NULL, &graph, NULL);
    at = strstr (graph, needle);
    pads = at ? strstr (at, "\"pads\"") : NULL;
    g_free (found);
    found = NULL;
    if (pads != NULL) {
      *pads = '\0';
      at = strstr (at, "\"preroll\":\"");
      found = at ? g_strndup (at, strchr (at + 11, '"') - at + 1) : NULL;
    }
    g_free (graph);
    if (g_strcmp0 (found, want) == 0)
      break;
    g_usleep (100 * G_TIME_SPAN_MILLISECOND);
  }
  g_assert_cmpstr (found, ==, want);
  g_free (found);
  g_free (needle);
  g_free (want);
}

static GstElement *
pause_pipeline (const gchar * description)
{
  GstElement *pipeline = gst_parse_launch (description, NULL);

  gst_element_set_state (pipeline, GST_STATE_PAUSED);
  return pipeline;
}

/* Sinks and bins with sinks, from their own async messages. */
static void
test_check_preroll (void)
{
  GstElement *tee = run_pipeline ("videotestsrc ! tee name=pr-tee ! "
      "fakesink name=pr-a pr-tee. ! fakesink name=pr-b");
  GstElement *no_wait = run_pipeline ("videotestsrc ! "
      "fakesink name=pr-nowait async=false");
  GstElement *bin = run_pipeline ("( name=pr-bin fakesink name=pr-inner ) "
      "videotestsrc ! pr-inner.");
  GstElement *live = pause_pipeline ("videotestsrc is-live=true ! "
      "fakesink name=pr-live");
  GstElement *eos = pause_pipeline ("fakesrc num-buffers=0 ! "
      "fakesink name=pr-eos");
  gchar *name = gst_element_get_name (tee);
  gchar *id = element_finding ("stuck", name);
  JsonNode *node;
  JsonObject *f;

  expect_preroll ("pr-a", "done");
  expect_preroll ("pr-b", "waiting");
  expect_preroll ("pr-nowait", "no-wait");
  expect_preroll ("pr-bin", "done");
  expect_preroll ("pr-inner", "done");
  /* Live: the sink waits in PAUSED, the source sends from PLAYING. */
  expect_preroll ("pr-live", "waiting");
  gst_element_set_state (live, GST_STATE_PLAYING);
  expect_preroll ("pr-live", "done");
  expect_preroll ("pr-eos", "eos");

  f = expect_finding (id, 8, &node);
  g_assert_nonnull (strstr (json_object_get_string_member (f, "hint"),
          "Prerolled: pr-a. Waiting for preroll: pr-b."));
  json_node_unref (node);

  stop_pipeline (tee);
  stop_pipeline (no_wait);
  stop_pipeline (bin);
  stop_pipeline (live);
  stop_pipeline (eos);
  g_free (name);
  g_free (id);
}

/* Healthy pipelines that look like problems to a guess must give no
 * finding at all. */
static void
test_check_quiet (void)
{
  const gchar *pipelines[] = {
    /* A tee with a queue on each branch. */
    "videotestsrc name=q1-src ! tee name=q1-tee ! queue ! fakesink "
        "q1-tee. ! queue ! fakesink",
    /* No queues, but sinks that neither preroll nor sync: nothing waits.
     * (sync=false alone still prerolls and does hang.) */
    "videotestsrc is-live=true name=q2-src ! tee name=q2-tee ! "
        "fakesink sync=false async=false q2-tee. ! "
        "fakesink sync=false async=false",
    /* An aggregator with its own thread. */
    "videotestsrc name=q3-a ! compositor name=q3-mix ! fakesink "
        "videotestsrc name=q3-b ! q3-mix.",
    /* A leaky queue that is not full. */
    "videotestsrc is-live=true name=q4-src ! "
        "queue name=q4-leaky leaky=downstream ! fakesink",
    /* Not live: full queues are the usual pace, not a stall. */
    "videotestsrc name=q5-src ! tee name=q5-tee ! queue name=q5-a ! "
        "fakesink sync=true q5-tee. ! queue name=q5-b ! fakesink sync=true",
  };
  GstElement *running[G_N_ELEMENTS (pipelines)];
  gchar *text;
  guint i;

  for (i = 0; i < G_N_ELEMENTS (pipelines); i++)
    running[i] = run_pipeline (pipelines[i]);
  /* Longer than the 3 s of stuck and blocked, the 5 s of full. */
  g_usleep (6 * G_USEC_PER_SEC);
  get_status ("api/findings", "GET", NULL, &text, NULL);
  for (i = 0; i < G_N_ELEMENTS (pipelines); i++) {
    gchar *mark = g_strdup_printf ("/q%u-", i + 1);

    if (strstr (text, mark) != NULL)
      g_error ("finding for a healthy pipeline: %s", text);
    g_free (mark);
    stop_pipeline (running[i]);
  }
  g_free (text);
}

/* A leaky queue whose output is blocked keeps dropping. */
static void
test_check_leaky (void)
{
  GstElement *pipeline = gst_parse_launch ("videotestsrc is-live=true ! "
      "queue name=lk-queue leaky=downstream max-size-buffers=3 ! "
      "fakesink async=false", NULL);
  GstElement *queue = gst_bin_get_by_name (GST_BIN (pipeline), "lk-queue");
  GstPad *src = gst_element_get_static_pad (queue, "src");
  gchar *id;
  JsonNode *node;

  gst_pad_add_probe (src, GST_PAD_PROBE_TYPE_BLOCK |
      GST_PAD_PROBE_TYPE_BUFFER, hold, NULL, NULL);
  gst_element_set_state (pipeline, GST_STATE_PLAYING);
  id = element_finding ("dropping", "lk-queue");
  expect_finding (id, 6, &node);
  json_node_unref (node);
  g_free (id);
  /* Full, but leaky: dropping is the finding, not "full". */
  g_usleep (5 * G_USEC_PER_SEC);
  id = element_finding ("full", "lk-queue");
  g_assert_null (wait_finding (id, 1));
  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (src);
  gst_object_unref (queue);
  gst_object_unref (pipeline);
  g_free (id);
}

/* A queue whose output is blocked stays full. */
static void
test_check_full_queue (void)
{
  GstElement *pipeline = gst_parse_launch ("videotestsrc is-live=true ! "
      "queue name=fq-queue max-size-buffers=3 ! fakesink async=false",
      NULL);
  GstElement *queue = gst_bin_get_by_name (GST_BIN (pipeline), "fq-queue");
  GstPad *src = gst_element_get_static_pad (queue, "src");
  gchar *id;
  JsonNode *node;

  gst_pad_add_probe (src, GST_PAD_PROBE_TYPE_BLOCK |
      GST_PAD_PROBE_TYPE_BUFFER, hold, NULL, NULL);
  gst_element_set_state (pipeline, GST_STATE_PLAYING);
  id = element_finding ("full", "fq-queue");
  expect_finding (id, 9, &node);
  json_node_unref (node);
  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (src);
  gst_object_unref (queue);
  gst_object_unref (pipeline);
  g_free (id);
}

/* identity makes buffers late; the sink drops those too late. */
static void
test_check_late_sink (void)
{
  GstElement *pipeline = run_pipeline ("videotestsrc is-live=true ! "
      "identity sleep-time=60000 ! fakesink name=late-sink sync=true "
      "max-lateness=1000000");
  gchar *id = element_finding ("late", "late-sink");
  JsonNode *node;

  expect_finding (id, 10, &node);
  json_node_unref (node);
  stop_pipeline (pipeline);
  g_free (id);
}

static void
test_check_silent_source (void)
{
  GstElement *pipeline = run_pipeline ("appsrc name=sl-src is-live=true ! "
      "fakesink async=false");
  gchar *id = element_finding ("silent", "sl-src");
  JsonNode *node;

  expect_finding (id, 9, &node);
  json_node_unref (node);
  stop_pipeline (pipeline);
  g_free (id);
}

/* A source with no caps that pushes buffer lists is not silent. */
static void
test_list_source_not_silent (void)
{
  GstElement *pipeline = run_pipeline ("appsrc name=ls-src is-live=true ! "
      "fakesink async=false");
  GstElement *src = gst_bin_get_by_name (GST_BIN (pipeline), "ls-src");
  GstBufferList *list = gst_buffer_list_new ();
  gchar *id = element_finding ("silent", "ls-src");
  GstFlowReturn ret;
  JsonNode *node;

  gst_buffer_list_add (list, gst_buffer_new_allocate (NULL, 16, NULL));
  gst_buffer_list_add (list, gst_buffer_new_allocate (NULL, 16, NULL));
  g_signal_emit_by_name (src, "push-buffer-list", list, &ret);
  g_assert_cmpint (ret, ==, GST_FLOW_OK);
  gst_buffer_list_unref (list);
  node = wait_finding (id, 7);
  if (node != NULL)
    g_error ("a source pushing buffer lists reported as silent");
  gst_object_unref (src);
  stop_pipeline (pipeline);
  g_free (id);
}

/* filesrc never gets caps on its pad (its template is ANY), pulled from or
 * pushing: still not silent while it is being read. */
static void
test_read_source_not_silent (void)
{
  /* 8 s of 8 kHz mono 16-bit audio. */
  const guint32 rate = 8000, bytes = 8 * rate * 2;
  guint8 header[44] = "RIFF....WAVEfmt ";
  guint8 *data = g_malloc0 (sizeof header + bytes);
  gchar *dir = g_dir_make_tmp ("pm-pull-XXXXXX", NULL);
  gchar *file = g_build_filename (dir, "tone.wav", NULL);
  const gchar *modes[] = { "", "queue ! " };
  guint i;

  if (!gst_registry_check_feature_version (gst_registry_get (), "wavparse",
          1, 0, 0)) {
    g_test_skip ("no wavparse");
    goto out;
  }
  GST_WRITE_UINT32_LE (header + 4, 36 + bytes);
  GST_WRITE_UINT32_LE (header + 16, 16);
  GST_WRITE_UINT16_LE (header + 20, 1);
  GST_WRITE_UINT16_LE (header + 22, 1);
  GST_WRITE_UINT32_LE (header + 24, rate);
  GST_WRITE_UINT32_LE (header + 28, rate * 2);
  GST_WRITE_UINT16_LE (header + 32, 2);
  GST_WRITE_UINT16_LE (header + 34, 16);
  memcpy (header + 36, "data", 4);
  GST_WRITE_UINT32_LE (header + 40, bytes);
  memcpy (data, header, sizeof header);
  g_assert_true (g_file_set_contents (file, (gchar *) data,
          sizeof header + bytes, NULL));

  /* In GStreamer descriptions a backslash escapes: Windows paths use "/". */
  g_strdelimit (file, "\\", '/');
  /* wavparse pulls from filesrc; after a queue, filesrc pushes. */
  for (i = 0; i < G_N_ELEMENTS (modes); i++) {
    gchar *description = g_strdup_printf ("filesrc name=pl-src "
        "location=\"%s\" ! %swavparse ! fakesink sync=true", file, modes[i]);
    GstElement *pipeline = run_pipeline (description);
    gchar *id = element_finding ("silent", "pl-src");
    JsonNode *node = wait_finding (id, 7);

    if (node != NULL)
      g_error ("%s: a source being read reported as silent", description);
    stop_pipeline (pipeline);
    g_free (id);
    g_free (description);
  }
out:
  g_remove (file);
  g_rmdir (dir);
  g_free (file);
  g_free (dir);
  g_free (data);
}

/* Polls /api/findings until it contains every one of texts, up to
 * timeout_s. */
static gboolean
findings_contain (const gchar * const *texts, gint timeout_s)
{
  gint64 end = g_get_monotonic_time () + timeout_s * G_USEC_PER_SEC;

  while (g_get_monotonic_time () < end) {
    gchar *text;
    gboolean all = TRUE;
    guint i;

    get_status ("api/findings", "GET", NULL, &text, NULL);
    for (i = 0; texts[i] != NULL; i++)
      all = all && strstr (text, texts[i]) != NULL;
    g_free (text);
    if (all)
      return TRUE;
    g_usleep (100 * G_TIME_SPAN_MILLISECOND);
  }
  return FALSE;
}

/* As gst-launch does it: the capsfilter made for decodebin's output is
 * removed when the link fails, decodebin's pad when the pipeline stops.
 * The failed link, with both sides' caps, stays reported through both. */
static void
test_check_link_after_error (void)
{
  const gchar *texts[] = { "le-dec:src_0 could not link to",
    "no common format", "\"common\":false", "rate=(int)8000", NULL
  };
  GstElement *next;
  gchar *text = NULL;
  gint i;
  /* Audio: on macOS a JPEG decoder sets up GL, which warns without an
   * NSApplication. */
  GstElement *pipeline = run_pipeline ("audiotestsrc is-live=true ! "
      "wavenc ! decodebin name=le-dec ! audio/x-raw,rate=8000 ! fakesink");

  g_assert_true (findings_contain (texts, 8));
  gst_element_set_state (pipeline, GST_STATE_NULL);
  g_usleep (G_USEC_PER_SEC);
  g_assert_true (findings_contain (texts, 1));
  gst_object_unref (pipeline);

  /* A pipeline starting makes them current again, before the 5 s. */
  next = run_pipeline ("fakesrc ! fakesink");
  for (i = 0; i < 30; i++) {
    g_free (text);
    get_status ("api/findings", "GET", NULL, &text, NULL);
    if (strstr (text, "le-dec") == NULL)
      break;
    g_usleep (100 * G_TIME_SPAN_MILLISECOND);
  }
  g_assert_null (strstr (text, "le-dec"));
  g_free (text);
  stop_pipeline (next);
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
  gint64 result = 0;
  guint i;

  ws_connect (&client, session, url, NULL);
  ws_send (&client, "{\"type\":\"watch-pad\",\"pad\":%" G_GINT64_FORMAT "}",
      pad);
  g_assert_cmpint (next_stats (&client, pad), >=, 0);

  stop_pipeline (pipeline);
  /* The stopped pipeline's last stats can come first. */
  for (i = 0; i < 3 && (result = next_stats (&client, pad)) >= 0; i++);
  g_assert_cmpint (result, ==, -1);
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
  g_test_add_func ("/recorder/secrets", test_recorder_secrets);
  g_test_add_func ("/recorder/files", test_recorder_files);
  g_test_add_func ("/recorder/limits", test_recorder_limits);
  g_test_add_func ("/recorder/no-folder", test_recorder_no_folder);
  g_test_add_func ("/recorder/broken-files", test_recorder_broken_files);
  g_test_add_func ("/recorder/many-files", test_recorder_many_files);
#ifdef G_OS_UNIX
  g_test_add_func ("/recorder/private", test_recorder_private);
  g_test_add_func ("/recorder/read-only", test_recorder_read_only);
  g_test_add_func ("/recorder/disk-full", test_recorder_disk_full);
#endif
  g_test_add_func ("/server/hello", test_hello);
  g_test_add_func ("/server/recordings", test_recordings);
  g_test_add_func ("/server/protocol", test_protocol);
  g_test_add_func ("/server/monitor-bad-commands", test_monitor_bad_commands);
  g_test_add_func ("/server/invalid-utf8", test_invalid_utf8);
  g_test_add_func ("/server/monitor-pad-gone", test_monitor_pad_gone);
  g_test_add_func ("/server/levels", test_levels);
  g_test_add_func ("/server/levels-multiqueue", test_levels_multiqueue);
  g_test_add_func ("/server/levels-buffering-clock",
      test_levels_buffering_clock);
  g_test_add_func ("/server/levels-websocket", test_levels_websocket);
  g_test_add_func ("/server/properties", test_properties);
  g_test_add_func ("/server/properties-not-answering",
      test_properties_not_answering);
  g_test_add_func ("/server/watch-property", test_watch_property);
  g_test_add_func ("/server/pages-latency", test_pages_latency);
  g_test_add_func ("/server/pages-all-stopped", test_pages_all_stopped);
  g_test_add_func ("/server/pages-properties", test_pages_properties);
  g_test_add_func ("/server/pages-pads", test_pages_pads);
  g_test_add_func ("/server/latency-element", test_latency_element);
  g_test_add_func ("/server/latency-all-limit", test_latency_all_limit);
  g_test_add_func ("/server/findings-flow", test_findings_flow);
  g_test_add_func ("/server/findings-stuck", test_findings_stuck);
  g_test_add_func ("/server/check-not-negotiated",
      test_check_not_negotiated);
  g_test_add_func ("/server/check-failed-link", test_check_failed_link);
  g_test_add_func ("/server/check-blocked", test_check_blocked);
  g_test_add_func ("/server/check-quiet", test_check_quiet);
  g_test_add_func ("/server/check-preroll", test_check_preroll);
  g_test_add_func ("/server/check-link-after-error",
      test_check_link_after_error);
  g_test_add_func ("/server/check-pull-end", test_check_pull_end);
  g_test_add_func ("/server/check-link-was-linked",
      test_check_link_was_linked);
  g_test_add_func ("/server/check-missing-plugin",
      test_check_missing_plugin);
  g_test_add_func ("/server/check-leaky", test_check_leaky);
  g_test_add_func ("/server/check-full-queue", test_check_full_queue);
  g_test_add_func ("/server/check-late-sink", test_check_late_sink);
  g_test_add_func ("/server/check-silent-source", test_check_silent_source);
  g_test_add_func ("/server/list-source-not-silent",
      test_list_source_not_silent);
  g_test_add_func ("/server/read-source-not-silent",
      test_read_source_not_silent);
  g_test_add_func ("/server/origin-check", test_origin_check);
  g_test_add_func ("/server/port-fallback", test_port_fallback);
  g_test_add_func ("/server/no-free-port", test_no_free_port);
  g_test_add_func ("/server/shutdown-with-client", test_shutdown_with_client);

  return g_test_run ();
}
