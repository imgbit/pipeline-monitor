/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <gst/gst.h>
#include <gst/gsttracer.h>

int
main (int argc, char **argv)
{
  const gchar *description = "videotestsrc ! queue ! fakesink";
  GError *error = NULL;
  GstElement *pipeline;
  GList *tracers, *l;

  g_setenv ("GST_TRACERS", "pipeline-monitor", FALSE);
  gst_init (&argc, &argv);
  if (argc > 1)
    description = argv[1];

  pipeline = gst_parse_launch (description, &error);
  if (pipeline == NULL) {
    g_printerr ("%s\n", error->message);
    return 1;
  }
  gst_element_set_state (pipeline, GST_STATE_PLAYING);
  gst_element_get_state (pipeline, NULL, NULL, 5 * GST_SECOND);

  tracers = gst_tracing_get_active_tracers ();
  for (l = tracers; l != NULL; l = l->next) {
    gchar *graph = NULL;

    if (g_object_class_find_property (G_OBJECT_GET_CLASS (l->data), "graph")) {
      g_object_get (l->data, "graph", &graph, NULL);
      g_print ("%s\n", graph);
      g_free (graph);
    }
  }
  g_list_free_full (tracers, gst_object_unref);

  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (pipeline);

  return 0;
}
