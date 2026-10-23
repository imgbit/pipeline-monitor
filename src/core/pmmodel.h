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
} PmElementRef;

typedef gboolean (*PmElementFilter) (GstElement * element, gboolean top_level);

void pm_model_count_queue_event (PmModel * model, gpointer element,
    gboolean overrun);
void pm_model_set_buffering (PmModel * model, gpointer element, gint percent);
GPtrArray *pm_model_find_elements (PmModel * model, PmElementFilter filter);
void pm_element_ref_free (PmElementRef * ref);
guint64 pm_model_pad_id (PmModel * model, gpointer pad);
GstElement *pm_model_get_element (PmModel * model, guint64 id);

/* Where data stopped, and pipelines not reaching their state. */
typedef struct
{
  guint64 pad;
  guint64 element;
  gchar *path;
  GstFlowReturn result;
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
  gchar *path;
  gchar *detail;
} PmSpot;

void pm_spot_free (PmSpot * spot);
GPtrArray *pm_model_unqueued_branches (PmModel * model);
GPtrArray *pm_model_unlinked_pads (PmModel * model);
GPtrArray *pm_model_starved_sinks (PmModel * model, guint64 pipeline);
GPtrArray *pm_model_silent_sources (PmModel * model, gint64 min_us);
void pm_model_record_missing (PmModel * model, gpointer element,
    const gchar * what);
GPtrArray *pm_model_missing_plugins (PmModel * model);

typedef struct
{
  guint64 element, src, sink;
  gchar *src_path, *sink_path;
  gchar *result;
  gchar *src_caps, *sink_caps;
} PmLinkFailure;

void pm_model_record_failed_link (PmModel * model, gpointer src,
    gpointer sink, const gchar * src_name, const gchar * sink_name,
    const gchar * result, const gchar * src_caps, const gchar * sink_caps);
GPtrArray *pm_model_failed_links (PmModel * model);
void pm_link_failure_free (PmLinkFailure * f);
gchar *pm_model_element_path (PmModel * model, gpointer element);

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
