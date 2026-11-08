/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef __PM_SERVER_H__
#define __PM_SERVER_H__

#include "pmmodel.h"

G_BEGIN_DECLS

typedef struct _PmServer PmServer;

PmServer *pm_server_new (PmModel * model, guint port, const gchar * ui_dir,
    GError ** error);
void pm_server_free (PmServer * server);

const gchar *pm_server_get_url (PmServer * server);
void pm_server_notify (PmServer * server);
void pm_server_wait_for_page (PmServer * server);
void pm_server_flush (PmServer * server, gboolean bye);
void pm_server_hold_findings (PmServer * server);
void pm_server_release_findings (PmServer * server);

/* Where recordings go and how big they may get; record: start at once. */
typedef struct
{
  const gchar *dir;
  guint64 file_max, total_max;
  gboolean record;
} PmRecordOptions;

#define PM_RECORD_FILE_MAX (100 * 1024 * 1024)
#define PM_RECORD_TOTAL_MAX (1024 * 1024 * 1024)

void pm_server_set_recordings (PmServer * server,
    const PmRecordOptions * options);
void pm_server_set_viewer (PmServer * server, const gchar * opened);

G_END_DECLS

#endif /* __PM_SERVER_H__ */
