/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "gstpipelinemonitortracer.h"
#include "pmmodel.h"
#include "pmserver.h"

#define DEFAULT_PORT 8765

GST_DEBUG_CATEGORY_STATIC (gst_pipeline_monitor_debug);
#define GST_CAT_DEFAULT gst_pipeline_monitor_debug

struct _GstPipelineMonitorTracer
{
  GstTracer parent;

  PmModel *model;
  PmServer *server;
};

enum
{
  PROP_0,
  PROP_GRAPH,
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

static void
do_element_new (GstTracer * tracer, GstClockTime ts, GstElement * element)
{
  GST_DEBUG_OBJECT (tracer, "new element %s (%s)", GST_OBJECT_NAME (element),
      G_OBJECT_TYPE_NAME (element));
  sync_element (GST_PIPELINE_MONITOR_TRACER (tracer), element);
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
  g_free (name);
}

static void
do_element_remove_pad (GstTracer * tracer, GstClockTime ts,
    GstElement * element, GstPad * pad)
{
  pm_model_remove_pad (GST_PIPELINE_MONITOR_TRACER (tracer)->model, pad);
}

static void
do_pad_link_post (GstTracer * tracer, GstClockTime ts, GstPad * src,
    GstPad * sink, GstPadLinkReturn result)
{
  if (result != GST_PAD_LINK_OK)
    return;
  pm_model_link (GST_PIPELINE_MONITOR_TRACER (tracer)->model,
      resolve_pad (src), resolve_pad (sink));
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
do_element_post_message_pre (GstTracer * tracer, GstClockTime ts,
    GstElement * element, GstMessage * message)
{
  if (GST_MESSAGE_TYPE (message) == GST_MESSAGE_STATE_CHANGED)
    sync_state (GST_PIPELINE_MONITOR_TRACER (tracer), element);
}

static void
do_element_change_state_post (GstTracer * tracer, GstClockTime ts,
    GstElement * element, GstStateChange transition,
    GstStateChangeReturn result)
{
  GstPipelineMonitorTracer *self = GST_PIPELINE_MONITOR_TRACER (tracer);
  GstState next = GST_STATE_TRANSITION_NEXT (transition);

  /* On success GStreamer completes the change right after this hook, and
   * posts no message when the state does not change (PLAYING to PLAYING).
   * Record the outcome now so the pending state does not go stale. */
  if ((result == GST_STATE_CHANGE_SUCCESS ||
          result == GST_STATE_CHANGE_NO_PREROLL) &&
      GST_STATE_PENDING (element) == next) {
    sync_element (self, element);
    pm_model_set_state (self->model, element, next, GST_STATE_VOID_PENDING);
    return;
  }
  sync_state (self, element);
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
    case PROP_GRAPH:
      g_value_take_string (value, pm_model_to_json (self->model));
      break;
    case PROP_URL:
      g_value_set_string (value,
          self->server ? pm_server_get_url (self->server) : NULL);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

/* Options come from GST_TRACERS="pipeline-monitor(port=9000,ui-dir=web)". */
static guint
parse_options (GstPipelineMonitorTracer * self, gchar ** ui_dir)
{
  gchar *params = NULL, *desc;
  GstStructure *s;
  gint port = DEFAULT_PORT;

  *ui_dir = NULL;
  g_object_get (self, "params", &params, NULL);
  if (params == NULL)
    return DEFAULT_PORT;

  desc = g_strdup_printf ("params,%s", params);
  s = gst_structure_from_string (desc, NULL);
  if (s == NULL) {
    GST_WARNING_OBJECT (self, "cannot parse options '%s'", params);
  } else {
    if (gst_structure_has_field (s, "port") &&
        (!gst_structure_get_int (s, "port", &port) || port < 0 ||
            port > G_MAXUINT16)) {
      GST_WARNING_OBJECT (self, "invalid port, using %d", DEFAULT_PORT);
      port = DEFAULT_PORT;
    }
    *ui_dir = g_strdup (gst_structure_get_string (s, "ui-dir"));
    gst_structure_free (s);
  }
  g_free (desc);
  g_free (params);
  return port;
}

static void
gst_pipeline_monitor_tracer_constructed (GObject * object)
{
  GstPipelineMonitorTracer *self = GST_PIPELINE_MONITOR_TRACER (object);
  GError *error = NULL;
  gchar *ui_dir;
  guint port;

  G_OBJECT_CLASS (gst_pipeline_monitor_tracer_parent_class)->constructed
      (object);

  port = parse_options (self, &ui_dir);
  self->server = pm_server_new (self->model, port, ui_dir, &error);
  g_free (ui_dir);
  if (self->server == NULL) {
    g_printerr ("pipeline-monitor: cannot start server: %s\n", error->message);
    g_error_free (error);
    return;
  }
  pm_model_set_changed_func (self->model,
      (PmModelChangedFunc) pm_server_notify, self->server);
  g_printerr ("pipeline-monitor: %s\n", pm_server_get_url (self->server));
}

static void
gst_pipeline_monitor_tracer_finalize (GObject * object)
{
  GstPipelineMonitorTracer *self = GST_PIPELINE_MONITOR_TRACER (object);

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

  g_object_class_install_property (gobject_class, PROP_GRAPH,
      g_param_spec_string ("graph", "Graph",
          "Snapshot of all pipelines in the process as JSON", NULL,
          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
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
  gst_tracing_register_hook (tracer, "element-post-message-pre",
      G_CALLBACK (do_element_post_message_pre));
  gst_tracing_register_hook (tracer, "object-destroyed",
      G_CALLBACK (do_object_destroyed));
}
