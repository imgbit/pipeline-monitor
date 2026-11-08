/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pmrecorder.h"
#include "pmjson.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <string.h>

#define NAME_PREFIX "pipeline-monitor-"
#define NAME_SUFFIX ".jsonl"
/* How much of a file is read for its header line. */
#define MAX_HEADER (1024 * 1024)

struct _PmRecorder
{
  GMutex lock;
  gchar *dir;
  guint64 file_max, total_max;
  /* The recording being written, if any. */
  FILE *file;
  gchar *name;
  gchar *kind;
  gint64 started;
  guint64 size;
};

PmRecorder *
pm_recorder_new (const gchar * dir, guint64 file_max, guint64 total_max)
{
  PmRecorder *self = g_new0 (PmRecorder, 1);

  g_mutex_init (&self->lock);
  self->dir = g_strdup (dir);
  self->file_max = file_max;
  self->total_max = total_max;
  return self;
}

void
pm_recorder_free (PmRecorder * self)
{
  pm_recorder_stop (self, "program-ended");
  g_free (self->dir);
  g_mutex_clear (&self->lock);
  g_free (self);
}

gchar *
pm_recorder_default_dir (void)
{
  return g_build_filename (g_get_user_data_dir (), "pipeline-monitor",
      "recordings", NULL);
}

/* Secrets */

/* Names of properties that hold secrets: a part (between - and _) is one of
 * these words, or the last part is "key" (srtp-key, not key-int-max). */
static gboolean
secret_name (const gchar * name)
{
  static const gchar *words[] = { "password", "passwd", "passphrase",
    "secret", "token", "credential", "credentials", "auth", NULL
  };
  gchar **parts = g_strsplit_set (name, "-_", -1);
  guint n = g_strv_length (parts), i;
  gboolean secret = n > 0 && g_ascii_strcasecmp (parts[n - 1], "key") == 0;

  for (i = 0; i < n && !secret; i++) {
    gchar *part = g_ascii_strdown (parts[i], -1);

    secret = g_strv_contains (words, part);
    g_free (part);
  }
  g_strfreev (parts);
  return secret;
}

/* user:password@ in a URI, in any text. */
static GRegex *
uri_login (void)
{
  static GRegex *regex;

  if (g_once_init_enter (&regex)) {
    GRegex *r = g_regex_new ("([a-zA-Z][a-zA-Z0-9+.-]*://)[^/@\\s]+@", 0, 0,
        NULL);

    g_once_init_leave (&regex, r);
  }
  return regex;
}

static void
hide_in (JsonNode * node)
{
  if (JSON_NODE_HOLDS_ARRAY (node)) {
    JsonArray *array = json_node_get_array (node);
    guint i;

    for (i = 0; i < json_array_get_length (array); i++)
      hide_in (json_array_get_element (array, i));
  } else if (JSON_NODE_HOLDS_OBJECT (node)) {
    JsonObject *object = json_node_get_object (node);
    JsonNode *name = json_object_get_member (object, "name");
    GList *members = json_object_get_members (object), *l;

    for (l = members; l != NULL; l = l->next) {
      JsonNode *member = json_object_get_member (object, l->data);

      /* A property: {"name":"...","value":...,"default":...} */
      if (name != NULL && JSON_NODE_HOLDS_VALUE (name) &&
          json_node_get_value_type (name) == G_TYPE_STRING &&
          (g_str_equal (l->data, "value") || g_str_equal (l->data, "default"))
          && JSON_NODE_HOLDS_VALUE (member) &&
          json_node_get_value_type (member) == G_TYPE_STRING &&
          secret_name (json_node_get_string (name)))
        json_node_set_string (member, "(hidden)");
      else
        hide_in (member);
    }
    g_list_free (members);
  } else if (JSON_NODE_HOLDS_VALUE (node) &&
      json_node_get_value_type (node) == G_TYPE_STRING) {
    const gchar *text = json_node_get_string (node);
    gchar *hidden = g_regex_replace (uri_login (), text, -1, 0,
        "\\1(hidden)@", 0, NULL);

    if (hidden != NULL && strcmp (hidden, text) != 0)
      json_node_set_string (node, hidden);
    g_free (hidden);
  }
}

/* The message (JSON text) with passwords in URIs and the values of
 * secret properties replaced by "(hidden)". */
gchar *
pm_recorder_hide_secrets (const gchar * message)
{
  JsonNode *root = json_from_string (message, NULL);
  gchar *text;

  if (root == NULL)
    return g_strdup (message);
  hide_in (root);
  text = json_to_string (root, FALSE);
  json_node_unref (root);
  return text;
}

/* Files */

static gboolean
valid_name (const gchar * name)
{
  const gchar *c;

  if (name == NULL || !g_str_has_prefix (name, NAME_PREFIX) ||
      !g_str_has_suffix (name, NAME_SUFFIX) || strstr (name, "..") != NULL)
    return FALSE;
  for (c = name; *c != '\0'; c++)
    if (!g_ascii_isalnum (*c) && *c != '-' && *c != '_' && *c != '.')
      return FALSE;
  return TRUE;
}

/* pipeline-monitor-<program>-<YYYYMMDD-HHMMSS>[-snapshot][-N].jsonl, not
 * yet in the folder. */
static gchar *
new_name (PmRecorder * self, const gchar * kind, const gchar * program)
{
  GDateTime *now = g_date_time_new_now_local ();
  gchar *stamp = g_date_time_format (now, "%Y%m%d-%H%M%S");
  GString *base = g_string_new (NAME_PREFIX);
  const gchar *c;
  gchar *name = NULL;
  guint n;

  for (c = program && *program ? program : "program"; *c != '\0' &&
      base->len < strlen (NAME_PREFIX) + 40; c++)
    g_string_append_c (base, g_ascii_isalnum (*c) || *c == '-' ||
        *c == '_' ? *c : '_');
  g_string_append_printf (base, "-%s%s", stamp,
      g_strcmp0 (kind, "snapshot") == 0 ? "-snapshot" : "");
  for (n = 1; name == NULL; n++) {
    gchar *candidate = n == 1 ?
        g_strconcat (base->str, NAME_SUFFIX, NULL) :
        g_strdup_printf ("%s-%u%s", base->str, n, NAME_SUFFIX);
    gchar *path = g_build_filename (self->dir, candidate, NULL);

    if (!g_file_test (path, G_FILE_TEST_EXISTS))
      name = candidate;
    else
      g_free (candidate);
    g_free (path);
  }
  g_string_free (base, TRUE);
  g_free (stamp);
  g_date_time_unref (now);
  return name;
}

typedef struct
{
  gchar *name;
  guint64 size;
  gint64 modified;
} PmFile;

static void
file_free (PmFile * f)
{
  g_free (f->name);
  g_free (f);
}

static gint
older_first (gconstpointer a, gconstpointer b)
{
  const PmFile *fa = *(PmFile * const *) a, *fb = *(PmFile * const *) b;

  return (fa->modified > fb->modified) - (fa->modified < fb->modified);
}

/* The recordings in the folder, oldest first. */
static GPtrArray *
list_files (PmRecorder * self)
{
  GPtrArray *files = g_ptr_array_new_with_free_func ((GDestroyNotify)
      file_free);
  GDir *dir = g_dir_open (self->dir, 0, NULL);
  const gchar *name;

  while (dir != NULL && (name = g_dir_read_name (dir)) != NULL) {
    gchar *path;
    GStatBuf st;

    if (!valid_name (name))
      continue;
    path = g_build_filename (self->dir, name, NULL);
    if (g_stat (path, &st) == 0) {
      PmFile *f = g_new0 (PmFile, 1);

      f->name = g_strdup (name);
      f->size = st.st_size;
      f->modified = (gint64) st.st_mtime * G_USEC_PER_SEC;
      g_ptr_array_add (files, f);
    }
    g_free (path);
  }
  if (dir != NULL)
    g_dir_close (dir);
  g_ptr_array_sort (files, older_first);
  return files;
}

/* Removes the oldest recordings (never the running one) until the folder
 * holds at most total_max bytes. */
static void
keep_total (PmRecorder * self)
{
  GPtrArray *files = list_files (self);
  guint64 total = 0;
  guint i;

  for (i = 0; i < files->len; i++)
    total += ((PmFile *) g_ptr_array_index (files, i))->size;
  for (i = 0; i < files->len && total > self->total_max; i++) {
    PmFile *f = g_ptr_array_index (files, i);
    gchar *path;

    if (g_strcmp0 (f->name, self->name) == 0)
      continue;
    path = g_build_filename (self->dir, f->name, NULL);
    if (g_unlink (path) == 0)
      total -= f->size;
    g_free (path);
  }
  g_ptr_array_unref (files);
}

static void
write_line (PmRecorder * self, const gchar * text)
{
  fputs (text, self->file);
  fputc ('\n', self->file);
  fflush (self->file);
  self->size += strlen (text) + 1;
}

static void
close_file (PmRecorder * self, const gchar * reason)
{
  gchar *body = g_strdup_printf ("{\"reason\":\"%s\"}", reason);
  gchar *end = pm_json_message ("end", body);

  write_line (self, end);
  fclose (self->file);
  self->file = NULL;
  g_clear_pointer (&self->name, g_free);
  g_clear_pointer (&self->kind, g_free);
  g_free (end);
  g_free (body);
}

/* Creates a recording (kind "recording" or "snapshot") and writes its
 * header with hello and view (JSON objects, or NULL). */
gboolean
pm_recorder_start (PmRecorder * self, const gchar * kind,
    const gchar * program, const gchar * hello, const gchar * view,
    GError ** error)
{
  GString *header;
  gchar *path, *hidden;
  gboolean ok = FALSE;

  g_mutex_lock (&self->lock);
  if (self->file != NULL) {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_BUSY,
        "A recording is already running.");
    goto done;
  }
  if (g_mkdir_with_parents (self->dir, 0700) != 0) {
    gint err = errno;

    g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (err),
        "Cannot create %s: %s", self->dir, g_strerror (err));
    goto done;
  }
  keep_total (self);

  self->name = new_name (self, kind, program);
  path = g_build_filename (self->dir, self->name, NULL);
  self->file = g_fopen (path, "wb");
  if (self->file == NULL) {
    gint err = errno;

    g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (err),
        "Cannot write %s: %s", path, g_strerror (err));
    g_clear_pointer (&self->name, g_free);
    g_free (path);
    goto done;
  }
  g_free (path);
  self->kind = g_strdup (kind);
  self->started = g_get_real_time ();
  self->size = 0;

  header = g_string_new ("{\"format\":\"pipeline-monitor-recording\",");
  g_string_append_printf (header, "\"version\":%d,\"protocol\":%d,"
      "\"kind\":", PM_RECORDING_FORMAT_VERSION, PM_PROTOCOL);
  pm_json_append_string (header, kind);
  g_string_append_printf (header, ",\"started\":%" G_GINT64_FORMAT
      ",\"hello\":%s,\"view\":%s}", self->started, hello ? hello : "null",
      view ? view : "null");
  hidden = pm_recorder_hide_secrets (header->str);
  write_line (self, hidden);
  g_free (hidden);
  g_string_free (header, TRUE);
  ok = TRUE;

done:
  g_mutex_unlock (&self->lock);
  return ok;
}

/* Appends a message, secrets hidden. At the file limit the recording ends
 * (reason "limit"). FALSE if no recording is running (any more). */
gboolean
pm_recorder_write (PmRecorder * self, const gchar * message)
{
  gchar *hidden;
  gboolean written = FALSE;

  g_mutex_lock (&self->lock);
  if (self->file != NULL) {
    hidden = pm_recorder_hide_secrets (message);
    if (self->size + strlen (hidden) + 1 > self->file_max) {
      close_file (self, "limit");
      keep_total (self);
    } else {
      write_line (self, hidden);
      written = TRUE;
    }
    g_free (hidden);
  }
  g_mutex_unlock (&self->lock);
  return written;
}

/* Ends the running recording, if any, with a last {"type":"end"} line. */
void
pm_recorder_stop (PmRecorder * self, const gchar * reason)
{
  g_mutex_lock (&self->lock);
  if (self->file != NULL) {
    close_file (self, reason);
    keep_total (self);
  }
  g_mutex_unlock (&self->lock);
}

gboolean
pm_recorder_running (PmRecorder * self)
{
  gboolean running;

  g_mutex_lock (&self->lock);
  running = self->file != NULL;
  g_mutex_unlock (&self->lock);
  return running;
}

/* The header of a recording file, parsed, or NULL. */
static JsonObject *
read_header (const gchar * path)
{
  FILE *file = g_fopen (path, "rb");
  GString *line = g_string_new (NULL);
  JsonNode *root;
  JsonObject *header = NULL;
  gint c;

  if (file == NULL) {
    g_string_free (line, TRUE);
    return NULL;
  }
  while ((c = fgetc (file)) != EOF && c != '\n' && line->len < MAX_HEADER)
    g_string_append_c (line, (gchar) c);
  fclose (file);
  root = json_from_string (line->str, NULL);
  if (root != NULL && JSON_NODE_HOLDS_OBJECT (root))
    header = json_object_ref (json_node_get_object (root));
  if (root != NULL)
    json_node_unref (root);
  g_string_free (line, TRUE);
  return header;
}

/* {"folder","file_max","total_max","recordings":[{"name","kind","started",
 * "last","size","running"}]}, newest first. */
gchar *
pm_recorder_list_json (PmRecorder * self)
{
  GString *json = g_string_new ("{\"folder\":");
  GPtrArray *files;
  gint i;

  g_mutex_lock (&self->lock);
  pm_json_append_string (json, self->dir);
  g_string_append_printf (json, ",\"file_max\":%" G_GUINT64_FORMAT
      ",\"total_max\":%" G_GUINT64_FORMAT ",\"recordings\":[",
      self->file_max, self->total_max);
  files = list_files (self);
  for (i = files->len - 1; i >= 0; i--) {
    PmFile *f = g_ptr_array_index (files, i);
    gboolean running = g_strcmp0 (f->name, self->name) == 0;
    gchar *path = g_build_filename (self->dir, f->name, NULL);
    JsonObject *header = running ? NULL : read_header (path);

    if (json->str[json->len - 1] != '[')
      g_string_append_c (json, ',');
    g_string_append (json, "{\"name\":");
    pm_json_append_string (json, f->name);
    g_string_append (json, ",\"kind\":");
    pm_json_append_string (json, running ? self->kind : header ?
        json_object_get_string_member_with_default (header, "kind", NULL) :
        NULL);
    g_string_append_printf (json, ",\"started\":%" G_GINT64_FORMAT
        ",\"last\":%" G_GINT64_FORMAT ",\"size\":%" G_GUINT64_FORMAT
        ",\"running\":%s}", running ? self->started : header ?
        json_object_get_int_member_with_default (header, "started", 0) : 0,
        running ? g_get_real_time () : f->modified, f->size,
        running ? "true" : "false");
    if (header != NULL)
      json_object_unref (header);
    g_free (path);
  }
  g_ptr_array_unref (files);
  g_mutex_unlock (&self->lock);
  g_string_append (json, "]}");
  return g_string_free (json, FALSE);
}

/* The full path of the recording called name, or NULL if there is none. */
gchar *
pm_recorder_path (PmRecorder * self, const gchar * name)
{
  gchar *path;

  if (!valid_name (name))
    return NULL;
  path = g_build_filename (self->dir, name, NULL);
  if (!g_file_test (path, G_FILE_TEST_IS_REGULAR))
    g_clear_pointer (&path, g_free);
  return path;
}

/* Deletes a recording that is not running. */
gboolean
pm_recorder_delete (PmRecorder * self, const gchar * name)
{
  gchar *path;
  gboolean deleted = FALSE;

  g_mutex_lock (&self->lock);
  if (g_strcmp0 (name, self->name) != 0 &&
      (path = pm_recorder_path (self, name)) != NULL) {
    deleted = g_unlink (path) == 0;
    g_free (path);
  }
  g_mutex_unlock (&self->lock);
  return deleted;
}
