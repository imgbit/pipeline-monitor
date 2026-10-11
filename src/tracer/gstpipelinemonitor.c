/* SPDX-License-Identifier: MIT */
#include "gstpipelinemonitortracer.h"

static gboolean
plugin_init (GstPlugin * plugin)
{
  return gst_tracer_register (plugin, "pipeline-monitor",
      GST_TYPE_PIPELINE_MONITOR_TRACER);
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR, GST_VERSION_MINOR, pipelinemonitor,
    "Pipeline monitor tracer", plugin_init, VERSION, "MIT/X11", PACKAGE,
    PACKAGE_ORIGIN)
