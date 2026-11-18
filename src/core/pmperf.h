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

G_END_DECLS

#endif
