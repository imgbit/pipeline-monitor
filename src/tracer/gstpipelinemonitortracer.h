/* SPDX-License-Identifier: MIT */
#ifndef __GST_PIPELINE_MONITOR_TRACER_H__
#define __GST_PIPELINE_MONITOR_TRACER_H__

#include <gst/gst.h>
#include <gst/gsttracer.h>

G_BEGIN_DECLS

#define GST_TYPE_PIPELINE_MONITOR_TRACER (gst_pipeline_monitor_tracer_get_type ())
G_DECLARE_FINAL_TYPE (GstPipelineMonitorTracer, gst_pipeline_monitor_tracer,
    GST, PIPELINE_MONITOR_TRACER, GstTracer)

G_END_DECLS

#endif /* __GST_PIPELINE_MONITOR_TRACER_H__ */
