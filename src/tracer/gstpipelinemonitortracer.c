/* SPDX-License-Identifier: MIT */
#include "gstpipelinemonitortracer.h"
#include "pmmodel.h"

GST_DEBUG_CATEGORY_STATIC (gst_pipeline_monitor_debug);
#define GST_CAT_DEFAULT gst_pipeline_monitor_debug

struct _GstPipelineMonitorTracer
{
  GstTracer parent;

  PmModel *model;
};

enum
{
  PROP_0,
  PROP_GRAPH,
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
  sync_state (GST_PIPELINE_MONITOR_TRACER (tracer), element);
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
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_pipeline_monitor_tracer_finalize (GObject * object)
{
  GstPipelineMonitorTracer *self = GST_PIPELINE_MONITOR_TRACER (object);

  pm_model_free (self->model);

  G_OBJECT_CLASS (gst_pipeline_monitor_tracer_parent_class)->finalize (object);
}

static void
gst_pipeline_monitor_tracer_class_init (GstPipelineMonitorTracerClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->get_property = gst_pipeline_monitor_tracer_get_property;
  gobject_class->finalize = gst_pipeline_monitor_tracer_finalize;

  g_object_class_install_property (gobject_class, PROP_GRAPH,
      g_param_spec_string ("graph", "Graph",
          "Snapshot of all pipelines in the process as JSON", NULL,
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
