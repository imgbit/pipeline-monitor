/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef __PM_RECORDER_H__
#define __PM_RECORDER_H__

#include <glib.h>

G_BEGIN_DECLS

/* Recordings: files of protocol messages, one per line (docs/recording.md),
 * in one folder, with a limit per file and in total. Thread-safe. */
typedef struct _PmRecorder PmRecorder;

#define PM_RECORDING_FORMAT_VERSION 1

PmRecorder *pm_recorder_new (const gchar * dir, guint64 file_max,
    guint64 total_max);
void pm_recorder_free (PmRecorder * self);

gboolean pm_recorder_start (PmRecorder * self, const gchar * kind,
    const gchar * program, const gchar * hello, const gchar * view,
    GError ** error);
gboolean pm_recorder_write (PmRecorder * self, const gchar * message);
void pm_recorder_stop (PmRecorder * self, const gchar * reason);
gboolean pm_recorder_running (PmRecorder * self);

gchar *pm_recorder_list_json (PmRecorder * self);
gchar *pm_recorder_path (PmRecorder * self, const gchar * name);
gboolean pm_recorder_delete (PmRecorder * self, const gchar * name);

gchar *pm_recorder_default_dir (void);
gchar *pm_recorder_hide_secrets (const gchar * message);

G_END_DECLS

#endif
