/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "gstpipelinemonitortracer.h"

static gboolean
plugin_init (GstPlugin * plugin)
{
  return gst_tracer_register (plugin, "pipeline-monitor",
      GST_TYPE_PIPELINE_MONITOR_TRACER);
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR, GST_VERSION_MINOR, pipelinemonitor,
    "Pipeline monitor tracer", plugin_init, VERSION, "LGPL", PACKAGE,
    PACKAGE_ORIGIN)
