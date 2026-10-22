/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pmprops.h"
#include "pmlevels.h"

#include <math.h>

/* Element properties as JSON. Getters are the element's own code and could
 * block, so they run on worker threads; the caller waits a limited time. A
 * property that does not answer in time is skipped (for its element type)
 * for as long as that read is still running; a getter that was only slow
 * is read again once it returns. */

typedef struct
{
  gint ref;
  GMutex lock;
  GCond cond;
  GstElement *element;
  gchar **names;
  /* Being read, for the "not answering" list. */
  const gchar *current;
  GString *json;
  gboolean done;
} PmReadJob;

static GMutex silent_lock;
/* "<type>:<property>" -> the job still reading it. */
static GHashTable *silent;
static GThreadPool *pool;

static void job_unref (PmReadJob * job);

static gchar *
silent_key (GstElement * element, const gchar * name)
{
  return g_strconcat (G_OBJECT_TYPE_NAME (element), ":", name, NULL);
}

static gboolean
is_silent (GstElement * element, const gchar * name)
{
  gchar *key = silent_key (element, name);
  PmReadJob *job;
  gboolean stuck = FALSE;

  g_mutex_lock (&silent_lock);
  job = silent ? g_hash_table_lookup (silent, key) : NULL;
  if (job != NULL) {
    g_mutex_lock (&job->lock);
    stuck = !job->done;
    g_mutex_unlock (&job->lock);
    if (!stuck)
      g_hash_table_remove (silent, key);
  }
  g_mutex_unlock (&silent_lock);
  g_free (key);
  return stuck;
}

static void
make_silent (PmReadJob * job, const gchar * name)
{
  g_mutex_lock (&silent_lock);
  if (silent == NULL)
    silent = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
        (GDestroyNotify) job_unref);
  g_atomic_int_inc (&job->ref);
  g_hash_table_insert (silent, silent_key (job->element, name), job);
  g_mutex_unlock (&silent_lock);
}

static void
job_unref (PmReadJob * job)
{
  if (!g_atomic_int_dec_and_test (&job->ref))
    return;
  gst_object_unref (job->element);
  g_strfreev (job->names);
  if (job->json != NULL)
    g_string_free (job->json, TRUE);
  g_mutex_clear (&job->lock);
  g_cond_clear (&job->cond);
  g_free (job);
}

static void
append_escaped (GString * json, const gchar * str)
{
  const guchar *p;

  g_string_append_c (json, '"');
  for (p = (const guchar *) str; *p != '\0'; p++) {
    if (*p == '"' || *p == '\\')
      g_string_append_printf (json, "\\%c", *p);
    else if (*p < 0x20)
      g_string_append_printf (json, "\\u%04x", *p);
    else
      g_string_append_c (json, *p);
  }
  g_string_append_c (json, '"');
}

static void
append_double (GString * json, gdouble d)
{
  gchar buf[G_ASCII_DTOSTR_BUF_SIZE];

  if (isnan (d) || isinf (d))
    g_string_append (json, "null");
  else
    g_string_append (json, g_ascii_dtostr (buf, sizeof (buf), d));
}

static gboolean
append_field (GQuark field, const GValue * value, gpointer user_data)
{
  GString *json = user_data;

  if (json->str[json->len - 1] != '{')
    g_string_append_c (json, ',');
  append_escaped (json, g_quark_to_string (field));
  g_string_append_c (json, ':');
  pm_props_append_value (json, value);
  return TRUE;
}

static void
append_structure (GString * json, const GstStructure * s)
{
  g_string_append (json, "{\"structure\":");
  append_escaped (json, gst_structure_get_name (s));
  g_string_append (json, ",\"fields\":{");
  gst_structure_foreach (s, append_field, json);
  g_string_append (json, "}}");
}

/* Any value as JSON: plain types as such, enums as {"nick","value"},
 * flags as a list of nicks, caps as text, structures with their fields,
 * objects by name; anything else as {"type": name}. */
void
pm_props_append_value (GString * json, const GValue * value)
{
  GType type = G_VALUE_TYPE (value);
  guint i;

  switch (G_TYPE_FUNDAMENTAL (type)) {
    case G_TYPE_BOOLEAN:
      g_string_append (json, g_value_get_boolean (value) ? "true" : "false");
      return;
    case G_TYPE_CHAR:
      g_string_append_printf (json, "%d", g_value_get_schar (value));
      return;
    case G_TYPE_UCHAR:
      g_string_append_printf (json, "%u", g_value_get_uchar (value));
      return;
    case G_TYPE_INT:
      g_string_append_printf (json, "%d", g_value_get_int (value));
      return;
    case G_TYPE_UINT:
      g_string_append_printf (json, "%u", g_value_get_uint (value));
      return;
    case G_TYPE_LONG:
      g_string_append_printf (json, "%ld", g_value_get_long (value));
      return;
    case G_TYPE_ULONG:
      g_string_append_printf (json, "%lu", g_value_get_ulong (value));
      return;
    case G_TYPE_INT64:
      g_string_append_printf (json, "%" G_GINT64_FORMAT,
          g_value_get_int64 (value));
      return;
    case G_TYPE_UINT64:
      g_string_append_printf (json, "%" G_GUINT64_FORMAT,
          g_value_get_uint64 (value));
      return;
    case G_TYPE_FLOAT:
      append_double (json, g_value_get_float (value));
      return;
    case G_TYPE_DOUBLE:
      append_double (json, g_value_get_double (value));
      return;
    case G_TYPE_STRING:
      if (g_value_get_string (value) == NULL)
        g_string_append (json, "null");
      else
        append_escaped (json, g_value_get_string (value));
      return;
    case G_TYPE_ENUM:{
      GEnumClass *klass = g_type_class_ref (type);
      GEnumValue *e = g_enum_get_value (klass, g_value_get_enum (value));

      g_string_append (json, "{\"nick\":");
      append_escaped (json, e ? e->value_nick : "?");
      g_string_append_printf (json, ",\"value\":%d}", g_value_get_enum (value));
      g_type_class_unref (klass);
      return;
    }
    case G_TYPE_FLAGS:{
      GFlagsClass *klass = g_type_class_ref (type);
      guint flags = g_value_get_flags (value);
      gboolean first = TRUE;

      g_string_append (json, "{\"flags\":[");
      for (i = 0; i < klass->n_values; i++) {
        GFlagsValue *f = &klass->values[i];

        if (f->value != 0 && (flags & f->value) == f->value) {
          if (!first)
            g_string_append_c (json, ',');
          append_escaped (json, f->value_nick);
          first = FALSE;
        }
      }
      g_string_append (json, "]}");
      g_type_class_unref (klass);
      return;
    }
    case G_TYPE_OBJECT:{
      GObject *object = g_value_get_object (value);

      if (object == NULL) {
        g_string_append (json, "null");
      } else {
        g_string_append (json, "{\"object\":");
        append_escaped (json, GST_IS_OBJECT (object) ?
            GST_OBJECT_NAME (object) : G_OBJECT_TYPE_NAME (object));
        g_string_append (json, ",\"type\":");
        append_escaped (json, G_OBJECT_TYPE_NAME (object));
        g_string_append_c (json, '}');
      }
      return;
    }
    default:
      break;
  }

  if (type == GST_TYPE_CAPS) {
    const GstCaps *caps = gst_value_get_caps (value);
    gchar *text = caps ? gst_caps_to_string (caps) : NULL;

    g_string_append (json, "{\"caps\":");
    if (text)
      append_escaped (json, text);
    else
      g_string_append (json, "null");
    g_string_append_c (json, '}');
    g_free (text);
  } else if (type == GST_TYPE_STRUCTURE) {
    const GstStructure *s = gst_value_get_structure (value);

    if (s)
      append_structure (json, s);
    else
      g_string_append (json, "null");
  } else if (type == GST_TYPE_FRACTION) {
    g_string_append_printf (json, "\"%d/%d\"",
        gst_value_get_fraction_numerator (value),
        gst_value_get_fraction_denominator (value));
  } else if (type == GST_TYPE_ARRAY || type == GST_TYPE_LIST) {
    guint n = type == GST_TYPE_ARRAY ? gst_value_array_get_size (value) :
        gst_value_list_get_size (value);

    g_string_append_c (json, '[');
    for (i = 0; i < n; i++) {
      const GValue *item = type == GST_TYPE_ARRAY ?
          gst_value_array_get_value (value, i) :
          gst_value_list_get_value (value, i);

      if (i > 0)
        g_string_append_c (json, ',');
      pm_props_append_value (json, item);
    }
    g_string_append_c (json, ']');
  } else if (type == G_TYPE_STRV) {
    const gchar *const *strv = g_value_get_boxed (value);

    g_string_append_c (json, '[');
    for (i = 0; strv && strv[i]; i++) {
      if (i > 0)
        g_string_append_c (json, ',');
      append_escaped (json, strv[i]);
    }
    g_string_append_c (json, ']');
  } else if (type == GST_TYPE_SAMPLE) {
    /* last-sample: only what it is, not the frame. */
    GstSample *sample = g_value_get_boxed (value);
    GstCaps *caps = sample ? gst_sample_get_caps (sample) : NULL;
    gchar *text = caps ? gst_caps_to_string (caps) : NULL;

    g_string_append (json, "{\"type\":\"GstSample\",\"caps\":");
    if (text)
      append_escaped (json, text);
    else
      g_string_append (json, "null");
    g_string_append_c (json, '}');
    g_free (text);
  } else {
    g_string_append (json, "{\"type\":");
    append_escaped (json, g_type_name (type));
    g_string_append_c (json, '}');
  }
}

/* Ranges of numbers and the choices of enums and flags, for the hover. */
static void
append_range (GString * json, GParamSpec * pspec)
{
  GValue min = G_VALUE_INIT, max = G_VALUE_INIT;
  gboolean have = TRUE;

#define RANGE(PT, GT, set) \
  if (G_IS_PARAM_SPEC_##PT (pspec)) { \
    g_value_init (&min, GT); g_value_init (&max, GT); \
    set (&min, G_PARAM_SPEC_##PT (pspec)->minimum); \
    set (&max, G_PARAM_SPEC_##PT (pspec)->maximum); \
  } else
  RANGE (INT, G_TYPE_INT, g_value_set_int)
      RANGE (UINT, G_TYPE_UINT, g_value_set_uint)
      RANGE (LONG, G_TYPE_LONG, g_value_set_long)
      RANGE (ULONG, G_TYPE_ULONG, g_value_set_ulong)
      RANGE (INT64, G_TYPE_INT64, g_value_set_int64)
      RANGE (UINT64, G_TYPE_UINT64, g_value_set_uint64)
      RANGE (FLOAT, G_TYPE_FLOAT, g_value_set_float)
      RANGE (DOUBLE, G_TYPE_DOUBLE, g_value_set_double)
      have = FALSE;
#undef RANGE

  if (have) {
    g_string_append (json, ",\"min\":");
    pm_props_append_value (json, &min);
    g_string_append (json, ",\"max\":");
    pm_props_append_value (json, &max);
    g_value_unset (&min);
    g_value_unset (&max);
  }

  if (G_IS_PARAM_SPEC_ENUM (pspec) || G_IS_PARAM_SPEC_FLAGS (pspec)) {
    gboolean is_enum = G_IS_PARAM_SPEC_ENUM (pspec);
    GTypeClass *klass = is_enum ?
        (GTypeClass *) G_PARAM_SPEC_ENUM (pspec)->enum_class :
        (GTypeClass *) G_PARAM_SPEC_FLAGS (pspec)->flags_class;
    guint n = is_enum ? ((GEnumClass *) klass)->n_values :
        ((GFlagsClass *) klass)->n_values;
    guint i;

    g_string_append (json, ",\"choices\":[");
    for (i = 0; i < n; i++) {
      if (i > 0)
        g_string_append_c (json, ',');
      append_escaped (json, is_enum ?
          ((GEnumClass *) klass)->values[i].value_nick :
          ((GFlagsClass *) klass)->values[i].value_nick);
    }
    g_string_append_c (json, ']');
  }
}

/* Only settings of plain types are compared with their default: name and
 * parent (from GstObject) always differ, and read-only values (levels,
 * counters) are not settings. */
static gboolean
comparable (GParamSpec * pspec)
{
  if (pspec->owner_type == GST_TYPE_OBJECT ||
      !(pspec->flags & G_PARAM_WRITABLE))
    return FALSE;
  switch (G_TYPE_FUNDAMENTAL (pspec->value_type)) {
    case G_TYPE_OBJECT:
    case G_TYPE_BOXED:
    case G_TYPE_POINTER:
    case G_TYPE_PARAM:
      return FALSE;
    default:
      return TRUE;
  }
}

static void
read_property (PmReadJob * job, GString * json, GParamSpec * pspec)
{
  GValue value = G_VALUE_INIT;
  const GValue *def = g_param_spec_get_default_value (pspec);

  g_string_append (json, "{\"name\":");
  append_escaped (json, pspec->name);
  g_string_append (json, ",\"type\":");
  append_escaped (json, g_type_name (pspec->value_type));
  g_string_append (json, ",\"blurb\":");
  append_escaped (json, g_param_spec_get_blurb (pspec) ?
      g_param_spec_get_blurb (pspec) : "");
  g_string_append_printf (json, ",\"writable\":%s",
      (pspec->flags & G_PARAM_WRITABLE) ? "true" : "false");

  if (is_silent (job->element, pspec->name)) {
    g_string_append (json, ",\"not_answering\":true}");
    return;
  }

  g_mutex_lock (&job->lock);
  job->current = pspec->name;
  g_mutex_unlock (&job->lock);

  g_value_init (&value, pspec->value_type);
  g_object_get_property (G_OBJECT (job->element), pspec->name, &value);
  g_string_append (json, ",\"value\":");
  pm_props_append_value (json, &value);
  if (comparable (pspec)) {
    g_string_append (json, ",\"default\":");
    pm_props_append_value (json, def);
    g_string_append_printf (json, ",\"changed\":%s",
        g_param_values_cmp (pspec, &value, def) != 0 ? "true" : "false");
  }
  append_range (json, pspec);
  g_value_unset (&value);
  g_string_append_c (json, '}');
}

static void
run_job (gpointer data, gpointer user_data)
{
  PmReadJob *job = data;
  GString *json = g_string_new ("[");
  guint n, i;
  GParamSpec **specs = g_object_class_list_properties (G_OBJECT_GET_CLASS
      (job->element), &n);

  for (i = 0; i < n; i++) {
    if (!(specs[i]->flags & G_PARAM_READABLE))
      continue;
    if (job->names != NULL &&
        !g_strv_contains ((const gchar * const *) job->names, specs[i]->name))
      continue;
    if (json->len > 1)
      g_string_append_c (json, ',');
    read_property (job, json, specs[i]);
  }
  g_string_append_c (json, ']');
  g_free (specs);

  g_mutex_lock (&job->lock);
  job->json = json;
  job->done = TRUE;
  g_cond_signal (&job->cond);
  g_mutex_unlock (&job->lock);
  job_unref (job);
}

/* The element's readable properties (all, or only names) as
 * {"schema":1,"time":...,"properties":[...]}. If a getter does not return
 * within timeout_us, that property is marked as not answering and
 * {"schema":1,"time":...,"not_answering":"name"} is returned instead. */
gchar *
pm_props_read (GstElement * element, const gchar * const *names,
    gint64 timeout_us)
{
  static gsize init;
  PmReadJob *job = g_new0 (PmReadJob, 1);
  gint64 deadline = g_get_monotonic_time () + timeout_us;
  GString *json = g_string_new (NULL);
  const gchar *stuck = NULL;

  if (g_once_init_enter (&init)) {
    /* Not exclusive, no limit: a stuck getter keeps only its own thread. */
    pool = g_thread_pool_new (run_job, NULL, -1, FALSE, NULL);
    g_once_init_leave (&init, 1);
  }

  job->ref = 2;
  g_mutex_init (&job->lock);
  g_cond_init (&job->cond);
  job->element = gst_object_ref (element);
  job->names = g_strdupv ((gchar **) names);
  g_thread_pool_push (pool, job, NULL);

  g_string_append_printf (json, "{\"schema\":%d,\"time\":%" G_GINT64_FORMAT,
      PM_SCHEMA, g_get_real_time ());
  g_mutex_lock (&job->lock);
  while (!job->done && g_cond_wait_until (&job->cond, &job->lock, deadline));
  if (job->done) {
    g_string_append (json, ",\"properties\":");
    g_string_append_len (json, job->json->str, job->json->len);
  } else {
    stuck = job->current;
    g_string_append (json, ",\"not_answering\":");
    append_escaped (json, stuck ? stuck : "?");
  }
  g_mutex_unlock (&job->lock);
  /* Not with the job locked: is_silent locks in the other order. */
  if (stuck != NULL)
    make_silent (job, stuck);
  job_unref (job);

  g_string_append_c (json, '}');
  return g_string_free (json, FALSE);
}
