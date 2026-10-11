/* SPDX-License-Identifier: MIT */
#include <string.h>
#include <gst/gst.h>
#include <gst/gsttracer.h>
#include <gst/gsttracerfactory.h>

static gint elements_seen;

static void
count_log (GstDebugCategory * category, GstDebugLevel level,
    const gchar * file, const gchar * function, gint line, GObject * object,
    GstDebugMessage * message, gpointer user_data)
{
  if (g_strcmp0 (gst_debug_category_get_name (category),
          "pipeline-monitor") != 0)
    return;
  if (strstr (gst_debug_message_get (message), "new element") != NULL)
    g_atomic_int_inc (&elements_seen);
}

static void
test_registered (void)
{
  GstPluginFeature *feature;

  feature = gst_registry_find_feature (gst_registry_get (), "pipeline-monitor",
      GST_TYPE_TRACER_FACTORY);
  g_assert_nonnull (feature);
  gst_object_unref (feature);
}

static void
test_active (void)
{
  GList *tracers, *l;
  gboolean found = FALSE;

  tracers = gst_tracing_get_active_tracers ();
  for (l = tracers; l != NULL; l = l->next) {
    if (g_strcmp0 (G_OBJECT_TYPE_NAME (l->data),
            "GstPipelineMonitorTracer") == 0)
      found = TRUE;
  }
  g_list_free_full (tracers, gst_object_unref);
  g_assert_true (found);
}

static void
test_sees_elements (void)
{
  GstElement *pipeline;
  GstBus *bus;
  GstMessage *msg;
  GError *error = NULL;

  g_atomic_int_set (&elements_seen, 0);

  pipeline = gst_parse_launch ("videotestsrc num-buffers=10 ! fakesink",
      &error);
  g_assert_no_error (error);
  g_assert_nonnull (pipeline);

  gst_element_set_state (pipeline, GST_STATE_PLAYING);
  bus = gst_element_get_bus (pipeline);
  msg = gst_bus_timed_pop_filtered (bus, 30 * GST_SECOND,
      GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
  g_assert_nonnull (msg);
  g_assert_cmpint (GST_MESSAGE_TYPE (msg), ==, GST_MESSAGE_EOS);
  gst_message_unref (msg);
  gst_object_unref (bus);

  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (pipeline);

  /* pipeline, videotestsrc and fakesink */
  g_assert_cmpint (g_atomic_int_get (&elements_seen), >=, 3);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  gst_init (&argc, &argv);
  gst_debug_set_threshold_for_name ("pipeline-monitor", GST_LEVEL_DEBUG);
  gst_debug_add_log_function (count_log, NULL, NULL);

  g_test_add_func ("/load/registered", test_registered);
  g_test_add_func ("/load/active", test_active);
  g_test_add_func ("/load/sees-elements", test_sees_elements);

  return g_test_run ();
}
