/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <gst/gst.h>

/* Runs a pipeline until it ends. If it has a tee named "t", a branch is
 * added and removed every two seconds to produce live updates. */

typedef struct
{
  GstElement *pipeline;
  GstElement *tee;
  GstElement *queue;
  GstElement *sink;
  GstPad *tee_pad;
} Branch;

static void
add_branch (Branch * b)
{
  GstPad *sinkpad;

  b->queue = gst_element_factory_make ("queue", "extra-queue");
  b->sink = gst_element_factory_make ("fakesink", "extra-sink");
  g_object_set (b->sink, "async", FALSE, NULL);
  gst_bin_add_many (GST_BIN (b->pipeline), b->queue, b->sink, NULL);
  gst_element_link (b->queue, b->sink);

  b->tee_pad = gst_element_request_pad_simple (b->tee, "src_%u");
  sinkpad = gst_element_get_static_pad (b->queue, "sink");
  gst_pad_link (b->tee_pad, sinkpad);
  gst_object_unref (sinkpad);

  gst_element_sync_state_with_parent (b->sink);
  gst_element_sync_state_with_parent (b->queue);
}

static void
remove_branch (Branch * b)
{
  gst_element_release_request_pad (b->tee, b->tee_pad);
  gst_clear_object (&b->tee_pad);
  gst_element_set_state (b->queue, GST_STATE_NULL);
  gst_element_set_state (b->sink, GST_STATE_NULL);
  gst_bin_remove_many (GST_BIN (b->pipeline), b->queue, b->sink, NULL);
  b->queue = b->sink = NULL;
}

static gboolean
toggle_branch (gpointer user_data)
{
  Branch *b = user_data;

  if (b->queue == NULL)
    add_branch (b);
  else
    remove_branch (b);
  return G_SOURCE_CONTINUE;
}

static gboolean
on_message (GstBus * bus, GstMessage * message, gpointer user_data)
{
  GError *error = NULL;

  switch (GST_MESSAGE_TYPE (message)) {
    case GST_MESSAGE_ERROR:
      gst_message_parse_error (message, &error, NULL);
      g_printerr ("error: %s\n", error->message);
      g_error_free (error);
      /* fall through */
    case GST_MESSAGE_EOS:
      g_main_loop_quit (user_data);
      break;
    default:
      break;
  }
  return G_SOURCE_CONTINUE;
}

int
main (int argc, char **argv)
{
  const gchar *description =
      "videotestsrc is-live=true ! tee name=t ! queue ! fakesink";
  Branch branch = { 0 };
  GError *error = NULL;
  GMainLoop *loop;
  GstBus *bus;

  g_setenv ("GST_TRACERS", "pipeline-monitor", FALSE);
  gst_init (&argc, &argv);
  if (argc > 1)
    description = argv[1];

  branch.pipeline = gst_parse_launch (description, &error);
  if (branch.pipeline == NULL) {
    g_printerr ("%s\n", error->message);
    return 1;
  }

  loop = g_main_loop_new (NULL, FALSE);
  bus = gst_element_get_bus (branch.pipeline);
  gst_bus_add_watch (bus, on_message, loop);

  branch.tee = gst_bin_get_by_name (GST_BIN (branch.pipeline), "t");
  if (branch.tee != NULL)
    g_timeout_add_seconds (2, toggle_branch, &branch);

  gst_element_set_state (branch.pipeline, GST_STATE_PLAYING);
  g_print ("Running, press Ctrl+C to stop\n");
  g_main_loop_run (loop);

  gst_element_set_state (branch.pipeline, GST_STATE_NULL);
  gst_clear_object (&branch.tee_pad);
  gst_clear_object (&branch.tee);
  gst_bus_remove_watch (bus);
  gst_object_unref (bus);
  gst_object_unref (branch.pipeline);
  g_main_loop_unref (loop);
  gst_deinit ();

  return 0;
}
