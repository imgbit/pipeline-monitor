/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <gst/gst.h>
#include <gst/gsttracer.h>
#include <libsoup/soup.h>

static gchar *
monitor_url (void)
{
  GList *tracers, *l;
  gchar *url = NULL;

  tracers = gst_tracing_get_active_tracers ();
  for (l = tracers; l != NULL; l = l->next) {
    if (url == NULL &&
        g_object_class_find_property (G_OBJECT_GET_CLASS (l->data), "url"))
      g_object_get (l->data, "url", &url, NULL);
  }
  g_list_free_full (tracers, gst_object_unref);
  return url;
}

int
main (int argc, char **argv)
{
  const gchar *description = "videotestsrc ! queue ! fakesink";
  GError *error = NULL;
  GstElement *pipeline;
  SoupSession *session;
  SoupMessage *msg;
  GBytes *body;
  gchar *url, *graph_url;

  g_setenv ("GST_TRACERS", "pipeline-monitor", FALSE);
  gst_init (&argc, &argv);
  if (argc > 1)
    description = argv[1];

  url = monitor_url ();
  if (url == NULL) {
    g_printerr ("pipeline-monitor is not running\n");
    return 1;
  }

  pipeline = gst_parse_launch (description, &error);
  if (pipeline == NULL) {
    g_printerr ("%s\n", error->message);
    return 1;
  }
  gst_element_set_state (pipeline, GST_STATE_PLAYING);
  gst_element_get_state (pipeline, NULL, NULL, 5 * GST_SECOND);

  session = soup_session_new ();
  graph_url = g_strconcat (url, "api/graph", NULL);
  msg = soup_message_new ("GET", graph_url);
  body = soup_session_send_and_read (session, msg, NULL, &error);
  if (body == NULL) {
    g_printerr ("%s\n", error->message);
    return 1;
  }
  g_print ("%.*s\n", (int) g_bytes_get_size (body),
      (const gchar *) g_bytes_get_data (body, NULL));

  g_bytes_unref (body);
  g_object_unref (msg);
  g_object_unref (session);
  g_free (graph_url);
  g_free (url);
  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (pipeline);

  return 0;
}
