/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <gst/gst.h>
#include <json-glib/json-glib.h>

#include "pmmodel.h"
#include "pmmonitor.h"

/* Model tests without a pipeline: objects are created and destroyed by the
 * test itself, in orders the tracer hooks can't easily produce. */

typedef struct
{
  PmModel *model;
  GstElement *element;
  GstPad *pad;
  GstPad *sink;
  PmMonitor *monitor;
} Fixture;

/* Any pointer serves as a client. */
#define CLIENT GINT_TO_POINTER (1)

static void
fixture_setup (Fixture * f, gconstpointer data)
{
  f->model = pm_model_new ();
  f->element = gst_object_ref_sink (gst_bin_new ("core-pipeline"));
  f->pad = gst_object_ref_sink (gst_pad_new ("core-pad", GST_PAD_SRC));
  pm_model_update_element (f->model, f->element, "core-pipeline", "bin",
      "GstBin", TRUE, TRUE);
  pm_model_add_pad (f->model, f->element, f->pad, "core-pad", GST_PAD_SRC,
      NULL, FALSE);
}

static void
fixture_teardown (Fixture * f, gconstpointer data)
{
  g_clear_pointer (&f->monitor, pm_monitor_free);
  pm_model_free (f->model);
  gst_clear_object (&f->sink);
  gst_clear_object (&f->pad);
  gst_object_unref (f->element);
}

/* The id the model gave the pad, read from its JSON like the page does. */
static guint64
pad_id (PmModel * model)
{
  gchar *json = pm_model_to_json (model);
  JsonParser *parser = json_parser_new ();
  JsonObject *element;
  guint64 id;

  g_assert_true (json_parser_load_from_data (parser, json, -1, NULL));
  element = json_array_get_object_element (json_object_get_array_member
      (json_node_get_object (json_parser_get_root (parser)), "pipelines"), 0);
  id = json_object_get_int_member (json_array_get_object_element
      (json_object_get_array_member (element, "pads"), 0), "id");
  g_object_unref (parser);
  g_free (json);
  return id;
}

static void
test_get_pad (Fixture * f, gconstpointer data)
{
  guint64 id = pad_id (f->model);
  GstPad *pad = pm_model_get_pad (f->model, id);

  g_assert_true (pad == f->pad);
  /* The caller gets its own reference. */
  g_assert_cmpint (GST_OBJECT_REFCOUNT_VALUE (f->pad), ==, 2);
  gst_object_unref (pad);

  g_assert_null (pm_model_get_pad (f->model, id + 1000));
}

static void
test_removed_pad (Fixture * f, gconstpointer data)
{
  guint64 id = pad_id (f->model);

  pm_model_remove_pad (f->model, f->pad);
  g_assert_null (pm_model_get_pad (f->model, id));
}

/* The pad is destroyed while the model still lists it, as when it goes
 * away just before the tracer's object-destroyed hook updates the model. */
static void
test_destroyed_pad (Fixture * f, gconstpointer data)
{
  guint64 id = pad_id (f->model);

  gst_clear_object (&f->pad);
  g_assert_null (pm_model_get_pad (f->model, id));
}

static void
test_removed_element (Fixture * f, gconstpointer data)
{
  guint64 id = pad_id (f->model);

  pm_model_remove_object (f->model, f->element);
  g_assert_null (pm_model_get_pad (f->model, id));
}

/* Pad monitoring: buffers are pushed by the test from core-pad into a sink
 * pad that drops them. */

static GstFlowReturn
drop_buffer (GstPad * pad, GstObject * parent, GstBuffer * buffer)
{
  gst_buffer_unref (buffer);
  return GST_FLOW_OK;
}

static void
link_sink (Fixture * f)
{
  gst_element_add_pad (f->element, f->pad);
  f->sink = gst_object_ref_sink (gst_pad_new ("core-sink", GST_PAD_SINK));
  gst_pad_set_chain_function (f->sink, drop_buffer);
  g_assert_cmpint (gst_pad_link (f->pad, f->sink), ==, GST_PAD_LINK_OK);
  gst_pad_set_active (f->sink, TRUE);
  gst_pad_set_active (f->pad, TRUE);
  f->monitor = pm_monitor_new (f->model);
}

static void
push_start (Fixture * f, GstClockTime base)
{
  GstSegment segment;

  gst_segment_init (&segment, GST_FORMAT_TIME);
  segment.base = base;
  g_assert_true (gst_pad_push_event (f->pad,
          gst_event_new_stream_start ("core")));
  g_assert_true (gst_pad_push_event (f->pad,
          gst_event_new_caps (gst_caps_new_empty_simple ("video/x-raw"))));
  g_assert_true (gst_pad_push_event (f->pad,
          gst_event_new_segment (&segment)));
}

static GstBuffer *
new_buffer (gsize size, GstClockTime pts)
{
  GstBuffer *buffer = gst_buffer_new_allocate (NULL, size, NULL);

  GST_BUFFER_PTS (buffer) = pts;
  return buffer;
}

/* The client's entry for the pad, as the page gets it. */
static JsonObject *
stats_of (Fixture * f, JsonNode ** root)
{
  gchar *json = pm_monitor_stats_json (f->monitor, CLIENT);
  JsonParser *parser = json_parser_new ();
  JsonArray *pads;

  g_assert_nonnull (json);
  g_assert_true (json_parser_load_from_data (parser, json, -1, NULL));
  *root = json_node_copy (json_parser_get_root (parser));
  g_object_unref (parser);
  g_free (json);
  pads = json_object_get_array_member (json_node_get_object (*root), "pads");
  g_assert_cmpuint (json_array_get_length (pads), ==, 1);
  return json_array_get_object_element (pads, 0);
}

static gboolean
has_flag (JsonObject * last, const gchar * flag)
{
  JsonArray *flags = json_object_get_array_member (last, "flags");
  guint i;

  for (i = 0; i < json_array_get_length (flags); i++)
    if (g_strcmp0 (json_array_get_string_element (flags, i), flag) == 0)
      return TRUE;
  return FALSE;
}

static void
test_monitor_values (Fixture * f, gconstpointer data)
{
  GstBuffer *buffer;
  GstBufferList *list;
  JsonNode *root;
  JsonObject *stats, *last, *segment, *events;
  gint i;

  link_sink (f);
  g_assert_true (pm_monitor_add (f->monitor, CLIENT, pad_id (f->model)));
  push_start (f, 0);

  buffer = new_buffer (100, GST_SECOND);
  GST_BUFFER_DURATION (buffer) = 40 * GST_MSECOND;
  GST_BUFFER_OFFSET (buffer) = 5;
  GST_BUFFER_FLAG_SET (buffer, GST_BUFFER_FLAG_DISCONT |
      GST_BUFFER_FLAG_DELTA_UNIT);
  g_assert_cmpint (gst_pad_push (f->pad, buffer), ==, GST_FLOW_OK);

  stats = stats_of (f, &root);
  g_assert_cmpint (json_object_get_int_member (stats, "buffers"), ==, 1);
  g_assert_cmpint (json_object_get_int_member (stats, "bytes"), ==, 100);
  last = json_object_get_object_member (stats, "last");
  g_assert_cmpint (json_object_get_int_member (last, "pts"), ==, GST_SECOND);
  g_assert_true (json_object_get_null_member (last, "dts"));
  g_assert_cmpint (json_object_get_int_member (last, "duration"), ==,
      40 * GST_MSECOND);
  g_assert_cmpint (json_object_get_int_member (last, "running_time"), ==,
      GST_SECOND);
  /* The element has no clock. */
  g_assert_true (json_object_get_null_member (last, "delay"));
  g_assert_cmpint (json_object_get_int_member (last, "size"), ==, 100);
  g_assert_cmpint (json_object_get_int_member (last, "offset"), ==, 5);
  g_assert_true (json_object_get_null_member (last, "offset_end"));
  g_assert_true (has_flag (last, "discont"));
  g_assert_true (has_flag (last, "delta-unit"));
  g_assert_false (has_flag (last, "header"));
  segment = json_object_get_object_member (stats, "segment");
  g_assert_cmpstr (json_object_get_string_member (segment, "format"), ==,
      "time");
  g_assert_cmpfloat (json_object_get_double_member (segment, "rate"), ==, 1);
  g_assert_true (json_object_get_null_member (segment, "stop"));
  events = json_object_get_object_member (stats, "events");
  g_assert_cmpint (json_object_get_int_member (events, "segment"), ==, 1);
  g_assert_cmpint (json_object_get_int_member (events, "caps"), ==, 1);
  g_assert_cmpint (json_object_get_int_member (events, "eos"), ==, 0);
  /* No tick yet. */
  g_assert_true (json_object_get_null_member (stats, "window"));
  json_node_unref (root);

  /* A list counts all its buffers; the last one is shown. */
  list = gst_buffer_list_new ();
  for (i = 0; i < 3; i++)
    gst_buffer_list_add (list, new_buffer (10, 2 * GST_SECOND + i));
  g_assert_cmpint (gst_pad_push_list (f->pad, list), ==, GST_FLOW_OK);
  g_assert_true (gst_pad_push_event (f->pad, gst_event_new_flush_start ()));
  g_assert_true (gst_pad_push_event (f->pad,
          gst_event_new_flush_stop (FALSE)));
  g_assert_true (gst_pad_push_event (f->pad, gst_event_new_eos ()));

  stats = stats_of (f, &root);
  g_assert_cmpint (json_object_get_int_member (stats, "buffers"), ==, 4);
  g_assert_cmpint (json_object_get_int_member (stats, "bytes"), ==, 130);
  last = json_object_get_object_member (stats, "last");
  g_assert_cmpint (json_object_get_int_member (last, "pts"), ==,
      2 * GST_SECOND + 2);
  events = json_object_get_object_member (stats, "events");
  g_assert_cmpint (json_object_get_int_member (events, "flush"), ==, 1);
  g_assert_cmpint (json_object_get_int_member (events, "eos"), ==, 1);
  json_node_unref (root);
}

/* Watching starts mid-stream: the segment sent before is used. */
static void
test_monitor_sticky_segment (Fixture * f, gconstpointer data)
{
  JsonNode *root;
  JsonObject *stats;

  link_sink (f);
  push_start (f, 5 * GST_SECOND);
  g_assert_true (pm_monitor_add (f->monitor, CLIENT, pad_id (f->model)));
  g_assert_cmpint (gst_pad_push (f->pad, new_buffer (1, GST_SECOND)), ==,
      GST_FLOW_OK);

  stats = stats_of (f, &root);
  g_assert_cmpint (json_object_get_int_member (json_object_get_object_member
          (stats, "segment"), "base"), ==, 5 * GST_SECOND);
  g_assert_cmpint (json_object_get_int_member (json_object_get_object_member
          (stats, "last"), "running_time"), ==, 6 * GST_SECOND);
  g_assert_cmpint (json_object_get_int_member (json_object_get_object_member
          (stats, "events"), "segment"), ==, 0);
  json_node_unref (root);
}

static void
test_monitor_delay (Fixture * f, gconstpointer data)
{
  GstClock *clock = gst_system_clock_obtain ();
  JsonNode *root;
  JsonObject *stats;
  gint64 delay;

  link_sink (f);
  gst_element_set_clock (f->element, clock);
  /* The pipeline started 10 s ago; a buffer for running time 0 is late. */
  gst_element_set_base_time (f->element, gst_clock_get_time (clock) -
      10 * GST_SECOND);
  g_assert_true (pm_monitor_add (f->monitor, CLIENT, pad_id (f->model)));
  push_start (f, 0);
  g_assert_cmpint (gst_pad_push (f->pad, new_buffer (1, 0)), ==,
      GST_FLOW_OK);

  stats = stats_of (f, &root);
  delay = json_object_get_int_member (json_object_get_object_member (stats,
          "last"), "delay");
  g_assert_cmpint (delay, >=, 10 * GST_SECOND);
  g_assert_cmpint (delay, <, 11 * GST_SECOND);
  json_node_unref (root);
  gst_element_set_clock (f->element, NULL);
  gst_object_unref (clock);
}

static void
test_monitor_window (Fixture * f, gconstpointer data)
{
  JsonNode *root;
  JsonObject *window;
  gint64 gap_min, gap_avg, gap_max;
  gint i;

  link_sink (f);
  g_assert_true (pm_monitor_add (f->monitor, CLIENT, pad_id (f->model)));
  push_start (f, 0);
  for (i = 0; i < 10; i++) {
    g_assert_cmpint (gst_pad_push (f->pad, new_buffer (50, i)), ==,
        GST_FLOW_OK);
    g_usleep (2 * G_TIME_SPAN_MILLISECOND);
  }
  pm_monitor_tick (f->monitor);

  window = json_object_get_object_member (stats_of (f, &root), "window");
  g_assert_cmpfloat (json_object_get_double_member (window, "buffers_per_s"),
      >, 0);
  g_assert_cmpfloat (json_object_get_double_member (window, "bytes_per_s"),
      >, 0);
  gap_min = json_object_get_int_member (window, "gap_min");
  gap_avg = json_object_get_int_member (window, "gap_avg");
  gap_max = json_object_get_int_member (window, "gap_max");
  g_assert_cmpint (gap_min, >=, 2 * GST_MSECOND);
  g_assert_cmpint (gap_min, <=, gap_avg);
  g_assert_cmpint (gap_avg, <=, gap_max);
  json_node_unref (root);

  /* Quiet slots keep the earlier ones in the window until they drop out. */
  for (i = 0; i < 3; i++)
    pm_monitor_tick (f->monitor);
  window = json_object_get_object_member (stats_of (f, &root), "window");
  g_assert_cmpfloat (json_object_get_double_member (window, "buffers_per_s"),
      >, 0);
  json_node_unref (root);
  pm_monitor_tick (f->monitor);
  window = json_object_get_object_member (stats_of (f, &root), "window");
  g_assert_cmpfloat (json_object_get_double_member (window, "buffers_per_s"),
      ==, 0);
  g_assert_true (json_object_get_null_member (window, "gap_avg"));
  json_node_unref (root);
}

#define OVERHEAD_BUFFERS 100000
#define OVERHEAD_LIMIT_NS 5000

static GstClockTime
push_many (Fixture * f)
{
  GstClockTime start = gst_util_get_timestamp ();
  gint i;

  for (i = 0; i < OVERHEAD_BUFFERS; i++)
    gst_pad_push (f->pad, new_buffer (0, i));
  return gst_util_get_timestamp () - start;
}

/* What a watched pad costs per buffer. Best of three runs each way. */
static void
test_monitor_overhead (Fixture * f, gconstpointer data)
{
  GstClockTime plain = GST_CLOCK_TIME_NONE, watched = GST_CLOCK_TIME_NONE;
  guint64 id;
  gdouble cost;
  gint run;

  link_sink (f);
  id = pad_id (f->model);
  push_start (f, 0);
  for (run = 0; run < 3; run++) {
    plain = MIN (plain, push_many (f));
    g_assert_true (pm_monitor_add (f->monitor, CLIENT, id));
    watched = MIN (watched, push_many (f));
    pm_monitor_remove (f->monitor, CLIENT, id);
  }
  cost = ((gdouble) watched - (gdouble) plain) / OVERHEAD_BUFFERS;
  g_test_message ("probe overhead: %.0f ns per buffer (%.0f ns without)",
      cost, (gdouble) plain / OVERHEAD_BUFFERS);
#ifndef PM_SANITIZED
  g_assert_cmpfloat (cost, <, OVERHEAD_LIMIT_NS);
#endif
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  gst_init (&argc, &argv);

  g_test_add ("/core/get-pad", Fixture, NULL, fixture_setup, test_get_pad,
      fixture_teardown);
  g_test_add ("/core/removed-pad", Fixture, NULL, fixture_setup,
      test_removed_pad, fixture_teardown);
  g_test_add ("/core/destroyed-pad", Fixture, NULL, fixture_setup,
      test_destroyed_pad, fixture_teardown);
  g_test_add ("/core/removed-element", Fixture, NULL, fixture_setup,
      test_removed_element, fixture_teardown);
  g_test_add ("/core/monitor-values", Fixture, NULL, fixture_setup,
      test_monitor_values, fixture_teardown);
  g_test_add ("/core/monitor-sticky-segment", Fixture, NULL, fixture_setup,
      test_monitor_sticky_segment, fixture_teardown);
  g_test_add ("/core/monitor-delay", Fixture, NULL, fixture_setup,
      test_monitor_delay, fixture_teardown);
  g_test_add ("/core/monitor-window", Fixture, NULL, fixture_setup,
      test_monitor_window, fixture_teardown);
  g_test_add ("/core/monitor-overhead", Fixture, NULL, fixture_setup,
      test_monitor_overhead, fixture_teardown);

  return g_test_run ();
}
