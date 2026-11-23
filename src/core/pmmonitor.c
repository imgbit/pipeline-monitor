/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pmmonitor.h"
#include "pmaudio.h"
#include "pmjson.h"
#include "pmlevels.h"

#include <math.h>
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
  guint64 lists, list_buffers;
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
  /* Replaced at CAPS events by the streaming thread, under the lock; the
   * probe reads it there without the lock (events and buffers of a pad
   * are not probed at the same time). */
  PmAudioMeter *audio;
  gchar *audio_reason;
  guint audio_seq;
};

typedef struct _PmEntry PmEntry;

struct _PmEntry
{
  guint64 pad_id;
  /* Known when watching started, kept for when the pad is gone. */
  gchar *pad_path;
  GWeakRef pad;
  gulong probe;
  PmPadStats *stats;
  GHashTable *clients;
  PmSlot window[HISTORY_SLOTS];
  guint64 ticks;
  /* Copied from the stats at each audio tick, for the messages. */
  guint audio_seq;
  gboolean audio_metered;
  GstAudioInfo audio_info;
  guint audio_shown;
  gchar *audio_reason;
  PmAudioLevel audio_levels[PM_AUDIO_SHOWN];
  /* What this round sends: new levels, and the format or the reason
   * (again) after a change or a new client. */
  gboolean audio_fresh, audio_announce, audio_new_client;
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
  pm_audio_meter_free (stats->audio);
  g_free (stats->audio_reason);
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

static void
set_audio (PmPadStats * stats, GstCaps * caps)
{
  gchar *reason;
  PmAudioMeter *meter = pm_audio_meter_new (caps, &reason), *old;

  g_mutex_lock (&stats->lock);
  old = stats->audio;
  stats->audio = meter;
  g_free (stats->audio_reason);
  stats->audio_reason = reason;
  stats->audio_seq++;
  g_mutex_unlock (&stats->lock);
  pm_audio_meter_free (old);
}

/* The levels of a buffer, measured before taking the lock. */
static gboolean
measure_audio (PmPadStats * stats, GstBuffer * buffer, PmAudioSum * sums)
{
  if (stats->audio == NULL)
    return FALSE;
  return pm_audio_measure (pm_audio_meter_info (stats->audio),
      pm_audio_meter_shown (stats->audio), buffer, sums);
}

/* Runs on the streaming thread for every buffer: keep it short. */
static GstPadProbeReturn
on_probe (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  PmPadStats *stats = user_data;
  GstClockTime arrival;
  PmAudioSum audio[PM_AUDIO_SHOWN], *audio_list = NULL;
  guint audio_buffers = 0;
  gboolean measured = FALSE;

  if (info->type & GST_PAD_PROBE_TYPE_EVENT_BOTH) {
    GstEvent *event = GST_PAD_PROBE_INFO_EVENT (info);

    if (GST_EVENT_TYPE (event) == GST_EVENT_CAPS) {
      GstCaps *caps;

      gst_event_parse_caps (event, &caps);
      set_audio (stats, caps);
    }
    g_mutex_lock (&stats->lock);
    count_event (stats, event);
    g_mutex_unlock (&stats->lock);
    return GST_PAD_PROBE_OK;
  }

  if (stats->audio != NULL) {
    if (info->type & GST_PAD_PROBE_TYPE_BUFFER_LIST) {
      GstBufferList *list = GST_PAD_PROBE_INFO_BUFFER_LIST (info);
      guint n = gst_buffer_list_length (list);

      audio_list = g_new0 (PmAudioSum, (gsize) n * PM_AUDIO_SHOWN);
      for (guint i = 0; i < n; i++)
        measured |= measure_audio (stats, gst_buffer_list_get (list, i),
            audio_list + i * PM_AUDIO_SHOWN);
      audio_buffers = n;
    } else {
      measured = measure_audio (stats, GST_PAD_PROBE_INFO_BUFFER (info),
          audio);
      audio_buffers = 1;
    }
  }

  arrival = gst_util_get_timestamp ();
  g_mutex_lock (&stats->lock);
  if (measured) {
    for (guint i = 0; i < audio_buffers; i++)
      pm_audio_meter_add (stats->audio,
          audio_list ? audio_list + i * PM_AUDIO_SHOWN : audio);
  }
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
    stats->values.lists++;
    stats->values.list_buffers += n;
    if (n > 0)
      set_last (stats, gst_buffer_list_get (list, n - 1));
  } else {
    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER (info);

    count_buffer (stats, buffer);
    set_last (stats, buffer);
  }
  g_mutex_unlock (&stats->lock);
  g_free (audio_list);
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

  /* A pad watched mid-stream already had its caps and segment. */
  {
    GstCaps *caps = gst_pad_get_current_caps (pad);

    if (caps != NULL) {
      gchar *reason;

      stats->audio = pm_audio_meter_new (caps, &reason);
      stats->audio_reason = reason;
      gst_caps_unref (caps);
    }
  }

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
  g_free (entry->pad_path);
  g_hash_table_unref (entry->clients);
  stats_unref (entry->stats);
  g_free (entry->audio_reason);
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
    entry->audio_new_client = TRUE;
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
  entry->pad_path = pm_model_path (monitor->model, pad_id);
  g_weak_ref_init (&entry->pad, pad);
  entry->clients = g_hash_table_new (NULL, NULL);
  g_hash_table_add (entry->clients, client);
  entry->stats = stats_new (pad);
  entry->audio_seq = G_MAXUINT;
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

static void
append_double (GString * json, const gchar * key, gdouble value)
{
  gchar buf[G_ASCII_DTOSTR_BUF_SIZE];

  g_string_append_printf (json, "\"%s\":%s,", key,
      g_ascii_formatd (buf, sizeof (buf), "%.3f", value));
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
  pm_json_append_value (json, "pts", v->pts);
  pm_json_append_value (json, "dts", v->dts);
  pm_json_append_value (json, "duration", v->duration);
  pm_json_append_value (json, "running_time", v->running_time);
  if (v->have_delay)
    g_string_append_printf (json, "\"delay\":%" G_GINT64_FORMAT ",",
        v->delay);
  else
    g_string_append (json, "\"delay\":null,");
  pm_json_append_value (json, "size", v->size);
  pm_json_append_value (json, "offset", v->offset);
  pm_json_append_value (json, "offset_end", v->offset_end);
  append_flags (json, v->flags);
  /* How long ago it passed. */
  pm_json_append_value (json, "age", now - v->arrival);
  pm_json_close (json, '}');
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
  pm_json_append_value (json, "start", s->start);
  pm_json_append_value (json, "stop", s->stop);
  pm_json_append_value (json, "base", s->base);
  pm_json_close (json, '}');
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
  pm_json_append_value (json, "gap_min", sum.gaps ? sum.gap_min : G_MAXUINT64);
  pm_json_append_value (json, "gap_avg", sum.gaps ? sum.gap_sum / sum.gaps :
      G_MAXUINT64);
  pm_json_append_value (json, "gap_max", sum.gaps ? sum.gap_max : G_MAXUINT64);
  pm_json_close (json, '}');
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
  pm_json_append_value (json, "pad", entry->pad_id);
  pm_json_append_member_string (json, "pad_path", entry->pad_path);
  pm_json_append_value (json, "buffers", v.buffers);
  pm_json_append_value (json, "bytes", v.bytes);
  pm_json_append_value (json, "lists", v.lists);
  pm_json_append_value (json, "list_buffers", v.list_buffers);
  append_last (json, &v, now);
  append_segment (json, &v);
  append_window (json, entry);
  g_string_append (json, "\"events\":{");
  for (i = 0; i < N_EVENTS; i++)
    pm_json_append_value (json, event_names[i], v.events[i]);
  pm_json_close (json, '}');
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

  json = g_string_new (NULL);
  g_string_append (json, "{\"pads\":[");
  for (i = 0; i < entries->len; i++) {
    PmEntry *entry = g_ptr_array_index (entries, i);
    GstPad *pad = g_weak_ref_get (&entry->pad);

    if (i > 0)
      g_string_append_c (json, ',');
    if (pad == NULL) {
      g_string_append_printf (json, "{\"pad\":%" G_GUINT64_FORMAT
          ",\"pad_path\":", entry->pad_id);
      pm_json_append_string (json, entry->pad_path);
      g_string_append (json, ",\"gone\":true}");
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

/* All watched pads with their number of clients, for GET /api/pads. */
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
        ",\"pad_path\":", i ? "," : "", entry->pad_id);
    pm_json_append_string (json, entry->pad_path);
    g_string_append_printf (json, ",\"viewers\":%u,\"buffers\":%"
        G_GUINT64_FORMAT "}", g_hash_table_size (entry->clients),
        read_buffers (entry));
  }
  g_ptr_array_unref (entries);
  g_mutex_unlock (&monitor->lock);
  g_string_append (json, "]}");
  return g_string_free (json, FALSE);
}

guint
pm_monitor_count (PmMonitor * monitor)
{
  guint n;

  g_mutex_lock (&monitor->lock);
  n = g_hash_table_size (monitor->entries);
  g_mutex_unlock (&monitor->lock);
  return n;
}

/* Ends the audio window of every watched pad. Called by the server every
 * 50 ms while pads are watched, before it sends "audio". */
void
pm_monitor_audio_tick (PmMonitor * monitor)
{
  GstClockTime now = gst_util_get_timestamp ();
  GHashTableIter iter;
  gpointer value;

  g_mutex_lock (&monitor->lock);
  g_hash_table_iter_init (&iter, monitor->entries);
  while (g_hash_table_iter_next (&iter, NULL, &value)) {
    PmEntry *entry = value;
    PmPadStats *stats = entry->stats;

    entry->audio_announce = entry->audio_new_client;
    entry->audio_new_client = FALSE;
    g_mutex_lock (&stats->lock);
    if (stats->audio_seq != entry->audio_seq) {
      entry->audio_seq = stats->audio_seq;
      entry->audio_metered = stats->audio != NULL;
      if (stats->audio != NULL) {
        entry->audio_info = *pm_audio_meter_info (stats->audio);
        entry->audio_shown = pm_audio_meter_shown (stats->audio);
      }
      g_free (entry->audio_reason);
      entry->audio_reason = g_strdup (stats->audio_reason);
      entry->audio_announce = TRUE;
    }
    entry->audio_fresh = stats->audio != NULL &&
        pm_audio_meter_take (stats->audio, now, entry->audio_levels);
    g_mutex_unlock (&stats->lock);
  }
  g_mutex_unlock (&monitor->lock);
}

static const gchar *
channel_label (GstAudioChannelPosition position, guint index, gchar * buf)
{
  static const struct
  {
    GstAudioChannelPosition position;
    const gchar *label;
  } labels[] = {
    {GST_AUDIO_CHANNEL_POSITION_MONO, "mono"},
    {GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT, "FL"},
    {GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT, "FR"},
    {GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER, "C"},
    {GST_AUDIO_CHANNEL_POSITION_LFE1, "LFE"},
    {GST_AUDIO_CHANNEL_POSITION_LFE2, "LFE2"},
    {GST_AUDIO_CHANNEL_POSITION_REAR_LEFT, "RL"},
    {GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT, "RR"},
    {GST_AUDIO_CHANNEL_POSITION_REAR_CENTER, "RC"},
    {GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT, "SL"},
    {GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT, "SR"},
    {GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER, "FLC"},
    {GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER, "FRC"},
    {GST_AUDIO_CHANNEL_POSITION_TOP_FRONT_LEFT, "TFL"},
    {GST_AUDIO_CHANNEL_POSITION_TOP_FRONT_RIGHT, "TFR"},
    {GST_AUDIO_CHANNEL_POSITION_TOP_FRONT_CENTER, "TFC"},
    {GST_AUDIO_CHANNEL_POSITION_TOP_CENTER, "TC"},
    {GST_AUDIO_CHANNEL_POSITION_TOP_REAR_LEFT, "TRL"},
    {GST_AUDIO_CHANNEL_POSITION_TOP_REAR_RIGHT, "TRR"},
    {GST_AUDIO_CHANNEL_POSITION_TOP_SIDE_LEFT, "TSL"},
    {GST_AUDIO_CHANNEL_POSITION_TOP_SIDE_RIGHT, "TSR"},
    {GST_AUDIO_CHANNEL_POSITION_TOP_REAR_CENTER, "TRC"},
    {GST_AUDIO_CHANNEL_POSITION_WIDE_LEFT, "WL"},
    {GST_AUDIO_CHANNEL_POSITION_WIDE_RIGHT, "WR"},
    {GST_AUDIO_CHANNEL_POSITION_SURROUND_LEFT, "SuL"},
    {GST_AUDIO_CHANNEL_POSITION_SURROUND_RIGHT, "SuR"},
  };

  for (guint i = 0; i < G_N_ELEMENTS (labels); i++)
    if (labels[i].position == position)
      return labels[i].label;
  g_snprintf (buf, 16, "%u", index + 1);
  return buf;
}

static void
append_db (GString * json, gdouble db)
{
  if (isfinite (db))
    g_string_append_printf (json, "%.2f", db);
  else
    g_string_append (json, "null");
}

static void
append_audio (GString * json, PmEntry * entry, GstClockTime now)
{
  const GstAudioInfo *info = &entry->audio_info;
  gchar buf[16];
  guint c;

  g_string_append_c (json, '{');
  pm_json_append_value (json, "pad", entry->pad_id);
  pm_json_append_member_string (json, "pad_path", entry->pad_path);
  if (!entry->audio_metered) {
    pm_json_append_member_string (json, "unavailable", entry->audio_reason);
    pm_json_close (json, '}');
    return;
  }
  pm_json_append_member_string (json, "format",
      gst_audio_format_to_string (GST_AUDIO_INFO_FORMAT (info)));
  pm_json_append_value (json, "rate", GST_AUDIO_INFO_RATE (info));
  pm_json_append_value (json, "channels", GST_AUDIO_INFO_CHANNELS (info));
  g_string_append_printf (json, "\"interleaved\":%s,",
      GST_AUDIO_INFO_LAYOUT (info) == GST_AUDIO_LAYOUT_INTERLEAVED ?
      "true" : "false");
  g_string_append (json, "\"positions\":[");
  for (c = 0; c < (guint) GST_AUDIO_INFO_CHANNELS (info); c++) {
    if (c > 0)
      g_string_append_c (json, ',');
    pm_json_append_string (json, GST_AUDIO_INFO_IS_UNPOSITIONED (info) ?
        (g_snprintf (buf, sizeof (buf), "%u", c + 1), buf) :
        channel_label (info->position[c], c, buf));
  }
  g_string_append (json, "],");
  pm_json_append_value (json, "shown", entry->audio_shown);
  if (!entry->audio_fresh) {
    g_string_append (json, "\"levels\":null}");
    return;
  }
  /* One window per value; a recording may hold several. */
  g_string_append (json, "\"levels\":[");
  for (c = 0; c < entry->audio_shown; c++) {
    const PmAudioLevel *l = &entry->audio_levels[c];

    g_string_append (json, c ? ",{\"peak\":[" : "{\"peak\":[");
    append_db (json, l->peak_db);
    g_string_append (json, "],\"rms\":[");
    append_db (json, l->rms_db);
    g_string_append (json, "],");
    pm_json_append_value (json, "clipped", l->clipped);
    pm_json_append_value (json, "clipped_total", l->clipped_total);
    if (GST_CLOCK_TIME_IS_VALID (l->silent_since))
      g_string_append_printf (json, "\"silent_for\":%" G_GUINT64_FORMAT ","
          "\"digital_silence\":%s}", now - l->silent_since,
          l->digital_silence ? "true" : "false");
    else
      g_string_append (json, "\"silent_for\":null,"
          "\"digital_silence\":false}");
  }
  g_string_append (json, "]}");
}

/* The client's audio pads with something to send this round, as
 * {"audio":[...]}, or NULL. */
gchar *
pm_monitor_audio_json (PmMonitor * monitor, gpointer client)
{
  GstClockTime now = gst_util_get_timestamp ();
  GString *json = NULL;
  GPtrArray *entries;
  guint i;

  g_mutex_lock (&monitor->lock);
  entries = entries_of (monitor, client);
  g_ptr_array_sort (entries, compare_entries);
  for (i = 0; i < entries->len; i++) {
    PmEntry *entry = g_ptr_array_index (entries, i);

    if (!entry->audio_fresh && !(entry->audio_announce &&
            (entry->audio_metered || entry->audio_reason != NULL)))
      continue;
    if (json == NULL)
      json = g_string_new ("{\"audio\":[");
    else
      g_string_append_c (json, ',');
    append_audio (json, entry, now);
  }
  g_ptr_array_unref (entries);
  g_mutex_unlock (&monitor->lock);
  if (json == NULL)
    return NULL;
  g_string_append (json, "]}");
  return g_string_free (json, FALSE);
}
