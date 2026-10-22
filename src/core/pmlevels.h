/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef __PM_LEVELS_H__
#define __PM_LEVELS_H__

#include "pmmodel.h"

G_BEGIN_DECLS

/* Version of the periodic messages (levels, pad-stats). */
#define PM_SCHEMA 1

gboolean pm_levels_is_queue (GstElement * element);
gchar *pm_levels_to_json (PmModel * model);

G_END_DECLS

#endif /* __PM_LEVELS_H__ */
