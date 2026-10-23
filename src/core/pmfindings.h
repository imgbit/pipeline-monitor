/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef __PM_FINDINGS_H__
#define __PM_FINDINGS_H__

#include "pmmodel.h"

G_BEGIN_DECLS

/* A pipeline counts as stuck when its pending state is not reached after
 * this long. */
#define PM_STUCK_AFTER_US (3 * G_USEC_PER_SEC)

gchar *pm_findings_to_json (PmModel * model, gint64 stuck_after_us);

G_END_DECLS

#endif /* __PM_FINDINGS_H__ */
