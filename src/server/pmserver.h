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

G_END_DECLS

#endif /* __PM_SERVER_H__ */
