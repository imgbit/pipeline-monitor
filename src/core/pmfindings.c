/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pmfindings.h"
#include "pmlevels.h"

#include <string.h>

/* Findings: problems found in the live pipeline, each at an element or a
 * pad, with an explanation and a hint. Built from what the model already
 * knows; checks for common causes are added over time. */

typedef enum
{
  PM_INFO,
  PM_WARNING,
  PM_ERROR,
} PmSeverity;

static const gchar *severity_names[] = { "info", "warning", "error" };

typedef struct
{
  gchar *id;
  PmSeverity severity;
  guint64 element;
  guint64 pad;
  gchar *text;
  gchar *hint;
  guint64 count;
  gint64 first, last;
  /* For format problems: what each side can handle. */
  gchar *upstream, *downstream;
  gboolean common;
} PmFinding;

static void
finding_free (PmFinding * f)
{
  g_free (f->id);
  g_free (f->text);
  g_free (f->hint);
  g_free (f->upstream);
  g_free (f->downstream);
  g_free (f);
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

/* What a result means where data stopped, and what to look at. */
static void
explain_flow (GstFlowReturn result, PmSeverity * severity,
    const gchar ** hint)
{
  switch (result) {
    case GST_FLOW_NOT_LINKED:
      *severity = PM_WARNING;
      *hint = "Pushing from this pad returned not-linked: the pad is not "
          "linked to anything.";
      break;
    case GST_FLOW_EOS:
      *severity = PM_INFO;
      *hint = "Pushing from this pad returned eos: downstream has already "
          "ended.";
      break;
    case GST_FLOW_NOT_NEGOTIATED:
      *severity = PM_ERROR;
      *hint = "No format was agreed on this link; what each side handles is "
          "below.";
      break;
    case GST_FLOW_NOT_SUPPORTED:
      *severity = PM_ERROR;
      *hint = "Pushing from this pad returned not-supported.";
      break;
    default:
      *severity = PM_ERROR;
      *hint = "Pushing from this pad returned an error; the element's "
          "message is under errors and warnings.";
      break;
  }
}



/* Most severe first, then by id, so the list does not reorder itself. */
static gint
compare_findings (gconstpointer a, gconstpointer b)
{
  const PmFinding *fa = *(PmFinding * const *) a;
  const PmFinding *fb = *(PmFinding * const *) b;

  if (fa->severity != fb->severity)
    return fb->severity - fa->severity;
  return g_strcmp0 (fa->id, fb->id);
}

/* Seconds kept for "in the last N s" counts. */
#define HISTORY 10
#define FULL_AFTER_US (5 * G_USEC_PER_SEC)
#define SILENT_AFTER_US (5 * G_USEC_PER_SEC)

/* Buffers counted at a pad by a probe; shared by the probe and the
 * queue's state, freed when both are done with it. */
typedef struct
{
  gint ref;
  gint count;
} PmCounter;

typedef struct
{
  GWeakRef pad;
  gulong id;
  PmCounter *counter;
} PmCountProbe;

/* What is remembered of a queue between ticks. */
typedef struct
{
  gint64 full_since;
  gboolean leaky;
  PmCountProbe in, out;
  guint prev_in, prev_out, prev_level;
  gboolean have_prev;
  guint64 drops[HISTORY];
  guint64 ticks;
} PmQueueState;

/* And of a sink: its "dropped" counter. */
typedef struct
{
  guint64 last;
  gboolean have_last;
  guint64 drops[HISTORY];
  guint64 ticks;
} PmSinkState;

struct _PmFindings
{
  PmModel *model;
  GHashTable *queues;
  GHashTable *sinks;
  guint64 tick;
};

static void
counter_unref (PmCounter * c)
{
  if (g_atomic_int_dec_and_test (&c->ref))
    g_free (c);
}

static GstPadProbeReturn
on_count (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  PmCounter *c = user_data;
  guint n = 1;

  if (info->type & GST_PAD_PROBE_TYPE_BUFFER_LIST)
    n = gst_buffer_list_length (GST_PAD_PROBE_INFO_BUFFER_LIST (info));
  g_atomic_int_add (&c->count, n);
  return GST_PAD_PROBE_OK;
}

static void
count_probe_add (PmCountProbe * p, GstElement * element, const gchar * name)
{
  GstPad *pad = gst_element_get_static_pad (element, name);

  if (pad == NULL)
    return;
  p->counter = g_new0 (PmCounter, 1);
  p->counter->ref = 2;
  g_weak_ref_init (&p->pad, pad);
  p->id = gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER |
      GST_PAD_PROBE_TYPE_BUFFER_LIST, on_count, p->counter,
      (GDestroyNotify) counter_unref);
  gst_object_unref (pad);
}

static void
count_probe_clear (PmCountProbe * p)
{
  GstPad *pad;

  if (p->counter == NULL)
    return;
  pad = g_weak_ref_get (&p->pad);
  if (pad != NULL) {
    gst_pad_remove_probe (pad, p->id);
    gst_object_unref (pad);
  }
  g_weak_ref_clear (&p->pad);
  counter_unref (p->counter);
  p->counter = NULL;
}

static void
queue_state_free (PmQueueState * q)
{
  count_probe_clear (&q->in);
  count_probe_clear (&q->out);
  g_free (q);
}

PmFindings *
pm_findings_new (PmModel * model)
{
  PmFindings *f = g_new0 (PmFindings, 1);

  f->model = model;
  f->queues = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free,
      (GDestroyNotify) queue_state_free);
  f->sinks = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free,
      g_free);
  return f;
}

void
pm_findings_free (PmFindings * f)
{
  g_hash_table_unref (f->queues);
  g_hash_table_unref (f->sinks);
  g_free (f);
}

static guint64 *
key_new (guint64 id)
{
  guint64 *key = g_new (guint64, 1);

  *key = id;
  return key;
}

static PmFinding *
finding_new (const gchar * kind, guint64 id, PmSeverity severity,
    guint64 element, guint64 pad)
{
  PmFinding *f = g_new0 (PmFinding, 1);

  f->id = g_strdup_printf ("%s:%" G_GUINT64_FORMAT, kind, id);
  f->severity = severity;
  f->element = element;
  f->pad = pad;
  return f;
}

static gchar *
caps_string (GstCaps * caps)
{
  gchar *text = caps ? gst_caps_to_string (caps) : NULL;

  gst_clear_caps (&caps);
  return text;
}

/* For a src pad whose push was not negotiated: what it sends (or could)
 * and what its peer accepts. */
static void
add_formats (PmFindings *self, PmFinding * f, guint64 pad_id)
{
  GstPad *pad = pm_model_get_pad (self->model, pad_id);
  GstCaps *offered, *accepted;

  if (pad == NULL)
    return;
  if (GST_PAD_IS_SRC (pad)) {
    offered = gst_pad_get_current_caps (pad);
    if (offered == NULL)
      offered = gst_pad_query_caps (pad, NULL);
    accepted = gst_pad_peer_query_caps (pad, NULL);
    f->common = offered && accepted && gst_caps_can_intersect (offered,
        accepted);
    f->upstream = caps_string (offered);
    f->downstream = caps_string (accepted);
  }
  gst_object_unref (pad);
}

static void
add_flow (PmFindings * self, GPtrArray * findings)
{
  GPtrArray *records = pm_model_flow_records (self->model);
  guint i;

  for (i = 0; i < records->len; i++) {
    PmFlowRecord *r = g_ptr_array_index (records, i);
    PmSeverity severity;
    const gchar *hint;
    PmFinding *f;

    explain_flow (r->result, &severity, &hint);
    f = finding_new ("flow", r->pad, severity, r->element, r->pad);
    f->text = g_strdup_printf ("Data stopped at %s: %s", r->path,
        gst_flow_get_name (r->result));
    f->hint = g_strdup (hint);
    f->count = r->count;
    f->first = r->first;
    f->last = r->last;
    if (r->result == GST_FLOW_NOT_NEGOTIATED)
      add_formats (self, f, r->pad);
    g_ptr_array_add (findings, f);
  }
  g_ptr_array_unref (records);
}

/* Pipelines not reaching their state, and for them: sinks that got no
 * data and how far data got. */
static void
add_stuck (PmFindings * self, GPtrArray * findings, gint64 stuck_after_us)
{
  GPtrArray *pending = pm_model_pending (self->model, stuck_after_us);
  guint i, j;

  for (i = 0; i < pending->len; i++) {
    PmPending *p = g_ptr_array_index (pending, i);
    PmFinding *f = finding_new ("stuck", p->element, PM_WARNING, p->element,
        0);
    GPtrArray *starved;

    f->text = g_strdup_printf ("%s has not reached %s for %" G_GINT64_FORMAT
        " s (it is %s)", p->path, gst_element_state_get_name (p->pending),
        p->since_us / G_USEC_PER_SEC, gst_element_state_get_name (p->state));
    {
      gchar *pending_list = pm_model_pending_elements (self->model,
          p->element);

      gchar *preroll = pm_model_preroll_summary (self->model, p->element);

      f->hint = pending_list ? g_strdup_printf ("Still changing state: %s.%s%s",
          pending_list, preroll ? " " : "", preroll ? preroll : "") :
          g_strdup (preroll);
      g_free (pending_list);
      g_free (preroll);
    }
    g_ptr_array_add (findings, f);

    starved = pm_model_starved_sinks (self->model, p->element);
    for (j = 0; j < starved->len; j++) {
      PmSpot *s = g_ptr_array_index (starved, j);

      f = finding_new ("starved", s->pad, PM_WARNING, s->element, s->pad);
      f->text = g_strdup_printf ("%s has no data", s->path);
      f->hint = s->detail ? g_strdup_printf ("The nearest pad before it "
          "that has received data is %s.", s->detail) :
          g_strdup ("No pad before it has received data yet.");
      g_ptr_array_add (findings, f);
    }
    g_ptr_array_unref (starved);
  }
  g_ptr_array_unref (pending);
}

static void
add_spots (GPtrArray * findings, GPtrArray * spots, const gchar * kind,
    PmSeverity severity, const gchar * text, const gchar * hint)
{
  guint i;

  for (i = 0; i < spots->len; i++) {
    PmSpot *s = g_ptr_array_index (spots, i);
    PmFinding *f = finding_new (kind, s->pad ? s->pad : s->element, severity,
        s->element, s->pad);

    f->text = g_strdup_printf (text, s->path, s->detail ? s->detail : "");
    f->hint = g_strdup (hint);
    g_ptr_array_add (findings, f);
  }
  g_ptr_array_unref (spots);
}

#define BLOCKED_AFTER_MS 3000

/* Pushes that have not returned for 3 s, where the thread is held. */
static void
add_blocked (PmFindings * self, GPtrArray * findings)
{
  GPtrArray *blocked = pm_model_blocked_pushes (self->model,
      pm_model_now_ms (), BLOCKED_AFTER_MS);
  guint i;

  for (i = 0; i < blocked->len; i++) {
    PmBlocked *b = g_ptr_array_index (blocked, i);
    PmFinding *f = finding_new ("blocked", b->pad, PM_WARNING, b->element,
        b->pad);

    f->text = g_strdup_printf ("Data flow blocked at %s%s%s for %d s",
        b->path, b->peer_path ? " → " : "", b->peer_path ? b->peer_path : "",
        b->ms / 1000);
    f->hint = b->peer_state ? g_strdup_printf ("The push into %s has not "
        "returned. %s.", b->peer_path, b->peer_state) :
        g_strdup ("The push from this pad has not returned.");
    g_ptr_array_add (findings, f);
  }
  g_ptr_array_unref (blocked);
}

static void
add_structure (PmFindings * self, GPtrArray * findings)
{
  GPtrArray *failed = pm_model_failed_links (self->model);
  guint i;

  add_spots (findings, pm_model_missing_plugins (self->model), "missing",
      PM_ERROR, "%s: missing %s", "It reported that no installed element "
      "can handle this.");
  add_spots (findings, pm_model_silent_sources (self->model,
          SILENT_AFTER_US), "silent", PM_INFO, "%s has sent no data for "
      "5 s while PLAYING%s", "");

  for (i = 0; i < failed->len; i++) {
    PmLinkFailure *l = g_ptr_array_index (failed, i);
    PmFinding *f = finding_new ("link", i, PM_ERROR, l->element, l->src);

    g_free (f->id);
    f->id = g_strdup_printf ("link:%s>%s", l->src_path, l->sink_path);
    f->text = g_strdup_printf ("%s could not link to %s: %s", l->src_path,
        l->sink_path, l->result);
    f->hint = g_strdup (g_strcmp0 (l->result,
            gst_pad_link_get_name (GST_PAD_LINK_NOFORMAT)) == 0 ?
        "The two pads have no format in common; what each side handles is "
        "below." : "What each side handles is below.");
    f->upstream = g_strdup (l->src_caps);
    f->downstream = g_strdup (l->sink_caps);
    g_ptr_array_add (findings, f);
  }
  g_ptr_array_unref (failed);
}

static gboolean
queue_filter (GstElement * element, gboolean top_level)
{
  return pm_levels_is_queue (element);
}

/* How full a queue is: the most blocking limit (for multiqueue, of its
 * fullest stream). */
static gdouble
fill_of (gpointer object, guint max_buffers, guint max_bytes,
    guint64 max_time)
{
  guint buffers = 0, bytes = 0;
  guint64 time = 0;
  gdouble fill = 0;

  g_object_get (object, "current-level-buffers", &buffers,
      "current-level-bytes", &bytes, "current-level-time", &time, NULL);
  if (max_buffers)
    fill = MAX (fill, (gdouble) buffers / max_buffers);
  if (max_bytes)
    fill = MAX (fill, (gdouble) bytes / max_bytes);
  if (max_time)
    fill = MAX (fill, (gdouble) time / max_time);
  return fill;
}

static gdouble
queue_fill (GstElement * queue)
{
  guint max_buffers = 0, max_bytes = 0;
  guint64 max_time = 0;
  gdouble fill = 0;
  GstIterator *it;
  GValue item = G_VALUE_INIT;

  g_object_get (queue, "max-size-buffers", &max_buffers, "max-size-bytes",
      &max_bytes, "max-size-time", &max_time, NULL);
  if (g_object_class_find_property (G_OBJECT_GET_CLASS (queue),
          "current-level-buffers"))
    return fill_of (queue, max_buffers, max_bytes, max_time);
  it = gst_element_iterate_sink_pads (queue);
  while (gst_iterator_next (it, &item) == GST_ITERATOR_OK) {
    GstPad *pad = g_value_get_object (&item);

    if (g_object_class_find_property (G_OBJECT_GET_CLASS (pad),
            "current-level-buffers"))
      fill = MAX (fill, fill_of (pad, max_buffers, max_bytes, max_time));
    g_value_reset (&item);
  }
  g_value_unset (&item);
  gst_iterator_free (it);
  return fill;
}

static gboolean
is_leaky (GstElement * queue)
{
  gint leaky = 0;

  if (!g_object_class_find_property (G_OBJECT_GET_CLASS (queue), "leaky"))
    return FALSE;
  g_object_get (queue, "leaky", &leaky, NULL);
  return leaky != 0;
}

static guint64
history_sum (const guint64 * h)
{
  guint64 sum = 0;
  guint i;

  for (i = 0; i < HISTORY; i++)
    sum += h[i];
  return sum;
}

/* Queues full for a while, and leaky queues dropping data: dropped = in -
 * out - growth of the level, counted by probes on leaky queues only. */
static void
add_queues (PmFindings * self, GPtrArray * findings, GHashTable * seen)
{
  GPtrArray *queues = pm_model_find_elements (self->model, queue_filter);
  gint64 now = g_get_monotonic_time ();
  guint i;

  for (i = 0; i < queues->len; i++) {
    PmElementRef *ref = g_ptr_array_index (queues, i);
    PmQueueState *q = g_hash_table_lookup (self->queues, &ref->id);
    gdouble fill = queue_fill (ref->element);
    PmFinding *f;

    if (q == NULL) {
      q = g_new0 (PmQueueState, 1);
      g_hash_table_insert (self->queues, key_new (ref->id), q);
    }
    g_hash_table_add (seen, key_new (ref->id));

    if (fill >= 0.99) {
      if (q->full_since == 0)
        q->full_since = now;
    } else {
      q->full_since = 0;
    }
    q->leaky = is_leaky (ref->element);
    /* A full leaky queue drops instead of waiting: reported below. Not
     * live, a full queue is the usual pace: the source waits, nothing is
     * lost. */
    if (!q->leaky && ref->live && q->full_since != 0 &&
        now - q->full_since >= FULL_AFTER_US) {
      f = finding_new ("full", ref->id, PM_WARNING, ref->id, 0);
      f->text = g_strdup_printf ("%s has been full for %" G_GINT64_FORMAT
          " s", ref->path, (now - q->full_since) / G_USEC_PER_SEC);
      f->hint = g_strdup ("The pipeline is live; elements before it wait "
          "until it has room.");
      g_ptr_array_add (findings, f);
    }

    if (q->leaky && q->in.counter == NULL) {
      count_probe_add (&q->in, ref->element, "sink");
      count_probe_add (&q->out, ref->element, "src");
    }
    if (q->leaky && q->in.counter && q->out.counter) {
      guint in = g_atomic_int_get (&q->in.counter->count);
      guint out = g_atomic_int_get (&q->out.counter->count);
      guint level = 0;
      gint64 dropped;

      g_object_get (ref->element, "current-level-buffers", &level, NULL);
      dropped = q->have_prev ? (gint64) (guint) (in - q->prev_in) -
          (gint64) (guint) (out - q->prev_out) - ((gint64) level -
          (gint64) q->prev_level) : 0;
      q->drops[q->ticks++ % HISTORY] = MAX (dropped, 0);
      q->prev_in = in;
      q->prev_out = out;
      q->prev_level = level;
      q->have_prev = TRUE;
      if (history_sum (q->drops) > 0) {
        f = finding_new ("dropping", ref->id, PM_WARNING, ref->id, 0);
        f->text = g_strdup_printf ("%s (leaky) dropped %" G_GUINT64_FORMAT
            " buffers in the last %d s", ref->path, history_sum (q->drops),
            HISTORY);
        f->hint = g_strdup ("It is leaky: when full it drops buffers instead "
            "of waiting.");
        g_ptr_array_add (findings, f);
      }
    }
  }
  g_ptr_array_unref (queues);
}

static gboolean
sink_filter (GstElement * element, gboolean top_level)
{
  GType type = g_type_from_name ("GstBaseSink");

  return type != 0 && g_type_is_a (G_OBJECT_TYPE (element), type);
}

/* Sinks dropping buffers (too late to show): basesink's "dropped". */
static void
add_sinks (PmFindings * self, GPtrArray * findings, GHashTable * seen)
{
  GPtrArray *sinks = pm_model_find_elements (self->model, sink_filter);
  guint i;

  for (i = 0; i < sinks->len; i++) {
    PmElementRef *ref = g_ptr_array_index (sinks, i);
    PmSinkState *s = g_hash_table_lookup (self->sinks, &ref->id);
    GstStructure *stats = NULL;
    guint64 dropped = 0;

    if (s == NULL) {
      s = g_new0 (PmSinkState, 1);
      g_hash_table_insert (self->sinks, key_new (ref->id), s);
    }
    g_hash_table_add (seen, key_new (ref->id));
    g_object_get (ref->element, "stats", &stats, NULL);
    if (stats == NULL)
      continue;
    gst_structure_get_uint64 (stats, "dropped", &dropped);
    gst_structure_free (stats);
    s->drops[s->ticks++ % HISTORY] = s->have_last && dropped > s->last ?
        dropped - s->last : 0;
    s->last = dropped;
    s->have_last = TRUE;
    if (history_sum (s->drops) > 0) {
      PmFinding *f = finding_new ("late", ref->id, PM_WARNING, ref->id, 0);

      f->text = g_strdup_printf ("%s dropped %" G_GUINT64_FORMAT " buffers "
          "in the last %d s", ref->path, history_sum (s->drops), HISTORY);
      f->hint = g_strdup ("It dropped them because they arrived later than "
          "its max-lateness.");
      g_ptr_array_add (findings, f);
    }
  }
  g_ptr_array_unref (sinks);
}

/* Forgets queues and sinks that are gone. */
static void
forget_others (GHashTable * table, GHashTable * seen)
{
  GHashTableIter iter;
  gpointer key;

  g_hash_table_iter_init (&iter, table);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    if (!g_hash_table_contains (seen, key))
      g_hash_table_iter_remove (&iter);
}

/* Once per second: all findings as {"schema":1,"findings":[{"id",
 * "severity","element","pad","text","hint","count","first","last",
 * "caps":{"upstream","downstream","common"}}]}. No time in it, so an
 * unchanged list gives the same text and need not be sent again. */
gchar *
pm_findings_tick (PmFindings * self, gint64 stuck_after_us)
{
  GPtrArray *findings = g_ptr_array_new_with_free_func ((GDestroyNotify)
      finding_free);
  /* Ids of the queues and sinks that still exist. */
  GHashTable *seen = g_hash_table_new_full (g_int64_hash, g_int64_equal,
      g_free, NULL);
  GString *json = g_string_new (NULL);
  guint i;

  add_flow (self, findings);
  add_blocked (self, findings);
  add_stuck (self, findings, stuck_after_us);
  add_structure (self, findings);
  add_queues (self, findings, seen);
  add_sinks (self, findings, seen);
  forget_others (self->queues, seen);
  forget_others (self->sinks, seen);
  g_ptr_array_sort (findings, compare_findings);

  g_string_append_printf (json, "{\"schema\":%d,\"findings\":[", PM_SCHEMA);
  for (i = 0; i < findings->len; i++) {
    PmFinding *f = g_ptr_array_index (findings, i);

    if (i > 0)
      g_string_append_c (json, ',');
    g_string_append (json, "{\"id\":");
    append_escaped (json, f->id);
    g_string_append_printf (json, ",\"severity\":\"%s\",\"element\":%"
        G_GUINT64_FORMAT, severity_names[f->severity], f->element);
    if (f->pad != 0)
      g_string_append_printf (json, ",\"pad\":%" G_GUINT64_FORMAT, f->pad);
    g_string_append (json, ",\"text\":");
    append_escaped (json, f->text);
    g_string_append (json, ",\"hint\":");
    append_escaped (json, f->hint ? f->hint : "");
    if (f->count > 0)
      g_string_append_printf (json, ",\"count\":%" G_GUINT64_FORMAT
          ",\"first\":%" G_GINT64_FORMAT ",\"last\":%" G_GINT64_FORMAT,
          f->count, f->first, f->last);
    if (f->upstream != NULL || f->downstream != NULL) {
      g_string_append (json, ",\"caps\":{\"upstream\":");
      append_escaped (json, f->upstream ? f->upstream : "");
      g_string_append (json, ",\"downstream\":");
      append_escaped (json, f->downstream ? f->downstream : "");
      g_string_append_printf (json, ",\"common\":%s}",
          f->common ? "true" : "false");
    }
    g_string_append_c (json, '}');
  }
  g_string_append (json, "]}");
  g_ptr_array_unref (findings);
  g_hash_table_unref (seen);
  return g_string_free (json, FALSE);
}
