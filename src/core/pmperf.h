/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef __PM_PERF_H__
#define __PM_PERF_H__

#include "pmmodel.h"

G_BEGIN_DECLS

typedef struct _PmPerf PmPerf;

PmPerf *pm_perf_new (PmModel * model);
void pm_perf_free (PmPerf * perf);
/* Samples every thread of the process; the body of a "performance"
 * message, with each thread's share of one core since the last sample. */
gchar *pm_perf_sample (PmPerf * perf);

/* A thread at this share of one core or more cannot go faster. */
#define PM_PERF_BUSY 0.90

typedef struct
{
  /* The graph's id of the thread, its name and its last share. */
  gint id;
  gchar *name;
  gdouble cpu;
} PmBusyThread;

/* Threads the graph knows that were busy in each of the last samples. */
GPtrArray *pm_perf_busy (PmPerf * perf, guint samples);

G_END_DECLS

#endif
