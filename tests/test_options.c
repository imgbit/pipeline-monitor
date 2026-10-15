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
    gchar *tracers = g_strdup_printf ("pipeline-monitor(port=0,ui-dir=%s)",
        dir);
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
    g_free (file);
    g_free (dir);
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

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/options/ui-dir", test_ui_dir);
  g_test_add_func ("/options/port-out-of-range", test_port_out_of_range);
  g_test_add_func ("/options/port-not-a-number", test_port_not_a_number);
  g_test_add_func ("/options/unparsable", test_unparsable_options);

  return g_test_run ();
}
