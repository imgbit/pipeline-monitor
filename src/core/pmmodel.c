/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pmmodel.h"

typedef struct _PmElement PmElement;

struct _PmElement
{
  guint64 id;
  gchar *name;
  gchar *factory;
  gchar *type;
  gboolean is_bin;
  gboolean is_pipeline;
  GstState state;
  GstState pending;
  guint64 errors;
  guint64 warnings;
  PmElement *parent;
  GPtrArray *children;
  GPtrArray *pads;
};

typedef struct _PmPad PmPad;

struct _PmPad
{
  guint64 id;
  gchar *name;
  GstPadDirection direction;
  gchar *presence;
  gboolean is_ghost;
  PmElement *element;
};

typedef struct _PmLink PmLink;

struct _PmLink
{
  gpointer src;
  gpointer sink;
};

#define LOG_SIZE 500

typedef struct _PmLogEntry PmLogEntry;

struct _PmLogEntry
{
  guint64 id;
  guint64 seq;
  gint64 first;
  gint64 last;
  guint64 count;
  PmLogLevel level;
  gchar *source;
  guint64 element_id;
  gchar *path;
  gchar *text;
  gchar *debug;
  gchar *domain;
  gint code;
  gchar *key;
  GList *link;
};

/* Objects are keyed by address. The model holds no references and only
 * reads an object while a caller passes it in. */
struct _PmModel
{
  GMutex lock;
  guint64 next_id;
  GHashTable *elements;
  GHashTable *pads;
  GHashTable *links_by_src;
  GHashTable *links_by_sink;

  PmModelChangedFunc changed_func;
  gpointer changed_data;

  /* Oldest first. Repeats of an entry update it and move it to the end. */
  gboolean log_enabled;
  GQueue log;
  GHashTable *log_by_key;
  guint64 next_log_id;
  guint64 log_seq;
};

static void
pm_element_free (PmElement * element)
{
  g_free (element->name);
  g_free (element->factory);
  g_free (element->type);
  g_ptr_array_unref (element->children);
  g_ptr_array_unref (element->pads);
  g_free (element);
}

static void
pm_log_entry_free (PmLogEntry * entry)
{
  g_free (entry->source);
  g_free (entry->path);
  g_free (entry->text);
  g_free (entry->debug);
  g_free (entry->domain);
  g_free (entry->key);
  g_free (entry);
}

static void
pm_pad_free (PmPad * pad)
{
  g_free (pad->name);
  g_free (pad->presence);
  g_free (pad);
}

PmModel *
pm_model_new (void)
{
  PmModel *model = g_new0 (PmModel, 1);

  g_mutex_init (&model->lock);
  model->next_id = 1;
  model->elements = g_hash_table_new_full (NULL, NULL, NULL,
      (GDestroyNotify) pm_element_free);
  model->pads = g_hash_table_new_full (NULL, NULL, NULL,
      (GDestroyNotify) pm_pad_free);
  model->links_by_src = g_hash_table_new (NULL, NULL);
  model->links_by_sink = g_hash_table_new_full (NULL, NULL, NULL, g_free);
  model->log_enabled = TRUE;
  g_queue_init (&model->log);
  model->log_by_key = g_hash_table_new (g_str_hash, g_str_equal);
  model->next_log_id = 1;

  return model;
}

void
pm_model_free (PmModel * model)
{
  g_hash_table_unref (model->links_by_src);
  g_hash_table_unref (model->links_by_sink);
  g_hash_table_unref (model->pads);
  g_hash_table_unref (model->elements);
  g_hash_table_unref (model->log_by_key);
  g_queue_clear_full (&model->log, (GDestroyNotify) pm_log_entry_free);
  g_mutex_clear (&model->lock);
  g_free (model);
}

/* Called with the lock held, so the callback must be cheap. Holding the lock
 * also means no call is in progress once the callback is cleared. */
static void
changed (PmModel * model)
{
  if (model->changed_func != NULL)
    model->changed_func (model->changed_data);
}

void
pm_model_set_changed_func (PmModel * model, PmModelChangedFunc func,
    gpointer user_data)
{
  g_mutex_lock (&model->lock);
  model->changed_func = func;
  model->changed_data = user_data;
  g_mutex_unlock (&model->lock);
}

static void
remove_link (PmModel * model, PmLink * link)
{
  changed (model);
  g_hash_table_remove (model->links_by_src, link->src);
  g_hash_table_remove (model->links_by_sink, link->sink);
}

static void
remove_links_of (PmModel * model, gpointer pad)
{
  PmLink *link;

  if ((link = g_hash_table_lookup (model->links_by_src, pad)))
    remove_link (model, link);
  if ((link = g_hash_table_lookup (model->links_by_sink, pad)))
    remove_link (model, link);
}

static void
remove_pad (PmModel * model, gpointer object)
{
  PmPad *pad = g_hash_table_lookup (model->pads, object);

  remove_links_of (model, object);
  if (pad == NULL)
    return;
  g_ptr_array_remove (pad->element->pads, object);
  g_hash_table_remove (model->pads, object);
  changed (model);
}

static void
set_parent (PmModel * model, gpointer object, PmElement * element,
    PmElement * parent)
{
  if (element->parent == parent)
    return;
  changed (model);
  if (element->parent != NULL)
    g_ptr_array_remove (element->parent->children, object);
  element->parent = parent;
  if (parent != NULL)
    g_ptr_array_add (parent->children, object);
}

static void
remove_element (PmModel * model, gpointer object, PmElement * element)
{
  while (element->children->len > 0) {
    gpointer child = g_ptr_array_index (element->children, 0);
    set_parent (model, child, g_hash_table_lookup (model->elements, child),
        NULL);
  }
  while (element->pads->len > 0)
    remove_pad (model, g_ptr_array_index (element->pads, 0));
  set_parent (model, object, element, NULL);
  g_hash_table_remove (model->elements, object);
}

static gboolean
replace_string (gchar ** dest, const gchar * value)
{
  if (value == NULL || g_strcmp0 (*dest, value) == 0)
    return FALSE;
  g_free (*dest);
  *dest = g_strdup (value);
  return TRUE;
}

void
pm_model_update_element (PmModel * model, gpointer object, const gchar * name,
    const gchar * factory, const gchar * type, gboolean is_bin,
    gboolean is_pipeline)
{
  PmElement *element;
  gboolean updated = FALSE;

  g_mutex_lock (&model->lock);
  element = g_hash_table_lookup (model->elements, object);
  if (element == NULL) {
    updated = TRUE;
    element = g_new0 (PmElement, 1);
    element->id = model->next_id++;
    element->state = GST_STATE_NULL;
    element->children = g_ptr_array_new ();
    element->pads = g_ptr_array_new ();
    g_hash_table_insert (model->elements, object, element);
  }
  updated |= replace_string (&element->name, name);
  updated |= replace_string (&element->factory, factory);
  updated |= replace_string (&element->type, type);
  updated |= element->is_bin != is_bin || element->is_pipeline != is_pipeline;
  element->is_bin = is_bin;
  element->is_pipeline = is_pipeline;
  if (updated)
    changed (model);
  g_mutex_unlock (&model->lock);
}

void
pm_model_set_parent (PmModel * model, gpointer object, gpointer parent)
{
  PmElement *element;

  g_mutex_lock (&model->lock);
  element = g_hash_table_lookup (model->elements, object);
  if (element != NULL) {
    set_parent (model, object, element,
        parent ? g_hash_table_lookup (model->elements, parent) : NULL);
  }
  g_mutex_unlock (&model->lock);
}

static void
store_state (PmModel * model, gpointer object, GstState state,
    GstState pending)
{
  PmElement *element = g_hash_table_lookup (model->elements, object);

  if (element != NULL && (element->state != state ||
          element->pending != pending)) {
    element->state = state;
    element->pending = pending;
    changed (model);
  }
}

/* Reads the state inside the lock so that concurrent callers cannot store
 * an older value over a newer one. */
void
pm_model_sync_state (PmModel * model, GstElement * object)
{
  g_mutex_lock (&model->lock);
  store_state (model, object, GST_STATE (object), GST_STATE_PENDING (object));
  g_mutex_unlock (&model->lock);
}

void
pm_model_set_state (PmModel * model, gpointer object, GstState state,
    GstState pending)
{
  g_mutex_lock (&model->lock);
  store_state (model, object, state, pending);
  g_mutex_unlock (&model->lock);
}

void
pm_model_add_pad (PmModel * model, gpointer element_object, gpointer object,
    const gchar * name, GstPadDirection direction, const gchar * presence,
    gboolean is_ghost)
{
  PmElement *element;
  PmPad *pad;

  g_mutex_lock (&model->lock);
  element = g_hash_table_lookup (model->elements, element_object);
  if (element == NULL)
    goto done;

  pad = g_hash_table_lookup (model->pads, object);
  if (pad == NULL) {
    pad = g_new0 (PmPad, 1);
    pad->id = model->next_id++;
    g_hash_table_insert (model->pads, object, pad);
  } else {
    g_ptr_array_remove (pad->element->pads, object);
  }
  replace_string (&pad->name, name);
  replace_string (&pad->presence, presence);
  pad->direction = direction;
  pad->is_ghost = is_ghost;
  pad->element = element;
  g_ptr_array_add (element->pads, object);
  changed (model);

done:
  g_mutex_unlock (&model->lock);
}

void
pm_model_remove_pad (PmModel * model, gpointer object)
{
  g_mutex_lock (&model->lock);
  remove_pad (model, object);
  g_mutex_unlock (&model->lock);
}

void
pm_model_link (PmModel * model, gpointer src, gpointer sink)
{
  PmLink *link;

  g_mutex_lock (&model->lock);
  if ((link = g_hash_table_lookup (model->links_by_src, src)))
    remove_link (model, link);
  if ((link = g_hash_table_lookup (model->links_by_sink, sink)))
    remove_link (model, link);

  link = g_new0 (PmLink, 1);
  link->src = src;
  link->sink = sink;
  g_hash_table_insert (model->links_by_src, src, link);
  g_hash_table_insert (model->links_by_sink, sink, link);
  changed (model);
  g_mutex_unlock (&model->lock);
}

void
pm_model_unlink (PmModel * model, gpointer src, gpointer sink)
{
  PmLink *link;

  g_mutex_lock (&model->lock);
  link = g_hash_table_lookup (model->links_by_src, src);
  if (link != NULL && link->sink == sink)
    remove_link (model, link);
  g_mutex_unlock (&model->lock);
}

void
pm_model_remove_object (PmModel * model, gpointer object)
{
  PmElement *element;

  g_mutex_lock (&model->lock);
  element = g_hash_table_lookup (model->elements, object);
  if (element != NULL)
    remove_element (model, object, element);
  else
    remove_pad (model, object);
  g_mutex_unlock (&model->lock);
}

static void
append_string (GString * json, const gchar * str)
{
  const guchar *p;

  g_string_append_c (json, '"');
  for (p = (const guchar *) (str ? str : ""); *p != '\0'; p++) {
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
}

static void
append_pad (GString * json, PmPad * pad)
{
  g_string_append_printf (json, "{\"id\":%" G_GUINT64_FORMAT ",\"name\":",
      pad->id);
  append_string (json, pad->name);
  g_string_append_printf (json, ",\"direction\":\"%s\"",
      pad->direction == GST_PAD_SRC ? "src" :
      pad->direction == GST_PAD_SINK ? "sink" : "unknown");
  if (pad->presence != NULL)
    g_string_append_printf (json, ",\"presence\":\"%s\"", pad->presence);
  g_string_append_printf (json, ",\"ghost\":%s}",
      pad->is_ghost ? "true" : "false");
}

static void
append_element (PmModel * model, GString * json, PmElement * element)
{
  guint i;

  g_string_append_printf (json, "{\"id\":%" G_GUINT64_FORMAT ",\"name\":",
      element->id);
  append_string (json, element->name);
  g_string_append (json, ",\"factory\":");
  append_string (json, element->factory);
  g_string_append (json, ",\"type\":");
  append_string (json, element->type);
  g_string_append_printf (json, ",\"bin\":%s,\"state\":\"%s\"",
      element->is_bin ? "true" : "false",
      gst_element_state_get_name (element->state));
  if (element->pending != GST_STATE_VOID_PENDING)
    g_string_append_printf (json, ",\"pending\":\"%s\"",
        gst_element_state_get_name (element->pending));
  if (element->errors > 0)
    g_string_append_printf (json, ",\"errors\":%" G_GUINT64_FORMAT,
        element->errors);
  if (element->warnings > 0)
    g_string_append_printf (json, ",\"warnings\":%" G_GUINT64_FORMAT,
        element->warnings);
  g_string_append (json, ",\"pads\":[");

  for (i = 0; i < element->pads->len; i++) {
    if (i > 0)
      g_string_append_c (json, ',');
    append_pad (json, g_hash_table_lookup (model->pads,
            g_ptr_array_index (element->pads, i)));
  }

  g_string_append (json, "],\"children\":[");
  for (i = 0; i < element->children->len; i++) {
    if (i > 0)
      g_string_append_c (json, ',');
    append_element (model, json, g_hash_table_lookup (model->elements,
            g_ptr_array_index (element->children, i)));
  }
  g_string_append (json, "]}");
}

static gint
compare_elements (gconstpointer a, gconstpointer b)
{
  const PmElement *ea = *(PmElement * const *) a;
  const PmElement *eb = *(PmElement * const *) b;

  return (ea->id > eb->id) - (ea->id < eb->id);
}

typedef struct _PmLinkIds PmLinkIds;

struct _PmLinkIds
{
  guint64 src;
  guint64 sink;
};

static gint
compare_links (gconstpointer a, gconstpointer b)
{
  const PmLinkIds *la = a, *lb = b;

  if (la->src != lb->src)
    return (la->src > lb->src) - (la->src < lb->src);
  return (la->sink > lb->sink) - (la->sink < lb->sink);
}

gchar *
pm_model_to_json (PmModel * model)
{
  GString *json = g_string_new ("{\"pipelines\":[");
  GPtrArray *roots = g_ptr_array_new ();
  GArray *links = g_array_new (FALSE, FALSE, sizeof (PmLinkIds));
  GHashTableIter iter;
  gpointer value;
  guint i;

  g_mutex_lock (&model->lock);

  g_hash_table_iter_init (&iter, model->elements);
  while (g_hash_table_iter_next (&iter, NULL, &value)) {
    PmElement *element = value;
    if (element->parent == NULL && element->is_pipeline)
      g_ptr_array_add (roots, element);
  }
  g_ptr_array_sort (roots, compare_elements);
  for (i = 0; i < roots->len; i++) {
    if (i > 0)
      g_string_append_c (json, ',');
    append_element (model, json, g_ptr_array_index (roots, i));
  }

  /* A link is stored as soon as it is made, which can be before its pads
   * are added to an element, so only export links with both ends known. */
  g_hash_table_iter_init (&iter, model->links_by_sink);
  while (g_hash_table_iter_next (&iter, NULL, &value)) {
    PmLink *link = value;
    PmPad *src = g_hash_table_lookup (model->pads, link->src);
    PmPad *sink = g_hash_table_lookup (model->pads, link->sink);
    if (src != NULL && sink != NULL) {
      PmLinkIds ids = { src->id, sink->id };
      g_array_append_val (links, ids);
    }
  }
  g_array_sort (links, compare_links);

  g_mutex_unlock (&model->lock);

  g_string_append (json, "],\"links\":[");
  for (i = 0; i < links->len; i++) {
    PmLinkIds *ids = &g_array_index (links, PmLinkIds, i);
    g_string_append_printf (json,
        "%s{\"src\":%" G_GUINT64_FORMAT ",\"sink\":%" G_GUINT64_FORMAT "}",
        i > 0 ? "," : "", ids->src, ids->sink);
  }
  g_string_append (json, "]}");

  g_ptr_array_unref (roots);
  g_array_unref (links);

  return g_string_free (json, FALSE);
}

void
pm_model_set_log_enabled (PmModel * model, gboolean enabled)
{
  g_mutex_lock (&model->lock);
  model->log_enabled = enabled;
  g_mutex_unlock (&model->lock);
}

static gchar *
element_path (PmElement * element)
{
  GString *path = g_string_new (NULL);

  for (; element != NULL; element = element->parent) {
    g_string_prepend (path, element->name ? element->name : "?");
    if (element->parent != NULL)
      g_string_prepend_c (path, '/');
  }
  return g_string_free (path, FALSE);
}

void
pm_model_add_log (PmModel * model, gpointer object, PmLogLevel level,
    const gchar * source, const gchar * text, const gchar * debug,
    const gchar * domain, gint code)
{
  PmElement *element;
  PmLogEntry *entry;
  gchar *key;
  gint64 now = g_get_real_time ();

  g_mutex_lock (&model->lock);
  if (!model->log_enabled)
    goto done;

  element = object ? g_hash_table_lookup (model->elements, object) : NULL;
  if (element != NULL) {
    if (level == PM_LOG_ERROR)
      element->errors++;
    else
      element->warnings++;
  }

  key = g_strdup_printf ("%" G_GUINT64_FORMAT "|%d|%s|%s",
      element ? element->id : 0, level, source, text);
  entry = g_hash_table_lookup (model->log_by_key, key);
  if (entry != NULL) {
    g_free (key);
    entry->count++;
    entry->last = now;
    g_queue_unlink (&model->log, entry->link);
    g_queue_push_tail_link (&model->log, entry->link);
  } else {
    entry = g_new0 (PmLogEntry, 1);
    entry->id = model->next_log_id++;
    entry->first = entry->last = now;
    entry->count = 1;
    entry->level = level;
    entry->source = g_strdup (source);
    entry->element_id = element ? element->id : 0;
    entry->path = element ? element_path (element) : g_strdup ("?");
    entry->text = g_strdup (text);
    entry->debug = g_strdup (debug);
    entry->domain = g_strdup (domain);
    entry->code = code;
    entry->key = key;
    g_queue_push_tail (&model->log, entry);
    entry->link = model->log.tail;
    g_hash_table_insert (model->log_by_key, entry->key, entry);

    if (model->log.length > LOG_SIZE) {
      PmLogEntry *oldest = g_queue_pop_head (&model->log);

      g_hash_table_remove (model->log_by_key, oldest->key);
      pm_log_entry_free (oldest);
    }
  }
  entry->seq = ++model->log_seq;
  changed (model);

done:
  g_mutex_unlock (&model->lock);
}

static void
append_log_entry (GString * json, PmLogEntry * entry)
{
  g_string_append_printf (json, "{\"id\":%" G_GUINT64_FORMAT
      ",\"level\":\"%s\",\"source\":", entry->id,
      entry->level == PM_LOG_ERROR ? "error" : "warning");
  append_string (json, entry->source);
  g_string_append_printf (json, ",\"element\":%" G_GUINT64_FORMAT
      ",\"path\":", entry->element_id);
  append_string (json, entry->path);
  g_string_append (json, ",\"text\":");
  append_string (json, entry->text);
  if (entry->debug != NULL) {
    g_string_append (json, ",\"debug\":");
    append_string (json, entry->debug);
  }
  if (entry->domain != NULL) {
    g_string_append (json, ",\"domain\":");
    append_string (json, entry->domain);
    g_string_append_printf (json, ",\"code\":%d", entry->code);
  }
  g_string_append_printf (json, ",\"count\":%" G_GUINT64_FORMAT
      ",\"first\":%" G_GINT64_FORMAT ",\"last\":%" G_GINT64_FORMAT "}",
      entry->count, entry->first / 1000, entry->last / 1000);
}

/* Entries added or repeated after sequence number "since", oldest first.
 * Times are milliseconds since the Unix epoch. */
gchar *
pm_model_log_to_json (PmModel * model, guint64 since)
{
  GString *json = g_string_new (NULL);
  gboolean first = TRUE;
  GList *l;

  g_mutex_lock (&model->lock);
  g_string_append_printf (json, "{\"enabled\":%s,\"seq\":%" G_GUINT64_FORMAT
      ",\"entries\":[", model->log_enabled ? "true" : "false",
      model->log_seq);
  for (l = model->log.head; l != NULL; l = l->next) {
    PmLogEntry *entry = l->data;

    if (entry->seq <= since)
      continue;
    if (!first)
      g_string_append_c (json, ',');
    append_log_entry (json, entry);
    first = FALSE;
  }
  g_mutex_unlock (&model->lock);
  g_string_append (json, "]}");

  return g_string_free (json, FALSE);
}
