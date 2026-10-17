/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <gst/gst.h>
#include <gst/gsttracer.h>
#include <glib/gstdio.h>
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

/* Waits for a graph message and checks whether it mentions needle. */
static gboolean
ws_next_graph_has (WsClient * client, const gchar * needle)
{
  gchar *message;
  gboolean found;

  if (g_queue_is_empty (&client->messages)) {
    client->got_message = FALSE;
    g_assert_true (wait_for (&client->got_message));
  }
  message = g_queue_pop_head (&client->messages);
  g_assert_true (g_str_has_prefix (message, "{\"type\":\"graph\",\"graph\":{"));
  found = strstr (message, needle) != NULL;
  g_free (message);
  return found;
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

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  gst_init (&argc, &argv);

  g_test_add_func ("/server/url", test_url);
  g_test_add_func ("/server/http", test_http);
  g_test_add_func ("/server/ui-files", test_ui_files);
  g_test_add_func ("/server/ui-dir", test_ui_dir);
  g_test_add_func ("/server/host-check", test_host_check);
  g_test_add_func ("/server/websocket", test_websocket);
  g_test_add_func ("/server/origin-check", test_origin_check);
  g_test_add_func ("/server/port-fallback", test_port_fallback);
  g_test_add_func ("/server/no-free-port", test_no_free_port);
  g_test_add_func ("/server/shutdown-with-client", test_shutdown_with_client);

  return g_test_run ();
}
