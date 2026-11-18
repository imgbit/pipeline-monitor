/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef __PM_FINDINGS_H__
#define __PM_FINDINGS_H__

#include "pmmodel.h"

G_BEGIN_DECLS

/* A pipeline counts as stuck when its pending state is not reached after
 * this long. */
#define PM_STUCK_AFTER_US (3 * G_USEC_PER_SEC)

typedef struct _PmFindings PmFindings;

/* A thread this busy for this many seconds is reported. */
#define PM_BUSY_SECONDS 5

typedef struct _PmPerf PmPerf;

PmFindings *pm_findings_new (PmModel * model);
/* Threads' CPU, for the busy-thread finding (NULL: none). */
void pm_findings_set_perf (PmFindings * findings, PmPerf * perf);

/* Elements out of their pipeline this long and alive are reported; they
 * pile up when their count grew over each of the last two windows. */
#define PM_LEFTOVER_GRACE_US (30 * G_USEC_PER_SEC)
#define PM_LEFTOVER_WINDOW_US (300 * G_USEC_PER_SEC)
/* Other times, for tests. */
void pm_findings_set_leftover_times (PmFindings * findings, gint64 grace_us,
    gint64 window_us);
void pm_findings_free (PmFindings * findings);
gchar *pm_findings_tick (PmFindings * findings, gint64 stuck_after_us);

G_END_DECLS

#endif /* __PM_FINDINGS_H__ */
