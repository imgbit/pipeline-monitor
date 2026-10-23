/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <gst/gst.h>
#include <gst/gsttracer.h>
#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include <string.h>

/* GST_TRACERS is read once by gst_init(), so each case runs in a child
 * process that sets it first. */

static gchar *
start_tracer (const gchar * tracers)
{
  GList *list, *l;
  gchar *url = NULL;

  g_setenv ("GST_TRACERS", tracers, TRUE);
  gst_init (NULL, NULL);

  list = gst_tracing_get_active_tracers ();
  for (l = list; l != NULL; l = l->next) {
    if (g_strcmp0 (G_OBJECT_TYPE_NAME (l->data),
            "GstPipelineMonitorTracer") == 0)
      g_object_get (l->data, "url", &url, NULL);
  }
  g_list_free_full (list, gst_object_unref);
  g_assert_nonnull (url);
  return url;
}

static guint
url_port (const gchar * url)
{
  GUri *uri = g_uri_parse (url, G_URI_FLAGS_NONE, NULL);
  guint port = g_uri_get_port (uri);

  g_uri_unref (uri);
  return port;
}

/* The default port, or one of the next nine if another program has it. */
static void
assert_default_port (const gchar * url)
{
  guint port = url_port (url);

  g_assert_cmpuint (port, >=, 8765);
  g_assert_cmpuint (port, <, 8775);
}

static void
run_in_subprocess (void)
{
  g_test_trap_subprocess (NULL, 0, G_TEST_SUBPROCESS_INHERIT_STDERR);
  g_test_trap_assert_passed ();
}

static void
test_ui_dir (void)
{
  if (g_test_subprocess ()) {
    gchar *dir = g_dir_make_tmp ("pm-options-XXXXXX", NULL);
    gchar *file = g_build_filename (dir, "app.js", NULL);
    /* Quoted with forward slashes: the option syntax cannot take a bare
     * Windows path (backslashes, short names with '~'). */
    gchar *path = g_strdelimit (g_strdup (dir), "\\", '/');
    gchar *tracers = g_strdup_printf ("pipeline-monitor(port=0,ui-dir=\"%s\")",
        path);
    SoupSession *session = soup_session_new ();
    SoupMessage *msg;
    GBytes *body;
    gchar *url, *app;

    g_assert_true (g_file_set_contents (file, "// from ui-dir", -1, NULL));
    url = start_tracer (tracers);
    g_assert_cmpuint (url_port (url), !=, 0);

    app = g_strconcat (url, "app.js", NULL);
    msg = soup_message_new ("GET", app);
    body = soup_session_send_and_read (session, msg, NULL, NULL);
    g_assert_cmpuint (soup_message_get_status (msg), ==, 200);
    g_assert_cmpmem (g_bytes_get_data (body, NULL), g_bytes_get_size (body),
        "// from ui-dir", 14);

    g_bytes_unref (body);
    g_object_unref (msg);
    g_object_unref (session);
    g_unlink (file);
    g_rmdir (dir);
    g_free (app);
    g_free (url);
    g_free (tracers);
    g_free (path);
    g_free (file);
    g_free (dir);
    return;
  }
  run_in_subprocess ();
}

static void
test_ui_dir_missing (void)
{
  if (g_test_subprocess ()) {
    gchar *url = start_tracer ("pipeline-monitor(port=0,ui-dir=pm-no-such-dir)");

    g_free (url);
    return;
  }
  g_test_trap_subprocess (NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
  g_test_trap_assert_passed ();
  g_test_trap_assert_stderr ("*ui-dir 'pm-no-such-dir' is not a folder*");
}

static void
test_log_off (void)
{
  if (g_test_subprocess ()) {
    gchar *url = start_tracer ("pipeline-monitor(port=0,log=false)");
    gchar *log_url = g_strconcat (url, "api/log", NULL);
    GstElement *element = gst_element_factory_make ("identity", NULL);
    GError *error = g_error_new_literal (GST_CORE_ERROR,
        GST_CORE_ERROR_FAILED, "ignored");
    SoupSession *session = soup_session_new ();
    SoupMessage *msg = soup_message_new ("GET", log_url);
    GBytes *body;

    gst_element_post_message (element,
        gst_message_new_warning (GST_OBJECT (element), error, NULL));
    body = soup_session_send_and_read (session, msg, NULL, NULL);
    g_assert_cmpmem (g_bytes_get_data (body, NULL), g_bytes_get_size (body),
        "{\"enabled\":false,\"seq\":0,\"entries\":[]}", 38);

    g_bytes_unref (body);
    g_object_unref (msg);
    g_object_unref (session);
    g_error_free (error);
    gst_object_unref (element);
    g_free (log_url);
    g_free (url);
    return;
  }
  run_in_subprocess ();
}

static void
test_port_out_of_range (void)
{
  if (g_test_subprocess ()) {
    gchar *url = start_tracer ("pipeline-monitor(port=99999)");

    assert_default_port (url);
    g_free (url);
    return;
  }
  run_in_subprocess ();
}

static void
test_port_not_a_number (void)
{
  if (g_test_subprocess ()) {
    gchar *url = start_tracer ("pipeline-monitor(port=abc)");

    assert_default_port (url);
    g_free (url);
    return;
  }
  run_in_subprocess ();
}

static void
test_unparsable_options (void)
{
  if (g_test_subprocess ()) {
    gchar *url = start_tracer ("pipeline-monitor(port 9000)");

    assert_default_port (url);
    g_free (url);
    return;
  }
  run_in_subprocess ();
}

/* A plain WebSocket client on its own thread: connects to the port after
 * a delay, then collects the text of every message until the server
 * closes. Enough for the server's unmasked, unfragmented messages. */
typedef struct
{
  guint port;
  guint delay_ms;
  GString *received;
  gint64 connected_at;
} PageThread;

static gboolean
read_all (GInputStream * in, guint8 * buf, gsize size)
{
  gsize got = 0;

  return g_input_stream_read_all (in, buf, size, &got, NULL, NULL) &&
      got == size;
}

static gpointer
page_thread (gpointer data)
{
  PageThread *page = data;
  GSocketClient *client = g_socket_client_new ();
  GSocketConnection *conn = NULL;
  GInputStream *in;
  gchar *request;
  gint tries;

  g_usleep (page->delay_ms * 1000);
  for (tries = 0; tries < 100 && conn == NULL; tries++) {
    conn = g_socket_client_connect_to_host (client, "127.0.0.1", page->port,
        NULL, NULL);
    if (conn == NULL)
      g_usleep (50 * 1000);
  }
  g_assert_nonnull (conn);
  request = g_strdup_printf ("GET /ws HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n"
      "Upgrade: websocket\r\nConnection: Upgrade\r\n"
      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
      "Sec-WebSocket-Version: 13\r\n\r\n", page->port);
  g_output_stream_write_all (g_io_stream_get_output_stream (G_IO_STREAM
          (conn)), request, strlen (request), NULL, NULL, NULL);
  g_free (request);
  page->connected_at = g_get_monotonic_time ();

  /* Skip the response headers, then read frames until the end. */
  in = g_io_stream_get_input_stream (G_IO_STREAM (conn));
  {
    guint8 c;
    gint newlines = 0;

    while (newlines < 2 && read_all (in, &c, 1))
      newlines = c == '\n' ? newlines + 1 : c == '\r' ? newlines : 0;
  }
  while (TRUE) {
    guint8 head[2], ext[8];
    guint64 len;
    gchar *payload;

    if (!read_all (in, head, 2))
      break;
    len = head[1] & 0x7f;
    if (len == 126) {
      if (!read_all (in, ext, 2))
        break;
      len = (ext[0] << 8) | ext[1];
    } else if (len == 127) {
      guint i;

      if (!read_all (in, ext, 8))
        break;
      for (len = 0, i = 0; i < 8; i++)
        len = (len << 8) | ext[i];
    }
    payload = g_malloc (len + 1);
    if (!read_all (in, (guint8 *) payload, len)) {
      g_free (payload);
      break;
    }
    payload[len] = '\0';
    /* Text frames only; a close frame ends it. */
    if ((head[0] & 0x0f) == 0x8) {
      g_free (payload);
      break;
    }
    g_string_append (page->received, payload);
    g_string_append_c (page->received, '\n');
    g_free (payload);
  }
  g_object_unref (conn);
  g_object_unref (client);
  return NULL;
}

static guint
free_port (void)
{
  GSocket *socket = g_socket_new (G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_STREAM,
      G_SOCKET_PROTOCOL_TCP, NULL);
  GInetAddress *loopback = g_inet_address_new_loopback (G_SOCKET_FAMILY_IPV4);
  GSocketAddress *address = g_inet_socket_address_new (loopback, 0);
  GSocketAddress *bound;
  guint port;

  g_assert_true (g_socket_bind (socket, address, FALSE, NULL));
  bound = g_socket_get_local_address (socket, NULL);
  port = g_inet_socket_address_get_port (G_INET_SOCKET_ADDRESS (bound));
  g_object_unref (bound);
  g_object_unref (address);
  g_object_unref (loopback);
  g_object_unref (socket);
  return port;
}

/* wait=true holds gst_init until a page connects. */
static void
test_wait (void)
{
  if (g_test_subprocess ()) {
    PageThread page = { free_port (), 300, g_string_new (NULL), 0 };
    gchar *tracers = g_strdup_printf ("pipeline-monitor(port=%u,wait=true)",
        page.port);
    GThread *thread = g_thread_new ("page", page_thread, &page);
    gchar *url = start_tracer (tracers);

    /* gst_init returned only once the page was there. */
    g_assert_cmpint (page.connected_at, >, 0);
    g_assert_cmpuint (url_port (url), ==, page.port);
    gst_deinit ();
    g_thread_join (thread);
    g_string_free (page.received, TRUE);
    g_free (url);
    g_free (tracers);
    return;
  }
  run_in_subprocess ();
}

/* A pipeline fails and the program ends: the page gets the problem and
 * "bye" before the connection closes. */
static void
test_final_state (void)
{
  if (g_test_subprocess ()) {
    PageThread page = { free_port (), 0, g_string_new (NULL), 0 };
    gchar *tracers = g_strdup_printf ("pipeline-monitor(port=%u,wait=true)",
        page.port);
    GThread *thread = g_thread_new ("page", page_thread, &page);
    GstElement *pipeline, *filter;
    GstCaps *video;
    GstBus *bus;
    GstMessage *msg;

    g_free (start_tracer (tracers));
    /* Linked first, impossible caps set afterwards: the source cannot
     * negotiate when it starts. */
    pipeline = gst_parse_launch ("audiotestsrc ! capsfilter name=f ! "
        "fakesink", NULL);
    filter = gst_bin_get_by_name (GST_BIN (pipeline), "f");
    video = gst_caps_from_string ("video/x-raw");
    g_object_set (filter, "caps", video, NULL);
    gst_caps_unref (video);
    gst_object_unref (filter);
    gst_element_set_state (pipeline, GST_STATE_PLAYING);
    bus = gst_element_get_bus (pipeline);
    msg = gst_bus_timed_pop_filtered (bus, 5 * GST_SECOND,
        GST_MESSAGE_ERROR);
    g_assert_nonnull (msg);
    gst_message_unref (msg);
    gst_object_unref (bus);
    gst_element_set_state (pipeline, GST_STATE_NULL);
    gst_object_unref (pipeline);
    gst_deinit ();

    g_thread_join (thread);
    g_assert_nonnull (strstr (page.received->str, "\"type\":\"findings\""));
    g_assert_nonnull (strstr (page.received->str, "not-negotiated"));
    g_assert_nonnull (strstr (page.received->str, "{\"type\":\"bye\"}"));
    g_string_free (page.received, TRUE);
    g_free (tracers);
    return;
  }
  run_in_subprocess ();
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/options/ui-dir", test_ui_dir);
  g_test_add_func ("/options/ui-dir-missing", test_ui_dir_missing);
  g_test_add_func ("/options/log-off", test_log_off);
  g_test_add_func ("/options/port-out-of-range", test_port_out_of_range);
  g_test_add_func ("/options/port-not-a-number", test_port_not_a_number);
  g_test_add_func ("/options/unparsable", test_unparsable_options);
  g_test_add_func ("/options/wait", test_wait);
  g_test_add_func ("/options/final-state", test_final_state);

  return g_test_run ();
}
