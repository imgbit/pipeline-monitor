/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef __PM_PROPS_H__
#define __PM_PROPS_H__

#include <gst/gst.h>

G_BEGIN_DECLS

gchar *pm_props_read (GstElement * element, const gchar * const *names,
    gint64 timeout_us);
void pm_props_append_value (GString * json, const GValue * value);

G_END_DECLS

#endif /* __PM_PROPS_H__ */
