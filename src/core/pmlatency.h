/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef __PM_LATENCY_H__
#define __PM_LATENCY_H__

#include "pmmodel.h"

G_BEGIN_DECLS

/* How long data takes through elements and between pads, measured on
 * demand by matching running times at pad probes. */
typedef struct _PmLatency PmLatency;

PmLatency *pm_latency_new (PmModel * model);
void pm_latency_free (PmLatency * latency);

void pm_latency_measure_element (PmLatency * latency, gpointer client,
    guint64 element, gboolean on);
void pm_latency_measure_path (PmLatency * latency, gpointer client,
    guint64 from, guint64 to, gboolean on);
void pm_latency_measure_all (PmLatency * latency, gpointer client,
    gboolean on);
void pm_latency_remove_client (PmLatency * latency, gpointer client);

void pm_latency_set_all_limit (PmLatency * latency, guint buffers_per_s);
gchar *pm_latency_tick (PmLatency * latency);

G_END_DECLS

#endif /* __PM_LATENCY_H__ */
