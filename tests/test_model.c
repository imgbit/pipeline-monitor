/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <gst/gst.h>
#include <gst/gsttracer.h>
#include <json-glib/json-glib.h>

#define EMPTY_GRAPH "{\"pipelines\":[],\"links\":[]}"

static gchar *
get_graph (void)
{
  GList *tracers, *l;
  gchar *graph = NULL;

  tracers = gst_tracing_get_active_tracers ();
  for (l = tracers; l != NULL; l = l->next) {
    if (g_strcmp0 (G_OBJECT_TYPE_NAME (l->data),
            "GstPipelineMonitorTracer") == 0)
      g_object_get (l->data, "graph", &graph, NULL);
  }
  g_list_free_full (tracers, gst_object_unref);
  g_assert_nonnull (graph);

  return graph;
}

static gchar *
join_sorted (GHashTable * set)
{
  GList *keys = g_list_sort (g_hash_table_get_keys (set),
      (GCompareFunc) g_strcmp0);
  GString *out = g_string_new (NULL);
  GList *l;

  for (l = keys; l != NULL; l = l->next)
    g_string_append_printf (out, "%s\n", (gchar *) l->data);
  g_list_free (keys);
  g_hash_table_unref (set);

  return g_string_free (out, FALSE);
}

/* Description of the graph as the tracer reports it. */

static void
describe_json_element (JsonObject * element, const gchar * parent_path,
    GHashTable * set, GHashTable * pad_paths)
{
  gchar *path = g_strdup_printf ("%s/%s", parent_path,
      json_object_get_string_member (element, "name"));
  JsonArray *pads = json_object_get_array_member (element, "pads");
  JsonArray *children = json_object_get_array_member (element, "children");
  guint i;

  g_hash_table_add (set, g_strdup_printf ("E %s %s %s", path,
          json_object_get_string_member (element, "type"),
          json_object_get_string_member (element, "state")));

  for (i = 0; i < json_array_get_length (pads); i++) {
    JsonObject *pad = json_array_get_object_element (pads, i);
    gchar *pad_path = g_strdup_printf ("%s:%s", path,
        json_object_get_string_member (pad, "name"));

    g_hash_table_add (set, g_strdup_printf ("P %s %s%s", pad_path,
            json_object_get_string_member (pad, "direction"),
            json_object_get_boolean_member (pad, "ghost") ? " ghost" : ""));
    g_hash_table_insert (pad_paths,
        GSIZE_TO_POINTER (json_object_get_int_member (pad, "id")), pad_path);
  }

  for (i = 0; i < json_array_get_length (children); i++)
    describe_json_element (json_array_get_object_element (children, i), path,
        set, pad_paths);

  g_free (path);
}

static gchar *
describe_json (const gchar * graph)
{
  GHashTable *set = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
      NULL);
  GHashTable *pad_paths = g_hash_table_new_full (NULL, NULL, NULL, g_free);
  JsonParser *parser = json_parser_new ();
  GError *error = NULL;
  JsonObject *root;
  JsonArray *array;
  guint i;

  json_parser_load_from_data (parser, graph, -1, &error);
  g_assert_no_error (error);
  root = json_node_get_object (json_parser_get_root (parser));

  array = json_object_get_array_member (root, "pipelines");
  for (i = 0; i < json_array_get_length (array); i++)
    describe_json_element (json_array_get_object_element (array, i), "", set,
        pad_paths);

  array = json_object_get_array_member (root, "links");
  for (i = 0; i < json_array_get_length (array); i++) {
    JsonObject *link = json_array_get_object_element (array, i);
    const gchar *src = g_hash_table_lookup (pad_paths,
        GSIZE_TO_POINTER (json_object_get_int_member (link, "src")));
    const gchar *sink = g_hash_table_lookup (pad_paths,
        GSIZE_TO_POINTER (json_object_get_int_member (link, "sink")));

    g_assert_nonnull (src);
    g_assert_nonnull (sink);
    g_hash_table_add (set, g_strdup_printf ("L %s -> %s", src, sink));
  }

  g_hash_table_unref (pad_paths);
  g_object_unref (parser);

  return join_sorted (set);
}

/* Description of the graph as GStreamer reports it. */

static gchar *
element_path (GstElement * element)
{
  GstObject *parent = GST_OBJECT_PARENT (element);
  gchar *parent_path, *path;

  parent_path = parent ? element_path (GST_ELEMENT (parent)) : g_strdup ("");
  path = g_strdup_printf ("%s/%s", parent_path, GST_OBJECT_NAME (element));
  g_free (parent_path);

  return path;
}

static gchar *
pad_path (GstPad * pad)
{
  GstElement *element = gst_pad_get_parent_element (pad);
  gchar *path, *res;

  g_assert_nonnull (element);
  path = element_path (element);
  res = g_strdup_printf ("%s:%s", path, GST_OBJECT_NAME (pad));
  g_free (path);
  gst_object_unref (element);

  return res;
}

static void
add_link (GHashTable * set, GstPad * src, GstPad * sink)
{
  gchar *src_path = pad_path (src);
  gchar *sink_path = pad_path (sink);

  g_hash_table_add (set, g_strdup_printf ("L %s -> %s", src_path, sink_path));
  g_free (src_path);
  g_free (sink_path);
}

static void
describe_pad (GstPad * pad, GHashTable * set)
{
  gchar *path = pad_path (pad);
  GstPad *peer = gst_pad_get_peer (pad);

  g_hash_table_add (set, g_strdup_printf ("P %s %s%s", path,
          GST_PAD_IS_SRC (pad) ? "src" : "sink",
          GST_IS_GHOST_PAD (pad) ? " ghost" : ""));
  g_free (path);

  if (peer != NULL && GST_PAD_IS_SRC (pad)) {
    if (GST_IS_PROXY_PAD (peer) && !GST_IS_GHOST_PAD (peer)) {
      GstProxyPad *ghost = gst_proxy_pad_get_internal (GST_PROXY_PAD (peer));
      add_link (set, pad, GST_PAD (ghost));
      gst_object_unref (ghost);
    } else {
      add_link (set, pad, peer);
    }
  }
  g_clear_object (&peer);

  if (GST_IS_GHOST_PAD (pad)) {
    GstPad *target = gst_ghost_pad_get_target (GST_GHOST_PAD (pad));
    if (target != NULL) {
      if (GST_PAD_IS_SRC (pad))
        add_link (set, target, pad);
      else
        add_link (set, pad, target);
      gst_object_unref (target);
    }
  }
}

static void
describe_element (GstElement * element, GHashTable * set)
{
  gchar *path = element_path (element);
  GstIterator *it;
  GValue item = G_VALUE_INIT;

  g_hash_table_add (set, g_strdup_printf ("E %s %s %s", path,
          G_OBJECT_TYPE_NAME (element),
          gst_element_state_get_name (GST_STATE (element))));
  g_free (path);

  it = gst_element_iterate_pads (element);
  while (gst_iterator_next (it, &item) == GST_ITERATOR_OK) {
    describe_pad (g_value_get_object (&item), set);
    g_value_reset (&item);
  }
  g_value_unset (&item);
  gst_iterator_free (it);

  if (GST_IS_BIN (element)) {
    it = gst_bin_iterate_elements (GST_BIN (element));
    while (gst_iterator_next (it, &item) == GST_ITERATOR_OK) {
      describe_element (g_value_get_object (&item), set);
      g_value_reset (&item);
    }
    g_value_unset (&item);
    gst_iterator_free (it);
  }
}

/* The tracer learns about a state change just after it happened, so allow
 * the two descriptions a moment to agree. */
static void
assert_graph_matches (GstElement * pipeline)
{
  gchar *actual = NULL, *expected = NULL;
  gint tries;

  for (tries = 0; tries < 200; tries++) {
    GHashTable *set = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
        NULL);
    gchar *graph = get_graph ();

    g_free (actual);
    g_free (expected);
    actual = describe_json (graph);
    describe_element (pipeline, set);
    expected = join_sorted (set);
    g_free (graph);

    if (g_strcmp0 (actual, expected) == 0)
      break;
    g_usleep (10 * G_TIME_SPAN_MILLISECOND);
  }

  g_assert_cmpstr (actual, ==, expected);

  g_free (expected);
  g_free (actual);
}

static void
assert_graph_empty (void)
{
  gchar *graph = get_graph ();

  g_assert_cmpstr (graph, ==, EMPTY_GRAPH);
  g_free (graph);
}

static void
set_state_and_wait (GstElement * pipeline, GstState state)
{
  g_assert_cmpint (gst_element_set_state (pipeline, state), !=,
      GST_STATE_CHANGE_FAILURE);
  g_assert_cmpint (gst_element_get_state (pipeline, NULL, NULL,
          30 * GST_SECOND), !=, GST_STATE_CHANGE_FAILURE);
}

static void
check_pipeline (const gchar * description)
{
  GError *error = NULL;
  GstElement *pipeline;

  assert_graph_empty ();

  pipeline = gst_parse_launch (description, &error);
  g_assert_no_error (error);
  assert_graph_matches (pipeline);

  set_state_and_wait (pipeline, GST_STATE_PAUSED);
  assert_graph_matches (pipeline);
  set_state_and_wait (pipeline, GST_STATE_PLAYING);
  assert_graph_matches (pipeline);
  set_state_and_wait (pipeline, GST_STATE_NULL);
  assert_graph_matches (pipeline);

  gst_object_unref (pipeline);
  assert_graph_empty ();
}

static gboolean
have_elements (const gchar * first, ...)
{
  const gchar *name;
  gboolean res = TRUE;
  va_list args;

  va_start (args, first);
  for (name = first; name != NULL; name = va_arg (args, const gchar *)) {
    GstElementFactory *factory = gst_element_factory_find (name);
    if (factory == NULL)
      res = FALSE;
    else
      gst_object_unref (factory);
  }
  va_end (args);

  if (!res)
    g_test_skip ("required elements are not available");
  return res;
}

static void
test_chain (void)
{
  check_pipeline ("fakesrc is-live=true ! queue ! fakesink");
}

static void
test_nested_bins (void)
{
  check_pipeline ("fakesrc is-live=true ! "
      "( queue ! ( identity ! identity ) ! queue ) ! fakesink");
}

static void
test_request_pads (void)
{
  check_pipeline ("fakesrc is-live=true ! tee name=t "
      "t. ! queue ! fakesink t. ! queue ! fakesink");
}

static void
test_sometimes_pads (void)
{
  if (!have_elements ("videotestsrc", "jpegenc", "decodebin3", NULL))
    return;
  check_pipeline ("videotestsrc is-live=true ! jpegenc ! decodebin3 ! "
      "fakesink");
}

static void
test_json_escaping (void)
{
  GstElement *pipeline = gst_pipeline_new ("quote\" backslash\\ tab\t");
  GstElement *sink = gst_element_factory_make ("fakesink", "new\nline");

  gst_bin_add (GST_BIN (pipeline), sink);
  assert_graph_matches (pipeline);

  gst_object_unref (pipeline);
  assert_graph_empty ();
}

static void
test_dynamic_changes (void)
{
  GError *error = NULL;
  GstElement *pipeline, *tee, *queue, *sink;
  GstPad *tee_pad, *queue_pad;

  assert_graph_empty ();

  pipeline = gst_parse_launch ("fakesrc is-live=true ! tee name=t ! queue ! "
      "fakesink", &error);
  g_assert_no_error (error);
  set_state_and_wait (pipeline, GST_STATE_PLAYING);
  assert_graph_matches (pipeline);

  tee = gst_bin_get_by_name (GST_BIN (pipeline), "t");
  queue = gst_element_factory_make ("queue", "extra-queue");
  sink = gst_element_factory_make ("fakesink", "extra-sink");
  g_object_set (sink, "async", FALSE, NULL);
  gst_bin_add_many (GST_BIN (pipeline), queue, sink, NULL);
  assert_graph_matches (pipeline);

  g_assert_true (gst_element_link (queue, sink));
  tee_pad = gst_element_request_pad_simple (tee, "src_%u");
  queue_pad = gst_element_get_static_pad (queue, "sink");
  g_assert_cmpint (gst_pad_link (tee_pad, queue_pad), ==, GST_PAD_LINK_OK);
  g_assert_true (gst_element_sync_state_with_parent (sink));
  g_assert_true (gst_element_sync_state_with_parent (queue));
  g_assert_cmpint (gst_element_get_state (pipeline, NULL, NULL,
          5 * GST_SECOND), ==, GST_STATE_CHANGE_SUCCESS);
  assert_graph_matches (pipeline);

  g_assert_true (gst_pad_unlink (tee_pad, queue_pad));
  assert_graph_matches (pipeline);

  gst_element_release_request_pad (tee, tee_pad);
  assert_graph_matches (pipeline);

  gst_element_set_state (queue, GST_STATE_NULL);
  gst_element_set_state (sink, GST_STATE_NULL);
  gst_bin_remove_many (GST_BIN (pipeline), queue, sink, NULL);
  assert_graph_matches (pipeline);

  gst_object_unref (queue_pad);
  gst_object_unref (tee_pad);
  gst_object_unref (tee);

  set_state_and_wait (pipeline, GST_STATE_NULL);
  gst_object_unref (pipeline);
  assert_graph_empty ();
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  gst_init (&argc, &argv);

  g_test_add_func ("/model/chain", test_chain);
  g_test_add_func ("/model/nested-bins", test_nested_bins);
  g_test_add_func ("/model/request-pads", test_request_pads);
  g_test_add_func ("/model/sometimes-pads", test_sometimes_pads);
  g_test_add_func ("/model/json-escaping", test_json_escaping);
  g_test_add_func ("/model/dynamic-changes", test_dynamic_changes);

  return g_test_run ();
}
