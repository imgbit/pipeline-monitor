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
/* The process's main thread as the operating system knows it (Linux: the
 * thread whose id is the process id; macOS: pthread_main_thread_np;
 * Windows: the thread that began at the program's entry point), 0 where it
 * cannot be told. */
guint64 pm_thread_main_os_id (void);

/* One thread of this process at a moment. */
typedef struct
{
  guint64 os_id;
  /* "" if it has none. */
  gchar *name;
  /* User and system time so far. */
  guint64 cpu_ns;
} PmThreadSample;

/* Every thread of this process now, as PmThreadSample (names freed with
 * the array); NULL where the platform cannot list them. Reads only this
 * process, so it needs no privileges. */
GArray *pm_thread_sample_all (void);

/* This process as a whole: user and system time so far, and the memory the
 * operating system counts for it (resident on Linux, footprint on macOS,
 * working set on Windows); FALSE where it cannot be read. */
gboolean pm_process_sample (guint64 * cpu_ns, guint64 * memory);

G_END_DECLS

#endif
