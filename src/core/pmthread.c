/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "pmthread.h"

#ifdef G_OS_WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif
#ifdef __linux__
#include <sys/syscall.h>
#include <unistd.h>
#endif

guint64
pm_thread_os_id (void)
{
#if defined(G_OS_WIN32)
  return GetCurrentThreadId ();
#elif defined(__APPLE__)
  guint64 id = 0;

  pthread_threadid_np (NULL, &id);
  return id;
#elif defined(__linux__)
  return (guint64) syscall (SYS_gettid);
#else
  return (guint64) (guintptr) pthread_self ();
#endif
}

gchar *
pm_thread_os_name (void)
{
#if defined(G_OS_WIN32)
  /* GetThreadDescription needs Windows 10 1607; GStreamer's tasks are
   * named from their messages instead. */
  return g_strdup ("");
#else
  char name[64] = "";

  if (pthread_getname_np (pthread_self (), name, sizeof (name)) != 0)
    name[0] = '\0';
  return g_strdup (name);
#endif
}
