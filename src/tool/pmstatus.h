/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef __PM_STATUS_H__
#define __PM_STATUS_H__

#include <glib.h>

G_BEGIN_DECLS

/* pipeline-monitor status [options] [FILE]: argv[0] is "status". */
gint pm_status_main (gint argc, gchar ** argv);

G_END_DECLS

#endif /* __PM_STATUS_H__ */
