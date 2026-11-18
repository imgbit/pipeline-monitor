/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pmperf.h"
#include "pmjson.h"
#include "pmthread.h"

/* CPU per thread, read by the server once a second (docs/protocol.md,
 * "performance"). */

struct _PmPerf
{
  PmModel *model;
  /* os id -> CPU ns at the last sample. */
  GHashTable *last;
  gint64 last_us;
};

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
  return perf;
}

void
pm_perf_free (PmPerf * perf)
{
  g_hash_table_unref (perf->last);
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
  GArray *all = pm_thread_sample_all ();
  gint64 now = g_get_monotonic_time ();
  gdouble seconds = perf->last_us ? (now - perf->last_us) / 1e6 : 0;
  GHashTable *next = g_hash_table_new_full (g_int64_hash, g_int64_equal,
      g_free, g_free);
  GArray *rows = g_array_new (FALSE, FALSE, sizeof (PmRow));
  GString *json = g_string_new (NULL);
  guint i;

  g_string_append_printf (json, "{\"cores\":%u,\"threads\":",
      g_get_num_processors ());
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
      g_string_append_printf (json, ",\"id\":%d,\"task\":%s", id,
          task ? "true" : "false");
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

done:
  g_hash_table_unref (perf->last);
  perf->last = next;
  perf->last_us = now;
  g_array_unref (rows);
  if (all != NULL)
    g_array_unref (all);
  return g_string_free (json, FALSE);
}
