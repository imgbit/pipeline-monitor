/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pmlevels.h"
#include "pmjson.h"

/* Queue levels and pipeline times, read from the elements' properties and
 * queries by the server every 250 ms. */

/* The queue types are in the core elements plugin, without public headers;
 * subclasses count too. */
static const gchar *
queue_kind (GstElement * element)
{
  static const struct
  {
    const gchar *type;
    const gchar *kind;
  } kinds[] = {
    {"GstQueue", "queue"},
    {"GstQueue2", "queue2"},
    {"GstMultiQueue", "multiqueue"},
  };
  guint i;

  for (i = 0; i < G_N_ELEMENTS (kinds); i++) {
    GType type = g_type_from_name (kinds[i].type);

    if (type != 0 && g_type_is_a (G_OBJECT_TYPE (element), type))
      return kinds[i].kind;
  }
  return NULL;
}

gboolean
pm_levels_is_queue (GstElement * element)
{
  return queue_kind (element) != NULL;
}

static gboolean
queue_filter (GstElement * element, gboolean top_level)
{
  return pm_levels_is_queue (element);
}

static gboolean
pipeline_filter (GstElement * element, gboolean top_level)
{
  return top_level && GST_IS_PIPELINE (element);
}

static void
append_string (GString * json, const gchar * key, const gchar * value)
{
  g_string_append_printf (json, "\"%s\":", key);
  pm_json_append_string (json, value);
  g_string_append_c (json, ',');
}

static gboolean
has_property (gpointer object, const gchar * name)
{
  return g_object_class_find_property (G_OBJECT_GET_CLASS (object),
      name) != NULL;
}

/* One level: buffers, bytes and time, from a queue or a multiqueue pad. */
static void
append_level (GString * json, gpointer object, guint64 pad)
{
  guint buffers = 0, bytes = 0;
  guint64 time = 0;

  g_object_get (object, "current-level-buffers", &buffers,
      "current-level-bytes", &bytes, "current-level-time", &time, NULL);
  g_string_append_c (json, '{');
  if (pad != 0)
    pm_json_append_value (json, "pad", pad);
  pm_json_append_value (json, "buffers", buffers);
  pm_json_append_value (json, "bytes", bytes);
  pm_json_append_value (json, "time", time);
  pm_json_close (json, '}');
  g_string_append_c (json, ',');
}

static void
append_leaky (GString * json, GstElement * element)
{
  GParamSpec *pspec = g_object_class_find_property (G_OBJECT_GET_CLASS
      (element), "leaky");
  GEnumValue *value;
  gint leaky = 0;

  if (pspec == NULL || !G_IS_PARAM_SPEC_ENUM (pspec))
    return;
  g_object_get (element, "leaky", &leaky, NULL);
  value = g_enum_get_value (G_PARAM_SPEC_ENUM (pspec)->enum_class, leaky);
  append_string (json, "leaky", value ? value->value_nick : NULL);
}

static void
append_queue (GString * json, PmModel * model, PmElementRef * ref)
{
  GstElement *element = ref->element;
  guint max_buffers = 0, max_bytes = 0;
  guint64 max_time = 0;

  g_object_get (element, "max-size-buffers", &max_buffers,
      "max-size-bytes", &max_bytes, "max-size-time", &max_time, NULL);

  g_string_append_c (json, '{');
  pm_json_append_value (json, "element", ref->id);
  append_string (json, "path", ref->path);
  append_string (json, "kind", queue_kind (element));
  append_leaky (json, element);
  g_string_append (json, "\"limits\":{");
  pm_json_append_value (json, "buffers", max_buffers);
  pm_json_append_value (json, "bytes", max_bytes);
  pm_json_append_value (json, "time", max_time);
  pm_json_close (json, '}');
  g_string_append_c (json, ',');
  pm_json_append_value (json, "overruns", ref->overruns);
  pm_json_append_value (json, "underruns", ref->underruns);
  if (ref->buffering >= 0)
    pm_json_append_value (json, "buffering", ref->buffering);
  else
    g_string_append (json, "\"buffering\":null,");

  g_string_append (json, "\"levels\":[");
  if (has_property (element, "current-level-buffers")) {
    append_level (json, element, 0);
  } else {
    /* multiqueue: one queue per sink pad (levels on the pads since 1.18). */
    GstIterator *it = gst_element_iterate_sink_pads (element);
    GValue item = G_VALUE_INIT;

    while (gst_iterator_next (it, &item) == GST_ITERATOR_OK) {
      GstPad *pad = g_value_get_object (&item);

      if (has_property (pad, "current-level-buffers"))
        append_level (json, pad, pm_model_pad_id (model, pad));
      g_value_reset (&item);
    }
    g_value_unset (&item);
    gst_iterator_free (it);
  }
  pm_json_close (json, ']');
  pm_json_close (json, '}');
  g_string_append_c (json, ',');
}

static void
append_pipeline (GString * json, PmModel * model, PmElementRef * ref)
{
  GstElement *pipeline = ref->element;
  GstClock *clock = gst_element_get_clock (pipeline);
  GstElement *provider = NULL;
  gchar *provider_path = NULL;
  GstClockTime base = gst_element_get_base_time (pipeline);
  GstClockTime now = clock ? gst_clock_get_time (clock) : GST_CLOCK_TIME_NONE;
  gint64 position = -1, duration = -1;
  GstState state;
  GstQuery *query;

  GST_OBJECT_LOCK (pipeline);
  state = GST_STATE (pipeline);
  if (GST_BIN (pipeline)->clock_provider)
    provider = gst_object_ref (GST_BIN (pipeline)->clock_provider);
  GST_OBJECT_UNLOCK (pipeline);
  if (provider != NULL) {
    provider_path = pm_model_element_path (model, provider);
    gst_object_unref (provider);
  }

  g_string_append_c (json, '{');
  pm_json_append_value (json, "element", ref->id);
  append_string (json, "path", ref->path);
  append_string (json, "state", gst_element_state_get_name (state));
  append_string (json, "clock", clock ? GST_OBJECT_NAME (clock) : NULL);
  append_string (json, "clock_type", clock ? G_OBJECT_TYPE_NAME (clock) :
      NULL);
  append_string (json, "clock_provider", provider_path);
  pm_json_append_value (json, "clock_time", now);
  pm_json_append_value (json, "base_time", clock ? base : GST_CLOCK_TIME_NONE);
  pm_json_append_value (json, "running_time", state == GST_STATE_PLAYING &&
      GST_CLOCK_TIME_IS_VALID (now) && now >= base ? now - base :
      GST_CLOCK_TIME_NONE);

  if (state >= GST_STATE_PAUSED) {
    gst_element_query_position (pipeline, GST_FORMAT_TIME, &position);
    gst_element_query_duration (pipeline, GST_FORMAT_TIME, &duration);
  }
  pm_json_append_value (json, "position", position);
  pm_json_append_value (json, "duration", duration);

  query = gst_query_new_latency ();
  if (state >= GST_STATE_PAUSED && gst_element_query (pipeline, query)) {
    gboolean live;
    GstClockTime min, max;

    gst_query_parse_latency (query, &live, &min, &max);
    g_string_append_printf (json, "\"latency\":{\"live\":%s,",
        live ? "true" : "false");
    pm_json_append_value (json, "min", min);
    pm_json_append_value (json, "max", max);
    pm_json_close (json, '}');
    g_string_append_c (json, ',');
  } else {
    g_string_append (json, "\"latency\":null,");
  }
  gst_query_unref (query);
  pm_json_close (json, '}');
  g_string_append_c (json, ',');

  g_free (provider_path);
  gst_clear_object (&clock);
}

static gint
compare_refs (gconstpointer a, gconstpointer b)
{
  const PmElementRef *ra = *(PmElementRef * const *) a;
  const PmElementRef *rb = *(PmElementRef * const *) b;

  return (ra->id > rb->id) - (ra->id < rb->id);
}

/* {"queues":[...],"pipelines":[...]} */
gchar *
pm_levels_to_json (PmModel * model)
{
  GString *json = g_string_new ("{");
  GPtrArray *found;
  guint i;


  /* Properties and queries are read without the model locked. */
  found = pm_model_find_elements (model, queue_filter);
  g_ptr_array_sort (found, compare_refs);
  g_string_append (json, "\"queues\":[");
  for (i = 0; i < found->len; i++)
    append_queue (json, model, g_ptr_array_index (found, i));
  pm_json_close (json, ']');
  g_string_append_c (json, ',');
  g_ptr_array_unref (found);

  found = pm_model_find_elements (model, pipeline_filter);
  g_ptr_array_sort (found, compare_refs);
  g_string_append (json, "\"pipelines\":[");
  for (i = 0; i < found->len; i++)
    append_pipeline (json, model, g_ptr_array_index (found, i));
  pm_json_close (json, ']');
  g_ptr_array_unref (found);

  g_string_append_c (json, '}');
  return g_string_free (json, FALSE);
}
