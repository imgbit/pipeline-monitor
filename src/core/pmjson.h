/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef __PM_JSON_H__
#define __PM_JSON_H__

#include <glib.h>

G_BEGIN_DECLS

void pm_json_append_string (GString * json, const gchar * str);
void pm_json_append_value (GString * json, const gchar * key, guint64 value);
void pm_json_close (GString * json, gchar end);

G_END_DECLS

#endif
