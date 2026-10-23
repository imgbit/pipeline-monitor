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

PmFindings *pm_findings_new (PmModel * model);
void pm_findings_free (PmFindings * findings);
gchar *pm_findings_tick (PmFindings * findings, gint64 stuck_after_us);

G_END_DECLS

#endif /* __PM_FINDINGS_H__ */
