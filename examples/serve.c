/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <gst/gst.h>
#include <string.h>

/* Usage: serve [--warnings] [--flow-log] [--set-caps NAME=CAPS]...
 *   [--second DESCRIPTION] [pipeline description]
 * Runs a pipeline until it ends. If it has a tee named "t", a branch is
 * added and removed every two seconds to produce live updates; with
 * --warnings the tee also posts a warning each time. An appsrc named
 * "subs" gets a subtitle every two seconds, with a GAP event in between.
 * Without a description (or an empty one) it runs video, audio and
 * subtitles; the audio sink plays 300 ms late, so its queue stays about
 * 60 % full. --set-caps sets the caps of the capsfilter NAME after the
 * pipeline is built, to make negotiation fail when it starts (a pipeline
 * description with such caps would not even link). --second runs another
 * pipeline next to it. --flow-log prints, every second, the buffers each
 * sink rendered and the last result of each source's pushes, and when the
 * branch comes and goes (for tests that find data not flowing). */

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

static gint64 flow_log_start = -1;

static gdouble
flow_log_time (void)
{
  return (g_get_monotonic_time () - flow_log_start) / 1e6;
}

static gboolean
log_flow (gpointer user_data)
{
  GstIterator *it = gst_bin_iterate_recurse (GST_BIN (user_data));
  GString *line = g_string_new (NULL);
  GValue item = G_VALUE_INIT;

  g_string_printf (line, "flow %.1f s:", flow_log_time ());
  while (gst_iterator_next (it, &item) == GST_ITERATOR_OK) {
    GstElement *element = g_value_get_object (&item);
    GstPad *src = gst_element_get_static_pad (element, "src");

    if (GST_OBJECT_FLAG_IS_SET (element, GST_ELEMENT_FLAG_SINK) &&
        g_object_class_find_property (G_OBJECT_GET_CLASS (element),
            "stats") != NULL) {
      GstStructure *stats = NULL;
      guint64 rendered = 0;

      g_object_get (element, "stats", &stats, NULL);
      if (stats != NULL) {
        gst_structure_get_uint64 (stats, "rendered", &rendered);
        gst_structure_free (stats);
      }
      g_string_append_printf (line, " %s=%" G_GUINT64_FORMAT,
          GST_ELEMENT_NAME (element), rendered);
    } else if (GST_OBJECT_FLAG_IS_SET (element, GST_ELEMENT_FLAG_SOURCE) &&
        src != NULL) {
      g_string_append_printf (line, " %s:%s", GST_ELEMENT_NAME (element),
          gst_flow_get_name (gst_pad_get_last_flow_return (src)));
    }
    if (src != NULL)
      gst_object_unref (src);
    g_value_reset (&item);
  }
  g_value_unset (&item);
  gst_iterator_free (it);
  g_printerr ("%s\n", line->str);
  g_string_free (line, TRUE);
  return G_SOURCE_CONTINUE;
}

static void
add_branch (Branch * b)
{
  GstPad *sinkpad;

  b->queue = gst_element_factory_make ("queue", "extra-queue");
  b->sink = gst_element_factory_make ("fakesink", "extra-sink");
  g_object_set (b->sink, "async", FALSE, NULL);
  gst_bin_add_many (GST_BIN (b->pipeline), b->queue, b->sink, NULL);
  gst_element_link (b->queue, b->sink);
  /* Playing before the tee is linked: a buffer pushed into a stopped queue
   * returns FLUSHING, which stops the tee's source for good. */
  gst_element_sync_state_with_parent (b->sink);
  gst_element_sync_state_with_parent (b->queue);

  b->tee_pad = gst_element_request_pad_simple (b->tee, "src_%u");
  sinkpad = gst_element_get_static_pad (b->queue, "sink");
  gst_pad_link (b->tee_pad, sinkpad);
  gst_object_unref (sinkpad);
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
  if (flow_log_start >= 0)
    g_printerr ("flow %.1f s: branch %s\n", flow_log_time (),
        b->queue ? "added" : "removed");
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
  const gchar *second_description = NULL;
  GstElement *subs, *second = NULL;
  GList *set_caps = NULL, *l;
  Branch branch = { 0 };
  GError *error = NULL;
  GMainLoop *loop;
  GstBus *bus;

  g_setenv ("GST_TRACERS", "pipeline-monitor", FALSE);
  gst_init (&argc, &argv);
  while (argc > 1 && g_str_has_prefix (argv[1], "--")) {
    if (g_strcmp0 (argv[1], "--warnings") == 0) {
      branch.warnings = TRUE;
    } else if (g_strcmp0 (argv[1], "--flow-log") == 0) {
      flow_log_start = g_get_monotonic_time ();
    } else if (g_strcmp0 (argv[1], "--set-caps") == 0 && argc > 2) {
      set_caps = g_list_append (set_caps, argv[2]);
      argv++;
      argc--;
    } else if (g_strcmp0 (argv[1], "--second") == 0 && argc > 2) {
      second_description = argv[2];
      argv++;
      argc--;
    } else {
      g_printerr ("unknown option %s\n", argv[1]);
      return 1;
    }
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
  for (l = set_caps; l != NULL; l = l->next) {
    gchar **parts = g_strsplit (l->data, "=", 2);
    GstElement *filter = gst_bin_get_by_name (GST_BIN (branch.pipeline),
        parts[0]);
    GstCaps *caps = parts[1] ? gst_caps_from_string (parts[1]) : NULL;

    if (filter == NULL || caps == NULL) {
      g_printerr ("--set-caps %s: no such element or bad caps\n",
          (gchar *) l->data);
      return 1;
    }
    g_object_set (filter, "caps", caps, NULL);
    gst_caps_unref (caps);
    gst_object_unref (filter);
    g_strfreev (parts);
  }
  g_list_free (set_caps);
  if (second_description != NULL) {
    second = gst_parse_launch (second_description, &error);
    if (second == NULL) {
      g_printerr ("--second: %s\n", error->message);
      return 1;
    }
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

  if (flow_log_start >= 0)
    g_timeout_add_seconds (1, log_flow, branch.pipeline);
  gst_element_set_state (branch.pipeline, GST_STATE_PLAYING);
  if (second != NULL)
    gst_element_set_state (second, GST_STATE_PLAYING);
  g_print ("Running, press Ctrl+C to stop\n");
  g_main_loop_run (loop);

  gst_element_set_state (branch.pipeline, GST_STATE_NULL);
  if (second != NULL) {
    gst_element_set_state (second, GST_STATE_NULL);
    gst_object_unref (second);
  }
  gst_clear_object (&branch.tee_pad);
  gst_clear_object (&branch.tee);
  gst_bus_remove_watch (bus);
  gst_object_unref (bus);
  gst_object_unref (branch.pipeline);
  g_main_loop_unref (loop);
  gst_deinit ();

  return 0;
}
