/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef __PM_MODEL_H__
#define __PM_MODEL_H__

#include <gst/gst.h>

G_BEGIN_DECLS

typedef struct _PmModel PmModel;

typedef void (*PmModelChangedFunc) (gpointer user_data);

PmModel *pm_model_new (void);
void pm_model_free (PmModel * model);

void pm_model_set_changed_func (PmModel * model, PmModelChangedFunc func,
    gpointer user_data);

void pm_model_update_element (PmModel * model, gpointer element,
    const gchar * name, const gchar * factory, const gchar * type,
    gboolean is_bin, gboolean is_pipeline);
void pm_model_set_parent (PmModel * model, gpointer element, gpointer parent);
void pm_model_sync_state (PmModel * model, GstElement * element);
void pm_model_set_state (PmModel * model, gpointer element, GstState state,
    GstState pending);

void pm_model_add_pad (PmModel * model, gpointer element, gpointer pad,
    const gchar * name, GstPadDirection direction, const gchar * presence,
    gboolean is_ghost);
void pm_model_remove_pad (PmModel * model, gpointer pad);
void pm_model_set_caps (PmModel * model, gpointer pad, const gchar * caps);
GstPad *pm_model_get_pad (PmModel * model, guint64 id);

void pm_model_link (PmModel * model, gpointer src, gpointer sink);
void pm_model_unlink (PmModel * model, gpointer src, gpointer sink);

void pm_model_remove_object (PmModel * model, gpointer object);

gchar *pm_model_to_json (PmModel * model);

/* Queues and pipelines, for levels and times. */
typedef struct
{
  GstElement *element;
  guint64 id;
  gchar *path;
  guint64 overruns;
  guint64 underruns;
  gint buffering;
  /* Its pipeline is live (its change to PAUSED returned NO_PREROLL). */
  gboolean live;
} PmElementRef;

typedef gboolean (*PmElementFilter) (GstElement * element, gboolean top_level);

void pm_model_count_queue_event (PmModel * model, gpointer element,
    gboolean overrun);
void pm_model_set_buffering (PmModel * model, gpointer element, gint percent);
void pm_model_set_live (PmModel * model, gpointer pipeline, gboolean live);
GPtrArray *pm_model_find_elements (PmModel * model, PmElementFilter filter);
void pm_element_ref_free (PmElementRef * ref);
guint64 pm_model_pad_id (PmModel * model, gpointer pad);
guint64 pm_model_element_id (PmModel * model, gpointer element);
/* A buffer out of appsrc whose memory last went into the appsink with
 * this id (0: from no appsink): its PTS and segment there and here
 * (segments NULL where none). */
void pm_model_pair_buffer (PmModel * model, gpointer appsrc, guint64 sink,
    GstClockTime in_pts, const GstSegment * in_segment,
    GstClockTime out_pts, const GstSegment * out_segment);
/* "from":[...],"unmatched":N, for an appsrc's levels entry (each member
 * followed by a comma), or NULL before any buffer. */
gchar *pm_model_pairs_json (PmModel * model, guint64 appsrc);
/* The last buffer out of an appsrc or into an appsink: its PTS and
 * segment (NULL where none). */
void pm_model_edge_buffer (PmModel * model, gpointer element,
    GstClockTime pts, const GstSegment * segment);
/* "key":{"pts":…,"segment":…}, for the element's levels entry, or NULL
 * before any buffer. */
gchar *pm_model_edge_json (PmModel * model, guint64 id, const gchar * key);
GstElement *pm_model_get_element (PmModel * model, guint64 id);

/* Where data stopped, and pipelines not reaching their state. */
typedef struct
{
  guint64 pad;
  guint64 element;
  /* The pad's path, and its element's. */
  gchar *path, *element_path;
  GstFlowReturn result;
  /* On a sink pad: a pull, not a push. */
  gboolean pulled;
  guint64 count;
  gint64 first, last;
} PmFlowRecord;

typedef struct
{
  guint64 element;
  gchar *path;
  GstState state, pending;
  gint64 since_us;
} PmPending;

void pm_model_record_flow (PmModel * model, gpointer pad, GstFlowReturn res);
GPtrArray *pm_model_flow_records (PmModel * model);
void pm_flow_record_free (PmFlowRecord * record);
GPtrArray *pm_model_pending (PmModel * model, gint64 min_us);
void pm_pending_free (PmPending * pending);

/* A place in the graph (element, and pad if any) with a detail. */
typedef struct
{
  guint64 element;
  guint64 pad;
  /* The pad's path if there is one, else the element's; the element's. */
  gchar *path, *element_path;
  gchar *detail;
} PmSpot;

void pm_spot_free (PmSpot * spot);
GPtrArray *pm_model_starved_sinks (PmModel * model, guint64 pipeline);
GPtrArray *pm_model_silent_sources (PmModel * model, gint64 min_us);
void pm_model_record_missing (PmModel * model, gpointer element,
    const gchar * what);
GPtrArray *pm_model_missing_plugins (PmModel * model);

typedef struct
{
  guint64 element, src, sink;
  gchar *src_path, *sink_path;
  /* The path of the element, or NULL if it is not known. */
  gchar *element_path;
  gchar *result;
  gchar *src_caps, *sink_caps;
} PmLinkFailure;

void pm_model_record_failed_link (PmModel * model, gpointer src,
    gpointer sink, const gchar * src_name, const gchar * sink_name,
    const gchar * result, const gchar * src_caps, const gchar * sink_caps);
GPtrArray *pm_model_failed_links (PmModel * model);
void pm_link_failure_free (PmLinkFailure * f);

/* A push in progress on a pad: start in ms since the tracer started, plus
 * one (0 = none). Written by the pad's streaming thread only. moved: data
 * has been pushed or pulled through the pad. thread: the model's id of the
 * thread that pushes out of it (src) or pulls through it (sink), 0 if none
 * yet; other_thread: a second one also pushing (a funnel's output), 0 if
 * none, until one of the two ends. */
typedef struct
{
  gint start_ms;
  gint moved;
  gint thread;
  gint other_thread;
} PmPushTiming;

typedef struct
{
  guint64 pad;
  guint64 element;
  gchar *path, *element_path;
  gchar *peer_path;
  gchar *peer_state;
  gint ms;
} PmBlocked;

PmPushTiming *pm_push_timing (GstPad * pad);

/* Threads that moved data or ran a task, numbered from 1 by the model.
 * pm_model_current_thread is called on every push: the calling thread's id,
 * known after its first call. */
gint pm_model_current_thread (PmModel * model);
/* The calling thread runs a GStreamer task (its name, "element:pad"). */
void pm_model_thread_task (PmModel * model, const gchar * name);
/* The calling thread's task has ended. */
void pm_model_thread_ended (PmModel * model);
/* The thread with this OS id, if the model knows it: its id, its name (to
 * free) and whether it runs a task. */
gboolean pm_model_thread_info (PmModel * model, guint64 os_id, gint * id,
    gchar ** name, gboolean * task);
/* Threads not in alive (PmThreadSample, sampled at sampled_us) have
 * ended: taken out of pads where another thread goes on, and forgotten
 * once no pad shows them. */
void pm_model_forget_threads (PmModel * model, GArray * alive,
    gint64 sampled_us);
/* How many threads the model remembers. */
guint pm_model_thread_count (PmModel * model);
/* The elements thread runs: those whose pads it pushes out of or pulls
 * through, and those it pushes into; oldest first, each with a new
 * reference. */
GPtrArray *pm_model_thread_elements (PmModel * model, gint thread);
/* An element taken out of a pipeline and not freed since. */
typedef struct
{
  guint64 id;
  gchar *name;
  /* The path it had, and the pipeline it left. */
  gchar *left_path;
  guint64 pipeline;
  gchar *pipeline_path;
  /* When it was taken out (monotonic µs), and the references held now. */
  gint64 removed_at;
  gint refs;
} PmLeftover;

/* Elements out of their pipeline since before older_than (monotonic µs)
 * and still alive, oldest first. */
GPtrArray *pm_model_leftovers (PmModel * model, gint64 older_than);
void pm_leftover_free (PmLeftover * left);
/* thread (from pm_model_current_thread) pushed through t's pad. */
void pm_model_note_pad_thread (PmModel * model, PmPushTiming * t,
    gint thread);
GPtrArray *pm_model_blocked_pushes (PmModel * model, gint now_ms,
    gint min_ms);
void pm_blocked_free (PmBlocked * b);
gchar *pm_model_pending_elements (PmModel * model, guint64 pipeline);

/* Preroll of a sink or a bin with sinks, from its own ASYNC_START and
 * ASYNC_DONE messages and state change results. */
typedef enum
{
  PM_PREROLL_UNKNOWN,
  PM_PREROLL_WAITING,
  PM_PREROLL_DONE,
  PM_PREROLL_DONE_EOS,
  PM_PREROLL_LIVE,
  PM_PREROLL_NO_WAIT,
} PmPreroll;

void pm_model_set_preroll (PmModel * model, gpointer element,
    PmPreroll preroll);
gchar *pm_model_preroll_summary (PmModel * model, guint64 pipeline);
void pm_model_set_ts_offset (gint64 offset);
gint pm_model_now_ms (void);
gchar *pm_model_element_path (PmModel * model, gpointer element);
gchar *pm_model_path (PmModel * model, guint64 id);

typedef enum
{
  PM_LOG_ERROR,
  PM_LOG_WARNING,
} PmLogLevel;

void pm_model_set_log_enabled (PmModel * model, gboolean enabled);
void pm_model_add_log (PmModel * model, gpointer element, PmLogLevel level,
    const gchar * source, const gchar * text, const gchar * debug,
    const gchar * domain, gint code);
gchar *pm_model_log_to_json (PmModel * model, guint64 since, guint64 * seq);

G_END_DECLS

#endif /* __PM_MODEL_H__ */
