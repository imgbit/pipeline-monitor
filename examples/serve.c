/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <gst/gst.h>
#include <string.h>

/* Usage: serve [--warnings] [pipeline description]
 * Runs a pipeline until it ends. If it has a tee named "t", a branch is
 * added and removed every two seconds to produce live updates; with
 * --warnings the tee also posts a warning each time. An appsrc named
 * "subs" gets a subtitle every two seconds, with a GAP event in between.
 * Without a description (or an empty one) it runs video, audio and
 * subtitles; the audio sink plays 300 ms late, so its queue stays about
 * 60 % full. */

#define DEFAULT_PIPELINE \
  "videotestsrc is-live=true ! jpegenc ! decodebin3 ! queue ! tee name=t ! " \
  "queue ! videoconvert ! fakesink " \
  "audiotestsrc is-live=true ! opusenc ! decodebin3 ! audioconvert ! " \
  "queue max-size-time=500000000 ! fakesink sync=true ts-offset=300000000 " \
  "appsrc name=subs is-live=true format=time " \
  "caps=text/x-raw,format=utf8 ! queue ! fakesink"

typedef struct
{
  GstElement *pipeline;
  GstElement *tee;
  GstElement *queue;
  GstElement *sink;
  GstPad *tee_pad;
  gboolean warnings;
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
  if (b->warnings)
    GST_ELEMENT_WARNING (b->tee, STREAM, FAILED, ("example warning: %s",
            b->queue ? "branch added" : "branch removed"), (NULL));
  return G_SOURCE_CONTINUE;
}

/* Every second: a one second subtitle, then a GAP of one second. */
static gboolean
push_subtitle (gpointer user_data)
{
  GstElement *subs = user_data;
  static guint count;
  GstClockTime now = gst_element_get_current_running_time (subs);

  if (!GST_CLOCK_TIME_IS_VALID (now))
    return G_SOURCE_CONTINUE;
  if (count++ % 2 == 0) {
    gchar *text = g_strdup_printf ("subtitle %u", count / 2 + 1);
    GstBuffer *buffer = gst_buffer_new_wrapped (text, strlen (text));
    GstFlowReturn ret;

    GST_BUFFER_PTS (buffer) = now;
    GST_BUFFER_DURATION (buffer) = GST_SECOND;
    g_signal_emit_by_name (subs, "push-buffer", buffer, &ret);
    gst_buffer_unref (buffer);
  } else {
    gst_element_send_event (subs, gst_event_new_gap (now, GST_SECOND));
  }
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
  const gchar *description = DEFAULT_PIPELINE;
  GstElement *subs;
  Branch branch = { 0 };
  GError *error = NULL;
  GMainLoop *loop;
  GstBus *bus;

  g_setenv ("GST_TRACERS", "pipeline-monitor", FALSE);
  gst_init (&argc, &argv);
  if (argc > 1 && g_strcmp0 (argv[1], "--warnings") == 0) {
    branch.warnings = TRUE;
    argv++;
    argc--;
  }
  if (argc > 1 && argv[1][0] != '\0')
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
  subs = gst_bin_get_by_name (GST_BIN (branch.pipeline), "subs");
  if (subs != NULL)
    g_timeout_add_seconds_full (G_PRIORITY_DEFAULT, 1, push_subtitle, subs,
        gst_object_unref);

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
