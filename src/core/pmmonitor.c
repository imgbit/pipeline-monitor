/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pmmonitor.h"

#include <string.h>

/* The window for rates and gaps: the last WINDOW_SLOTS ticks of the
 * server (250 ms each), about 1 s. For sparse streams (subtitles) it grows
 * until it holds two buffers, up to HISTORY_SLOTS (10 s). */
#define WINDOW_SLOTS 4
#define HISTORY_SLOTS 40
/* How often the probe reads the element's clock and base time again. */
#define CLOCK_REFRESH (100 * GST_MSECOND)

/* Counts between two ticks. Times are arrival times at the pad. */
typedef struct
{
  GstClockTime begin, end;
  guint64 buffers, bytes;
  GstClockTime gap_min, gap_max, gap_sum;
  guint64 gaps;
} PmSlot;

enum
{
  EVENT_SEGMENT,
  EVENT_FLUSH,
  EVENT_EOS,
  EVENT_CAPS,
  EVENT_GAP,
  N_EVENTS
};

static const gchar *event_names[N_EVENTS] = {
  "segment", "flush", "eos", "caps", "gap"
};

/* Totals and the last buffer, copied out as a whole by the server. */
typedef struct
{
  guint64 buffers, bytes;
  gboolean have_buffer;
  GstClockTime pts, dts, duration, running_time;
  gboolean have_delay;
  gint64 delay;
  gsize size;
  guint64 offset, offset_end;
  GstBufferFlags flags;
  GstClockTime arrival;
  gboolean have_segment;
  GstSegment segment;
  guint64 events[N_EVENTS];
} PmValues;

/* Filled by the probe on the streaming thread, read by the server. Shared
 * by the entry and the probe; freed when both are done with it. */
typedef struct _PmPadStats PmPadStats;

struct _PmPadStats
{
  gint ref;
  GMutex lock;
  PmValues values;
  PmSlot slot;
  GstClockTime last_arrival;
  GstClock *clock;
  GstClockTime base_time;
  GstClockTime refreshed;
};

typedef struct _PmEntry PmEntry;

struct _PmEntry
{
  guint64 pad_id;
  GWeakRef pad;
  gulong probe;
  PmPadStats *stats;
  GHashTable *clients;
  PmSlot window[HISTORY_SLOTS];
  guint64 ticks;
};

struct _PmMonitor
{
  PmModel *model;
  GMutex lock;
  GHashTable *entries;
};

static void
stats_unref (PmPadStats * stats)
{
  if (!g_atomic_int_dec_and_test (&stats->ref))
    return;
  gst_clear_object (&stats->clock);
  g_mutex_clear (&stats->lock);
  g_free (stats);
}

static void
refresh_clock (PmPadStats * stats, GstPad * pad, GstClockTime now)
{
  GstElement *element = gst_pad_get_parent_element (pad);

  gst_clear_object (&stats->clock);
  if (element != NULL) {
    stats->clock = gst_element_get_clock (element);
    stats->base_time = gst_element_get_base_time (element);
    gst_object_unref (element);
  }
  stats->refreshed = now;
}

static void
count_buffer (PmPadStats * stats, GstBuffer * buffer)
{
  gsize size = gst_buffer_get_size (buffer);

  stats->values.buffers++;
  stats->values.bytes += size;
  stats->slot.buffers++;
  stats->slot.bytes += size;
}

static void
set_last (PmPadStats * stats, GstBuffer * buffer)
{
  PmValues *v = &stats->values;
  GstClockTime ts;

  v->have_buffer = TRUE;
  v->pts = GST_BUFFER_PTS (buffer);
  v->dts = GST_BUFFER_DTS (buffer);
  v->duration = GST_BUFFER_DURATION (buffer);
  v->size = gst_buffer_get_size (buffer);
  v->offset = GST_BUFFER_OFFSET (buffer);
  v->offset_end = GST_BUFFER_OFFSET_END (buffer);
  v->flags = GST_BUFFER_FLAGS (buffer);

  ts = GST_CLOCK_TIME_IS_VALID (v->pts) ? v->pts : v->dts;
  v->running_time = GST_CLOCK_TIME_NONE;
  if (v->have_segment && v->segment.format == GST_FORMAT_TIME &&
      GST_CLOCK_TIME_IS_VALID (ts))
    v->running_time = gst_segment_to_running_time (&v->segment,
        GST_FORMAT_TIME, ts);

  /* Positive: later than the buffer's running time on the clock. */
  v->have_delay = stats->clock != NULL &&
      GST_CLOCK_TIME_IS_VALID (v->running_time);
  if (v->have_delay)
    v->delay = GST_CLOCK_DIFF (stats->base_time + v->running_time,
        gst_clock_get_time (stats->clock));
}

static void
count_gap (PmPadStats * stats, GstClockTime arrival)
{
  PmSlot *slot = &stats->slot;

  if (GST_CLOCK_TIME_IS_VALID (stats->last_arrival)) {
    GstClockTime gap = arrival - stats->last_arrival;

    slot->gap_min = slot->gaps == 0 ? gap : MIN (slot->gap_min, gap);
    slot->gap_max = MAX (slot->gap_max, gap);
    slot->gap_sum += gap;
    slot->gaps++;
  }
  stats->last_arrival = arrival;
}

static void
count_event (PmPadStats * stats, GstEvent * event)
{
  PmValues *v = &stats->values;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_SEGMENT:
      gst_event_copy_segment (event, &v->segment);
      v->have_segment = TRUE;
      v->events[EVENT_SEGMENT]++;
      stats->refreshed = GST_CLOCK_TIME_NONE;
      break;
    case GST_EVENT_FLUSH_START:
      v->events[EVENT_FLUSH]++;
      break;
    case GST_EVENT_FLUSH_STOP:
      /* The time spent flushing is not a gap between buffers. */
      stats->last_arrival = GST_CLOCK_TIME_NONE;
      break;
    case GST_EVENT_EOS:
      v->events[EVENT_EOS]++;
      break;
    case GST_EVENT_CAPS:
      v->events[EVENT_CAPS]++;
      break;
    case GST_EVENT_GAP:
      v->events[EVENT_GAP]++;
      break;
    default:
      break;
  }
}

/* Runs on the streaming thread for every buffer: keep it short. */
static GstPadProbeReturn
on_probe (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  PmPadStats *stats = user_data;
  GstClockTime arrival;

  if (info->type & GST_PAD_PROBE_TYPE_EVENT_BOTH) {
    g_mutex_lock (&stats->lock);
    count_event (stats, GST_PAD_PROBE_INFO_EVENT (info));
    g_mutex_unlock (&stats->lock);
    return GST_PAD_PROBE_OK;
  }

  arrival = gst_util_get_timestamp ();
  g_mutex_lock (&stats->lock);
  stats->values.arrival = arrival;
  if (!GST_CLOCK_TIME_IS_VALID (stats->refreshed) ||
      arrival - stats->refreshed >= CLOCK_REFRESH)
    refresh_clock (stats, pad, arrival);
  count_gap (stats, arrival);
  if (info->type & GST_PAD_PROBE_TYPE_BUFFER_LIST) {
    GstBufferList *list = GST_PAD_PROBE_INFO_BUFFER_LIST (info);
    guint i, n = gst_buffer_list_length (list);

    for (i = 0; i < n; i++)
      count_buffer (stats, gst_buffer_list_get (list, i));
    if (n > 0)
      set_last (stats, gst_buffer_list_get (list, n - 1));
  } else {
    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER (info);

    count_buffer (stats, buffer);
    set_last (stats, buffer);
  }
  g_mutex_unlock (&stats->lock);
  return GST_PAD_PROBE_OK;
}

static PmPadStats *
stats_new (GstPad * pad)
{
  PmPadStats *stats = g_new0 (PmPadStats, 1);
  GstEvent *segment;

  /* One reference for the entry, one for the probe. */
  stats->ref = 2;
  g_mutex_init (&stats->lock);
  stats->last_arrival = GST_CLOCK_TIME_NONE;
  stats->refreshed = GST_CLOCK_TIME_NONE;
  stats->slot.begin = gst_util_get_timestamp ();
  gst_segment_init (&stats->values.segment, GST_FORMAT_UNDEFINED);

  /* A pad watched mid-stream already had its segment. */
  segment = gst_pad_get_sticky_event (pad, GST_EVENT_SEGMENT, 0);
  if (segment != NULL) {
    gst_event_copy_segment (segment, &stats->values.segment);
    stats->values.have_segment = TRUE;
    gst_event_unref (segment);
  }
  return stats;
}

static void
entry_free (PmEntry * entry)
{
  GstPad *pad = g_weak_ref_get (&entry->pad);

  /* A pad that is gone took its probe with it. */
  if (pad != NULL) {
    gst_pad_remove_probe (pad, entry->probe);
    gst_object_unref (pad);
  }
  g_weak_ref_clear (&entry->pad);
  g_hash_table_unref (entry->clients);
  stats_unref (entry->stats);
  g_free (entry);
}

PmMonitor *
pm_monitor_new (PmModel * model)
{
  PmMonitor *monitor = g_new0 (PmMonitor, 1);

  monitor->model = model;
  g_mutex_init (&monitor->lock);
  monitor->entries = g_hash_table_new_full (g_int64_hash, g_int64_equal, NULL,
      (GDestroyNotify) entry_free);
  return monitor;
}

void
pm_monitor_free (PmMonitor * monitor)
{
  g_hash_table_unref (monitor->entries);
  g_mutex_clear (&monitor->lock);
  g_free (monitor);
}

/* Returns FALSE if the pad is unknown or gone. */
gboolean
pm_monitor_add (PmMonitor * monitor, gpointer client, guint64 pad_id)
{
  PmEntry *entry;
  GstPad *pad;

  g_mutex_lock (&monitor->lock);
  entry = g_hash_table_lookup (monitor->entries, &pad_id);
  if (entry != NULL) {
    g_hash_table_add (entry->clients, client);
    g_mutex_unlock (&monitor->lock);
    return TRUE;
  }

  pad = pm_model_get_pad (monitor->model, pad_id);
  if (pad == NULL) {
    g_mutex_unlock (&monitor->lock);
    return FALSE;
  }

  entry = g_new0 (PmEntry, 1);
  entry->pad_id = pad_id;
  g_weak_ref_init (&entry->pad, pad);
  entry->clients = g_hash_table_new (NULL, NULL);
  g_hash_table_add (entry->clients, client);
  entry->stats = stats_new (pad);
  entry->probe = gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER |
      GST_PAD_PROBE_TYPE_BUFFER_LIST | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM |
      GST_PAD_PROBE_TYPE_EVENT_FLUSH, on_probe, entry->stats,
      (GDestroyNotify) stats_unref);
  gst_object_unref (pad);
  g_hash_table_insert (monitor->entries, &entry->pad_id, entry);
  g_mutex_unlock (&monitor->lock);
  return TRUE;
}

static void
remove_client_from (PmMonitor * monitor, PmEntry * entry, gpointer client)
{
  g_hash_table_remove (entry->clients, client);
  if (g_hash_table_size (entry->clients) == 0)
    g_hash_table_remove (monitor->entries, &entry->pad_id);
}

void
pm_monitor_remove (PmMonitor * monitor, gpointer client, guint64 pad_id)
{
  PmEntry *entry;

  g_mutex_lock (&monitor->lock);
  entry = g_hash_table_lookup (monitor->entries, &pad_id);
  if (entry != NULL)
    remove_client_from (monitor, entry, client);
  g_mutex_unlock (&monitor->lock);
}

static GPtrArray *
entries_of (PmMonitor * monitor, gpointer client)
{
  GPtrArray *found = g_ptr_array_new ();
  GHashTableIter iter;
  gpointer value;

  g_hash_table_iter_init (&iter, monitor->entries);
  while (g_hash_table_iter_next (&iter, NULL, &value)) {
    PmEntry *entry = value;

    if (client == NULL || g_hash_table_contains (entry->clients, client))
      g_ptr_array_add (found, entry);
  }
  return found;
}

static gint
compare_entries (gconstpointer a, gconstpointer b)
{
  const PmEntry *ea = *(PmEntry * const *) a;
  const PmEntry *eb = *(PmEntry * const *) b;

  return (ea->pad_id > eb->pad_id) - (ea->pad_id < eb->pad_id);
}

void
pm_monitor_remove_client (PmMonitor * monitor, gpointer client)
{
  GPtrArray *entries;
  guint i;

  g_mutex_lock (&monitor->lock);
  entries = entries_of (monitor, client);
  for (i = 0; i < entries->len; i++)
    remove_client_from (monitor, g_ptr_array_index (entries, i), client);
  g_ptr_array_unref (entries);
  g_mutex_unlock (&monitor->lock);
}

static guint64
read_buffers (PmEntry * entry)
{
  guint64 buffers;

  g_mutex_lock (&entry->stats->lock);
  buffers = entry->stats->values.buffers;
  g_mutex_unlock (&entry->stats->lock);
  return buffers;
}

/* Closes the current slot of every watched pad. Called by the server every
 * 250 ms, before it sends the stats. */
void
pm_monitor_tick (PmMonitor * monitor)
{
  GstClockTime now = gst_util_get_timestamp ();
  GHashTableIter iter;
  gpointer value;

  g_mutex_lock (&monitor->lock);
  g_hash_table_iter_init (&iter, monitor->entries);
  while (g_hash_table_iter_next (&iter, NULL, &value)) {
    PmEntry *entry = value;
    PmSlot *slot = &entry->window[entry->ticks++ % HISTORY_SLOTS];

    g_mutex_lock (&entry->stats->lock);
    *slot = entry->stats->slot;
    memset (&entry->stats->slot, 0, sizeof (PmSlot));
    entry->stats->slot.begin = now;
    g_mutex_unlock (&entry->stats->lock);
    slot->end = now;
  }
  g_mutex_unlock (&monitor->lock);
}

/* Appends "key":value, with null for GST_CLOCK_TIME_NONE, which is also
 * GST_BUFFER_OFFSET_NONE and -1 in segments. */
static void
append_value (GString * json, const gchar * key, guint64 value)
{
  if (value == G_MAXUINT64)
    g_string_append_printf (json, "\"%s\":null,", key);
  else
    g_string_append_printf (json, "\"%s\":%" G_GUINT64_FORMAT ",", key,
        value);
}

static void
append_double (GString * json, const gchar * key, gdouble value)
{
  gchar buf[G_ASCII_DTOSTR_BUF_SIZE];

  g_string_append_printf (json, "\"%s\":%s,", key,
      g_ascii_formatd (buf, sizeof (buf), "%.3f", value));
}

/* Ends an object built from append_* calls. */
static void
close_object (GString * json)
{
  if (json->str[json->len - 1] == ',')
    json->str[json->len - 1] = '}';
  else
    g_string_append_c (json, '}');
}

static void
append_flags (GString * json, GstBufferFlags flags)
{
  GFlagsClass *klass = g_type_class_ref (GST_TYPE_BUFFER_FLAGS);
  gboolean first = TRUE;
  guint i;

  g_string_append (json, "\"flags\":[");
  for (i = 0; i < klass->n_values; i++) {
    const GFlagsValue *f = &klass->values[i];

    if (f->value >= GST_BUFFER_FLAG_LAST || !(flags & f->value))
      continue;
    g_string_append_printf (json, "%s\"%s\"", first ? "" : ",",
        f->value_nick);
    first = FALSE;
  }
  g_string_append (json, "],");
  g_type_class_unref (klass);
}

static void
append_last (GString * json, const PmValues * v, GstClockTime now)
{
  if (!v->have_buffer) {
    g_string_append (json, "\"last\":null,");
    return;
  }
  g_string_append (json, "\"last\":{");
  append_value (json, "pts", v->pts);
  append_value (json, "dts", v->dts);
  append_value (json, "duration", v->duration);
  append_value (json, "running_time", v->running_time);
  if (v->have_delay)
    g_string_append_printf (json, "\"delay\":%" G_GINT64_FORMAT ",",
        v->delay);
  else
    g_string_append (json, "\"delay\":null,");
  append_value (json, "size", v->size);
  append_value (json, "offset", v->offset);
  append_value (json, "offset_end", v->offset_end);
  append_flags (json, v->flags);
  /* How long ago it passed. */
  append_value (json, "age", now - v->arrival);
  close_object (json);
  g_string_append_c (json, ',');
}

static void
append_segment (GString * json, const PmValues * v)
{
  const GstSegment *s = &v->segment;

  if (!v->have_segment) {
    g_string_append (json, "\"segment\":null,");
    return;
  }
  g_string_append_printf (json, "\"segment\":{\"format\":\"%s\",",
      gst_format_get_name (s->format));
  append_double (json, "rate", s->rate);
  append_value (json, "start", s->start);
  append_value (json, "stop", s->stop);
  append_value (json, "base", s->base);
  close_object (json);
  g_string_append_c (json, ',');
}

static const PmSlot *
slot_back (PmEntry * entry, guint back)
{
  return &entry->window[(entry->ticks - 1 - back) % HISTORY_SLOTS];
}

/* Rates and gaps over the newest closed slots: at least WINDOW_SLOTS, more
 * until there are two buffers. */
static void
append_window (GString * json, PmEntry * entry)
{
  guint available = MIN (entry->ticks, HISTORY_SLOTS), n = 0;
  PmSlot sum = { 0, };
  gdouble seconds;

  if (available == 0) {
    g_string_append (json, "\"window\":null,");
    return;
  }
  while (n < available && (n < WINDOW_SLOTS || sum.buffers < 2)) {
    const PmSlot *slot = slot_back (entry, n++);

    sum.buffers += slot->buffers;
    sum.bytes += slot->bytes;
    if (slot->gaps > 0) {
      sum.gap_min = sum.gaps == 0 ? slot->gap_min :
          MIN (sum.gap_min, slot->gap_min);
      sum.gap_max = MAX (sum.gap_max, slot->gap_max);
      sum.gap_sum += slot->gap_sum;
      sum.gaps += slot->gaps;
    }
  }
  seconds = (gdouble) (slot_back (entry, 0)->end -
      slot_back (entry, n - 1)->begin) / GST_SECOND;

  g_string_append (json, "\"window\":{");
  append_double (json, "seconds", seconds);
  append_double (json, "buffers_per_s", seconds > 0 ? sum.buffers / seconds
      : 0);
  append_double (json, "bytes_per_s", seconds > 0 ? sum.bytes / seconds : 0);
  append_value (json, "gap_min", sum.gaps ? sum.gap_min : G_MAXUINT64);
  append_value (json, "gap_avg", sum.gaps ? sum.gap_sum / sum.gaps :
      G_MAXUINT64);
  append_value (json, "gap_max", sum.gaps ? sum.gap_max : G_MAXUINT64);
  close_object (json);
  g_string_append_c (json, ',');
}

static void
append_entry (GString * json, PmEntry * entry, GstClockTime now)
{
  PmValues v;
  guint i;

  g_mutex_lock (&entry->stats->lock);
  v = entry->stats->values;
  g_mutex_unlock (&entry->stats->lock);

  g_string_append_c (json, '{');
  append_value (json, "pad", entry->pad_id);
  append_value (json, "buffers", v.buffers);
  append_value (json, "bytes", v.bytes);
  append_last (json, &v, now);
  append_segment (json, &v);
  append_window (json, entry);
  g_string_append (json, "\"events\":{");
  for (i = 0; i < N_EVENTS; i++)
    append_value (json, event_names[i], v.events[i]);
  close_object (json);
  g_string_append_c (json, '}');
}

/* The client's pads as {"pads":[...]}, or NULL if it watches none. A pad
 * that is gone is reported once and then dropped for this client. */
gchar *
pm_monitor_stats_json (PmMonitor * monitor, gpointer client)
{
  GstClockTime now = gst_util_get_timestamp ();
  GString *json;
  GPtrArray *entries;
  guint i;

  g_mutex_lock (&monitor->lock);
  entries = entries_of (monitor, client);
  if (entries->len == 0) {
    g_ptr_array_unref (entries);
    g_mutex_unlock (&monitor->lock);
    return NULL;
  }
  g_ptr_array_sort (entries, compare_entries);

  json = g_string_new ("{\"pads\":[");
  for (i = 0; i < entries->len; i++) {
    PmEntry *entry = g_ptr_array_index (entries, i);
    GstPad *pad = g_weak_ref_get (&entry->pad);

    if (i > 0)
      g_string_append_c (json, ',');
    if (pad == NULL) {
      g_string_append_printf (json, "{\"pad\":%" G_GUINT64_FORMAT
          ",\"gone\":true}", entry->pad_id);
      remove_client_from (monitor, entry, client);
      continue;
    }
    append_entry (json, entry, now);
    gst_object_unref (pad);
  }
  g_string_append (json, "]}");
  g_ptr_array_unref (entries);
  g_mutex_unlock (&monitor->lock);
  return g_string_free (json, FALSE);
}

/* All watched pads with their number of clients, for GET /api/monitors. */
gchar *
pm_monitor_list_json (PmMonitor * monitor)
{
  GString *json = g_string_new ("{\"pads\":[");
  GPtrArray *entries;
  guint i;

  g_mutex_lock (&monitor->lock);
  entries = entries_of (monitor, NULL);
  g_ptr_array_sort (entries, compare_entries);
  for (i = 0; i < entries->len; i++) {
    PmEntry *entry = g_ptr_array_index (entries, i);

    g_string_append_printf (json, "%s{\"pad\":%" G_GUINT64_FORMAT
        ",\"viewers\":%u,\"buffers\":%" G_GUINT64_FORMAT "}", i ? "," : "",
        entry->pad_id, g_hash_table_size (entry->clients),
        read_buffers (entry));
  }
  g_ptr_array_unref (entries);
  g_mutex_unlock (&monitor->lock);
  g_string_append (json, "]}");
  return g_string_free (json, FALSE);
}
