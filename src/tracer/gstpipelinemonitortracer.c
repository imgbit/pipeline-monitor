/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "gstpipelinemonitortracer.h"
#include "pmlevels.h"
#include "pmmodel.h"
#include "pmserver.h"

#include <stdlib.h>
#include <string.h>

#define DEFAULT_PORT 8765

GST_DEBUG_CATEGORY_STATIC (gst_pipeline_monitor_debug);
#define GST_CAT_DEFAULT gst_pipeline_monitor_debug

struct _GstPipelineMonitorTracer
{
  GstTracer parent;

  PmModel *model;
  PmServer *server;
  gboolean log;
  gboolean flow;
  gboolean wait;
  /* appsinks and appsrcs made; pairing: both exist. */
  gint appsinks, appsrcs, pairing;
};

/* The running server, for the exit handler (programs that exit without
 * gst_deinit). */
static PmServer *exiting_server;
static GMutex exiting_lock;

enum
{
  PROP_0,
  PROP_URL,
};

#define _do_init \
    GST_DEBUG_CATEGORY_INIT (gst_pipeline_monitor_debug, "pipeline-monitor", 0, \
        "pipeline monitor tracer");

G_DEFINE_TYPE_WITH_CODE (GstPipelineMonitorTracer, gst_pipeline_monitor_tracer,
    GST_TYPE_TRACER, _do_init);

static void
sync_element (GstPipelineMonitorTracer * self, GstElement * element)
{
  GstElementFactory *factory = gst_element_get_factory (element);
  gchar *name = gst_object_get_name (GST_OBJECT (element));

  pm_model_update_element (self->model, element, name,
      factory ? GST_OBJECT_NAME (factory) : NULL, G_OBJECT_TYPE_NAME (element),
      GST_IS_BIN (element), GST_IS_PIPELINE (element));
  g_free (name);
}

/* The inner side of a ghost pad is a proxy pad that belongs to no element.
 * Use the ghost pad in its place so links can be followed across bins. */
static gpointer
resolve_pad (GstPad * pad)
{
  GstProxyPad *ghost;

  if (!GST_IS_PROXY_PAD (pad) || GST_IS_GHOST_PAD (pad))
    return pad;

  ghost = gst_proxy_pad_get_internal (GST_PROXY_PAD (pad));
  if (ghost == NULL)
    return pad;
  gst_object_unref (ghost);
  return ghost;
}

/* queue and multiqueue signal when they fill up or run empty; only counted
 * here, on the streaming thread. */
static void
on_overrun (GstElement * element, GstPipelineMonitorTracer * self)
{
  pm_model_count_queue_event (self->model, element, TRUE);
}

static void
on_underrun (GstElement * element, GstPipelineMonitorTracer * self)
{
  pm_model_count_queue_event (self->model, element, FALSE);
}

static gboolean
is_type (GstElement * element, const gchar * name)
{
  GType type = g_type_from_name (name);

  return type != 0 && g_type_is_a (G_OBJECT_TYPE (element), type);
}

static void
do_element_new (GstTracer * tracer, GstClockTime ts, GstElement * element)
{
  GstPipelineMonitorTracer *self = GST_PIPELINE_MONITOR_TRACER (tracer);
  static gsize calibrated;

  /* Hook timestamps count from GStreamer's start: note once where that
   * is, for the age of pushes in progress. */
  if (g_once_init_enter (&calibrated)) {
    pm_model_set_ts_offset (gst_util_get_timestamp () - ts);
    g_once_init_leave (&calibrated, 1);
  }

  GST_DEBUG_OBJECT (tracer, "new element %s (%s)", GST_OBJECT_NAME (element),
      G_OBJECT_TYPE_NAME (element));
  sync_element (self, element);
  if (is_type (element, "GstAppSink"))
    g_atomic_int_inc (&self->appsinks);
  else if (is_type (element, "GstAppSrc"))
    g_atomic_int_inc (&self->appsrcs);
  if (g_atomic_int_get (&self->appsinks) && g_atomic_int_get (&self->appsrcs))
    g_atomic_int_set (&self->pairing, 1);
  if (pm_levels_is_queue (element) &&
      g_signal_lookup ("overrun", G_OBJECT_TYPE (element)) != 0) {
    g_signal_connect_object (element, "overrun", G_CALLBACK (on_overrun),
        self, 0);
    g_signal_connect_object (element, "underrun", G_CALLBACK (on_underrun),
        self, 0);
  }
}

static void
do_bin_add_post (GstTracer * tracer, GstClockTime ts, GstBin * bin,
    GstElement * element, gboolean result)
{
  GstPipelineMonitorTracer *self = GST_PIPELINE_MONITOR_TRACER (tracer);

  if (!result)
    return;
  sync_element (self, GST_ELEMENT (bin));
  sync_element (self, element);
  pm_model_set_parent (self->model, element, bin);
}

static void
do_bin_remove_pre (GstTracer * tracer, GstClockTime ts, GstBin * bin,
    GstElement * element)
{
  GstPipelineMonitorTracer *self = GST_PIPELINE_MONITOR_TRACER (tracer);

  if (GST_OBJECT_PARENT (element) == GST_OBJECT_CAST (bin))
    pm_model_set_parent (self->model, element, NULL);
}

static void
sync_caps (GstPad * pad, GParamSpec * pspec, GstPipelineMonitorTracer * self)
{
  GstCaps *caps = gst_pad_get_current_caps (pad);
  gchar *text = caps ? gst_caps_to_string (caps) : NULL;

  pm_model_set_caps (self->model, pad, text);
  g_free (text);
  if (caps != NULL)
    gst_caps_unref (caps);
}

/* GStreamer notifies "caps" whenever a caps event is stored on the pad or
 * its events are cleared (e.g. on going back to READY). */
static void
watch_caps (GstPipelineMonitorTracer * self, GstPad * pad)
{
  if (g_signal_handler_find (pad, G_SIGNAL_MATCH_FUNC | G_SIGNAL_MATCH_DATA,
          0, 0, NULL, sync_caps, self) == 0)
    g_signal_connect_object (pad, "notify::caps", G_CALLBACK (sync_caps),
        self, 0);
  sync_caps (pad, NULL, self);
}

static void
do_element_add_pad (GstTracer * tracer, GstClockTime ts, GstElement * element,
    GstPad * pad)
{
  GstPipelineMonitorTracer *self = GST_PIPELINE_MONITOR_TRACER (tracer);
  GstPadTemplate *templ = gst_pad_get_pad_template (pad);
  gchar *name = gst_object_get_name (GST_OBJECT (pad));
  const gchar *presence = NULL;

  if (templ != NULL) {
    switch (GST_PAD_TEMPLATE_PRESENCE (templ)) {
      case GST_PAD_ALWAYS:
        presence = "always";
        break;
      case GST_PAD_SOMETIMES:
        presence = "sometimes";
        break;
      case GST_PAD_REQUEST:
        presence = "request";
        break;
    }
    gst_object_unref (templ);
  }

  sync_element (self, element);
  pm_model_add_pad (self->model, element, pad, name,
      gst_pad_get_direction (pad), presence, GST_IS_GHOST_PAD (pad));
  watch_caps (self, pad);
  g_free (name);
}

static void
do_element_remove_pad (GstTracer * tracer, GstClockTime ts,
    GstElement * element, GstPad * pad)
{
  pm_model_remove_pad (GST_PIPELINE_MONITOR_TRACER (tracer)->model, pad);
}

/* decodebin3 links some pads twice and ignores the result; linking to the
 * pad that is already the peer is harmless. */
static gboolean
already_linked (GstPad * src, GstPad * sink, GstPadLinkReturn result)
{
  GstPad *peer;
  gboolean same;

  if (result != GST_PAD_LINK_WAS_LINKED)
    return FALSE;
  peer = gst_pad_get_peer (src);
  same = peer == sink;
  gst_clear_object (&peer);
  return same;
}

static void
log_link_failure (GstPipelineMonitorTracer * self, GstPad * src, GstPad * sink,
    GstPadLinkReturn result)
{
  GstElement *element = gst_pad_get_parent_element (resolve_pad (src));
  gchar *src_name = gst_object_get_path_string (GST_OBJECT (src));
  gchar *sink_name = gst_object_get_path_string (GST_OBJECT (sink));
  gchar *text = g_strdup_printf ("cannot link %s to %s: %s", src_name,
      sink_name, gst_pad_link_get_name (result));

  if (element != NULL)
    sync_element (self, element);
  pm_model_add_log (self->model, element, PM_LOG_WARNING, "link", text, NULL,
      NULL, 0);
  g_free (text);
  g_free (sink_name);
  g_free (src_name);
  gst_clear_object (&element);
}

static gchar *
caps_text (GstPad * pad)
{
  GstCaps *caps = gst_pad_query_caps (pad, NULL);
  gchar *text = caps ? gst_caps_to_string (caps) : NULL;

  gst_clear_caps (&caps);
  return text;
}

/* Kept for the findings, with what each pad could handle. */
static void
record_failed_link (GstPipelineMonitorTracer * self, GstPad * src,
    GstPad * sink, GstPadLinkReturn result)
{
  gchar *src_name = g_strdup_printf ("%s:%s", GST_DEBUG_PAD_NAME (src));
  gchar *sink_name = g_strdup_printf ("%s:%s", GST_DEBUG_PAD_NAME (sink));
  gchar *src_caps = caps_text (src);
  gchar *sink_caps = caps_text (sink);

  pm_model_record_failed_link (self->model, resolve_pad (src),
      resolve_pad (sink), src_name, sink_name, gst_pad_link_get_name (result),
      src_caps, sink_caps);
  g_free (src_name);
  g_free (sink_name);
  g_free (src_caps);
  g_free (sink_caps);
}

static void
do_pad_link_post (GstTracer * tracer, GstClockTime ts, GstPad * src,
    GstPad * sink, GstPadLinkReturn result)
{
  GstPipelineMonitorTracer *self = GST_PIPELINE_MONITOR_TRACER (tracer);

  if (result != GST_PAD_LINK_OK) {
    if (already_linked (src, sink, result))
      return;
    if (self->log)
      log_link_failure (self, src, sink, result);
    record_failed_link (self, src, sink, result);
    return;
  }
  pm_model_link (self->model, resolve_pad (src), resolve_pad (sink));
}

static void
do_pad_unlink_post (GstTracer * tracer, GstClockTime ts, GstPad * src,
    GstPad * sink, gboolean result)
{
  if (!result)
    return;
  pm_model_unlink (GST_PIPELINE_MONITOR_TRACER (tracer)->model,
      resolve_pad (src), resolve_pad (sink));
}

static void
sync_state (GstPipelineMonitorTracer * self, GstElement * element)
{
  sync_element (self, element);
  pm_model_sync_state (self->model, element);
}

static void
log_message (GstPipelineMonitorTracer * self, GstElement * element,
    GstMessage * message)
{
  GError *error = NULL;
  gchar *debug = NULL;
  PmLogLevel level;

  if (GST_MESSAGE_TYPE (message) == GST_MESSAGE_ERROR) {
    gst_message_parse_error (message, &error, &debug);
    level = PM_LOG_ERROR;
  } else {
    gst_message_parse_warning (message, &error, &debug);
    level = PM_LOG_WARNING;
  }
  sync_element (self, element);
  /* basesink: upstream reports too little latency room for its processing
   * deadline (the text is translated; the debug part is not). */
  if (level == PM_LOG_WARNING && error->domain == GST_CORE_ERROR &&
      error->code == GST_CORE_ERROR_CLOCK && debug != NULL &&
      strstr (debug, "the processing deadline") != NULL)
    pm_model_note_no_room (self->model, element);
  pm_model_add_log (self->model, element, level, "message", error->message,
      debug, g_quark_to_string (error->domain), error->code);
  g_free (debug);
  g_error_free (error);
}

/* Sources (basesrc) stop on their own when negotiation or linking fails,
 * without a push the flow hooks would see: their error says "streaming
 * stopped, reason not-negotiated". Recorded on the element's src pad like
 * a failed push. */
static void
record_stopped (GstPipelineMonitorTracer * self, GstElement * element,
    GstMessage * message)
{
  static const struct
  {
    const gchar *reason;
    GstFlowReturn result;
  } reasons[] = {
    {"reason not-negotiated", GST_FLOW_NOT_NEGOTIATED},
    {"reason not-linked", GST_FLOW_NOT_LINKED},
  };
  gchar *debug = NULL;
  GstPad *pad;
  guint i;

  gst_message_parse_error (message, NULL, &debug);
  for (i = 0; debug != NULL && i < G_N_ELEMENTS (reasons); i++) {
    if (strstr (debug, reasons[i].reason) == NULL)
      continue;
    pad = gst_element_get_static_pad (element, "src");
    if (pad != NULL) {
      pm_model_record_flow (self->model, pad, reasons[i].result);
      gst_object_unref (pad);
    }
    break;
  }
  g_free (debug);
}

/* Sinks and bins with sinks, whose PAUSED waits for data. */
static gboolean
has_preroll (GstElement * element)
{
  return GST_OBJECT_FLAG_IS_SET (element, GST_ELEMENT_FLAG_SINK) &&
      !GST_IS_PIPELINE (element);
}

/* Set on a pad when EOS is pushed into it, cleared when a new stream or
 * a flush starts: the pad's own EOS flag is set only after the sink has
 * handled the event, and it prerolls while handling it. */
static GQuark
eos_quark (void)
{
  static GQuark quark;

  if (quark == 0)
    quark = g_quark_from_static_string ("pm-eos");
  return quark;
}

/* appsink -> program -> appsrc, found from the data: about every 100 ms a
 * buffer going into each appsink has its memory marked (qdata, unseen by
 * the program); a marked memory leaving an appsrc proves the link. A buffer
 * into an appsink that is not marked loses any old mark (a buffer pool
 * reuses memory), so a mark always names the appsink last passed. */

#define MARK_EVERY_US (100 * G_TIME_SPAN_MILLISECOND)

enum
{
  ROLE_OTHER = 1,
  ROLE_INTO_APPSINK,
  ROLE_FROM_APPSRC,
};

/* A pad's role, kept with the peer it was worked out for, and what the
 * role needs: the appsink's id or the appsrc, the segment last sent, when
 * the last mark was made. Pushes on a pad are serialized. */
typedef struct
{
  GstPad *peer;
  gint role;
  guint64 sink;
  GstElement *appsrc;
  guint64 appsrc_id;
  GstSegment segment;
  gboolean has_segment;
  gint64 last_mark;
  guint64 last_slot;
} PmPadRole;

/* The appsinks one buffer went into (a tee: several), its PTS and the
 * first one's segment, when it was marked. */
#define MARK_SINKS 4

/* Read and changed by the appsinks' and appsrcs' threads: under
 * mark_lock (looking a mark up is safe without). */
typedef struct
{
  guint64 sinks[MARK_SINKS];
  guint n_sinks;
  /* The appsrcs it already left, and how it left each: an appsink it
   * goes into later is linked to them too. */
  struct
  {
    guint64 id;
    GstClockTime pts, delay;
    GstSegment segment;
    gboolean has_segment;
  } appsrcs[MARK_SINKS];
  guint n_appsrcs;
  GstClockTime pts;
  GstSegment segment;
  gboolean has_segment;
  gint64 time;
} PmMark;

static GQuark mark_quark;
static gint live_marks;
static GMutex mark_lock;

static void
mark_free (gpointer data)
{
  g_free (data);
  g_atomic_int_add (&live_marks, -1);
}

/* Kept with the pad's push timing: no second lookup per push. */
static PmPadRole *
pad_role (GstPipelineMonitorTracer * self, GstPad * pad, PmPushTiming * t)
{
  PmPadRole *r = t->pairing;
  GstPad *peer = GST_PAD_PEER (pad);
  GstObject *parent, *into;

  if (G_LIKELY (r != NULL && r->peer == peer && r->role != 0))
    return r;
  if (r == NULL)
    t->pairing = r = g_new0 (PmPadRole, 1);
  r->peer = peer;
  parent = gst_pad_get_parent (pad);
  into = peer ? gst_pad_get_parent (peer) : NULL;
  r->role = ROLE_OTHER;
  if (into && GST_IS_ELEMENT (into) && is_type (GST_ELEMENT (into),
          "GstAppSink")) {
    r->role = ROLE_INTO_APPSINK;
    r->sink = pm_model_element_id (self->model, into);
  } else if (parent && GST_IS_ELEMENT (parent) &&
      is_type (GST_ELEMENT (parent), "GstAppSrc")) {
    r->role = ROLE_FROM_APPSRC;
    /* The pad's own element: it outlives the pad. */
    r->appsrc = GST_ELEMENT (parent);
    r->appsrc_id = pm_model_element_id (self->model, parent);
  }
  gst_clear_object (&into);
  gst_clear_object (&parent);
  return r;
}

static void
pair_segment (GstPad * pad, GstEvent * event)
{
  PmPushTiming *t = pm_push_timing (pad);
  PmPadRole *r = t->pairing;
  const GstSegment *segment;

  /* Before the first buffer: the role is worked out then. */
  if (r == NULL)
    t->pairing = r = g_new0 (PmPadRole, 1);
  gst_event_parse_segment (event, &segment);
  gst_segment_copy_into (segment, &r->segment);
  r->has_segment = TRUE;
}

/* now: the hook's timestamp, in µs. */
static void
pair_buffer (GstPipelineMonitorTracer * self, PmPadRole * r, GstBuffer * b,
    gint64 now)
{
  guint i, n = gst_buffer_n_memory (b);

  if (r->role == ROLE_INTO_APPSINK) {
    /* Which buffers: the first in each 100 ms of PTS, so that appsinks
     * fed the same buffers (a tee) mark the same ones; by the clock when
     * there is no PTS. */
    guint64 slot = GST_BUFFER_PTS_IS_VALID (b) ? GST_BUFFER_PTS (b) /
        (MARK_EVERY_US * GST_USECOND) + 1 : 0;
    gboolean sample = r->sink != 0 && (slot ? slot != r->last_slot :
        now - r->last_mark >= MARK_EVERY_US);

    /* A mark already there: the same buffer going into another appsink
     * too (a tee: same PTS) adds this one; else the memory was reused
     * for a new buffer (a buffer pool) and the old mark goes. So a mark
     * names exactly the appsinks this buffer went into. Looking, adding
     * and making a mark are one step under the lock: appsinks fed by a
     * tee do it at the same moment. */
    if (slot)
      r->last_slot = slot;
    if (sample)
      r->last_mark = now;
    for (i = 0; i < n && (sample || g_atomic_int_get (&live_marks)); i++) {
      GstMiniObject *mem = GST_MINI_OBJECT (gst_buffer_peek_memory (b, i));
      PmMark copy;
      gboolean joined = FALSE;
      guint k, j;
      PmMark *old;

      if (!sample && G_LIKELY (gst_mini_object_get_qdata (mem,
                  mark_quark) == NULL))
        continue;
      g_mutex_lock (&mark_lock);
      old = gst_mini_object_get_qdata (mem, mark_quark);
      if (old && GST_BUFFER_PTS_IS_VALID (b) && old->pts ==
          GST_BUFFER_PTS (b) && r->sink != 0) {
        for (k = 0; k < old->n_sinks && old->sinks[k] != r->sink; k++);
        if (k == old->n_sinks && k < MARK_SINKS) {
          old->sinks[old->n_sinks++] = r->sink;
          copy = *old;
          joined = TRUE;
        }
      } else {
        if (old)
          gst_mini_object_set_qdata (mem, mark_quark, NULL, NULL);
        if (sample) {
          PmMark *m = g_new0 (PmMark, 1);

          m->sinks[0] = r->sink;
          m->n_sinks = 1;
          m->pts = GST_BUFFER_PTS (b);
          m->has_segment = r->has_segment;
          if (r->has_segment)
            gst_segment_copy_into (&r->segment, &m->segment);
          m->time = now;
          g_atomic_int_inc (&live_marks);
          gst_mini_object_set_qdata (mem, mark_quark, m, mark_free);
        }
      }
      g_mutex_unlock (&mark_lock);
      /* Already out of appsrcs: each is fed by every appsink of this
       * buffer, shared. */
      for (j = 0; joined && j < copy.n_appsrcs; j++)
        for (k = 0; k < copy.n_sinks; k++)
          pm_model_fed_by_id (self->model, copy.appsrcs[j].id,
              copy.sinks[k], TRUE, copy.pts, copy.has_segment ?
              &copy.segment : NULL, copy.appsrcs[j].pts,
              copy.appsrcs[j].has_segment ? &copy.appsrcs[j].segment : NULL,
              copy.appsrcs[j].delay);
    }
  } else if (r->role == ROLE_FROM_APPSRC) {
    GstMiniObject *mem = NULL;
    PmMark *found, copy;
    guint k;

    if (!g_atomic_int_get (&live_marks))
      return;
    for (i = 0; i < n && mem == NULL; i++)
      if (gst_mini_object_get_qdata (GST_MINI_OBJECT (gst_buffer_peek_memory
                  (b, i)), mark_quark))
        mem = GST_MINI_OBJECT (gst_buffer_peek_memory (b, i));
    if (G_LIKELY (mem == NULL))
      return;
    /* The mark stays: the same memory may leave other appsrcs too. */
    g_mutex_lock (&mark_lock);
    found = gst_mini_object_get_qdata (mem, mark_quark);
    if (found) {
      for (k = 0; k < found->n_appsrcs && found->appsrcs[k].id !=
          r->appsrc_id; k++);
      if (k == found->n_appsrcs && k < MARK_SINKS) {
        found->appsrcs[k].id = r->appsrc_id;
        found->appsrcs[k].pts = GST_BUFFER_PTS (b);
        found->appsrcs[k].delay = (now - found->time) * GST_USECOND;
        found->appsrcs[k].has_segment = r->has_segment;
        if (r->has_segment)
          gst_segment_copy_into (&r->segment, &found->appsrcs[k].segment);
        found->n_appsrcs++;
      }
      copy = *found;
    }
    g_mutex_unlock (&mark_lock);
    if (found == NULL)
      return;
    for (i = 0; i < copy.n_sinks; i++)
      pm_model_fed_by (self->model, r->appsrc, copy.sinks[i],
          copy.n_sinks > 1, copy.pts, copy.has_segment ? &copy.segment :
          NULL, GST_BUFFER_PTS (b), r->has_segment ? &r->segment : NULL,
          (now - copy.time) * GST_USECOND);
  }
}

static gboolean
pair_list_item (GstBuffer ** buffer, guint idx, gpointer user_data)
{
  gpointer *args = user_data;

  pair_buffer (args[0], args[1], *buffer, GPOINTER_TO_SIZE (args[2]));
  return TRUE;
}

static void
do_pad_push_event_pre (GstTracer * tracer, GstClockTime ts, GstPad * pad,
    GstEvent * event)
{
  GstEventType type = GST_EVENT_TYPE (event);
  GstPad *peer;

  if (type == GST_EVENT_SEGMENT && g_atomic_int_get
      (&GST_PIPELINE_MONITOR_TRACER (tracer)->pairing))
    pair_segment (pad, event);
  if (type != GST_EVENT_EOS && type != GST_EVENT_STREAM_START &&
      type != GST_EVENT_FLUSH_STOP)
    return;
  peer = gst_pad_get_peer (pad);
  if (peer == NULL)
    return;
  g_object_set_qdata (G_OBJECT (peer), eos_quark (),
      GINT_TO_POINTER (type == GST_EVENT_EOS));
  gst_object_unref (peer);
}

/* Prerolled on EOS: EOS has reached the sink's own sink pad. */
static gboolean
at_eos (GstElement * element)
{
  GstPad *pad;
  gboolean eos;

  if (GST_IS_BIN (element))
    return FALSE;
  pad = gst_element_get_static_pad (element, "sink");
  if (pad == NULL)
    return FALSE;
  eos = GST_PAD_IS_EOS (pad) ||
      g_object_get_qdata (G_OBJECT (pad), eos_quark ()) != NULL;
  gst_object_unref (pad);
  return eos;
}

static void
do_element_post_message_pre (GstTracer * tracer, GstClockTime ts,
    GstElement * element, GstMessage * message)
{
  GstPipelineMonitorTracer *self = GST_PIPELINE_MONITOR_TRACER (tracer);

  switch (GST_MESSAGE_TYPE (message)) {
    case GST_MESSAGE_STATE_CHANGED:
      sync_state (self, element);
      break;
    case GST_MESSAGE_ERROR:
    case GST_MESSAGE_WARNING:
      /* Bins re-post their children's messages; keep only the original. */
      if (GST_MESSAGE_SRC (message) != GST_OBJECT_CAST (element))
        break;
      if (self->log)
        log_message (self, element, message);
      if (GST_MESSAGE_TYPE (message) == GST_MESSAGE_ERROR) {
        record_stopped (self, element, message);
        /* The program may end right after an error: send what is known
         * now, and keep the findings as they are while it stops. */
        if (self->server != NULL) {
          pm_server_hold_findings (self->server);
          pm_server_flush (self->server, FALSE);
        }
      }
      break;
    case GST_MESSAGE_ELEMENT:
      /* decodebin and playbin post this when no element can handle a
       * stream (no decoder for its caps, no source for a URI...). */
      if (GST_MESSAGE_SRC (message) == GST_OBJECT_CAST (element) &&
          gst_message_has_name (message, "missing-plugin")) {
        const GstStructure *s = gst_message_get_structure (message);
        const gchar *name = gst_structure_get_string (s, "name");
        const gchar *type = gst_structure_get_string (s, "type");
        gchar *detail = NULL;
        gchar *what;

        if (name == NULL && gst_structure_has_field (s, "detail"))
          detail = gst_value_serialize (gst_structure_get_value (s,
                  "detail"));
        what = name ? g_strdup (name) :
            g_strdup_printf ("%s for %s", type ? type : "element",
            detail ? detail : "?");
        sync_element (self, element);
        pm_model_record_missing (self->model, element, what);
        g_free (what);
        g_free (detail);
      }
      break;
    case GST_MESSAGE_STREAM_STATUS:{
      /* Posted by a pad's task from inside its thread when it starts and
       * stops; bins post it again, so only the pad's own element. */
      GstObject *src = GST_MESSAGE_SRC (message);
      GstStreamStatusType type;
      GstElement *owner;

      if (!GST_IS_PAD (src) || GST_OBJECT_PARENT (src) !=
          GST_OBJECT_CAST (element))
        break;
      gst_message_parse_stream_status (message, &type, &owner);
      if (type == GST_STREAM_STATUS_TYPE_ENTER) {
        const GValue *task = gst_message_get_stream_status_object (message);

        if (task != NULL && G_VALUE_HOLDS (task, GST_TYPE_TASK))
          pm_model_thread_task (self->model,
              GST_OBJECT_NAME (g_value_get_object (task)));
      } else if (type == GST_STREAM_STATUS_TYPE_LEAVE) {
        pm_model_thread_ended (self->model);
      }
      break;
    }
    case GST_MESSAGE_ASYNC_START:
    case GST_MESSAGE_ASYNC_DONE:
      if (GST_MESSAGE_SRC (message) == GST_OBJECT_CAST (element) &&
          has_preroll (element)) {
        sync_element (self, element);
        pm_model_set_preroll (self->model, element,
            GST_MESSAGE_TYPE (message) == GST_MESSAGE_ASYNC_START ?
            PM_PREROLL_WAITING : at_eos (element) ? PM_PREROLL_DONE_EOS :
            PM_PREROLL_DONE);
      }
      break;
    case GST_MESSAGE_BUFFERING:
      if (GST_MESSAGE_SRC (message) == GST_OBJECT_CAST (element)) {
        gint percent;

        gst_message_parse_buffering (message, &percent);
        pm_model_set_buffering (self->model, element, percent);
      }
      break;
    default:
      break;
  }
}

static void
do_element_change_state_post (GstTracer * tracer, GstClockTime ts,
    GstElement * element, GstStateChange transition,
    GstStateChangeReturn result)
{
  GstPipelineMonitorTracer *self = GST_PIPELINE_MONITOR_TRACER (tracer);
  GstState next = GST_STATE_TRANSITION_NEXT (transition);

  /* A failing child makes every parent bin fail too; record the child. */
  if (result == GST_STATE_CHANGE_FAILURE && self->log && !GST_IS_BIN (element)) {
    gchar *text = g_strdup_printf ("state change from %s to %s failed",
        gst_element_state_get_name (GST_STATE_TRANSITION_CURRENT (transition)),
        gst_element_state_get_name (next));

    sync_element (self, element);
    pm_model_add_log (self->model, element, PM_LOG_ERROR, "state-change", text,
        NULL, NULL, 0);
    g_free (text);
  }

  if (GST_IS_PIPELINE (element) && GST_OBJECT_PARENT (element) == NULL) {
    if (transition == GST_STATE_CHANGE_READY_TO_PAUSED && self->server != NULL)
      pm_server_release_findings (self->server);
    if (transition == GST_STATE_CHANGE_PAUSED_TO_READY) {
      sync_element (self, element);
      pm_model_set_live (self->model, element, FALSE);
    } else if (result == GST_STATE_CHANGE_NO_PREROLL) {
      sync_element (self, element);
      pm_model_set_live (self->model, element, TRUE);
    }
  }

  if (has_preroll (element)) {
    if (transition == GST_STATE_CHANGE_PAUSED_TO_READY) {
      sync_element (self, element);
      pm_model_set_preroll (self->model, element, PM_PREROLL_UNKNOWN);
    } else if (result == GST_STATE_CHANGE_NO_PREROLL &&
        next == GST_STATE_PAUSED) {
      sync_element (self, element);
      pm_model_set_preroll (self->model, element, PM_PREROLL_LIVE);
    } else if (result == GST_STATE_CHANGE_SUCCESS &&
        transition == GST_STATE_CHANGE_READY_TO_PAUSED) {
      sync_element (self, element);
      pm_model_set_preroll (self->model, element, PM_PREROLL_NO_WAIT);
    }
  }

  /* On success GStreamer completes the change right after this hook, and
   * posts no message when the state does not change (PLAYING to PLAYING).
   * Record the outcome now so the pending state does not go stale. */
  if ((result == GST_STATE_CHANGE_SUCCESS ||
          result == GST_STATE_CHANGE_NO_PREROLL) &&
      GST_STATE_PENDING (element) == next) {
    sync_element (self, element);
    pm_model_set_state (self->model, element, next, GST_STATE_VOID_PENDING);
  } else {
    sync_state (self, element);
  }

  /* A stopped pipeline is often destroyed right away: send its final
   * state before the next update would. */
  if (transition == GST_STATE_CHANGE_READY_TO_NULL &&
      GST_IS_PIPELINE (element) && self->server != NULL)
    pm_server_flush (self->server, FALSE);
}

/* Data flow results. These run for every push and pull of every pad: keep
 * the OK case to one comparison. Flushing is normal during seeks and
 * state changes. */
static void
record_flow (GstTracer * tracer, GstPad * pad, GstFlowReturn res)
{
  if (G_LIKELY (res == GST_FLOW_OK || res == GST_FLOW_FLUSHING))
    return;
  pm_model_record_flow (GST_PIPELINE_MONITOR_TRACER (tracer)->model,
      resolve_pad (pad), res);
}

/* Pushes in progress: start and end, for blocked data flow. ms + 1, so
 * that 0 means no push. */
static void
do_pad_push_pre (GstTracer * tracer, GstClockTime ts, GstPad * pad,
    gpointer data)
{
  PmModel *model = GST_PIPELINE_MONITOR_TRACER (tracer)->model;
  PmPushTiming *t = pm_push_timing (pad);
  gint thread = pm_model_current_thread (model);

  g_atomic_int_set (&t->start_ms, (gint) (ts / GST_MSECOND) + 1);
  if (G_UNLIKELY (!g_atomic_int_get (&t->moved)))
    g_atomic_int_set (&t->moved, 1);
  pm_model_note_pad_thread (model, t, thread);
  if (G_UNLIKELY (g_atomic_int_get (&GST_PIPELINE_MONITOR_TRACER
              (tracer)->pairing))) {
    GstPipelineMonitorTracer *self = GST_PIPELINE_MONITOR_TRACER (tracer);
    PmPadRole *r = pad_role (self, pad, t);
    gint64 now = ts / GST_USECOND;

    if (r->role == ROLE_OTHER)
      return;
    if (GST_IS_BUFFER (data)) {
      pair_buffer (self, r, data, now);
    } else if (GST_IS_BUFFER_LIST (data)) {
      gpointer args[] = { self, r, GSIZE_TO_POINTER (now) };

      gst_buffer_list_foreach (data, pair_list_item, args);
    }
  }
}

static void
do_pad_push_post (GstTracer * tracer, GstClockTime ts, GstPad * pad,
    GstFlowReturn res)
{
  g_atomic_int_set (&pm_push_timing (pad)->start_ms, 0);
  record_flow (tracer, pad, res);
}

static void
do_pad_pull_range_post (GstTracer * tracer, GstClockTime ts, GstPad * pad,
    GstBuffer * buffer, GstFlowReturn res)
{
  /* Reading past the end of what upstream has is how pulling ends. */
  if (res == GST_FLOW_EOS)
    return;
  /* Pulled pads get no caps: remember on the pad pulled from that data
   * came out of it. */
  if (res == GST_FLOW_OK) {
    PmModel *model = GST_PIPELINE_MONITOR_TRACER (tracer)->model;
    PmPushTiming *t = pm_push_timing (pad);
    gint thread = pm_model_current_thread (model);

    /* The pulling pad's thread carries the data over the link. */
    pm_model_note_pad_thread (model, t, thread);
    if (G_UNLIKELY (!g_atomic_int_get (&t->moved))) {
      GstPad *peer = gst_pad_get_peer (pad);

      if (peer != NULL) {
        g_atomic_int_set (&pm_push_timing (peer)->moved, 1);
        gst_object_unref (peer);
      }
      g_atomic_int_set (&t->moved, 1);
    }
  }
  record_flow (tracer, pad, res);
}

static void
do_object_destroyed (GstTracer * tracer, GstClockTime ts, GstObject * object)
{
  pm_model_remove_object (GST_PIPELINE_MONITOR_TRACER (tracer)->model, object);
}

static void
gst_pipeline_monitor_tracer_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstPipelineMonitorTracer *self = GST_PIPELINE_MONITOR_TRACER (object);

  switch (prop_id) {
    case PROP_URL:
      g_value_set_string (value,
          self->server ? pm_server_get_url (self->server) : NULL);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gboolean
get_bool_option (const GstStructure * s, const gchar * name, gboolean def)
{
  gboolean value;
  const gchar *str;

  if (gst_structure_get_boolean (s, name, &value))
    return value;
  str = gst_structure_get_string (s, name);
  if (str == NULL)
    return def;
  return !(g_ascii_strcasecmp (str, "false") == 0 ||
      g_ascii_strcasecmp (str, "no") == 0 || g_strcmp0 (str, "0") == 0);
}

/* A size like 100M or 1G (K, M, G: powers of 1024), or def. */
static guint64
get_size_option (const GstStructure * s, const gchar * name, guint64 def)
{
  const gchar *str = gst_structure_get_string (s, name);
  gint value;
  gchar *end = NULL;
  guint64 size;

  if (str == NULL)
    return gst_structure_get_int (s, name, &value) && value > 0 ? value : def;
  size = g_ascii_strtoull (str, &end, 10);
  switch (g_ascii_toupper (*end)) {
    case 'G':
      size *= 1024;
      /* fall through */
    case 'M':
      size *= 1024;
      /* fall through */
    case 'K':
      size *= 1024;
      end++;
      break;
    default:
      break;
  }
  if (size == 0 || end == str || *end != '\0') {
    g_printerr ("pipeline-monitor: invalid %s '%s', using the default\n", name,
        str);
    return def;
  }
  return size;
}

/* Options come from
 * GST_TRACERS="pipeline-monitor(port=9000,ui-dir=web,log=false)". */
static guint
parse_options (GstPipelineMonitorTracer * self, gchar ** ui_dir,
    PmRecordOptions * record)
{
  gchar *params = NULL, *desc;
  GstStructure *s;
  gint port = DEFAULT_PORT;

  *ui_dir = NULL;
  record->dir = NULL;
  record->file_max = PM_RECORD_FILE_MAX;
  record->total_max = PM_RECORD_TOTAL_MAX;
  record->record = FALSE;
  g_object_get (self, "params", &params, NULL);
  if (params == NULL)
    return DEFAULT_PORT;

  desc = g_strdup_printf ("params,%s", params);
  s = gst_structure_from_string (desc, NULL);
  if (s == NULL) {
    g_printerr ("pipeline-monitor: cannot parse options '%s', using "
        "defaults\n", params);
  } else {
    if (gst_structure_has_field (s, "port") &&
        (!gst_structure_get_int (s, "port", &port) || port < 0 ||
            port > G_MAXUINT16)) {
      g_printerr ("pipeline-monitor: invalid port, using %d\n", DEFAULT_PORT);
      port = DEFAULT_PORT;
    }
    *ui_dir = g_strdup (gst_structure_get_string (s, "ui-dir"));
    self->log = get_bool_option (s, "log", TRUE);
    self->flow = get_bool_option (s, "flow", TRUE);
    self->wait = get_bool_option (s, "wait", FALSE);
    record->dir = g_strdup (gst_structure_get_string (s, "recordings"));
    record->file_max = get_size_option (s, "record-file-max",
        PM_RECORD_FILE_MAX);
    record->total_max = get_size_option (s, "record-total-max",
        PM_RECORD_TOTAL_MAX);
    record->record = get_bool_option (s, "record", FALSE);
    gst_structure_free (s);
  }
  g_free (desc);
  g_free (params);
  return port;
}

static gsize exit_handler;

/* The pages get the final state and "bye" also when the program exits
 * without gst_deinit (which would finalize the tracer). */
static void
flush_at_exit (void)
{
  g_mutex_lock (&exiting_lock);
  if (exiting_server != NULL)
    pm_server_flush (exiting_server, TRUE);
  g_mutex_unlock (&exiting_lock);
}

static void
gst_pipeline_monitor_tracer_constructed (GObject * object)
{
  GstPipelineMonitorTracer *self = GST_PIPELINE_MONITOR_TRACER (object);
  GError *error = NULL;
  PmRecordOptions record;
  gchar *ui_dir;
  guint port;

  G_OBJECT_CLASS (gst_pipeline_monitor_tracer_parent_class)->constructed
      (object);

  self->log = TRUE;
  self->flow = TRUE;
  port = parse_options (self, &ui_dir, &record);
  pm_model_set_log_enabled (self->model, self->log);
  if (self->flow) {
    GstTracer *tracer = GST_TRACER (self);

    gst_tracing_register_hook (tracer, "pad-push-pre",
        G_CALLBACK (do_pad_push_pre));
    gst_tracing_register_hook (tracer, "pad-push-list-pre",
        G_CALLBACK (do_pad_push_pre));
    gst_tracing_register_hook (tracer, "pad-push-post",
        G_CALLBACK (do_pad_push_post));
    gst_tracing_register_hook (tracer, "pad-push-list-post",
        G_CALLBACK (do_pad_push_post));
    gst_tracing_register_hook (tracer, "pad-pull-range-post",
        G_CALLBACK (do_pad_pull_range_post));
  }
  if (ui_dir != NULL && !g_file_test (ui_dir, G_FILE_TEST_IS_DIR))
    g_printerr ("pipeline-monitor: ui-dir '%s' is not a folder, the page "
        "will not load\n", ui_dir);
  self->server = pm_server_new (self->model, port, ui_dir, &error);
  g_free (ui_dir);
  if (self->server == NULL) {
    g_printerr ("pipeline-monitor: cannot start server: %s\n", error->message);
    g_error_free (error);
    g_free ((gchar *) record.dir);
    return;
  }
  pm_server_set_recordings (self->server, &record);
  g_free ((gchar *) record.dir);
  pm_model_set_changed_func (self->model,
      (PmModelChangedFunc) pm_server_notify, self->server);
  g_printerr ("pipeline-monitor: %s\n", pm_server_get_url (self->server));

  g_mutex_lock (&exiting_lock);
  exiting_server = self->server;
  g_mutex_unlock (&exiting_lock);
  if (g_once_init_enter (&exit_handler)) {
    atexit (flush_at_exit);
    g_once_init_leave (&exit_handler, 1);
  }

  if (self->wait) {
    g_printerr ("pipeline-monitor: waiting for the page to be opened\n");
    pm_server_wait_for_page (self->server);
  }
}

static void
gst_pipeline_monitor_tracer_finalize (GObject * object)
{
  GstPipelineMonitorTracer *self = GST_PIPELINE_MONITOR_TRACER (object);

  g_mutex_lock (&exiting_lock);
  if (exiting_server == self->server)
    exiting_server = NULL;
  g_mutex_unlock (&exiting_lock);
  if (self->server != NULL) {
    pm_model_set_changed_func (self->model, NULL, NULL);
    pm_server_free (self->server);
  }
  pm_model_free (self->model);

  G_OBJECT_CLASS (gst_pipeline_monitor_tracer_parent_class)->finalize (object);
}

static void
gst_pipeline_monitor_tracer_class_init (GstPipelineMonitorTracerClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->get_property = gst_pipeline_monitor_tracer_get_property;
  gobject_class->constructed = gst_pipeline_monitor_tracer_constructed;
  gobject_class->finalize = gst_pipeline_monitor_tracer_finalize;
  mark_quark = g_quark_from_static_string ("pipeline-monitor-appsink-mark");

  g_object_class_install_property (gobject_class, PROP_URL,
      g_param_spec_string ("url", "URL",
          "Address of the web server, NULL if it is not running", NULL,
          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
}

static void
gst_pipeline_monitor_tracer_init (GstPipelineMonitorTracer * self)
{
  GstTracer *tracer = GST_TRACER (self);

  self->model = pm_model_new ();

  gst_tracing_register_hook (tracer, "element-new",
      G_CALLBACK (do_element_new));
  gst_tracing_register_hook (tracer, "bin-add-post",
      G_CALLBACK (do_bin_add_post));
  gst_tracing_register_hook (tracer, "bin-remove-pre",
      G_CALLBACK (do_bin_remove_pre));
  gst_tracing_register_hook (tracer, "element-add-pad",
      G_CALLBACK (do_element_add_pad));
  gst_tracing_register_hook (tracer, "element-remove-pad",
      G_CALLBACK (do_element_remove_pad));
  gst_tracing_register_hook (tracer, "pad-link-post",
      G_CALLBACK (do_pad_link_post));
  gst_tracing_register_hook (tracer, "pad-unlink-post",
      G_CALLBACK (do_pad_unlink_post));
  gst_tracing_register_hook (tracer, "element-change-state-post",
      G_CALLBACK (do_element_change_state_post));
  gst_tracing_register_hook (tracer, "pad-push-event-pre",
      G_CALLBACK (do_pad_push_event_pre));
  gst_tracing_register_hook (tracer, "element-post-message-pre",
      G_CALLBACK (do_element_post_message_pre));
  gst_tracing_register_hook (tracer, "object-destroyed",
      G_CALLBACK (do_object_destroyed));
}
