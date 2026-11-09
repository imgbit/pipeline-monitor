/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */

/* Any bytes as a recording file, a recording's name and a message to hide
 * secrets in. Nothing may crash or corrupt memory, and hiding secrets in
 * valid JSON must give valid JSON. */

#include "pmrecorder.h"

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <stdint.h>
#include <string.h>

static PmRecorder *recorder;
static gchar *file;

int
LLVMFuzzerInitialize (int *argc, char ***argv)
{
  gchar *dir = g_dir_make_tmp ("pm-fuzz-XXXXXX", NULL);

  /* A GLib warning or critical is a failure too. */
  g_log_set_always_fatal (G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL |
      G_LOG_LEVEL_WARNING);
  recorder = pm_recorder_new (dir, 1 << 20, 1 << 30);
  file = g_build_filename (dir, "pipeline-monitor-fuzz.jsonl", NULL);
  return 0;
}

int
LLVMFuzzerTestOneInput (const uint8_t * data, size_t size)
{
  gchar *text = g_strndup ((const gchar *) data, size);
  JsonNode *in = json_from_string (text, NULL);
  gchar *hidden = pm_recorder_hide_secrets (text);
  gchar *list, *path;

  /* Valid JSON stays valid JSON. */
  if (in != NULL) {
    JsonNode *out = json_from_string (hidden, NULL);

    g_assert_nonnull (out);
    json_node_unref (out);
    json_node_unref (in);
  }
  g_free (hidden);

  /* The input as a name: a path only for a valid recording name. */
  path = pm_recorder_path (recorder, text);
  g_free (path);

  /* The input as a recording file: listed without trouble. */
  g_assert_true (g_file_set_contents (file, (const gchar *) data, size,
          NULL));
  list = pm_recorder_list_json (recorder);
  g_assert_nonnull (strstr (list, "pipeline-monitor-fuzz.jsonl"));
  g_free (list);
  g_free (text);
  return 0;
}
