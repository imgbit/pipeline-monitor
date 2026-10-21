/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef __PM_MONITOR_H__
#define __PM_MONITOR_H__

#include "pmmodel.h"

G_BEGIN_DECLS

/* Pads watched by clients (browsers), each with a pad probe. A pad has one
 * probe however many clients watch it. */
typedef struct _PmMonitor PmMonitor;

PmMonitor *pm_monitor_new (PmModel * model);
void pm_monitor_free (PmMonitor * monitor);

gboolean pm_monitor_add (PmMonitor * monitor, gpointer client, guint64 pad);
void pm_monitor_remove (PmMonitor * monitor, gpointer client, guint64 pad);
void pm_monitor_remove_client (PmMonitor * monitor, gpointer client);

void pm_monitor_tick (PmMonitor * monitor);
gchar *pm_monitor_stats_json (PmMonitor * monitor, gpointer client);
gchar *pm_monitor_list_json (PmMonitor * monitor);

G_END_DECLS

#endif /* __PM_MONITOR_H__ */
