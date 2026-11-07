/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pmjson.h"

/* A message: {"type","protocol","time"} followed by the members of body,
 * a JSON object (or NULL for none). time: µs since 1970. */
gchar *
pm_json_message (const gchar * type, const gchar * body)
{
  gboolean members = body != NULL && body[0] == '{' && body[1] != '}';

  return g_strdup_printf ("{\"type\":\"%s\",\"protocol\":%d,\"time\":%"
      G_GINT64_FORMAT "%s%s", type, PM_PROTOCOL, g_get_real_time (),
      members ? "," : "}", members ? body + 1 : "");
}

/* Appends str as a JSON string, or null. Text that is not valid UTF-8
 * (GStreamer does not enforce it everywhere) gets replacement characters:
 * the WebSocket only carries valid UTF-8. */
void
pm_json_append_string (GString * json, const gchar * str)
{
  gchar *valid = NULL;
  const guchar *p;

  if (str == NULL) {
    g_string_append (json, "null");
    return;
  }
  if (!g_utf8_validate (str, -1, NULL))
    str = valid = g_utf8_make_valid (str, -1);

  g_string_append_c (json, '"');
  for (p = (const guchar *) str; *p != '\0'; p++) {
    switch (*p) {
      case '"':
        g_string_append (json, "\\\"");
        break;
      case '\\':
        g_string_append (json, "\\\\");
        break;
      case '\n':
        g_string_append (json, "\\n");
        break;
      case '\r':
        g_string_append (json, "\\r");
        break;
      case '\t':
        g_string_append (json, "\\t");
        break;
      default:
        if (*p < 0x20)
          g_string_append_printf (json, "\\u%04x", *p);
        else
          g_string_append_c (json, *p);
        break;
    }
  }
  g_string_append_c (json, '"');
  g_free (valid);
}

/* Appends "key":value, with null for G_MAXUINT64. */
void
pm_json_append_value (GString * json, const gchar * key, guint64 value)
{
  if (value == G_MAXUINT64)
    g_string_append_printf (json, "\"%s\":null,", key);
  else
    g_string_append_printf (json, "\"%s\":%" G_GUINT64_FORMAT ",", key,
        value);
}

/* Ends an object or array built from "member," parts. */
void
pm_json_close (GString * json, gchar end)
{
  if (json->str[json->len - 1] == ',')
    json->str[json->len - 1] = end;
  else
    g_string_append_c (json, end);
}
