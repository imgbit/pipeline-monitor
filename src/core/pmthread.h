/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef __PM_THREAD_H__
#define __PM_THREAD_H__

#include <glib.h>

G_BEGIN_DECLS

/* The calling thread as the operating system knows it. */
guint64 pm_thread_os_id (void);
/* Its name, "" if it has none. */
gchar *pm_thread_os_name (void);

G_END_DECLS

#endif
