/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef __PM_TEST_ENVELOPE_H__
#define __PM_TEST_ENVELOPE_H__

#include <glib.h>
#include <string.h>

/* Checks that text is a message of this type (type, protocol 1, time) and
 * returns the rest as an object: {"type":"graph",...,"time":1,"a":2} gives
 * {"a":2}. Takes text. */
static inline gchar *
without_envelope (gchar * text, const gchar * type)
{
  gchar *prefix = g_strdup_printf ("{\"type\":\"%s\",\"protocol\":1,\"time\":",
      type);
  const gchar *p = text;
  gchar *rest;

  g_assert_true (g_str_has_prefix (p, prefix));
  p += strlen (prefix);
  g_assert_true (g_ascii_isdigit (*p));
  while (g_ascii_isdigit (*p))
    p++;
  rest = g_strconcat ("{", *p == ',' ? p + 1 : p, NULL);
  g_free (prefix);
  g_free (text);
  return rest;
}

#endif
