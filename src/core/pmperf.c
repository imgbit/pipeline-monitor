/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pmperf.h"
#include "pmjson.h"
#include "pmthread.h"

/* CPU per thread and the process's CPU and memory, read by the server once
 * a second (docs/protocol.md, "performance"). */

struct _PmPerf
{
  PmModel *model;
  /* os id -> CPU ns at the last sample. */
  GHashTable *last;
  gint64 last_us;
  /* The process's CPU ns at the last sample, 0 if none. */
  guint64 last_cpu;
  /* PmBusy of the threads the graph knows, from the last sample. */
  GPtrArray *busy;
};

typedef struct
{
  guint64 os_id;
  gint id;
  gchar *name;
  gdouble cpu;
  /* Samples in a row at PM_PERF_BUSY or more. */
  guint samples;
} PmBusy;

static void
busy_free (PmBusy * b)
{
  g_free (b->name);
  g_free (b);
}

typedef struct
{
  PmThreadSample *sample;
  /* Share of one core since the last sample, < 0 if not known yet. */
  gdouble cpu;
} PmRow;

PmPerf *
pm_perf_new (PmModel * model)
{
  PmPerf *perf = g_new0 (PmPerf, 1);

  perf->model = model;
  perf->last = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free,
      g_free);
  perf->busy = g_ptr_array_new_with_free_func ((GDestroyNotify) busy_free);
  return perf;
}

void
pm_perf_free (PmPerf * perf)
{
  g_hash_table_unref (perf->last);
  g_ptr_array_unref (perf->busy);
  g_free (perf);
}

/* Busiest first, the ones without a value yet last. */
static gint
compare_rows (gconstpointer a, gconstpointer b)
{
  const PmRow *ra = a, *rb = b;

  return (ra->cpu < rb->cpu) - (ra->cpu > rb->cpu);
}

gchar *
pm_perf_sample (PmPerf * perf)
{
  gint64 now = g_get_monotonic_time ();
  GArray *all = pm_thread_sample_all ();
  gdouble seconds = perf->last_us ? (now - perf->last_us) / 1e6 : 0;
  GHashTable *next = g_hash_table_new_full (g_int64_hash, g_int64_equal,
      g_free, g_free);
  GArray *rows = g_array_new (FALSE, FALSE, sizeof (PmRow));
  GString *json = g_string_new (NULL);
  GPtrArray *busy = g_ptr_array_new_with_free_func ((GDestroyNotify)
      busy_free);
  guint64 cpu_ns, memory;
  guint i;

  g_string_append_printf (json, "{\"cores\":%u,\"process\":",
      g_get_num_processors ());
  if (pm_process_sample (&cpu_ns, &memory)) {
    gchar buf[G_ASCII_DTOSTR_BUF_SIZE];

    g_string_append (json, "{\"cpu\":");
    if (perf->last_cpu > 0 && seconds > 0 && cpu_ns >= perf->last_cpu)
      g_string_append (json, g_ascii_formatd (buf, sizeof (buf), "%.3f",
              (cpu_ns - perf->last_cpu) / 1e9 / seconds));
    else
      g_string_append (json, "null");
    g_string_append_printf (json, ",\"memory\":%" G_GUINT64_FORMAT
        ",\"threads\":", memory);
    if (all != NULL)
      g_string_append_printf (json, "%u}", all->len);
    else
      g_string_append (json, "null}");
    perf->last_cpu = cpu_ns;
  } else {
    g_string_append (json, "null");
  }
  g_string_append (json, ",\"threads\":");
  if (all == NULL) {
    /* The platform cannot list threads. */
    g_string_append (json, "null}");
    goto done;
  }
  for (i = 0; i < all->len; i++) {
    PmThreadSample *s = &g_array_index (all, PmThreadSample, i);
    guint64 *before = g_hash_table_lookup (perf->last, &s->os_id);
    PmRow row = { s, -1 };

    if (before != NULL && seconds > 0 && s->cpu_ns >= *before)
      row.cpu = (s->cpu_ns - *before) / 1e9 / seconds;
    g_array_append_val (rows, row);
    g_hash_table_insert (next, g_memdup2 (&s->os_id, sizeof (guint64)),
        g_memdup2 (&s->cpu_ns, sizeof (guint64)));
  }
  g_array_sort (rows, compare_rows);

  g_string_append_c (json, '[');
  for (i = 0; i < rows->len; i++) {
    PmRow *row = &g_array_index (rows, PmRow, i);
    gint id = 0;
    gchar *name = NULL;
    gboolean task = FALSE;
    gchar buf[G_ASCII_DTOSTR_BUF_SIZE];

    if (i > 0)
      g_string_append_c (json, ',');
    g_string_append_printf (json, "{\"os\":%" G_GUINT64_FORMAT,
        row->sample->os_id);
    /* A thread the graph knows: its id there, and its task's name. */
    if (pm_model_thread_info (perf->model, row->sample->os_id, &id, &name,
            &task)) {
      PmBusy *b = g_new0 (PmBusy, 1);
      guint k;

      g_string_append_printf (json, ",\"id\":%d,\"task\":%s", id,
          task ? "true" : "false");
      b->os_id = row->sample->os_id;
      b->id = id;
      /* Windows names no threads but GStreamer's tasks. */
      b->name = name && *name ? g_strdup (name) : *row->sample->name ?
          g_strdup (row->sample->name) : g_strdup_printf ("%"
          G_GUINT64_FORMAT, row->sample->os_id);
      b->cpu = row->cpu;
      for (k = 0; k < perf->busy->len; k++) {
        PmBusy *before = g_ptr_array_index (perf->busy, k);

        if (before->os_id == b->os_id)
          b->samples = before->samples;
      }
      b->samples = row->cpu >= PM_PERF_BUSY ? b->samples + 1 : 0;
      g_ptr_array_add (busy, b);
    }
    g_string_append (json, ",\"name\":");
    pm_json_append_string (json, name && *name ? name : row->sample->name);
    g_string_append (json, ",\"cpu\":");
    if (row->cpu < 0)
      g_string_append (json, "null");
    else
      g_string_append (json, g_ascii_formatd (buf, sizeof (buf), "%.3f",
              row->cpu));
    g_string_append_c (json, '}');
    g_free (name);
  }
  g_string_append (json, "]}");
  pm_model_forget_threads (perf->model, all, now);

done:
  g_ptr_array_unref (perf->busy);
  perf->busy = busy;
  g_hash_table_unref (perf->last);
  perf->last = next;
  perf->last_us = now;
  g_array_unref (rows);
  if (all != NULL)
    g_array_unref (all);
  return g_string_free (json, FALSE);
}

static void
busy_thread_free (PmBusyThread * b)
{
  g_free (b->name);
  g_free (b);
}

GPtrArray *
pm_perf_busy (PmPerf * perf, guint samples)
{
  GPtrArray *found = g_ptr_array_new_with_free_func ((GDestroyNotify)
      busy_thread_free);
  guint i;

  for (i = 0; i < perf->busy->len; i++) {
    PmBusy *b = g_ptr_array_index (perf->busy, i);
    PmBusyThread *t;

    if (b->samples < samples)
      continue;
    t = g_new0 (PmBusyThread, 1);
    t->id = b->id;
    t->name = g_strdup (b->name);
    t->cpu = b->cpu;
    g_ptr_array_add (found, t);
  }
  return found;
}
