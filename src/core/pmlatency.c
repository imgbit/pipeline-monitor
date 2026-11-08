/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pmlatency.h"
#include "pmjson.h"
#include "pmlevels.h"

#include <string.h>

/* A measurement records, at its input pads, when each span of running time
 * arrived, and looks up, at its output pads, when the data now leaving
 * came in. For an element the inputs are its sink pads and the outputs its
 * src pads; for a path from pad A to pad B, A and B. Probes only read.
 *
 * Inputs are always recorded (a short ring of marks); outputs are looked
 * up at most every SAMPLE_GAP, so busy pads (RTP) cost little. */

#define MARKS 512
/* Marks older than this are not matched (a stream that loops or seeks
 * repeats running times). */
#define MAX_AGE (5 * GST_SECOND)
/* Buffers already inside when measuring starts come out without a known
 * input: not counted until one matches, or for this long. */
#define SETTLE_TIME (2 * GST_SECOND)
#define SAMPLE_GAP (10 * GST_MSECOND)
#define WINDOW_SLOTS 4
/* Reported latency is queried every REPORT_TICKS ticks (250 ms each). */
#define REPORT_TICKS 4
#define DEFAULT_ALL_LIMIT 200000

typedef struct
{
  GstClockTime start, end, arrival;
} PmMark;

typedef struct
{
  GstClockTime min, max, sum;
  guint64 matched, unmatched;
} PmSlot;

typedef struct _PmMeasure PmMeasure;

struct _PmMeasure
{
  gint ref;
  GMutex lock;
  PmMark marks[MARKS];
  guint head, count;
  PmSlot slot;
  GstClockTime last;
  GstClockTime started;
  gboolean settled;
  /* Probe calls since the last tick. */
  gint calls;

  /* Server thread only */
  gboolean is_path;
  guint64 a, b;
  GPtrArray *probes;
  GHashTable *clients;
  gboolean all;
  PmSlot window[WINDOW_SLOTS];
  guint64 ticks;
  /* Whether the element has sink and src pads: a source or a sink has
   * nothing in between to measure. */
  gboolean inputs, outputs;
  gboolean have_reported;
  gboolean reported_live;
  GstClockTime reported_min, reported_max;
  /* This tick's result as a JSON object, for the pages that asked. */
  gchar *json;
};

typedef struct
{
  PmMeasure *measure;
  gboolean record;
  gboolean have_segment;
  GstSegment segment;
  GstClockTime last_sample;
} PmProbeData;

/* What the owner keeps to remove a probe again. */
typedef struct
{
  GWeakRef pad;
  gulong id;
} PmProbeRef;

struct _PmLatency
{
  PmModel *model;
  GHashTable *elements;
  GHashTable *paths;
  GHashTable *all_clients;
  /* Clients that had "measure all" on when the limit stopped it. */
  GHashTable *stopped_clients;
  /* Clients sent results at the last tick: when theirs end, one more
   * (empty) message tells them. */
  GHashTable *active_clients;
  guint all_limit;
  /* Buffers/s seen by "measure all" probes, and why it last stopped. */
  guint64 all_rate;
  guint64 all_stopped_rate;
  guint64 tick;
};

static void
measure_unref (PmMeasure * m)
{
  if (!g_atomic_int_dec_and_test (&m->ref))
    return;
  g_mutex_clear (&m->lock);
  if (m->probes != NULL)
    g_ptr_array_unref (m->probes);
  if (m->clients != NULL)
    g_hash_table_unref (m->clients);
  g_free (m->json);
  g_free (m);
}

static void
probe_data_free (PmProbeData * data)
{
  measure_unref (data->measure);
  g_free (data);
}

static void
probe_ref_free (PmProbeRef * ref)
{
  GstPad *pad = g_weak_ref_get (&ref->pad);

  /* A pad that is gone took its probe with it. */
  if (pad != NULL) {
    gst_pad_remove_probe (pad, ref->id);
    gst_object_unref (pad);
  }
  g_weak_ref_clear (&ref->pad);
  g_free (ref);
}

static PmMeasure *
measure_new (gboolean is_path, guint64 a, guint64 b)
{
  PmMeasure *m = g_new0 (PmMeasure, 1);

  m->ref = 1;
  g_mutex_init (&m->lock);
  m->is_path = is_path;
  m->a = a;
  m->b = b;
  m->last = GST_CLOCK_TIME_NONE;
  m->started = gst_util_get_timestamp ();
  m->probes = g_ptr_array_new_with_free_func ((GDestroyNotify)
      probe_ref_free);
  m->clients = g_hash_table_new (NULL, NULL);
  return m;
}

/* Removes the probes first: they hold references to the measurement. */
static void
measure_free (PmMeasure * m)
{
  g_ptr_array_set_size (m->probes, 0);
  measure_unref (m);
}

/* The running time span of a buffer, or FALSE if it has no time. */
static gboolean
buffer_span (PmProbeData * data, GstBuffer * buffer, GstClockTime * start,
    GstClockTime * end)
{
  GstClockTime ts = GST_BUFFER_PTS_IS_VALID (buffer) ?
      GST_BUFFER_PTS (buffer) : GST_BUFFER_DTS (buffer);

  if (!data->have_segment || data->segment.format != GST_FORMAT_TIME ||
      !GST_CLOCK_TIME_IS_VALID (ts))
    return FALSE;
  *start = gst_segment_to_running_time (&data->segment, GST_FORMAT_TIME, ts);
  if (!GST_CLOCK_TIME_IS_VALID (*start))
    return FALSE;
  *end = GST_BUFFER_DURATION_IS_VALID (buffer) ?
      *start + GST_BUFFER_DURATION (buffer) : *start;
  return TRUE;
}

static void
record (PmMeasure * m, GstClockTime start, GstClockTime end,
    GstClockTime now)
{
  PmMark *mark;

  g_mutex_lock (&m->lock);
  mark = &m->marks[m->head];
  mark->start = start;
  mark->end = end;
  mark->arrival = now;
  m->head = (m->head + 1) % MARKS;
  m->count = MIN (m->count + 1, MARKS);
  g_mutex_unlock (&m->lock);
}

/* Finds, newest first, the input whose span contains start. */
static void
match (PmMeasure * m, GstClockTime start, GstClockTime now)
{
  PmSlot *slot = &m->slot;
  guint i;

  g_mutex_lock (&m->lock);
  for (i = 0; i < m->count; i++) {
    PmMark *mark = &m->marks[(m->head + MARKS - 1 - i) % MARKS];

    if (now - mark->arrival > MAX_AGE)
      break;
    if (mark->start <= start && (start < mark->end ||
            (mark->start == mark->end && start == mark->start))) {
      GstClockTime t = now - mark->arrival;

      slot->min = slot->matched == 0 ? t : MIN (slot->min, t);
      slot->max = MAX (slot->max, t);
      slot->sum += t;
      slot->matched++;
      m->last = t;
      m->settled = TRUE;
      g_mutex_unlock (&m->lock);
      return;
    }
  }
  if (!m->settled && now - m->started < SETTLE_TIME) {
    g_mutex_unlock (&m->lock);
    return;
  }
  m->settled = TRUE;
  slot->unmatched++;
  g_mutex_unlock (&m->lock);
}

/* Runs on the streaming thread for every buffer: keep it short. */
static GstPadProbeReturn
on_probe (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  PmProbeData *data = user_data;
  PmMeasure *m = data->measure;
  GstBuffer *first, *last;
  GstClockTime now, start, end, last_start, last_end;

  g_atomic_int_inc (&m->calls);

  if (info->type & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
    GstEvent *event = GST_PAD_PROBE_INFO_EVENT (info);

    if (GST_EVENT_TYPE (event) == GST_EVENT_SEGMENT) {
      gst_event_copy_segment (event, &data->segment);
      data->have_segment = TRUE;
    }
    return GST_PAD_PROBE_OK;
  }

  if (info->type & GST_PAD_PROBE_TYPE_BUFFER_LIST) {
    GstBufferList *list = GST_PAD_PROBE_INFO_BUFFER_LIST (info);
    guint n = gst_buffer_list_length (list);

    if (n == 0)
      return GST_PAD_PROBE_OK;
    first = gst_buffer_list_get (list, 0);
    last = gst_buffer_list_get (list, n - 1);
  } else {
    first = last = GST_PAD_PROBE_INFO_BUFFER (info);
  }

  now = gst_util_get_timestamp ();
  if (data->record) {
    /* A list counts as one span, from its first to its last buffer. */
    if (buffer_span (data, first, &start, &end) &&
        buffer_span (data, last, &last_start, &last_end))
      record (m, start, MAX (end, last_end), now);
    return GST_PAD_PROBE_OK;
  }

  if (GST_CLOCK_TIME_IS_VALID (data->last_sample) &&
      now - data->last_sample < SAMPLE_GAP)
    return GST_PAD_PROBE_OK;
  data->last_sample = now;
  if (buffer_span (data, first, &start, &end)) {
    match (m, start, now);
  } else {
    g_mutex_lock (&m->lock);
    m->slot.unmatched++;
    g_mutex_unlock (&m->lock);
  }
  return GST_PAD_PROBE_OK;
}

static gboolean
has_probe (PmMeasure * m, GstPad * pad)
{
  guint i;

  for (i = 0; i < m->probes->len; i++) {
    PmProbeRef *ref = g_ptr_array_index (m->probes, i);
    GstPad *other = g_weak_ref_get (&ref->pad);

    if (other != NULL)
      gst_object_unref (other);
    if (other == pad)
      return TRUE;
  }
  return FALSE;
}

static void
add_probe (PmMeasure * m, GstPad * pad, gboolean record)
{
  PmProbeData *data;
  PmProbeRef *ref;
  GstEvent *segment;

  if (has_probe (m, pad))
    return;
  data = g_new0 (PmProbeData, 1);
  data->measure = m;
  g_atomic_int_inc (&m->ref);
  data->record = record;
  data->last_sample = GST_CLOCK_TIME_NONE;
  gst_segment_init (&data->segment, GST_FORMAT_UNDEFINED);
  /* Measured mid-stream: the segment came before. */
  segment = gst_pad_get_sticky_event (pad, GST_EVENT_SEGMENT, 0);
  if (segment != NULL) {
    gst_event_copy_segment (segment, &data->segment);
    data->have_segment = TRUE;
    gst_event_unref (segment);
  }

  ref = g_new0 (PmProbeRef, 1);
  g_weak_ref_init (&ref->pad, pad);
  ref->id = gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER |
      GST_PAD_PROBE_TYPE_BUFFER_LIST | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
      on_probe, data, (GDestroyNotify) probe_data_free);
  g_ptr_array_add (m->probes, ref);
}

/* Adds probes to pads that have none yet (request pads come and go).
 * Returns FALSE if the element or a pad of the path is gone. */
static gboolean
update_probes (PmLatency * latency, PmMeasure * m)
{
  GstIterator *it;
  GValue item = G_VALUE_INIT;
  GstElement *element;

  if (m->is_path) {
    GstPad *from = pm_model_get_pad (latency->model, m->a);
    GstPad *to = pm_model_get_pad (latency->model, m->b);
    gboolean ok = from != NULL && to != NULL;

    if (ok) {
      add_probe (m, from, TRUE);
      add_probe (m, to, FALSE);
    }
    gst_clear_object (&from);
    gst_clear_object (&to);
    return ok;
  }

  element = pm_model_get_element (latency->model, m->a);
  if (element == NULL)
    return FALSE;
  GST_OBJECT_LOCK (element);
  m->inputs = element->numsinkpads > 0;
  m->outputs = element->numsrcpads > 0;
  GST_OBJECT_UNLOCK (element);
  it = gst_element_iterate_pads (element);
  while (gst_iterator_next (it, &item) == GST_ITERATOR_OK) {
    GstPad *pad = g_value_get_object (&item);

    add_probe (m, pad, GST_PAD_IS_SINK (pad));
    g_value_reset (&item);
  }
  g_value_unset (&item);
  gst_iterator_free (it);
  gst_object_unref (element);
  return TRUE;
}

/* The element's own latency as it reports it: the latency query answered
 * at its src pad minus the one answered upstream of its sink pad; for a
 * source, what its src pad answers. FALSE if it has no src pad, is not
 * PAUSED or PLAYING, or does not answer. */
gboolean
pm_latency_reported (GstElement * element, gboolean * live,
    GstClockTime * min, GstClockTime * max)
{
  GstPad *src = gst_element_get_static_pad (element, "src");
  GstPad *sink = gst_element_get_static_pad (element, "sink");
  GstQuery *out, *in;
  gboolean ok = FALSE, live_in;
  GstClockTime out_min, out_max, min_in = 0, max_in = 0;

  if (src != NULL && GST_STATE (element) >= GST_STATE_PAUSED) {
    out = gst_query_new_latency ();
    in = gst_query_new_latency ();
    if (gst_pad_query (src, out) && (sink == NULL ||
            gst_pad_peer_query (sink, in))) {
      gst_query_parse_latency (out, live, &out_min, &out_max);
      if (sink != NULL)
        gst_query_parse_latency (in, &live_in, &min_in, &max_in);
      *min = out_min > min_in ? out_min - min_in : 0;
      *max = !GST_CLOCK_TIME_IS_VALID (out_max) ||
          !GST_CLOCK_TIME_IS_VALID (max_in) ? GST_CLOCK_TIME_NONE :
          out_max > max_in ? out_max - max_in : 0;
      ok = TRUE;
    }
    gst_query_unref (out);
    gst_query_unref (in);
  }
  gst_clear_object (&src);
  gst_clear_object (&sink);
  return ok;
}

static void
update_reported (PmLatency * latency, PmMeasure * m)
{
  GstElement *element = pm_model_get_element (latency->model, m->a);

  m->have_reported = element != NULL && pm_latency_reported (element,
      &m->reported_live, &m->reported_min, &m->reported_max);
  gst_clear_object (&element);
}

PmLatency *
pm_latency_new (PmModel * model)
{
  PmLatency *latency = g_new0 (PmLatency, 1);

  latency->model = model;
  latency->elements = g_hash_table_new_full (g_int64_hash, g_int64_equal,
      NULL, (GDestroyNotify) measure_free);
  latency->paths = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
      (GDestroyNotify) measure_free);
  latency->all_clients = g_hash_table_new (NULL, NULL);
  latency->stopped_clients = g_hash_table_new (NULL, NULL);
  latency->active_clients = g_hash_table_new (NULL, NULL);
  latency->all_limit = DEFAULT_ALL_LIMIT;
  return latency;
}

void
pm_latency_free (PmLatency * latency)
{
  g_hash_table_unref (latency->elements);
  g_hash_table_unref (latency->paths);
  g_hash_table_unref (latency->all_clients);
  g_hash_table_unref (latency->stopped_clients);
  g_hash_table_unref (latency->active_clients);
  g_free (latency);
}

void
pm_latency_set_all_limit (PmLatency * latency, guint buffers_per_s)
{
  latency->all_limit = buffers_per_s;
}

static PmMeasure *
element_measure (PmLatency * latency, guint64 id, gboolean create)
{
  PmMeasure *m = g_hash_table_lookup (latency->elements, &id);

  if (m == NULL && create) {
    m = measure_new (FALSE, id, 0);
    g_hash_table_insert (latency->elements, &m->a, m);
  }
  return m;
}

/* Drops a measurement nobody needs any more. */
static void
release (GHashTable * table, gconstpointer key, PmMeasure * m)
{
  if (g_hash_table_size (m->clients) == 0 && !m->all)
    g_hash_table_remove (table, key);
}

void
pm_latency_measure_element (PmLatency * latency, gpointer client,
    guint64 element, gboolean on)
{
  PmMeasure *m = element_measure (latency, element, on);

  if (m == NULL)
    return;
  if (on) {
    g_hash_table_add (m->clients, client);
    update_probes (latency, m);
  } else {
    g_hash_table_remove (m->clients, client);
    release (latency->elements, &element, m);
  }
}

void
pm_latency_measure_path (PmLatency * latency, gpointer client,
    guint64 from, guint64 to, gboolean on)
{
  gchar *key = g_strdup_printf ("%" G_GUINT64_FORMAT ">%" G_GUINT64_FORMAT,
      from, to);
  PmMeasure *m = g_hash_table_lookup (latency->paths, key);

  if (on) {
    if (m == NULL) {
      m = measure_new (TRUE, from, to);
      g_hash_table_insert (latency->paths, g_strdup (key), m);
    }
    g_hash_table_add (m->clients, client);
    update_probes (latency, m);
  } else if (m != NULL) {
    g_hash_table_remove (m->clients, client);
    release (latency->paths, key, m);
  }
  g_free (key);
}

static void
stop_all (PmLatency * latency)
{
  GHashTableIter iter;
  gpointer value;

  g_hash_table_remove_all (latency->all_clients);
  g_hash_table_iter_init (&iter, latency->elements);
  while (g_hash_table_iter_next (&iter, NULL, &value)) {
    PmMeasure *m = value;

    m->all = FALSE;
    if (g_hash_table_size (m->clients) == 0)
      g_hash_table_iter_remove (&iter);
  }
  latency->all_rate = 0;
}

void
pm_latency_measure_all (PmLatency * latency, gpointer client, gboolean on)
{
  g_hash_table_remove (latency->stopped_clients, client);
  if (on) {
    g_hash_table_add (latency->all_clients, client);
  } else {
    g_hash_table_remove (latency->all_clients, client);
    if (g_hash_table_size (latency->all_clients) == 0)
      stop_all (latency);
  }
}

static void
remove_client_from (GHashTable * table, gpointer client)
{
  GHashTableIter iter;
  gpointer value;

  g_hash_table_iter_init (&iter, table);
  while (g_hash_table_iter_next (&iter, NULL, &value)) {
    PmMeasure *m = value;

    g_hash_table_remove (m->clients, client);
    if (g_hash_table_size (m->clients) == 0 && !m->all)
      g_hash_table_iter_remove (&iter);
  }
}

void
pm_latency_remove_client (PmLatency * latency, gpointer client)
{
  remove_client_from (latency->elements, client);
  remove_client_from (latency->paths, client);
  pm_latency_measure_all (latency, client, FALSE);
  g_hash_table_remove (latency->active_clients, client);
}

/* Elements worth measuring with "measure all": not bins (their children
 * are measured), with data going in and out. */
static gboolean
all_filter (GstElement * element, gboolean top_level)
{
  return !GST_IS_BIN (element) && element->numsinkpads > 0 &&
      element->numsrcpads > 0;
}

static void
sync_all (PmLatency * latency)
{
  GPtrArray *found = pm_model_find_elements (latency->model, all_filter);
  guint i;

  for (i = 0; i < found->len; i++) {
    PmElementRef *ref = g_ptr_array_index (found, i);

    element_measure (latency, ref->id, TRUE)->all = TRUE;
  }
  g_ptr_array_unref (found);
}

/* "key":"<path of the element or pad with this id>", */
static void
append_path (GString * json, const gchar * key, PmLatency * latency,
    guint64 id)
{
  gchar *path = pm_model_path (latency->model, id);

  pm_json_append_member_string (json, key, path);
  g_free (path);
}

/* Closes the current slot into the window and appends "measured". */
static void
append_measured (GString * json, PmMeasure * m)
{
  PmSlot sum = { 0, };
  GstClockTime last;
  guint n, i;

  g_mutex_lock (&m->lock);
  m->window[m->ticks++ % WINDOW_SLOTS] = m->slot;
  memset (&m->slot, 0, sizeof (PmSlot));
  last = m->last;
  g_mutex_unlock (&m->lock);

  n = MIN (m->ticks, WINDOW_SLOTS);
  for (i = 0; i < n; i++) {
    PmSlot *slot = &m->window[i];

    if (slot->matched > 0) {
      sum.min = sum.matched == 0 ? slot->min : MIN (sum.min, slot->min);
      sum.max = MAX (sum.max, slot->max);
      sum.sum += slot->sum;
    }
    sum.matched += slot->matched;
    sum.unmatched += slot->unmatched;
  }

  g_string_append (json, "\"measured\":{");
  pm_json_append_value (json, "min", sum.matched ? sum.min : G_MAXUINT64);
  pm_json_append_value (json, "avg", sum.matched ? sum.sum / sum.matched :
      G_MAXUINT64);
  pm_json_append_value (json, "max", sum.matched ? sum.max : G_MAXUINT64);
  pm_json_append_value (json, "last", last);
  pm_json_append_value (json, "matched", sum.matched);
  pm_json_append_value (json, "unmatched", sum.unmatched);
  pm_json_close (json, '}');
  g_string_append_c (json, ',');
}

/* Keeps this tick's result of m as a JSON object without its closing
 * brace: the element or path, and "measured". */
static void
keep_result (PmMeasure * m, GString * json)
{
  g_free (m->json);
  pm_json_close (json, '}');
  m->json = g_string_free (json, FALSE);
}

/* Every 250 ms on the server thread: keeps probes up to date, applies the
 * "measure all" limit and computes this tick's results, once however many
 * pages want them; pm_latency_message then picks each page's. */
void
pm_latency_tick (PmLatency * latency)
{
  GHashTableIter iter;
  gpointer key, value;
  guint64 all_calls = 0;
  gboolean report = latency->tick++ % REPORT_TICKS == 0;

  if (g_hash_table_size (latency->all_clients) > 0)
    sync_all (latency);
  else if (latency->all_rate != 0)
    stop_all (latency);

  g_hash_table_iter_init (&iter, latency->elements);
  while (g_hash_table_iter_next (&iter, &key, &value)) {
    PmMeasure *m = value;
    gint calls = g_atomic_int_and (&m->calls, 0);
    GString *json;

    if (!update_probes (latency, m)) {
      g_hash_table_iter_remove (&iter);
      continue;
    }
    if (m->all)
      all_calls += calls;
    if (report)
      update_reported (latency, m);
    json = g_string_new ("{");
    pm_json_append_value (json, "element", m->a);
    append_path (json, "path", latency, m->a);
    g_string_append_printf (json, "\"inputs\":%s,\"outputs\":%s,",
        m->inputs ? "true" : "false", m->outputs ? "true" : "false");
    append_measured (json, m);
    if (m->have_reported) {
      g_string_append_printf (json, "\"reported\":{\"live\":%s,",
          m->reported_live ? "true" : "false");
      pm_json_append_value (json, "min", m->reported_min);
      pm_json_append_value (json, "max", m->reported_max);
      pm_json_close (json, '}');
      g_string_append_c (json, ',');
    } else {
      g_string_append (json, "\"reported\":null,");
    }
    keep_result (m, json);
  }

  g_hash_table_iter_init (&iter, latency->paths);
  while (g_hash_table_iter_next (&iter, &key, &value)) {
    PmMeasure *m = value;
    GString *json;

    g_atomic_int_set (&m->calls, 0);
    if (!update_probes (latency, m)) {
      g_hash_table_iter_remove (&iter);
      continue;
    }
    json = g_string_new ("{");
    pm_json_append_value (json, "from", m->a);
    append_path (json, "from_path", latency, m->a);
    pm_json_append_value (json, "to", m->b);
    append_path (json, "to_path", latency, m->b);
    append_measured (json, m);
    keep_result (m, json);
  }

  /* Calls in this tick, as a rate per second. */
  if (g_hash_table_size (latency->all_clients) > 0) {
    latency->all_rate = all_calls * (1000 / 250);
    if (latency->all_rate > latency->all_limit) {
      latency->all_stopped_rate = latency->all_rate;
      g_hash_table_iter_init (&iter, latency->all_clients);
      while (g_hash_table_iter_next (&iter, &key, NULL))
        g_hash_table_add (latency->stopped_clients, key);
      stop_all (latency);
    }
  }
}

/* Appends the results of table that client wants, as a JSON array. */
static gboolean
append_wanted (GString * json, GHashTable * table, gpointer client,
    gboolean all)
{
  GHashTableIter iter;
  gpointer value;
  gboolean any = FALSE;

  g_string_append_c (json, '[');
  g_hash_table_iter_init (&iter, table);
  while (g_hash_table_iter_next (&iter, NULL, &value)) {
    PmMeasure *m = value;

    /* client NULL: what any page measures. */
    if (m->json == NULL || !(client == NULL ? g_hash_table_size (m->clients) >
            0 || m->all : g_hash_table_contains (m->clients, client) ||
            (all && m->all)))
      continue;
    if (any)
      g_string_append_c (json, ',');
    g_string_append (json, m->json);
    any = TRUE;
  }
  g_string_append (json, "],");
  return any;
}

/* Everything measured for any page, after pm_latency_tick, for
 * recordings; NULL when nothing is measured. */
gchar *
pm_latency_message_all (PmLatency * latency)
{
  gboolean all = g_hash_table_size (latency->all_clients) > 0;
  GString *json = g_string_new ("{\"elements\":");
  gboolean any;

  any = append_wanted (json, latency->elements, NULL, all);
  g_string_append (json, "\"paths\":");
  any = append_wanted (json, latency->paths, NULL, FALSE) || any;
  if (!any && !all) {
    g_string_free (json, TRUE);
    return NULL;
  }
  g_string_append_printf (json, "\"all\":{\"on\":%s,",
      all ? "true" : "false");
  pm_json_append_value (json, "buffers_per_s", all ? latency->all_rate : 0);
  pm_json_append_value (json, "stopped_rate", G_MAXUINT64);
  pm_json_close (json, '}');
  g_string_append_c (json, '}');
  return g_string_free (json, FALSE);
}

/* The "latency" data for one page, after pm_latency_tick: only the
 * elements and paths it measures (all elements with "measure all"), and
 * the "measure all" state only if it had it on. NULL when it measures
 * nothing; once, when its last measurement ends, an empty one. */
gchar *
pm_latency_message (PmLatency * latency, gpointer client)
{
  gboolean all = g_hash_table_contains (latency->all_clients, client);
  gboolean stopped = g_hash_table_contains (latency->stopped_clients, client);
  GString *json = g_string_new ("{");
  gboolean any;

  g_string_append (json, "\"elements\":");
  any = append_wanted (json, latency->elements, client, all);
  g_string_append (json, "\"paths\":");
  any = append_wanted (json, latency->paths, client, FALSE) || any;

  if (!any && !all && !stopped) {
    if (!g_hash_table_remove (latency->active_clients, client)) {
      g_string_free (json, TRUE);
      return NULL;
    }
  } else {
    g_hash_table_add (latency->active_clients, client);
  }

  g_string_append_printf (json, "\"all\":{\"on\":%s,",
      all ? "true" : "false");
  pm_json_append_value (json, "buffers_per_s", all ? latency->all_rate : 0);
  pm_json_append_value (json, "stopped_rate",
      stopped ? latency->all_stopped_rate : G_MAXUINT64);
  pm_json_close (json, '}');
  g_string_append_c (json, '}');
  return g_string_free (json, FALSE);
}
