/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef __PM_JSON_H__
#define __PM_JSON_H__

#include <glib.h>

G_BEGIN_DECLS

/* The protocol's version, in every message (docs/protocol.md). */
#define PM_PROTOCOL 1

gchar *pm_json_message (const gchar * type, const gchar * body);
void pm_json_append_string (GString * json, const gchar * str);
void pm_json_append_value (GString * json, const gchar * key, guint64 value);
void pm_json_append_member_string (GString * json, const gchar * key,
    const gchar * value);
void pm_json_close (GString * json, gchar end);

G_END_DECLS

#endif
