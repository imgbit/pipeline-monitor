/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */

/* pipeline-monitor status: what the page shows, as text, for people and
 * for AI assistants that run commands. From running programs (the
 * tracer's HTTP API) or from a recording. */

#include "pmstatus.h"

#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef G_OS_UNIX
#include <unistd.h>
#endif

#define FIRST_PORT 8765
#define PORTS 10

/* One program's state: the latest message of each type. */
typedef struct
{
  gchar *source;
  JsonNode *hello, *graph, *levels, *findings, *perf;
  /* The last graph with pipelines, and its time (recordings: from the
   * start, µs). */
  JsonNode *seen_graph;
  gint64 seen_at;
  /* Recordings only: the last properties of each element. */
  GHashTable *props;
  /* Log entries by id, as last seen. */
  GHashTable *log;
  gint64 started, at, length;
  gchar *end;
  gboolean recording;
} Status;

static void
status_free (Status * s)
{
  g_free (s->source);
  g_clear_pointer (&s->hello, json_node_unref);
  g_clear_pointer (&s->graph, json_node_unref);
  g_clear_pointer (&s->seen_graph, json_node_unref);
  g_clear_pointer (&s->levels, json_node_unref);
  g_clear_pointer (&s->findings, json_node_unref);
  g_clear_pointer (&s->perf, json_node_unref);
  g_clear_pointer (&s->props, g_hash_table_unref);
  g_hash_table_unref (s->log);
  g_free (s->end);
  g_free (s);
}

static Status *
status_new (const gchar * source)
{
  Status *s = g_new0 (Status, 1);

  s->source = g_strdup (source);
  s->log = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free,
      (GDestroyNotify) json_node_unref);
  s->props = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free,
      (GDestroyNotify) json_node_unref);
  return s;
}

static JsonObject *
obj (JsonNode * node)
{
  return node && JSON_NODE_HOLDS_OBJECT (node) ? json_node_get_object (node)
      : NULL;
}

static const gchar *
str (JsonObject * o, const gchar * key)
{
  JsonNode *n = o ? json_object_get_member (o, key) : NULL;

  return n && JSON_NODE_HOLDS_VALUE (n) && json_node_get_value_type (n) ==
      G_TYPE_STRING ? json_node_get_string (n) : NULL;
}

static gboolean
has_number (JsonObject * o, const gchar * key)
{
  JsonNode *n = o ? json_object_get_member (o, key) : NULL;

  return n && JSON_NODE_HOLDS_VALUE (n) && json_node_get_value_type (n) !=
      G_TYPE_STRING && json_node_get_value_type (n) != G_TYPE_BOOLEAN;
}

static gdouble
num (JsonObject * o, const gchar * key)
{
  return has_number (o, key) ? json_object_get_double_member (o, key) : 0;
}

static gint64
inum (JsonObject * o, const gchar * key)
{
  return has_number (o, key) ? json_object_get_int_member (o, key) : 0;
}

static JsonArray *
arr (JsonObject * o, const gchar * key)
{
  JsonNode *n = o ? json_object_get_member (o, key) : NULL;

  return n && JSON_NODE_HOLDS_ARRAY (n) ? json_node_get_array (n) : NULL;
}

static JsonObject *
child_obj (JsonObject * o, const gchar * key)
{
  return obj (o ? json_object_get_member (o, key) : NULL);
}

/* Keeps one message: the latest of its type; log entries merged by id. */
static void
keep (Status * s, JsonNode * node)
{
  JsonObject *m = obj (node);
  const gchar *type = str (m, "type");
  JsonNode **slot = NULL;

  if (type == NULL)
    return;
  if (g_str_equal (type, "hello"))
    slot = &s->hello;
  else if (g_str_equal (type, "graph"))
    slot = &s->graph;
  else if (g_str_equal (type, "levels"))
    slot = &s->levels;
  else if (g_str_equal (type, "findings"))
    slot = &s->findings;
  else if (g_str_equal (type, "performance"))
    slot = &s->perf;
  if (slot != NULL) {
    g_clear_pointer (slot, json_node_unref);
    *slot = json_node_ref (node);
    if (slot == &s->graph && arr (m, "pipelines") &&
        json_array_get_length (arr (m, "pipelines"))) {
      g_clear_pointer (&s->seen_graph, json_node_unref);
      s->seen_graph = json_node_ref (node);
      s->seen_at = inum (m, "time") - s->started;
    }
  } else if (g_str_equal (type, "log")) {
    JsonArray *entries = arr (m, "entries");
    guint i;

    for (i = 0; entries && i < json_array_get_length (entries); i++) {
      JsonNode *e = json_array_get_element (entries, i);
      gint64 *id = g_new (gint64, 1);

      *id = inum (obj (e), "id");
      g_hash_table_replace (s->log, id, json_node_ref (e));
    }
  } else if (g_str_equal (type, "properties")) {
    JsonArray *elements = arr (m, "elements");
    guint i;

    for (i = 0; elements && i < json_array_get_length (elements); i++) {
      JsonNode *e = json_array_get_element (elements, i);
      gint64 *id = g_new (gint64, 1);

      *id = inum (obj (e), "element");
      g_hash_table_replace (s->props, id, json_node_ref (e));
    }
  } else if (g_str_equal (type, "end")) {
    g_free (s->end);
    s->end = g_strdup (str (m, "reason"));
  }
}

/* Running programs */

static SoupSession *
session_get (void)
{
  static SoupSession *session;

  if (session == NULL)
    session = soup_session_new_with_options ("timeout", 3, NULL);
  return session;
}

/* GET base + path as JSON, or NULL (404, refused, not JSON). */
static JsonNode *
fetch (const gchar * base, const gchar * path)
{
  gchar *url = g_strconcat (base, path, NULL);
  SoupMessage *msg = soup_message_new ("GET", url);
  GBytes *body = NULL;
  JsonNode *node = NULL;

  g_free (url);
  if (msg == NULL)
    return NULL;
  body = soup_session_send_and_read (session_get (), msg, NULL, NULL);
  if (body != NULL && soup_message_get_status (msg) == SOUP_STATUS_OK) {
    gsize size;
    const gchar *data = g_bytes_get_data (body, &size);

    gchar *text = g_strndup (data, size);

    node = json_from_string (text, NULL);
    g_free (text);
  }
  g_clear_pointer (&body, g_bytes_unref);
  g_object_unref (msg);
  return node;
}

/* A program's state from its tracer at base ("http://host:port/"); NULL
 * if nothing answers there, or it is a viewer (pipeline-monitor). */
static Status *
read_program (const gchar * base)
{
  JsonNode *hello = fetch (base, "api/hello");
  Status *s;
  JsonNode *n;
  const gchar *paths[] = { "api/graph", "api/levels", "api/findings",
    "api/log", "api/performance", NULL
  };
  guint i;

  if (hello == NULL || !obj (hello) ||
      json_object_get_boolean_member_with_default (obj (hello), "viewer",
          FALSE)) {
    g_clear_pointer (&hello, json_node_unref);
    return NULL;
  }
  s = status_new (base);
  keep (s, hello);
  json_node_unref (hello);
  for (i = 0; paths[i] != NULL; i++) {
    n = fetch (base, paths[i]);
    if (n != NULL) {
      keep (s, n);
      json_node_unref (n);
    }
  }
  return s;
}

/* Recordings */

/* "12.5", "1:02.5", "1:00:02.5" in seconds; -1 if not a time. */
static gdouble
parse_time (const gchar * text)
{
  gchar **parts = g_strsplit (text, ":", 3);
  guint n = g_strv_length (parts), i;
  gdouble total = 0;

  for (i = 0; i < n; i++) {
    gchar *end;
    gdouble v = g_ascii_strtod (parts[i], &end);

    if (*parts[i] == '\0' || *end != '\0' || v < 0 || (i < n - 1 &&
            v != (gdouble) (gint64) v)) {
      g_strfreev (parts);
      return -1;
    }
    total = total * 60 + v;
  }
  g_strfreev (parts);
  return n ? total : -1;
}

static Status *
read_recording (const gchar * file, gdouble at_s, GError ** error)
{
  GFile *f = g_file_new_for_commandline_arg (file);
  GFileInputStream *in = g_file_read (f, NULL, error);
  GDataInputStream *data;
  JsonParser *parser;
  Status *s;
  gchar *line;
  gboolean first = TRUE;

  g_object_unref (f);
  if (in == NULL)
    return NULL;
  data = g_data_input_stream_new (G_INPUT_STREAM (in));
  parser = json_parser_new ();
  s = status_new (file);
  s->recording = TRUE;
  while ((line = g_data_input_stream_read_line_utf8 (data, NULL, NULL,
              error)) != NULL) {
    JsonObject *m;
    gint64 time;

    if (!json_parser_load_from_data (parser, line, -1, NULL)) {
      g_free (line);
      continue;
    }
    g_free (line);
    m = obj (json_parser_get_root (parser));
    if (first) {
      first = FALSE;
      if (g_strcmp0 (str (m, "format"), "pipeline-monitor-recording") != 0) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
            "not a pipeline-monitor recording");
        break;
      }
      s->started = inum (m, "started");
      if (json_object_has_member (m, "hello")) {
        JsonNode *hello = json_node_copy (json_object_get_member (m,
                "hello"));

        json_object_set_string_member (json_node_get_object (hello), "type",
            "hello");
        keep (s, hello);
        json_node_unref (hello);
      }
      continue;
    }
    time = inum (m, "time");
    if (time > s->started)
      s->length = time - s->started;
    if (at_s >= 0 && time > s->started + (gint64) (at_s * G_USEC_PER_SEC))
      continue;
    s->at = MAX (s->at, time - s->started);
    keep (s, json_parser_get_root (parser));
  }
  g_object_unref (parser);
  g_object_unref (data);
  g_object_unref (in);
  if (error && *error) {
    status_free (s);
    return NULL;
  }
  return s;
}

/* Text */

/* H:MM:SS.mmm from ns. */
static gchar *
clock_text (gint64 ns)
{
  gint64 ms = ns / 1000000;

  return g_strdup_printf ("%" G_GINT64_FORMAT ":%02d:%02d.%03d",
      ms / 3600000, (gint) (ms / 60000 % 60), (gint) (ms / 1000 % 60),
      (gint) (ms % 1000));
}

static gchar *
span_text (gdouble ns)
{
  if (ns >= 1e9)
    return g_strdup_printf ("%.3g s", ns / 1e9);
  if (ns >= 1e6)
    return g_strdup_printf ("%.3g ms", ns / 1e6);
  if (ns >= 1e3)
    return g_strdup_printf ("%.3g us", ns / 1e3);
  return g_strdup_printf ("%.0f ns", ns);
}

/* The local time of a µs wall-clock stamp: HH:MM:SS. */
static gchar *
wall_text (gint64 us)
{
  GDateTime *t = g_date_time_new_from_unix_local (us / G_USEC_PER_SEC);
  gchar *text = t ? g_date_time_format (t, "%H:%M:%S") : g_strdup ("?");

  if (t)
    g_date_time_unref (t);
  return text;
}

static void
count_elements (JsonObject * e, guint * n)
{
  JsonArray *children = arr (e, "children");
  guint i;

  for (i = 0; children && i < json_array_get_length (children); i++) {
    (*n)++;
    count_elements (json_array_get_object_element (children, i), n);
  }
}

/* How full a queue is (the nearest limit), with that limit's name. */
static gdouble
queue_fill (JsonObject * q, const gchar ** limit)
{
  static const gchar *keys[] = { "buffers", "bytes", "time" };
  JsonObject *limits = child_obj (q, "limits");
  JsonArray *levels = arr (q, "levels");
  gdouble fill = 0;
  guint i, k;

  *limit = NULL;
  for (i = 0; levels && i < json_array_get_length (levels); i++) {
    JsonObject *level = json_array_get_object_element (levels, i);

    for (k = 0; k < G_N_ELEMENTS (keys); k++) {
      gdouble max = num (limits, keys[k]);

      if (max > 0 && has_number (level, keys[k]) &&
          num (level, keys[k]) / max >= fill) {
        fill = num (level, keys[k]) / max;
        *limit = keys[k];
      }
    }
  }
  if (fill == 0)
    *limit = NULL;
  return fill;
}

static gint
compare_fill (gconstpointer a, gconstpointer b)
{
  const gchar *la, *lb;
  gdouble fa = queue_fill (*(JsonObject **) a, &la);
  gdouble fb = queue_fill (*(JsonObject **) b, &lb);

  return fa < fb ? 1 : fa > fb ? -1 : 0;
}

static const gchar *
short_name (const gchar * path)
{
  const gchar *slash = path ? strrchr (path, '/') : NULL;

  return slash ? slash + 1 : path ? path : "?";
}

static gboolean
in_pipeline (const gchar * path, const gchar * pipeline)
{
  gsize n = strlen (pipeline);

  return path && strncmp (path, pipeline, n) == 0 && path[n] == '/';
}

static void
print_header (Status * s)
{
  JsonObject *h = obj (s->hello);
  const gchar *program = str (h, "program");

  if (s->recording) {
    gchar *at = clock_text (s->at * 1000), *len = clock_text (s->length *
        1000);
    GDateTime *t = g_date_time_new_from_unix_local (s->started /
        G_USEC_PER_SEC);
    gchar *when = t ? g_date_time_format (t, "%Y-%m-%d %H:%M:%S") : NULL;

    g_print ("Recording %s\n  of %s (pid %" G_GINT64_FORMAT "), started %s; "
        "at %s of %s%s%s\n", s->source, program ? program : "a program",
        inum (h, "pid"), when ? when : "?", at, len, s->end ? "; ended: " :
        "", s->end ? s->end : "");
    if (t)
      g_date_time_unref (t);
    g_free (when);
    g_free (at);
    g_free (len);
  } else {
    g_print ("%s (pid %" G_GINT64_FORMAT ") at %s\n", program ? program :
        "a program", inum (h, "pid"), s->source);
  }
  g_print ("  GStreamer %s, pipeline-monitor %s\n", str (h, "gstreamer") ?
      str (h, "gstreamer") : "?", str (h, "version") ? str (h, "version") :
      "?");
}

static void
print_pipelines (Status * s)
{
  JsonArray *pipelines = arr (obj (s->graph), "pipelines");
  JsonArray *times = arr (obj (s->levels), "pipelines");
  JsonArray *queues = arr (obj (s->levels), "queues");
  guint i, j;

  if ((!pipelines || json_array_get_length (pipelines) == 0) &&
      s->recording && s->seen_graph) {
    gchar *at = clock_text (s->seen_at * 1000);

    g_print ("No pipelines at this time; as last seen, at %s:\n", at);
    g_free (at);
    pipelines = arr (obj (s->seen_graph), "pipelines");
  }
  if (!pipelines || json_array_get_length (pipelines) == 0) {
    g_print ("No pipelines\n");
    return;
  }
  for (i = 0; i < json_array_get_length (pipelines); i++) {
    JsonObject *p = json_array_get_object_element (pipelines, i);
    JsonObject *t = NULL;
    const gchar *path = str (p, "path");
    GPtrArray *full = g_ptr_array_new ();
    guint n = 0;
    GString *line = g_string_new (NULL);

    for (j = 0; times && j < json_array_get_length (times); j++)
      if (inum (json_array_get_object_element (times, j), "element") ==
          inum (p, "id"))
        t = json_array_get_object_element (times, j);
    g_string_append_printf (line, "%s  %s", path, str (p, "state") ?
        str (p, "state") : "?");
    if (str (p, "pending"))
      g_string_append_printf (line, " -> %s", str (p, "pending"));
    if (t) {
      JsonObject *latency = child_obj (t, "latency");

      if (latency && json_object_get_boolean_member_with_default (latency,
              "live", FALSE))
        g_string_append (line, "  live");
      if (has_number (t, "running_time")) {
        gchar *rt = clock_text (inum (t, "running_time"));

        g_string_append_printf (line, "  running %s", rt);
        g_free (rt);
      }
      if (latency && has_number (latency, "min")) {
        gchar *min = span_text (num (latency, "min"));

        g_string_append_printf (line, "  latency %s", min);
        g_free (min);
      }
      if (str (t, "clock"))
        g_string_append_printf (line, "  clock %s", str (t, "clock"));
    }
    g_print ("%s\n", line->str);
    g_string_free (line, TRUE);

    count_elements (p, &n);
    g_print ("  %u element%s", n, n == 1 ? "" : "s");
    for (j = 0; queues && j < json_array_get_length (queues); j++) {
      JsonObject *q = json_array_get_object_element (queues, j);

      if (in_pipeline (str (q, "path"), path))
        g_ptr_array_add (full, q);
    }
    g_ptr_array_sort (full, compare_fill);
    if (full->len) {
      g_print ("; queues, fullest first:");
      for (j = 0; j < full->len && j < 3; j++) {
        JsonObject *q = g_ptr_array_index (full, j);
        const gchar *limit;
        gdouble fill = queue_fill (q, &limit);

        g_print ("%s %s %.0f %%%s%s", j ? "," : "",
            short_name (str (q, "path")), fill * 100, limit ? " " : "",
            limit ? limit : "");
      }
      if (full->len > 3)
        g_print (" (%u more)", full->len - 3);
    }
    g_print ("\n");
    g_ptr_array_unref (full);
  }
}

static void
print_performance (Status * s)
{
  JsonObject *p = child_obj (obj (s->perf), "process");

  if (p == NULL)
    return;
  g_print ("Process: CPU %.0f %% of a core, memory %.1f MiB, %"
      G_GINT64_FORMAT " threads\n", num (p, "cpu") * 100,
      num (p, "memory") / 1048576, inum (p, "threads"));
}

static gint
severity_rank (const gchar * severity)
{
  return g_strcmp0 (severity, "error") == 0 ? 0 :
      g_strcmp0 (severity, "warning") == 0 ? 1 : 2;
}

/* Problems, at path_filter only if set; returns how many are errors. */
static guint
print_problems (Status * s, const gchar * path_filter, const gchar * indent)
{
  JsonArray *list = arr (obj (s->findings), "findings");
  guint i, shown = 0, errors = 0;

  for (i = 0; list && i < json_array_get_length (list); i++) {
    JsonObject *f = json_array_get_object_element (list, i);

    if (path_filter && g_strcmp0 (str (f, "path"), path_filter) != 0)
      continue;
    if (shown++ == 0)
      g_print ("%sProblems\n", indent);
    if (severity_rank (str (f, "severity")) == 0)
      errors++;
    g_print ("%s  %-8s %s", indent, str (f, "severity"), str (f, "text"));
    if (inum (f, "count") > 1)
      g_print (" (%" G_GINT64_FORMAT " times)", inum (f, "count"));
    g_print ("\n");
    if (str (f, "hint") && *str (f, "hint"))
      g_print ("%s           %s\n", indent, str (f, "hint"));
    if (child_obj (f, "caps")) {
      JsonObject *caps = child_obj (f, "caps");

      g_print ("%s           upstream offers: %s\n%s           downstream "
          "accepts: %s\n", indent, str (caps, "upstream") &&
          *str (caps, "upstream") ? str (caps, "upstream") : "?", indent,
          str (caps, "downstream") && *str (caps, "downstream") ?
          str (caps, "downstream") : "?");
      if (!json_object_get_boolean_member_with_default (caps, "common", TRUE))
        g_print ("%s           The two sides have no format in common.\n",
            indent);
    }
  }
  if (shown == 0 && path_filter == NULL)
    g_print ("%sProblems: none found\n", indent);
  return errors;
}

static guint
count_errors (Status * s)
{
  JsonArray *list = arr (obj (s->findings), "findings");
  guint i, errors = 0;

  for (i = 0; list && i < json_array_get_length (list); i++)
    if (severity_rank (str (json_array_get_object_element (list, i),
                "severity")) == 0)
      errors++;
  return errors;
}

static gint
compare_entries (gconstpointer a, gconstpointer b)
{
  gint64 la = inum (obj (*(JsonNode **) a), "last");
  gint64 lb = inum (obj (*(JsonNode **) b), "last");

  return la < lb ? -1 : la > lb;
}

/* Log entries, oldest first, at path_filter only if set. */
static GPtrArray *
log_entries (Status * s, const gchar * path_filter)
{
  GPtrArray *entries = g_ptr_array_new ();
  GHashTableIter iter;
  gpointer value;

  g_hash_table_iter_init (&iter, s->log);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    if (!path_filter || g_strcmp0 (str (obj (value), "path"),
            path_filter) == 0)
      g_ptr_array_add (entries, value);
  g_ptr_array_sort (entries, compare_entries);
  return entries;
}

static void
print_entry (JsonObject * e, const gchar * indent)
{
  gchar *when = wall_text (inum (e, "last"));
  const gchar *debug = str (e, "debug");

  g_print ("%s%s %-7s %s: %s", indent, when, str (e, "level"),
      str (e, "path") ? str (e, "path") : "?", str (e, "text"));
  if (inum (e, "count") > 1)
    g_print (" (%" G_GINT64_FORMAT " times)", inum (e, "count"));
  g_print ("\n");
  if (debug && *debug) {
    /* GStreamer's debug part: the source line, then the details. */
    const gchar *nl = strchr (debug, '\n');

    g_print ("%s    %s\n", indent, nl ? nl + 1 : debug);
  }
  g_free (when);
}

static void
print_log_summary (Status * s)
{
  GPtrArray *entries = log_entries (s, NULL);
  guint i, errors = 0, warnings = 0;

  for (i = 0; i < entries->len; i++) {
    if (g_strcmp0 (str (obj (g_ptr_array_index (entries, i)), "level"),
            "error") == 0)
      errors++;
    else
      warnings++;
  }
  if (entries->len == 0) {
    g_print ("Log: no errors or warnings\n");
  } else {
    JsonObject *last = obj (g_ptr_array_index (entries, entries->len - 1));

    g_print ("Log: %u error%s, %u warning%s; the last (--log for all):\n",
        errors, errors == 1 ? "" : "s", warnings, warnings == 1 ? "" : "s");
    print_entry (last, "  ");
  }
  g_ptr_array_unref (entries);
}

static void
print_log (Status * s, gint n)
{
  GPtrArray *entries = log_entries (s, NULL);
  guint i;

  if (entries->len == 0)
    g_print ("Log: no errors or warnings\n");
  for (i = n > 0 && entries->len > (guint) n ? entries->len - n : 0;
      i < entries->len; i++)
    print_entry (obj (g_ptr_array_index (entries, i)), "");
  g_ptr_array_unref (entries);
}

/* Graph: element and pad indexes, links. */

typedef struct
{
  GHashTable *pads;             /* id -> JsonObject pad */
  GHashTable *owner;            /* pad id -> JsonObject element */
  GHashTable *peer;             /* src pad id -> sink pad id, and back */
} Index;

static void
index_element (Index * x, JsonObject * e)
{
  JsonArray *pads = arr (e, "pads"), *children = arr (e, "children");
  guint i;

  for (i = 0; pads && i < json_array_get_length (pads); i++) {
    JsonObject *p = json_array_get_object_element (pads, i);
    gint64 *id = g_new (gint64, 1);

    *id = inum (p, "id");
    g_hash_table_insert (x->pads, id, p);
    g_hash_table_insert (x->owner, id, e);
  }
  for (i = 0; children && i < json_array_get_length (children); i++)
    index_element (x, json_array_get_object_element (children, i));
}

static void
index_build (Index * x, Status * s)
{
  JsonArray *pipelines = arr (obj (s->graph), "pipelines");
  JsonArray *links = arr (obj (s->graph), "links");
  guint i;

  x->pads = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free, NULL);
  x->owner = g_hash_table_new (g_int64_hash, g_int64_equal);
  x->peer = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free,
      g_free);
  for (i = 0; pipelines && i < json_array_get_length (pipelines); i++)
    index_element (x, json_array_get_object_element (pipelines, i));
  for (i = 0; links && i < json_array_get_length (links); i++) {
    JsonObject *l = json_array_get_object_element (links, i);
    gint64 *a = g_new (gint64, 1), *b = g_new (gint64, 1);
    gint64 *c = g_new (gint64, 1), *d = g_new (gint64, 1);

    *a = *d = inum (l, "src");
    *b = *c = inum (l, "sink");
    g_hash_table_insert (x->peer, a, b);
    g_hash_table_insert (x->peer, c, d);
  }
}

static void
index_clear (Index * x)
{
  g_hash_table_unref (x->pads);
  g_hash_table_unref (x->owner);
  g_hash_table_unref (x->peer);
}

/* "element:pad" of the pad linked to this one, or NULL. */
static gchar *
peer_name (Index * x, JsonObject * pad)
{
  gint64 id = inum (pad, "id");
  gint64 *peer = g_hash_table_lookup (x->peer, &id);
  JsonObject *p, *e;

  if (peer == NULL)
    return NULL;
  p = g_hash_table_lookup (x->pads, peer);
  e = g_hash_table_lookup (x->owner, peer);
  return g_strdup_printf ("%s:%s", e ? str (e, "name") : "?",
      p ? str (p, "name") : "?");
}

static void
print_pad (Index * x, JsonObject * pad, const gchar * indent)
{
  gchar *peer = peer_name (x, pad);
  gboolean src = g_strcmp0 (str (pad, "direction"), "src") == 0;

  g_print ("%s%s %s%s", indent, str (pad, "name"), peer ? (src ? "-> " :
          "<- ") : "", peer ? peer : "(not linked)");
  if (g_strcmp0 (str (pad, "presence"), "always") != 0 && str (pad,
          "presence"))
    g_print (" [%s]", str (pad, "presence"));
  if (str (pad, "caps"))
    g_print ("\n%s    %s", indent, str (pad, "caps"));
  g_print ("\n");
  g_free (peer);
}

static void
print_tree (Index * x, JsonObject * e, gint depth)
{
  JsonArray *pads = arr (e, "pads"), *children = arr (e, "children");
  gchar *indent = g_strnfill (depth * 2, ' ');
  gchar *pad_indent = g_strnfill (depth * 2 + 4, ' ');
  guint i;

  g_print ("%s%s (%s) %s%s%s\n", indent, str (e, "name"), str (e,
          "factory") ? str (e, "factory") : str (e, "type"), str (e,
          "state") ? str (e, "state") : "", str (e, "pending") ? " -> " : "",
      str (e, "pending") ? str (e, "pending") : "");
  for (i = 0; pads && i < json_array_get_length (pads); i++) {
    JsonObject *pad = json_array_get_object_element (pads, i);

    /* Each link once, from its src side; unlinked pads both ways. */
    if (g_strcmp0 (str (pad, "direction"), "src") == 0 ||
        !g_hash_table_contains (x->peer, &(gint64) {
            inum (pad, "id")}))
      print_pad (x, pad, pad_indent);
  }
  for (i = 0; children && i < json_array_get_length (children); i++)
    print_tree (x, json_array_get_object_element (children, i), depth + 1);
  g_free (indent);
  g_free (pad_indent);
}

static void
print_graph (Status * s)
{
  JsonArray *pipelines = arr (obj (s->graph), "pipelines");
  Index x;
  guint i;

  index_build (&x, s);
  for (i = 0; pipelines && i < json_array_get_length (pipelines); i++)
    print_tree (&x, json_array_get_object_element (pipelines, i), 0);
  index_clear (&x);
}

/* Elements whose name or path is name, or whose path ends with /name. */
static void
find_elements (JsonObject * e, const gchar * name, GPtrArray * found)
{
  JsonArray *children = arr (e, "children");
  const gchar *path = str (e, "path");
  guint i;

  if (g_strcmp0 (str (e, "name"), name) == 0 || g_strcmp0 (path, name) == 0
      || (path && g_str_has_suffix (path, name) &&
          path[strlen (path) - strlen (name) - 1] == '/' &&
          strchr (name, '/') != NULL))
    g_ptr_array_add (found, e);
  for (i = 0; children && i < json_array_get_length (children); i++)
    find_elements (json_array_get_object_element (children, i), name, found);
}

static gchar *
value_text (JsonNode * v)
{
  JsonObject *o = obj (v);

  if (o) {
    if (str (o, "nick"))
      return g_strdup (str (o, "nick"));
    if (str (o, "caps"))
      return g_strdup (str (o, "caps"));
    if (str (o, "object"))
      return g_strdup (str (o, "object"));
    if (json_object_has_member (o, "object"))
      return g_strdup ("none");
  }
  if (v && JSON_NODE_HOLDS_NULL (v))
    return g_strdup ("none");
  if (v && JSON_NODE_HOLDS_VALUE (v) && json_node_get_value_type (v) ==
      G_TYPE_STRING)
    return g_strdup (json_node_get_string (v));
  return v ? json_to_string (v, FALSE) : g_strdup ("?");
}

static void
print_properties (JsonObject * element_props)
{
  JsonArray *props = arr (element_props, "properties");
  guint i, shown = 0;

  for (i = 0; props && i < json_array_get_length (props); i++) {
    JsonObject *p = json_array_get_object_element (props, i);
    gchar *value;

    if (!json_object_get_boolean_member_with_default (p, "changed", FALSE))
      continue;
    if (shown++ == 0)
      g_print ("Properties changed from their defaults\n");
    value = value_text (json_object_get_member (p, "value"));
    g_print ("  %s = %s\n", str (p, "name"), value);
    g_free (value);
  }
  if (props && shown == 0)
    g_print ("Properties: all at their defaults\n");
}

static gchar *
amount_text (const gchar * key, gdouble v)
{
  /* Plain ASCII: the terminal may not take UTF-8. */
  if (g_str_equal (key, "bytes"))
    return v >= 1e6 ? g_strdup_printf ("%.1f MB", v / 1e6) : v >= 1e3 ?
        g_strdup_printf ("%.1f kB", v / 1e3) : g_strdup_printf ("%.0f bytes",
        v);
  if (g_str_equal (key, "time"))
    return span_text (v);
  return g_strdup_printf ("%.0f", v);
}

static void
print_queue (Status * s, gint64 id)
{
  JsonArray *queues = arr (obj (s->levels), "queues");
  static const gchar *keys[] = { "buffers", "bytes", "time" };
  guint i, j, k;

  for (i = 0; queues && i < json_array_get_length (queues); i++) {
    JsonObject *q = json_array_get_object_element (queues, i);
    JsonObject *limits = child_obj (q, "limits");
    JsonArray *levels = arr (q, "levels");
    JsonObject *counts = child_obj (q, "counts");

    if (inum (q, "element") != id)
      continue;
    g_print ("Queue (%s)\n", str (q, "kind"));
    if (!levels || json_array_get_length (levels) == 0)
      g_print ("  level: not available\n");
    for (j = 0; levels && j < json_array_get_length (levels); j++) {
      JsonObject *level = json_array_get_object_element (levels, j);

      g_print ("  ");
      if (has_number (level, "pad"))
        g_print ("%s: ", short_name (str (level, "pad_path")));
      for (k = 0; k < G_N_ELEMENTS (keys); k++) {
        gdouble max = num (limits, keys[k]);

        gchar *v = !has_number (level, keys[k]) ? g_strdup ("?") :
            amount_text (keys[k], num (level, keys[k]));
        gchar *m = max > 0 ? amount_text (keys[k], max) : g_strdup ("no limit");

        g_print ("%s%s %s / %s", k ? ", " : "", keys[k], v, m);
        g_free (v);
        g_free (m);
      }
      g_print ("\n");
    }
    if (str (q, "at_limit")) {
      const gchar *a = str (q, "at_limit");

      g_print ("  at a limit: %s\n", g_str_equal (a, "drops") ? "drops" :
          g_str_equal (a, "signals") ? "only signals enough-data, and keeps "
          "queueing" : g_strcmp0 (str (q, "kind"), "appsink") == 0 ?
          "waits for the program to pull, holding up the pipeline" :
          "the program's push waits");
    }
    if (counts && has_number (counts, "in"))
      g_print ("  in %" G_GINT64_FORMAT ", out %" G_GINT64_FORMAT
          ", dropped %" G_GINT64_FORMAT "\n", inum (counts, "in"),
          inum (counts, "out"), inum (counts, "dropped"));
    if (has_number (q, "overruns"))
      g_print ("  overruns %" G_GINT64_FORMAT ", underruns %"
          G_GINT64_FORMAT "\n", inum (q, "overruns"), inum (q, "underruns"));
    if (str (q, "leaky") && g_strcmp0 (str (q, "leaky"), "no") != 0 &&
        g_strcmp0 (str (q, "leaky"), "none") != 0)
      g_print ("  leaky: %s\n", str (q, "leaky"));
    if (json_object_has_member (q, "segment")) {
      JsonObject *seg = child_obj (q, "segment");

      if (seg) {
        gboolean time = g_strcmp0 (str (seg, "format"), "time") == 0;
        gchar *start = time ? clock_text (inum (seg, "start")) :
            g_strdup_printf ("%" G_GINT64_FORMAT, inum (seg, "start"));
        gchar *base = time ? clock_text (inum (seg, "base")) :
            g_strdup_printf ("%" G_GINT64_FORMAT, inum (seg, "base"));

        g_print ("  segment %s: %s, from %s, base %s, rate %g\n",
            g_strcmp0 (str (q, "kind"), "appsrc") == 0 ? "sent" : "received",
            str (seg, "format"), start, base, num (seg, "rate"));
        g_free (start);
        g_free (base);
      }
      else
        g_print ("  segment: none yet\n");
    }
  }
}

static gint
print_element (Status * s, const gchar * name)
{
  JsonArray *pipelines = arr (obj (s->graph), "pipelines");
  GPtrArray *found = g_ptr_array_new ();
  JsonObject *e;
  JsonArray *pads;
  JsonNode *props = NULL;
  GPtrArray *entries;
  Index x;
  guint i;

  for (i = 0; pipelines && i < json_array_get_length (pipelines); i++)
    find_elements (json_array_get_object_element (pipelines, i), name, found);
  if (found->len != 1) {
    if (found->len == 0)
      g_printerr ("pipeline-monitor: no element %s in %s\n", name,
          s->source);
    else {
      g_printerr ("pipeline-monitor: %u elements named %s; give the path:\n",
          found->len, name);
      for (i = 0; i < found->len; i++)
        g_printerr ("  %s\n", str (g_ptr_array_index (found, i), "path"));
    }
    g_ptr_array_unref (found);
    return 2;
  }
  e = g_ptr_array_index (found, 0);
  g_ptr_array_unref (found);
  index_build (&x, s);
  g_print ("%s (%s, %s) %s\n", str (e, "path"), str (e, "factory") ?
      str (e, "factory") : "?", str (e, "type"), str (e, "state"));
  pads = arr (e, "pads");
  if (pads && json_array_get_length (pads))
    g_print ("Pads\n");
  for (i = 0; pads && i < json_array_get_length (pads); i++) {
    JsonObject *pad = json_array_get_object_element (pads, i);

    print_pad (&x, pad, "  ");
  }
  index_clear (&x);
  print_queue (s, inum (e, "id"));
  if (s->recording) {
    gint64 id = inum (e, "id");
    JsonNode *n = g_hash_table_lookup (s->props, &id);

    props = n ? json_node_ref (n) : NULL;
  } else {
    gchar *path = g_strdup_printf ("api/properties?element=%" G_GINT64_FORMAT,
        inum (e, "id"));
    JsonNode *answer = fetch (s->source, path);
    JsonArray *elements = arr (obj (answer), "elements");

    if (elements && json_array_get_length (elements))
      props = json_node_ref (json_array_get_element (elements, 0));
    g_clear_pointer (&answer, json_node_unref);
    g_free (path);
  }
  if (props) {
    JsonObject *reported = child_obj (obj (props), "reported");

    if (reported) {
      gchar *min = span_text (num (reported, "min"));
      gchar *max = has_number (reported, "max") ? span_text (num (reported,
              "max")) : g_strdup ("none");

      g_print ("Latency it reports: %s, min %s, max %s\n",
          json_object_get_boolean_member_with_default (reported, "live",
              FALSE) ? "live" : "not live", min, max);
      g_free (min);
      g_free (max);
    }
    print_properties (obj (props));
    json_node_unref (props);
  } else if (s->recording) {
    g_print ("Properties: not in the recording\n");
  }
  print_problems (s, str (e, "path"), "");
  entries = log_entries (s, str (e, "path"));
  if (entries->len)
    g_print ("Log\n");
  for (i = 0; i < entries->len; i++)
    print_entry (obj (g_ptr_array_index (entries, i)), "  ");
  g_ptr_array_unref (entries);
  return 0;
}

static void
print_json (Status * s)
{
  JsonNode *nodes[] = { s->hello, s->graph, s->levels, s->findings, s->perf };
  GPtrArray *entries = log_entries (s, NULL);
  JsonBuilder *b = json_builder_new ();
  JsonNode *log;
  gchar *text;
  guint i;

  for (i = 0; i < G_N_ELEMENTS (nodes); i++) {
    if (nodes[i] == NULL)
      continue;
    text = json_to_string (nodes[i], FALSE);
    g_print ("%s\n", text);
    g_free (text);
  }
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "type");
  json_builder_add_string_value (b, "log");
  json_builder_set_member_name (b, "entries");
  json_builder_begin_array (b);
  for (i = 0; i < entries->len; i++)
    json_builder_add_value (b, json_node_copy (g_ptr_array_index (entries,
                i)));
  json_builder_end_array (b);
  json_builder_end_object (b);
  log = json_builder_get_root (b);
  text = json_to_string (log, FALSE);
  g_print ("%s\n", text);
  g_free (text);
  json_node_unref (log);
  g_object_unref (b);
  g_ptr_array_unref (entries);
}

typedef struct
{
  gboolean graph, json, watch;
  gint log;
  gchar *element, *url, *at;
} Options;

/* One program or recording, as asked; the exit code it gives. */
static gint
show (Status * s, Options * o)
{
  gint errors = count_errors (s);

  if (o->json) {
    print_json (s);
    return errors ? 1 : 0;
  }
  print_header (s);
  if (o->element)
    return print_element (s, o->element) ? 2 : errors ? 1 : 0;
  if (o->graph) {
    print_graph (s);
    return errors ? 1 : 0;
  }
  if (o->log >= 0) {
    print_log (s, o->log);
    return errors ? 1 : 0;
  }
  print_pipelines (s);
  print_performance (s);
  print_problems (s, NULL, "");
  print_log_summary (s);
  return errors ? 1 : 0;
}

/* The programs found (in port order), each a Status. */
static GPtrArray *
find_programs (const gchar * url)
{
  GPtrArray *found = g_ptr_array_new_with_free_func ((GDestroyNotify)
      status_free);
  gint port;

  if (url) {
    gchar *base = g_str_has_suffix (url, "/") ? g_strdup (url) :
        g_strconcat (url, "/", NULL);
    Status *s = read_program (base);

    if (s)
      g_ptr_array_add (found, s);
    g_free (base);
    return found;
  }
  for (port = FIRST_PORT; port < FIRST_PORT + PORTS; port++) {
    gchar *base = g_strdup_printf ("http://127.0.0.1:%d/", port);
    Status *s = read_program (base);

    if (s)
      g_ptr_array_add (found, s);
    g_free (base);
  }
  return found;
}

static gint
show_programs (Options * o)
{
  GPtrArray *programs = find_programs (o->url);
  gint code = 0;
  guint i;

  if (programs->len == 0) {
    if (o->url)
      g_printerr ("pipeline-monitor: no program with the tracer answers at "
          "%s\n", o->url);
    else
      g_printerr ("pipeline-monitor: no program with the tracer found on "
          "ports %d-%d (run it with GST_TRACERS=pipeline-monitor, or give "
          "--url)\n", FIRST_PORT, FIRST_PORT + PORTS - 1);
    g_ptr_array_unref (programs);
    return 3;
  }
  for (i = 0; i < programs->len; i++) {
    gint c;

    if (i > 0 && !o->json)
      g_print ("\n");
    c = show (g_ptr_array_index (programs, i), o);
    code = MAX (code, c);
  }
  g_ptr_array_unref (programs);
  return code;
}

/* UTF-8 as it is: g_print would turn what a non-UTF-8 locale cannot show
 * into "?", and assistants' shells often run in the C locale. */
static void
print_utf8 (const gchar * text)
{
  fputs (text, stdout);
}

static const gchar *usage =
    "[FILE.jsonl]\n\n"
    "What pipeline-monitor sees, as text: every running program with the\n"
    "tracer (ports 8765-8774, or --url), or a recording.\n\n"
    "Exit code: 0, or 1 when an error was found; 2 wrong use; 3 nothing to\n"
    "read.";

gint
pm_status_main (gint argc, gchar ** argv)
{
  Options o = { 0, };
  gchar *log_text = NULL;
  gboolean log_flag = FALSE;
  GOptionEntry entries[] = {
    {"graph", 0, 0, G_OPTION_ARG_NONE, &o.graph,
        "Every element, its pads, links and caps", NULL},
    {"element", 0, 0, G_OPTION_ARG_STRING, &o.element,
          "One element (name or path): pads, queue, changed properties, "
          "problems, log", "NAME"},
    {"log", 0, 0, G_OPTION_ARG_NONE, &log_flag,
        "The errors and warnings logged", NULL},
    {"last", 0, 0, G_OPTION_ARG_STRING, &log_text,
        "With --log, only the last N entries", "N"},
    {"watch", 0, 0, G_OPTION_ARG_NONE, &o.watch,
        "Again every second, until Ctrl+C", NULL},
    {"json", 0, 0, G_OPTION_ARG_NONE, &o.json,
        "The plugin's own messages, one JSON object per line", NULL},
    {"url", 0, 0, G_OPTION_ARG_STRING, &o.url,
        "A program at this address (e.g. http://127.0.0.1:8765/)", "URL"},
    {"at", 0, 0, G_OPTION_ARG_STRING, &o.at,
        "In a recording: the state at this time from its start (12.5, "
        "1:02.5)", "TIME"},
    {NULL, 0, 0, 0, NULL, NULL, NULL}
  };
  GOptionContext *context = g_option_context_new (usage);
  GError *error = NULL;
  gint code;

  g_set_print_handler (print_utf8);
  /* The name in --help. */
  g_set_prgname ("pipeline-monitor status");
  g_option_context_add_main_entries (context, entries, NULL);
  if (!g_option_context_parse (context, &argc, &argv, &error)) {
    g_printerr ("pipeline-monitor: %s\n", error->message);
    g_error_free (error);
    g_option_context_free (context);
    return 2;
  }
  g_option_context_free (context);
  o.log = -1;
  if (log_flag)
    o.log = log_text ? atoi (log_text) : 0;
  if (argc > 2 || (argc == 2 && (o.url || o.watch)) || (o.at && argc != 2) ||
      (log_text && !log_flag) || (log_text && atoi (log_text) <= 0)) {
    g_printerr ("pipeline-monitor: see pipeline-monitor status --help\n");
    return 2;
  }

  if (argc == 2) {
    gdouble at = o.at ? parse_time (o.at) : -1;
    Status *s;

    if (o.at && at < 0) {
      g_printerr ("pipeline-monitor: --at takes a time such as 12.5 or "
          "1:02.5\n");
      return 2;
    }
    if (g_str_has_suffix (argv[1], ".dot")) {
      g_printerr ("pipeline-monitor: status reads recordings; to see a .dot "
          "file, use pipeline-monitor open %s\n", argv[1]);
      return 2;
    }
    s = read_recording (argv[1], at, &error);
    if (s == NULL) {
      g_printerr ("pipeline-monitor: cannot read %s: %s\n", argv[1],
          error->message);
      g_error_free (error);
      return 3;
    }
    code = show (s, &o);
    status_free (s);
    return code;
  }

  if (!o.watch)
    return show_programs (&o);
  for (;;) {
#ifdef G_OS_UNIX
    if (isatty (1))
      g_print ("\033[H\033[2J");
#endif
    code = show_programs (&o);
    fflush (stdout);
    g_usleep (G_USEC_PER_SEC);
  }
  return code;
}
