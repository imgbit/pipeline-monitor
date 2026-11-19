/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "pmthread.h"

#include <string.h>

#ifdef G_OS_WIN32
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#else
#include <pthread.h>
#include <sys/resource.h>
#endif
#ifdef __linux__
#include <sys/syscall.h>
#include <unistd.h>
#include <stdlib.h>
#endif
#ifdef __APPLE__
#include <mach/mach.h>
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

#if defined(G_OS_WIN32)
/* ThreadQuerySetWin32StartAddress: where a thread began. */
#define THREAD_START_ADDRESS 9
typedef LONG (WINAPI * QueryThread) (HANDLE, int, PVOID, ULONG, PULONG);
#elif defined(__APPLE__)
/* In libSystem and stable for years, but not in the SDK's headers. */
extern pthread_t pthread_main_thread_np (void);
#endif

guint64
pm_thread_main_os_id (void)
{
#if defined(G_OS_WIN32)
  /* The main thread is the one that began at the program's entry point. */
  QueryThread query = (QueryThread) (void (*)(void))
      GetProcAddress (GetModuleHandleW (L"ntdll.dll"),
      "NtQueryInformationThread");
  HMODULE program = GetModuleHandleW (NULL);
  IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *) program;
  IMAGE_NT_HEADERS *nt;
  gpointer entry_point;
  HANDLE snapshot;
  DWORD self = GetCurrentProcessId ();
  THREADENTRY32 entry;
  guint64 found = 0;

  if (query == NULL || program == NULL)
    return 0;
  nt = (IMAGE_NT_HEADERS *) ((guint8 *) program + dos->e_lfanew);
  entry_point = (guint8 *) program + nt->OptionalHeader.AddressOfEntryPoint;
  snapshot = CreateToolhelp32Snapshot (TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE)
    return 0;
  entry.dwSize = sizeof (entry);
  if (Thread32First (snapshot, &entry)) {
    do {
      HANDLE thread;
      gpointer start = NULL;

      if (entry.th32OwnerProcessID != self)
        continue;
      thread = OpenThread (THREAD_QUERY_INFORMATION, FALSE,
          entry.th32ThreadID);
      if (thread == NULL)
        continue;
      if (query (thread, THREAD_START_ADDRESS, &start, sizeof (start),
              NULL) == 0 && start == entry_point)
        found = entry.th32ThreadID;
      CloseHandle (thread);
    } while (found == 0 && Thread32Next (snapshot, &entry));
  }
  CloseHandle (snapshot);
  return found;
#elif defined(__APPLE__)
  guint64 id = 0;

  if (pthread_threadid_np (pthread_main_thread_np (), &id) != 0)
    return 0;
  return id;
#elif defined(__linux__)
  return (guint64) getpid ();
#else
  return 0;
#endif
}

static void
sample_clear (PmThreadSample * sample)
{
  g_free (sample->name);
}

static GArray *
new_samples (void)
{
  GArray *all = g_array_new (FALSE, TRUE, sizeof (PmThreadSample));

  g_array_set_clear_func (all, (GDestroyNotify) sample_clear);
  return all;
}

#if defined(__linux__)
/* /proc/self/task/TID/stat: "TID (name) state ...", utime and stime are
 * the 14th and 15th fields, in clock ticks. */
static gboolean
read_stat (const gchar * tid, PmThreadSample * sample)
{
  gchar *path = g_strdup_printf ("/proc/self/task/%s/stat", tid);
  gchar *text = NULL, *open, *close, **fields;
  gboolean ok = FALSE;

  if (g_file_get_contents (path, &text, NULL, NULL) &&
      (open = strchr (text, '(')) != NULL &&
      (close = strrchr (text, ')')) != NULL && close > open) {
    /* After ") ": field 3 (state) onwards. */
    fields = g_strsplit (close + 2, " ", 14);
    if (g_strv_length (fields) >= 13) {
      guint64 ticks = g_ascii_strtoull (fields[11], NULL, 10) +
          g_ascii_strtoull (fields[12], NULL, 10);

      sample->os_id = g_ascii_strtoull (tid, NULL, 10);
      sample->name = g_strndup (open + 1, close - open - 1);
      sample->cpu_ns = ticks * (G_GUINT64_CONSTANT (1000000000) /
          sysconf (_SC_CLK_TCK));
      ok = TRUE;
    }
    g_strfreev (fields);
  }
  g_free (text);
  g_free (path);
  return ok;
}
#endif

GArray *
pm_thread_sample_all (void)
{
#if defined(__linux__)
  GDir *dir = g_dir_open ("/proc/self/task", 0, NULL);
  GArray *all;
  const gchar *tid;

  if (dir == NULL)
    return NULL;
  all = new_samples ();
  while ((tid = g_dir_read_name (dir)) != NULL) {
    PmThreadSample sample = { 0 };

    if (read_stat (tid, &sample))
      g_array_append_val (all, sample);
  }
  g_dir_close (dir);
  return all;
#elif defined(__APPLE__)
  thread_act_array_t threads;
  mach_msg_type_number_t count;
  GArray *all;
  guint i;

  if (task_threads (mach_task_self (), &threads, &count) != KERN_SUCCESS)
    return NULL;
  all = new_samples ();
  for (i = 0; i < count; i++) {
    thread_identifier_info_data_t id;
    thread_basic_info_data_t basic;
    thread_extended_info_data_t extended;
    mach_msg_type_number_t n = THREAD_IDENTIFIER_INFO_COUNT;
    PmThreadSample sample = { 0 };

    if (thread_info (threads[i], THREAD_IDENTIFIER_INFO, (thread_info_t) & id,
            &n) == KERN_SUCCESS) {
      n = THREAD_BASIC_INFO_COUNT;
      if (thread_info (threads[i], THREAD_BASIC_INFO, (thread_info_t) & basic,
              &n) == KERN_SUCCESS) {
        sample.os_id = id.thread_id;
        sample.cpu_ns =
            ((guint64) basic.user_time.seconds + basic.system_time.seconds) *
            G_GUINT64_CONSTANT (1000000000) +
            ((guint64) basic.user_time.microseconds +
            basic.system_time.microseconds) * 1000;
        n = THREAD_EXTENDED_INFO_COUNT;
        sample.name = thread_info (threads[i], THREAD_EXTENDED_INFO,
            (thread_info_t) & extended, &n) == KERN_SUCCESS ?
            g_strndup (extended.pth_name, sizeof (extended.pth_name)) :
            g_strdup ("");
        g_array_append_val (all, sample);
      }
    }
    mach_port_deallocate (mach_task_self (), threads[i]);
  }
  vm_deallocate (mach_task_self (), (vm_address_t) threads,
      count * sizeof (*threads));
  return all;
#elif defined(G_OS_WIN32)
  HANDLE snapshot = CreateToolhelp32Snapshot (TH32CS_SNAPTHREAD, 0);
  DWORD self = GetCurrentProcessId ();
  THREADENTRY32 entry;
  GArray *all;

  if (snapshot == INVALID_HANDLE_VALUE)
    return NULL;
  all = new_samples ();
  entry.dwSize = sizeof (entry);
  if (Thread32First (snapshot, &entry)) {
    do {
      HANDLE thread;
      FILETIME created, exited, kernel, user;

      if (entry.th32OwnerProcessID != self)
        continue;
      thread = OpenThread (THREAD_QUERY_LIMITED_INFORMATION, FALSE,
          entry.th32ThreadID);
      if (thread == NULL)
        continue;
      if (GetThreadTimes (thread, &created, &exited, &kernel, &user)) {
        PmThreadSample sample = { 0 };
        guint64 k = ((guint64) kernel.dwHighDateTime << 32) |
            kernel.dwLowDateTime;
        guint64 u = ((guint64) user.dwHighDateTime << 32) | user.dwLowDateTime;

        sample.os_id = entry.th32ThreadID;
        sample.name = g_strdup ("");
        /* 100 ns units. */
        sample.cpu_ns = (k + u) * 100;
        g_array_append_val (all, sample);
      }
      CloseHandle (thread);
    } while (Thread32Next (snapshot, &entry));
  }
  CloseHandle (snapshot);
  return all;
#else
  return NULL;
#endif
}

gboolean
pm_process_sample (guint64 * cpu_ns, guint64 * memory)
{
#if defined(G_OS_WIN32)
  FILETIME created, exited, kernel, user;
  PROCESS_MEMORY_COUNTERS counters = { 0 };

  if (!GetProcessTimes (GetCurrentProcess (), &created, &exited, &kernel,
          &user) || !K32GetProcessMemoryInfo (GetCurrentProcess (), &counters,
          sizeof (counters)))
    return FALSE;
  /* 100 ns units. */
  *cpu_ns = ((((guint64) kernel.dwHighDateTime << 32) | kernel.dwLowDateTime)
      + (((guint64) user.dwHighDateTime << 32) | user.dwLowDateTime)) * 100;
  *memory = counters.WorkingSetSize;
  return TRUE;
#else
  struct rusage usage;

  if (getrusage (RUSAGE_SELF, &usage) != 0)
    return FALSE;
  *cpu_ns = ((guint64) usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) *
      G_GUINT64_CONSTANT (1000000000) +
      ((guint64) usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) * 1000;
#if defined(__APPLE__)
  {
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;

    if (task_info (mach_task_self (), TASK_VM_INFO, (task_info_t) & info,
            &count) != KERN_SUCCESS)
      return FALSE;
    *memory = info.phys_footprint;
  }
#elif defined(__linux__)
  {
    gchar *text = NULL;
    gchar **fields;
    gboolean ok = FALSE;

    /* /proc/self/statm: size resident ... in pages. */
    if (g_file_get_contents ("/proc/self/statm", &text, NULL, NULL)) {
      fields = g_strsplit (text, " ", 3);
      if (g_strv_length (fields) >= 2) {
        *memory = g_ascii_strtoull (fields[1], NULL, 10) *
            sysconf (_SC_PAGESIZE);
        ok = TRUE;
      }
      g_strfreev (fields);
    }
    g_free (text);
    if (!ok)
      return FALSE;
  }
#else
  /* ru_maxrss: the peak, not now; better than nothing. */
  *memory = (guint64) usage.ru_maxrss * 1024;
#endif
  return TRUE;
#endif
}
