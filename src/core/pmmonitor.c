/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pmmonitor.h"

/* Filled by the probe on the streaming thread, read by the server. Shared
 * by the entry and the probe; freed when both are done with it. */
typedef struct _PmPadStats PmPadStats;

struct _PmPadStats
{
  gint ref;
  GMutex lock;
  guint64 buffers;
};

typedef struct _PmEntry PmEntry;

struct _PmEntry
{
  guint64 pad_id;
  GWeakRef pad;
  gulong probe;
  PmPadStats *stats;
  GHashTable *clients;
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
  g_mutex_clear (&stats->lock);
  g_free (stats);
}

/* Runs on the streaming thread for every buffer: keep it short. */
static GstPadProbeReturn
on_probe (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  PmPadStats *stats = user_data;
  guint n = 1;

  if (info->type & GST_PAD_PROBE_TYPE_BUFFER_LIST)
    n = gst_buffer_list_length (GST_PAD_PROBE_INFO_BUFFER_LIST (info));
  g_mutex_lock (&stats->lock);
  stats->buffers += n;
  g_mutex_unlock (&stats->lock);
  return GST_PAD_PROBE_OK;
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
  entry->stats = g_new0 (PmPadStats, 1);
  entry->stats->ref = 2;
  g_mutex_init (&entry->stats->lock);
  entry->probe = gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER |
      GST_PAD_PROBE_TYPE_BUFFER_LIST, on_probe, entry->stats,
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
  buffers = entry->stats->buffers;
  g_mutex_unlock (&entry->stats->lock);
  return buffers;
}

/* The client's pads as {"pads":[...]}, or NULL if it watches none. A pad
 * that is gone is reported once and then dropped for this client. */
gchar *
pm_monitor_stats_json (PmMonitor * monitor, gpointer client)
{
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
    g_string_append_printf (json, "{\"pad\":%" G_GUINT64_FORMAT
        ",\"buffers\":%" G_GUINT64_FORMAT "}", entry->pad_id,
        read_buffers (entry));
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
