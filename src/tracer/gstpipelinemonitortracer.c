/* SPDX-License-Identifier: MIT */
#include "gstpipelinemonitortracer.h"

GST_DEBUG_CATEGORY_STATIC (gst_pipeline_monitor_debug);
#define GST_CAT_DEFAULT gst_pipeline_monitor_debug

struct _GstPipelineMonitorTracer
{
  GstTracer parent;
};

#define _do_init \
    GST_DEBUG_CATEGORY_INIT (gst_pipeline_monitor_debug, "pipeline-monitor", 0, \
        "pipeline monitor tracer");

G_DEFINE_TYPE_WITH_CODE (GstPipelineMonitorTracer, gst_pipeline_monitor_tracer,
    GST_TYPE_TRACER, _do_init);

static void
do_element_new (GstTracer * tracer, GstClockTime ts, GstElement * element)
{
  GST_DEBUG_OBJECT (tracer, "new element %s (%s)", GST_OBJECT_NAME (element),
      G_OBJECT_TYPE_NAME (element));
}

static void
gst_pipeline_monitor_tracer_class_init (GstPipelineMonitorTracerClass * klass)
{
}

static void
gst_pipeline_monitor_tracer_init (GstPipelineMonitorTracer * self)
{
  gst_tracing_register_hook (GST_TRACER (self), "element-new",
      G_CALLBACK (do_element_new));
}
