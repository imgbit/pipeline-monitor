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
  /* When the pending state was set (monotonic µs), 0 if none. */
  gint64 pending_since;
  /* When the state last changed (monotonic µs). */
  gint64 state_since;
  /* Descriptions from missing-plugin messages. */
  GPtrArray *missing;
  guint64 errors;
  guint64 warnings;
  /* Queues: counted from their signals and BUFFERING messages. */
  guint64 overruns;
  guint64 underruns;
  gint buffering;
  /* Gives the element back while it exists, without keeping it alive. */
  GWeakRef ref;
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
  gchar *caps;
  gboolean is_ghost;
  /* The last push or pull that did not return OK, if any: data stopped
   * here. Times in µs since 1970. */
  GstFlowReturn flow;
  guint64 flow_count;
  gint64 flow_first, flow_last;
  PmElement *element;
  /* Gives the pad back while it exists, without keeping it alive. */
  GWeakRef ref;
};

/* A link that could not be made, with what each pad could handle. */
typedef struct
{
  gpointer src, sink;
  gchar *src_path, *sink_path;
  gchar *result;
  gchar *src_caps, *sink_caps;
} PmFailedLink;

#define MAX_FAILED_LINKS 20

static void failed_link_free (PmFailedLink * f);

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
  /* PmFailedLink, oldest first. */
  GQueue failed_links;

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
  if (element->missing != NULL)
    g_ptr_array_unref (element->missing);
  g_weak_ref_clear (&element->ref);
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
  g_free (pad->caps);
  g_weak_ref_clear (&pad->ref);
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
  g_queue_clear_full (&model->failed_links, (GDestroyNotify) failed_link_free);
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
failed_link_free (PmFailedLink * f)
{
  g_free (f->src_path);
  g_free (f->sink_path);
  g_free (f->result);
  g_free (f->src_caps);
  g_free (f->sink_caps);
  g_free (f);
}

/* Forgets failed links of a pad: it is gone, or linked now. */
static void
forget_failed_links (PmModel * model, gpointer pad)
{
  GList *l = model->failed_links.head;

  while (l != NULL) {
    GList *next = l->next;
    PmFailedLink *f = l->data;

    if (f->src == pad || f->sink == pad) {
      failed_link_free (f);
      g_queue_delete_link (&model->failed_links, l);
      changed (model);
    }
    l = next;
  }
}

static void
remove_pad (PmModel * model, gpointer object)
{
  PmPad *pad = g_hash_table_lookup (model->pads, object);

  remove_links_of (model, object);
  forget_failed_links (model, object);
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
    element->buffering = -1;
    g_weak_ref_init (&element->ref, object);
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
    if (state != element->state)
      element->state_since = g_get_monotonic_time ();
    if (pending == GST_STATE_VOID_PENDING)
      element->pending_since = 0;
    else if (pending != element->pending)
      element->pending_since = g_get_monotonic_time ();
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
    g_weak_ref_init (&pad->ref, object);
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

/* Returns a new reference to the pad with this id, or NULL if the model
 * doesn't know it or it no longer exists. Only used when the page pins a
 * pad, so a linear search is fine. */
GstPad *
pm_model_get_pad (PmModel * model, guint64 id)
{
  GHashTableIter iter;
  gpointer value;
  GstPad *object = NULL;

  g_mutex_lock (&model->lock);
  g_hash_table_iter_init (&iter, model->pads);
  while (g_hash_table_iter_next (&iter, NULL, &value)) {
    PmPad *pad = value;

    if (pad->id == id) {
      object = g_weak_ref_get (&pad->ref);
      break;
    }
  }
  g_mutex_unlock (&model->lock);
  return object;
}

/* caps is the pad's current caps as text, or NULL when it has none. */
void
pm_model_set_caps (PmModel * model, gpointer object, const gchar * caps)
{
  PmPad *pad;

  g_mutex_lock (&model->lock);
  pad = g_hash_table_lookup (model->pads, object);
  if (pad != NULL && g_strcmp0 (pad->caps, caps) != 0) {
    g_free (pad->caps);
    pad->caps = g_strdup (caps);
    changed (model);
  }
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

  forget_failed_links (model, src);
  forget_failed_links (model, sink);
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
  if (pad->caps != NULL) {
    g_string_append (json, ",\"caps\":");
    append_string (json, pad->caps);
  }
  if (pad->flow_count > 0)
    g_string_append_printf (json, ",\"flow\":{\"result\":\"%s\","
        "\"count\":%" G_GUINT64_FORMAT ",\"first\":%" G_GINT64_FORMAT
        ",\"last\":%" G_GINT64_FORMAT "}", gst_flow_get_name (pad->flow),
        pad->flow_count, pad->flow_first, pad->flow_last);
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
pm_model_count_queue_event (PmModel * model, gpointer object,
    gboolean overrun)
{
  PmElement *element;

  g_mutex_lock (&model->lock);
  element = g_hash_table_lookup (model->elements, object);
  if (element != NULL) {
    if (overrun)
      element->overruns++;
    else
      element->underruns++;
  }
  g_mutex_unlock (&model->lock);
}

void
pm_model_set_buffering (PmModel * model, gpointer object, gint percent)
{
  PmElement *element;

  g_mutex_lock (&model->lock);
  element = g_hash_table_lookup (model->elements, object);
  if (element != NULL)
    element->buffering = percent;
  g_mutex_unlock (&model->lock);
}

void
pm_element_ref_free (PmElementRef * ref)
{
  gst_object_unref (ref->element);
  g_free (ref->path);
  g_free (ref);
}

/* The elements that exist and match filter, each with a new reference. The
 * filter runs with the model locked and must not call back into it. */
GPtrArray *
pm_model_find_elements (PmModel * model, PmElementFilter filter)
{
  GPtrArray *found = g_ptr_array_new_with_free_func ((GDestroyNotify)
      pm_element_ref_free);
  GHashTableIter iter;
  gpointer value;

  g_mutex_lock (&model->lock);
  g_hash_table_iter_init (&iter, model->elements);
  while (g_hash_table_iter_next (&iter, NULL, &value)) {
    PmElement *element = value;
    GstElement *object = g_weak_ref_get (&element->ref);
    PmElementRef *ref;

    if (object == NULL)
      continue;
    if (!filter (object, element->parent == NULL)) {
      gst_object_unref (object);
      continue;
    }
    ref = g_new0 (PmElementRef, 1);
    ref->element = object;
    ref->id = element->id;
    ref->path = element_path (element);
    ref->overruns = element->overruns;
    ref->underruns = element->underruns;
    ref->buffering = element->buffering;
    g_ptr_array_add (found, ref);
  }
  g_mutex_unlock (&model->lock);
  return found;
}

/* A push or pull at pad did not return OK. Repeats only count up; the
 * graph is sent again only when the result is new for the pad. */
void
pm_model_record_flow (PmModel * model, gpointer object, GstFlowReturn res)
{
  PmPad *pad;
  gint64 now = g_get_real_time ();

  g_mutex_lock (&model->lock);
  pad = g_hash_table_lookup (model->pads, object);
  if (pad != NULL) {
    if (pad->flow_count == 0 || pad->flow != res) {
      pad->flow = res;
      pad->flow_count = 0;
      pad->flow_first = now;
      changed (model);
    }
    pad->flow_count++;
    pad->flow_last = now;
  }
  g_mutex_unlock (&model->lock);
}

void
pm_flow_record_free (PmFlowRecord * record)
{
  g_free (record->path);
  g_free (record);
}

/* Pads where data stopped (a flow record), for the findings. */
GPtrArray *
pm_model_flow_records (PmModel * model)
{
  GPtrArray *found = g_ptr_array_new_with_free_func ((GDestroyNotify)
      pm_flow_record_free);
  GHashTableIter iter;
  gpointer value;

  g_mutex_lock (&model->lock);
  g_hash_table_iter_init (&iter, model->pads);
  while (g_hash_table_iter_next (&iter, NULL, &value)) {
    PmPad *pad = value;
    PmFlowRecord *record;
    gchar *element;

    if (pad->flow_count == 0 || pad->element == NULL)
      continue;
    record = g_new0 (PmFlowRecord, 1);
    record->pad = pad->id;
    record->element = pad->element->id;
    element = element_path (pad->element);
    record->path = g_strdup_printf ("%s:%s", element, pad->name);
    g_free (element);
    record->result = pad->flow;
    record->count = pad->flow_count;
    record->first = pad->flow_first;
    record->last = pad->flow_last;
    g_ptr_array_add (found, record);
  }
  g_mutex_unlock (&model->lock);
  return found;
}

void
pm_pending_free (PmPending * pending)
{
  g_free (pending->path);
  g_free (pending);
}

/* Top-level pipelines that have had a pending state for at least
 * min_us. */
GPtrArray *
pm_model_pending (PmModel * model, gint64 min_us)
{
  GPtrArray *found = g_ptr_array_new_with_free_func ((GDestroyNotify)
      pm_pending_free);
  GHashTableIter iter;
  gpointer value;
  gint64 now = g_get_monotonic_time ();

  g_mutex_lock (&model->lock);
  g_hash_table_iter_init (&iter, model->elements);
  while (g_hash_table_iter_next (&iter, NULL, &value)) {
    PmElement *element = value;
    PmPending *pending;

    if (!element->is_pipeline || element->parent != NULL ||
        element->pending_since == 0 || now - element->pending_since < min_us)
      continue;
    pending = g_new0 (PmPending, 1);
    pending->element = element->id;
    pending->path = element_path (element);
    pending->state = element->state;
    pending->pending = element->pending;
    pending->since_us = now - element->pending_since;
    g_ptr_array_add (found, pending);
  }
  g_mutex_unlock (&model->lock);
  return found;
}

/* A new reference to the element with this id, or NULL if the model does
 * not know it or it is gone. */
GstElement *
pm_model_get_element (PmModel * model, guint64 id)
{
  GHashTableIter iter;
  gpointer value;
  GstElement *object = NULL;

  g_mutex_lock (&model->lock);
  g_hash_table_iter_init (&iter, model->elements);
  while (g_hash_table_iter_next (&iter, NULL, &value)) {
    PmElement *element = value;

    if (element->id == id) {
      object = g_weak_ref_get (&element->ref);
      break;
    }
  }
  g_mutex_unlock (&model->lock);
  return object;
}

/* The model's id of a pad, or 0 if it is not known. */
guint64
pm_model_pad_id (PmModel * model, gpointer object)
{
  PmPad *pad;
  guint64 id;

  g_mutex_lock (&model->lock);
  pad = g_hash_table_lookup (model->pads, object);
  id = pad ? pad->id : 0;
  g_mutex_unlock (&model->lock);
  return id;
}

/* The path of an element (pipeline0/bin/name), or NULL if not known. */
gchar *
pm_model_element_path (PmModel * model, gpointer object)
{
  PmElement *element;
  gchar *path;

  g_mutex_lock (&model->lock);
  element = g_hash_table_lookup (model->elements, object);
  path = element ? element_path (element) : NULL;
  g_mutex_unlock (&model->lock);
  return path;
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
 * "seq" receives the sequence number to pass as "since" next time.
 * Times are milliseconds since the Unix epoch. */
gchar *
pm_model_log_to_json (PmModel * model, guint64 since, guint64 * seq)
{
  GString *json = g_string_new (NULL);
  gboolean first = TRUE;
  GList *l;

  g_mutex_lock (&model->lock);
  g_string_append_printf (json, "{\"enabled\":%s,\"seq\":%" G_GUINT64_FORMAT
      ",\"entries\":[", model->log_enabled ? "true" : "false",
      model->log_seq);
  if (seq != NULL)
    *seq = model->log_seq;
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

/* Problems in the structure of the graph, for the findings. */

void
pm_spot_free (PmSpot * spot)
{
  g_free (spot->path);
  g_free (spot->detail);
  g_free (spot);
}

static PmSpot *
spot_new (PmElement * element, PmPad * pad, const gchar * detail)
{
  PmSpot *spot = g_new0 (PmSpot, 1);
  gchar *path = element_path (element);

  spot->element = element->id;
  spot->pad = pad ? pad->id : 0;
  spot->path = pad ? g_strdup_printf ("%s:%s", path, pad->name) :
      g_strdup (path);
  spot->detail = g_strdup (detail);
  g_free (path);
  return spot;
}

static GPtrArray *
spots_new (void)
{
  return g_ptr_array_new_with_free_func ((GDestroyNotify) pm_spot_free);
}

static gboolean
is_queue (PmElement * element)
{
  return g_strcmp0 (element->factory, "queue") == 0 ||
      g_strcmp0 (element->factory, "queue2") == 0 ||
      g_strcmp0 (element->factory, "multiqueue") == 0;
}

static guint
count_pads (PmModel * model, PmElement * element, GstPadDirection direction)
{
  guint i, n = 0;

  for (i = 0; i < element->pads->len; i++) {
    PmPad *pad = g_hash_table_lookup (model->pads,
        g_ptr_array_index (element->pads, i));

    if (pad != NULL && pad->direction == direction)
      n++;
  }
  return n;
}

static gboolean
is_sink (PmModel * model, PmElement * element)
{
  return !element->is_bin && count_pads (model, element, GST_PAD_SINK) > 0 &&
      count_pads (model, element, GST_PAD_SRC) == 0;
}

static PmElement *
top_level (PmElement * element)
{
  while (element->parent != NULL)
    element = element->parent;
  return element;
}

/* The pads data goes to from pad: its peer, the inside of a ghost pad, or
 * for an element's sink pad, the element's src pads. */
static void
downstream_of (PmModel * model, gpointer pad, GPtrArray * out)
{
  PmPad *p = g_hash_table_lookup (model->pads, pad);
  PmLink *link = g_hash_table_lookup (model->links_by_src, pad);
  guint i;

  if (link != NULL) {
    g_ptr_array_add (out, link->sink);
    return;
  }
  if (p == NULL || p->direction != GST_PAD_SINK || p->element->is_bin)
    return;
  for (i = 0; i < p->element->pads->len; i++) {
    gpointer other = g_ptr_array_index (p->element->pads, i);
    PmPad *o = g_hash_table_lookup (model->pads, other);

    if (o != NULL && o->direction == GST_PAD_SRC)
      g_ptr_array_add (out, other);
  }
}

/* The pads data comes from into pad: the reverse of downstream_of. */
static void
upstream_of (PmModel * model, gpointer pad, GPtrArray * out)
{
  PmPad *p = g_hash_table_lookup (model->pads, pad);
  PmLink *link = g_hash_table_lookup (model->links_by_sink, pad);
  guint i;

  if (link != NULL) {
    g_ptr_array_add (out, link->src);
    return;
  }
  if (p == NULL || p->direction != GST_PAD_SRC || p->element->is_bin)
    return;
  for (i = 0; i < p->element->pads->len; i++) {
    gpointer other = g_ptr_array_index (p->element->pads, i);
    PmPad *o = g_hash_table_lookup (model->pads, other);

    if (o != NULL && o->direction == GST_PAD_SINK)
      g_ptr_array_add (out, other);
  }
}

/* Walks downstream from pad; returns the first sink reached without
 * passing a queue, or NULL. */
static PmElement *
sink_without_queue (PmModel * model, gpointer start)
{
  GHashTable *seen = g_hash_table_new (NULL, NULL);
  GPtrArray *level = g_ptr_array_new ();
  PmElement *found = NULL;

  g_ptr_array_add (level, start);
  while (level->len > 0 && found == NULL) {
    GPtrArray *next = g_ptr_array_new ();
    guint i, j;

    for (i = 0; i < level->len && found == NULL; i++) {
      GPtrArray *out = g_ptr_array_new ();

      downstream_of (model, g_ptr_array_index (level, i), out);
      for (j = 0; j < out->len; j++) {
        gpointer pad = g_ptr_array_index (out, j);
        PmPad *p = g_hash_table_lookup (model->pads, pad);

        if (p == NULL || g_hash_table_contains (seen, pad))
          continue;
        g_hash_table_add (seen, pad);
        if (is_queue (p->element))
          continue;
        if (is_sink (model, p->element)) {
          found = p->element;
          break;
        }
        g_ptr_array_add (next, pad);
      }
      g_ptr_array_unref (out);
    }
    g_ptr_array_unref (level);
    level = next;
  }
  g_ptr_array_unref (level);
  g_hash_table_unref (seen);
  return found;
}

/* Elements that split data into several branches (tee, demuxers): each
 * branch needs a queue before its sink, or all branches share one
 * streaming thread and wait for each other (a pipeline stuck in PAUSED).
 * One spot per branch reaching a sink without a queue. */
GPtrArray *
pm_model_unqueued_branches (PmModel * model)
{
  GPtrArray *found = spots_new ();
  GHashTableIter iter;
  gpointer value;

  g_mutex_lock (&model->lock);
  g_hash_table_iter_init (&iter, model->elements);
  while (g_hash_table_iter_next (&iter, NULL, &value)) {
    PmElement *element = value;
    guint i, linked = 0;

    if (element->is_bin || is_queue (element))
      continue;
    for (i = 0; i < element->pads->len; i++) {
      gpointer pad = g_ptr_array_index (element->pads, i);

      if (g_hash_table_contains (model->links_by_src, pad))
        linked++;
    }
    if (linked < 2)
      continue;
    for (i = 0; i < element->pads->len; i++) {
      gpointer pad = g_ptr_array_index (element->pads, i);
      PmElement *sink;
      gchar *path;

      if (!g_hash_table_contains (model->links_by_src, pad))
        continue;
      sink = sink_without_queue (model, pad);
      if (sink == NULL)
        continue;
      path = element_path (sink);
      g_ptr_array_add (found, spot_new (element,
              g_hash_table_lookup (model->pads, pad), path));
      g_free (path);
    }
  }
  g_mutex_unlock (&model->lock);
  return found;
}

/* src pads with no link while their element runs: data on them has
 * nowhere to go. Always pads count too: a link the program tried with
 * gst_element_link and that failed leaves them unlinked. */
GPtrArray *
pm_model_unlinked_pads (PmModel * model)
{
  GPtrArray *found = spots_new ();
  GHashTableIter iter;
  gpointer key, value;

  g_mutex_lock (&model->lock);
  g_hash_table_iter_init (&iter, model->pads);
  while (g_hash_table_iter_next (&iter, &key, &value)) {
    PmPad *pad = value;

    if (pad->direction != GST_PAD_SRC || pad->is_ghost ||
        pad->element->state < GST_STATE_PAUSED ||
        g_hash_table_contains (model->links_by_src, key))
      continue;
    g_ptr_array_add (found, spot_new (pad->element, pad, NULL));
  }
  g_mutex_unlock (&model->lock);
  return found;
}

/* Sinks of a pipeline whose sink pads have no caps: no data reached them.
 * detail: the pad nearest upstream that has caps (how far data got), or
 * NULL if none. */
GPtrArray *
pm_model_starved_sinks (PmModel * model, guint64 pipeline)
{
  GPtrArray *found = spots_new ();
  GHashTableIter iter;
  gpointer value;

  g_mutex_lock (&model->lock);
  g_hash_table_iter_init (&iter, model->elements);
  while (g_hash_table_iter_next (&iter, NULL, &value)) {
    PmElement *element = value;
    guint i;

    if (!is_sink (model, element) || top_level (element)->id != pipeline)
      continue;
    for (i = 0; i < element->pads->len; i++) {
      gpointer start = g_ptr_array_index (element->pads, i);
      PmPad *pad = g_hash_table_lookup (model->pads, start);
      GHashTable *seen;
      GPtrArray *level;
      gchar *reached = NULL;

      if (pad == NULL || pad->caps != NULL)
        continue;
      seen = g_hash_table_new (NULL, NULL);
      level = g_ptr_array_new ();
      g_ptr_array_add (level, start);
      while (level->len > 0 && reached == NULL) {
        GPtrArray *next = g_ptr_array_new ();
        guint j, k;

        for (j = 0; j < level->len && reached == NULL; j++) {
          GPtrArray *in = g_ptr_array_new ();

          upstream_of (model, g_ptr_array_index (level, j), in);
          for (k = 0; k < in->len; k++) {
            gpointer up = g_ptr_array_index (in, k);
            PmPad *u = g_hash_table_lookup (model->pads, up);
            gchar *path;

            if (u == NULL || g_hash_table_contains (seen, up))
              continue;
            g_hash_table_add (seen, up);
            if (u->caps != NULL) {
              path = element_path (u->element);
              reached = g_strdup_printf ("%s:%s", path, u->name);
              g_free (path);
              break;
            }
            g_ptr_array_add (next, up);
          }
          g_ptr_array_unref (in);
        }
        g_ptr_array_unref (level);
        level = next;
      }
      g_ptr_array_unref (level);
      g_hash_table_unref (seen);
      g_ptr_array_add (found, spot_new (element, pad, reached));
      g_free (reached);
    }
  }
  g_mutex_unlock (&model->lock);
  return found;
}

/* Sources in PLAYING for at least min_us whose src pads have no caps yet:
 * they have not produced anything. */
GPtrArray *
pm_model_silent_sources (PmModel * model, gint64 min_us)
{
  GPtrArray *found = spots_new ();
  GHashTableIter iter;
  gpointer value;
  gint64 now = g_get_monotonic_time ();

  g_mutex_lock (&model->lock);
  g_hash_table_iter_init (&iter, model->elements);
  while (g_hash_table_iter_next (&iter, NULL, &value)) {
    PmElement *element = value;
    guint i, srcs = 0, with_caps = 0;

    if (element->is_bin || element->state != GST_STATE_PLAYING ||
        now - element->state_since < min_us ||
        count_pads (model, element, GST_PAD_SINK) > 0)
      continue;
    for (i = 0; i < element->pads->len; i++) {
      PmPad *pad = g_hash_table_lookup (model->pads,
          g_ptr_array_index (element->pads, i));

      if (pad == NULL || pad->direction != GST_PAD_SRC)
        continue;
      srcs++;
      if (pad->caps != NULL)
        with_caps++;
    }
    if (srcs > 0 && with_caps == 0)
      g_ptr_array_add (found, spot_new (element, NULL, NULL));
  }
  g_mutex_unlock (&model->lock);
  return found;
}

/* A link that failed while building. src_name and sink_name are used when
 * the pads are not in the model (pads of elements outside a pipeline). */
void
pm_model_record_failed_link (PmModel * model, gpointer src, gpointer sink,
    const gchar * src_name, const gchar * sink_name, const gchar * result,
    const gchar * src_caps, const gchar * sink_caps)
{
  PmFailedLink *f = g_new0 (PmFailedLink, 1);
  PmPad *pad;

  g_mutex_lock (&model->lock);
  forget_failed_links (model, src);
  f->src = src;
  f->sink = sink;
  pad = g_hash_table_lookup (model->pads, src);
  f->src_path = pad ? g_strdup_printf ("%s:%s", pad->element->name,
      pad->name) : g_strdup (src_name);
  pad = g_hash_table_lookup (model->pads, sink);
  f->sink_path = pad ? g_strdup_printf ("%s:%s", pad->element->name,
      pad->name) : g_strdup (sink_name);
  f->result = g_strdup (result);
  f->src_caps = g_strdup (src_caps);
  f->sink_caps = g_strdup (sink_caps);
  g_queue_push_tail (&model->failed_links, f);
  if (model->failed_links.length > MAX_FAILED_LINKS)
    failed_link_free (g_queue_pop_head (&model->failed_links));
  changed (model);
  g_mutex_unlock (&model->lock);
}

void
pm_link_failure_free (PmLinkFailure * f)
{
  g_free (f->src_path);
  g_free (f->sink_path);
  g_free (f->result);
  g_free (f->src_caps);
  g_free (f->sink_caps);
  g_free (f);
}

GPtrArray *
pm_model_failed_links (PmModel * model)
{
  GPtrArray *found = g_ptr_array_new_with_free_func ((GDestroyNotify)
      pm_link_failure_free);
  GList *l;

  g_mutex_lock (&model->lock);
  for (l = model->failed_links.head; l != NULL; l = l->next) {
    PmFailedLink *f = l->data;
    PmLinkFailure *copy = g_new0 (PmLinkFailure, 1);
    PmPad *src = g_hash_table_lookup (model->pads, f->src);
    PmPad *sink = g_hash_table_lookup (model->pads, f->sink);

    copy->src = src ? src->id : 0;
    copy->sink = sink ? sink->id : 0;
    copy->element = src ? src->element->id : sink ? sink->element->id : 0;
    copy->src_path = g_strdup (f->src_path);
    copy->sink_path = g_strdup (f->sink_path);
    copy->result = g_strdup (f->result);
    copy->src_caps = g_strdup (f->src_caps);
    copy->sink_caps = g_strdup (f->sink_caps);
    g_ptr_array_add (found, copy);
  }
  g_mutex_unlock (&model->lock);
  return found;
}

/* A missing-plugin message: what could not be found (a decoder for some
 * caps, a URI handler...). */
void
pm_model_record_missing (PmModel * model, gpointer object, const gchar * what)
{
  PmElement *element;

  g_mutex_lock (&model->lock);
  element = g_hash_table_lookup (model->elements, object);
  if (element != NULL) {
    if (element->missing == NULL)
      element->missing = g_ptr_array_new_with_free_func (g_free);
    if (!g_ptr_array_find_with_equal_func (element->missing, what,
            g_str_equal, NULL)) {
      g_ptr_array_add (element->missing, g_strdup (what));
      changed (model);
    }
  }
  g_mutex_unlock (&model->lock);
}

GPtrArray *
pm_model_missing_plugins (PmModel * model)
{
  GPtrArray *found = spots_new ();
  GHashTableIter iter;
  gpointer value;

  g_mutex_lock (&model->lock);
  g_hash_table_iter_init (&iter, model->elements);
  while (g_hash_table_iter_next (&iter, NULL, &value)) {
    PmElement *element = value;
    guint i;

    for (i = 0; element->missing && i < element->missing->len; i++)
      g_ptr_array_add (found, spot_new (element, NULL,
              g_ptr_array_index (element->missing, i)));
  }
  g_mutex_unlock (&model->lock);
  return found;
}
