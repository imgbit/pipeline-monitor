/* SPDX-License-Identifier: MIT */
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

  return model;
}

void
pm_model_free (PmModel * model)
{
  g_hash_table_unref (model->links_by_src);
  g_hash_table_unref (model->links_by_sink);
  g_hash_table_unref (model->pads);
  g_hash_table_unref (model->elements);
  g_mutex_clear (&model->lock);
  g_free (model);
}

static void
remove_link (PmModel * model, PmLink * link)
{
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
}

static void
set_parent (PmModel * model, gpointer object, PmElement * element,
    PmElement * parent)
{
  if (element->parent == parent)
    return;
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

static void
replace_string (gchar ** dest, const gchar * value)
{
  if (value == NULL || g_strcmp0 (*dest, value) == 0)
    return;
  g_free (*dest);
  *dest = g_strdup (value);
}

void
pm_model_update_element (PmModel * model, gpointer object, const gchar * name,
    const gchar * factory, const gchar * type, gboolean is_bin,
    gboolean is_pipeline)
{
  PmElement *element;

  g_mutex_lock (&model->lock);
  element = g_hash_table_lookup (model->elements, object);
  if (element == NULL) {
    element = g_new0 (PmElement, 1);
    element->id = model->next_id++;
    element->state = GST_STATE_NULL;
    element->children = g_ptr_array_new ();
    element->pads = g_ptr_array_new ();
    g_hash_table_insert (model->elements, object, element);
  }
  replace_string (&element->name, name);
  replace_string (&element->factory, factory);
  replace_string (&element->type, type);
  element->is_bin = is_bin;
  element->is_pipeline = is_pipeline;
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

/* Reads the state inside the lock so that concurrent callers cannot store
 * an older value over a newer one. */
void
pm_model_sync_state (PmModel * model, GstElement * object)
{
  PmElement *element;

  g_mutex_lock (&model->lock);
  element = g_hash_table_lookup (model->elements, object);
  if (element != NULL) {
    element->state = GST_STATE (object);
    element->pending = GST_STATE_PENDING (object);
  }
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
