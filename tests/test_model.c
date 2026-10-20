/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <gst/gst.h>
#include <gst/gsttracer.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <string.h>

#define EMPTY_GRAPH "{\"pipelines\":[],\"links\":[]}"

static SoupSession *session;
static gchar *graph_url;
static gchar *log_url;

static gchar *
http_get (const gchar * url)
{
  SoupMessage *msg = soup_message_new ("GET", url);
  GError *error = NULL;
  GBytes *body;
  gchar *text;

  body = soup_session_send_and_read (session, msg, NULL, &error);
  g_assert_no_error (error);
  g_assert_cmpuint (soup_message_get_status (msg), ==, 200);
  text = g_strndup (g_bytes_get_data (body, NULL), g_bytes_get_size (body));

  g_bytes_unref (body);
  g_object_unref (msg);
  return text;
}

static gchar *
get_graph (void)
{
  return http_get (graph_url);
}

static gchar *
tracer_url (void)
{
  GList *tracers, *l;
  gchar *url = NULL;

  tracers = gst_tracing_get_active_tracers ();
  for (l = tracers; l != NULL; l = l->next) {
    if (g_strcmp0 (G_OBJECT_TYPE_NAME (l->data),
            "GstPipelineMonitorTracer") == 0)
      g_object_get (l->data, "url", &url, NULL);
  }
  g_list_free_full (tracers, gst_object_unref);
  g_assert_nonnull (url);
  return url;
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

  g_hash_table_add (set, g_strdup_printf ("E %s %s %s/%s", path,
          json_object_get_string_member (element, "type"),
          json_object_get_string_member (element, "state"),
          json_object_get_string_member_with_default (element, "pending",
              "VOID_PENDING")));

  for (i = 0; i < json_array_get_length (pads); i++) {
    JsonObject *pad = json_array_get_object_element (pads, i);
    gchar *pad_path = g_strdup_printf ("%s:%s", path,
        json_object_get_string_member (pad, "name"));

    g_hash_table_add (set, g_strdup_printf ("P %s %s%s caps=%s", pad_path,
            json_object_get_string_member (pad, "direction"),
            json_object_get_boolean_member (pad, "ghost") ? " ghost" : "",
            json_object_get_string_member_with_default (pad, "caps", "-")));
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
  GstCaps *caps = gst_pad_get_current_caps (pad);
  gchar *caps_text = caps ? gst_caps_to_string (caps) : g_strdup ("-");

  g_hash_table_add (set, g_strdup_printf ("P %s %s%s caps=%s", path,
          GST_PAD_IS_SRC (pad) ? "src" : "sink",
          GST_IS_GHOST_PAD (pad) ? " ghost" : "", caps_text));
  g_free (caps_text);
  if (caps != NULL)
    gst_caps_unref (caps);
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

  g_hash_table_add (set, g_strdup_printf ("E %s %s %s/%s", path,
          G_OBJECT_TYPE_NAME (element),
          gst_element_state_get_name (GST_STATE (element)),
          gst_element_state_get_name (GST_STATE_PENDING (element))));
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
assert_pipelines_match (GstElement ** pipelines, guint n_pipelines)
{
  gchar *actual = NULL, *expected = NULL;
  gint tries;
  guint i;

  for (tries = 0; tries < 200; tries++) {
    GHashTable *set = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
        NULL);
    gchar *graph = get_graph ();

    g_free (actual);
    g_free (expected);
    actual = describe_json (graph);
    for (i = 0; i < n_pipelines; i++)
      describe_element (pipelines[i], set);
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
assert_graph_matches (GstElement * pipeline)
{
  assert_pipelines_match (&pipeline, 1);
}

/* The last reference to a pipeline can be dropped by a GStreamer thread
 * a moment after the test releases its own. */
static void
assert_graph_empty (void)
{
  gchar *graph = NULL;
  gint tries;

  for (tries = 0; tries < 200; tries++) {
    g_free (graph);
    graph = get_graph ();
    if (g_strcmp0 (graph, EMPTY_GRAPH) == 0)
      break;
    g_usleep (10 * G_TIME_SPAN_MILLISECOND);
  }
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
  if (!have_elements ("audiotestsrc", "wavenc", "decodebin3", NULL))
    return;
  check_pipeline ("audiotestsrc is-live=true ! wavenc ! decodebin3 ! "
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

/* Every pipeline in the process is reported; loose bins are not. */
static void
test_two_pipelines (void)
{
  GError *error = NULL;
  GstElement *pipelines[2], *loose;

  assert_graph_empty ();

  pipelines[0] = gst_parse_launch ("fakesrc ! queue ! fakesink", &error);
  g_assert_no_error (error);
  pipelines[1] = gst_parse_launch ("fakesrc is-live=true ! identity ! "
      "fakesink async=false", &error);
  g_assert_no_error (error);
  loose = gst_bin_new ("loose");
  gst_bin_add (GST_BIN (loose), gst_element_factory_make ("identity", NULL));

  set_state_and_wait (pipelines[0], GST_STATE_PLAYING);
  set_state_and_wait (pipelines[1], GST_STATE_PAUSED);
  assert_pipelines_match (pipelines, 2);

  set_state_and_wait (pipelines[0], GST_STATE_NULL);
  gst_object_unref (pipelines[0]);
  assert_pipelines_match (&pipelines[1], 1);

  set_state_and_wait (pipelines[1], GST_STATE_NULL);
  gst_object_unref (pipelines[1]);
  gst_object_unref (loose);
  assert_graph_empty ();
}

/* Caps checks */

static gboolean
graph_has (const gchar * needle)
{
  gchar *graph = get_graph ();
  gboolean found = strstr (graph, needle) != NULL;

  g_free (graph);
  return found;
}

/* Waits up to 2 s for the graph to (not) contain needle. */
static gboolean
wait_graph_has (const gchar * needle, gboolean expected)
{
  gint tries;

  for (tries = 0; tries < 200; tries++) {
    if (graph_has (needle) == expected)
      return TRUE;
    g_usleep (10 * G_TIME_SPAN_MILLISECOND);
  }
  return FALSE;
}

static void
test_caps (void)
{
  GError *error = NULL;
  GstElement *pipeline, *filter;
  GstCaps *caps;

  assert_graph_empty ();
  pipeline = gst_parse_launch ("videotestsrc is-live=true ! "
      "capsfilter name=cf caps=video/x-raw,format=I420,width=320,height=240,"
      "framerate=30/1 ! fakesink", &error);
  g_assert_no_error (error);
  filter = gst_bin_get_by_name (GST_BIN (pipeline), "cf");

  set_state_and_wait (pipeline, GST_STATE_PLAYING);
  assert_graph_matches (pipeline);
  g_assert_true (wait_graph_has ("width=(int)320", TRUE));

  /* Renegotiation while playing. */
  caps = gst_caps_from_string ("video/x-raw,format=I420,width=640,height=480,"
      "framerate=30/1");
  g_object_set (filter, "caps", caps, NULL);
  gst_caps_unref (caps);
  g_assert_true (wait_graph_has ("width=(int)640", TRUE));
  assert_graph_matches (pipeline);

  /* Going back to READY clears the pads' caps. */
  set_state_and_wait (pipeline, GST_STATE_READY);
  g_assert_true (wait_graph_has ("\"caps\"", FALSE));
  assert_graph_matches (pipeline);

  set_state_and_wait (pipeline, GST_STATE_NULL);
  gst_object_unref (filter);
  gst_object_unref (pipeline);
  assert_graph_empty ();
}

/* Log checks */

static JsonParser *
get_log (JsonArray ** entries)
{
  JsonParser *parser = json_parser_new ();
  gchar *json = http_get (log_url);
  GError *error = NULL;

  json_parser_load_from_data (parser, json, -1, &error);
  g_assert_no_error (error);
  g_free (json);
  *entries = json_object_get_array_member (json_node_get_object
      (json_parser_get_root (parser)), "entries");
  return parser;
}

/* Entries whose element path ends with "/name". */
static GPtrArray *
log_entries_of (JsonArray * entries, const gchar * name)
{
  GPtrArray *found = g_ptr_array_new ();
  gchar *suffix = g_strconcat ("/", name, NULL);
  guint i;

  for (i = 0; i < json_array_get_length (entries); i++) {
    JsonObject *entry = json_array_get_object_element (entries, i);

    if (g_str_has_suffix (json_object_get_string_member (entry, "path"),
            suffix))
      g_ptr_array_add (found, entry);
  }
  g_free (suffix);
  return found;
}

static gint64
find_count (JsonObject * element, const gchar * name, const gchar * field)
{
  JsonArray *children = json_object_get_array_member (element, "children");
  guint i;

  if (g_strcmp0 (json_object_get_string_member (element, "name"), name) == 0)
    return json_object_get_int_member_with_default (element, field, 0);
  for (i = 0; i < json_array_get_length (children); i++) {
    gint64 n = find_count (json_array_get_object_element (children, i), name,
        field);
    if (n >= 0)
      return n;
  }
  return -1;
}

/* The error or warning count the graph shows for an element. */
static gint64
graph_count (const gchar * name, const gchar * field)
{
  JsonParser *parser = json_parser_new ();
  gchar *graph = get_graph ();
  JsonArray *pipelines;
  gint64 n = -1;
  guint i;

  g_assert_true (json_parser_load_from_data (parser, graph, -1, NULL));
  pipelines = json_object_get_array_member (json_node_get_object
      (json_parser_get_root (parser)), "pipelines");
  for (i = 0; i < json_array_get_length (pipelines) && n < 0; i++)
    n = find_count (json_array_get_object_element (pipelines, i), name, field);
  g_object_unref (parser);
  g_free (graph);
  return n;
}

static void
post (GstElement * element, GstMessageType type, const gchar * text)
{
  GError *error = g_error_new_literal (GST_CORE_ERROR, GST_CORE_ERROR_FAILED,
      text);
  GstMessage *msg = type == GST_MESSAGE_ERROR ?
      gst_message_new_error (GST_OBJECT (element), error, "some detail") :
      gst_message_new_warning (GST_OBJECT (element), error, "some detail");

  gst_element_post_message (element, msg);
  g_error_free (error);
}

static void
test_log_messages (void)
{
  GstElement *pipeline = gst_pipeline_new (NULL);
  GstElement *outer = gst_bin_new ("log-outer");
  GstElement *inner = gst_bin_new ("log-inner");
  GstElement *element = gst_element_factory_make ("identity", "log-msg");
  GPtrArray *found;
  JsonParser *parser;
  JsonArray *entries;
  JsonObject *entry;

  gst_bin_add (GST_BIN (inner), element);
  gst_bin_add (GST_BIN (outer), inner);
  gst_bin_add (GST_BIN (pipeline), outer);
  post (element, GST_MESSAGE_ERROR, "it broke");
  post (element, GST_MESSAGE_WARNING, "it wobbled");

  parser = get_log (&entries);
  found = log_entries_of (entries, "log-msg");
  g_assert_cmpuint (found->len, ==, 2);
  entry = g_ptr_array_index (found, 0);
  g_assert_cmpstr (json_object_get_string_member (entry, "level"), ==,
      "error");
  g_assert_cmpstr (json_object_get_string_member (entry, "source"), ==,
      "message");
  g_assert_true (g_str_has_suffix (json_object_get_string_member (entry,
              "path"), "/log-outer/log-inner/log-msg"));
  g_assert_cmpstr (json_object_get_string_member (entry, "text"), ==,
      "it broke");
  g_assert_cmpstr (json_object_get_string_member (entry, "debug"), ==,
      "some detail");
  g_assert_cmpstr (json_object_get_string_member (entry, "domain"), ==,
      g_quark_to_string (GST_CORE_ERROR));
  g_assert_cmpint (json_object_get_int_member (entry, "code"), ==,
      GST_CORE_ERROR_FAILED);
  entry = g_ptr_array_index (found, 1);
  g_assert_cmpstr (json_object_get_string_member (entry, "level"), ==,
      "warning");
  g_ptr_array_unref (found);
  g_object_unref (parser);

  g_assert_cmpint (graph_count ("log-msg", "errors"), ==, 1);
  g_assert_cmpint (graph_count ("log-msg", "warnings"), ==, 1);
  g_assert_cmpint (graph_count ("log-inner", "errors"), ==, 0);

  gst_object_unref (pipeline);
}

static void
test_log_repeats (void)
{
  GstElement *pipeline = gst_pipeline_new (NULL);
  GstElement *element = gst_element_factory_make ("identity", "log-repeat");
  GstBus *bus = gst_element_get_bus (pipeline);
  GPtrArray *found;
  JsonParser *parser;
  JsonArray *entries;
  gint i;

  gst_bus_set_flushing (bus, TRUE);
  gst_bin_add (GST_BIN (pipeline), element);
  for (i = 0; i < 10000; i++)
    post (element, GST_MESSAGE_WARNING, "same again");
  post (element, GST_MESSAGE_WARNING, "something else");

  parser = get_log (&entries);
  found = log_entries_of (entries, "log-repeat");
  g_assert_cmpuint (found->len, ==, 2);
  g_assert_cmpint (json_object_get_int_member (g_ptr_array_index (found, 0),
          "count"), ==, 10000);
  g_assert_cmpint (json_object_get_int_member (g_ptr_array_index (found, 1),
          "count"), ==, 1);
  g_ptr_array_unref (found);
  g_object_unref (parser);
  g_assert_cmpint (graph_count ("log-repeat", "warnings"), ==, 10001);

  gst_object_unref (bus);
  gst_object_unref (pipeline);
}

static void
test_log_state_change (void)
{
  GError *error = NULL;
  GstElement *pipeline = gst_parse_launch ("filesrc name=log-file "
      "location=/nonexistent/pipeline-monitor ! fakesink", &error);
  GPtrArray *found;
  JsonParser *parser;
  JsonArray *entries;
  JsonObject *entry;
  gchar *name = gst_element_get_name (pipeline);
  guint i;

  g_assert_no_error (error);
  g_assert_cmpint (gst_element_set_state (pipeline, GST_STATE_PAUSED), ==,
      GST_STATE_CHANGE_FAILURE);

  /* filesrc posts its own error; GstBaseSrc may add a generic one. */
  parser = get_log (&entries);
  found = log_entries_of (entries, "log-file");
  g_assert_cmpuint (found->len, >=, 2);
  entry = g_ptr_array_index (found, 0);
  g_assert_cmpstr (json_object_get_string_member (entry, "source"), ==,
      "message");
  g_assert_cmpstr (json_object_get_string_member (entry, "level"), ==,
      "error");
  g_assert_cmpint (json_object_get_int_member (entry, "code"), ==,
      GST_RESOURCE_ERROR_NOT_FOUND);
  entry = g_ptr_array_index (found, found->len - 1);
  g_assert_cmpstr (json_object_get_string_member (entry, "source"), ==,
      "state-change");
  g_assert_cmpstr (json_object_get_string_member (entry, "text"), ==,
      "state change from READY to PAUSED failed");
  g_ptr_array_unref (found);

  /* The pipeline fails because of filesrc and is not logged itself. */
  for (i = 0; i < json_array_get_length (entries); i++)
    g_assert_cmpstr (json_object_get_string_member
        (json_array_get_object_element (entries, i), "path"), !=, name);
  g_object_unref (parser);

  g_free (name);
  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (pipeline);
}

static void
test_log_link (void)
{
  GError *error = NULL;
  GstElement *pipeline = gst_parse_launch ("audiotestsrc name=log-asrc "
      "videoconvert name=log-vconv fakesrc name=log-src fakesink name=log-sink1 "
      "fakesink name=log-sink2", &error);
  GstPad *pads[5];
  const gchar *names[5][2] = {
    {"log-asrc", "src"}, {"log-vconv", "sink"}, {"log-src", "src"},
    {"log-sink1", "sink"}, {"log-sink2", "sink"}
  };
  GPtrArray *found;
  JsonParser *parser;
  JsonArray *entries;
  guint i;

  g_assert_no_error (error);
  for (i = 0; i < G_N_ELEMENTS (pads); i++) {
    GstElement *e = gst_bin_get_by_name (GST_BIN (pipeline), names[i][0]);
    pads[i] = gst_element_get_static_pad (e, names[i][1]);
    gst_object_unref (e);
  }

  g_assert_cmpint (gst_pad_link (pads[0], pads[1]), ==,
      GST_PAD_LINK_NOFORMAT);
  g_assert_cmpint (gst_pad_link (pads[2], pads[3]), ==, GST_PAD_LINK_OK);
  /* Linking again to the same peer is harmless; to another one is not. */
  g_assert_cmpint (gst_pad_link (pads[2], pads[3]), ==,
      GST_PAD_LINK_WAS_LINKED);
  g_assert_cmpint (gst_pad_link (pads[2], pads[4]), ==,
      GST_PAD_LINK_WAS_LINKED);

  parser = get_log (&entries);
  found = log_entries_of (entries, "log-asrc");
  g_assert_cmpuint (found->len, ==, 1);
  g_assert_cmpstr (json_object_get_string_member (g_ptr_array_index (found,
              0), "source"), ==, "link");
  g_assert_nonnull (strstr (json_object_get_string_member
          (g_ptr_array_index (found, 0), "text"), "no common format"));
  g_ptr_array_unref (found);
  found = log_entries_of (entries, "log-src");
  g_assert_cmpuint (found->len, ==, 1);
  g_assert_nonnull (strstr (json_object_get_string_member
          (g_ptr_array_index (found, 0), "text"), "log-sink2"));
  g_ptr_array_unref (found);
  g_object_unref (parser);

  for (i = 0; i < G_N_ELEMENTS (pads); i++)
    gst_object_unref (pads[i]);
  gst_object_unref (pipeline);
}

/* Runs last: it fills the whole log. */
static void
test_log_limit (void)
{
  GstElement *pipeline = gst_pipeline_new (NULL);
  GstElement *element = gst_element_factory_make ("identity", "log-limit");
  GstBus *bus = gst_element_get_bus (pipeline);
  JsonParser *parser;
  JsonArray *entries;
  gint i;

  gst_bus_set_flushing (bus, TRUE);
  gst_bin_add (GST_BIN (pipeline), element);
  for (i = 0; i < 600; i++) {
    gchar *text = g_strdup_printf ("warning %d", i);
    post (element, GST_MESSAGE_WARNING, text);
    g_free (text);
  }

  parser = get_log (&entries);
  g_assert_cmpuint (json_array_get_length (entries), ==, 500);
  g_assert_cmpstr (json_object_get_string_member
      (json_array_get_object_element (entries, 0), "text"), ==,
      "warning 100");
  g_object_unref (parser);
  g_assert_cmpint (graph_count ("log-limit", "warnings"), ==, 600);

  gst_object_unref (bus);
  gst_object_unref (pipeline);
}

int
main (int argc, char **argv)
{
  gchar *url;
  gint ret;

  g_test_init (&argc, &argv, NULL);
  gst_init (&argc, &argv);
  session = soup_session_new ();
  url = tracer_url ();
  graph_url = g_strconcat (url, "api/graph", NULL);
  log_url = g_strconcat (url, "api/log", NULL);
  g_free (url);

  g_test_add_func ("/model/chain", test_chain);
  g_test_add_func ("/model/nested-bins", test_nested_bins);
  g_test_add_func ("/model/request-pads", test_request_pads);
  g_test_add_func ("/model/sometimes-pads", test_sometimes_pads);
  g_test_add_func ("/model/json-escaping", test_json_escaping);
  g_test_add_func ("/model/dynamic-changes", test_dynamic_changes);
  g_test_add_func ("/model/two-pipelines", test_two_pipelines);
  g_test_add_func ("/model/caps", test_caps);
  g_test_add_func ("/log/messages", test_log_messages);
  g_test_add_func ("/log/repeats", test_log_repeats);
  g_test_add_func ("/log/state-change", test_log_state_change);
  g_test_add_func ("/log/link", test_log_link);
  g_test_add_func ("/log/limit", test_log_limit);

  ret = g_test_run ();
  g_free (graph_url);
  g_free (log_url);
  g_object_unref (session);
  return ret;
}
