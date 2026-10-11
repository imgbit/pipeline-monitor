plugin_c_args = [
  '-DPACKAGE="pipeline-monitor"',
  '-DVERSION="@0@"'.format(meson.project_version()),
  '-DPACKAGE_ORIGIN="Unknown package origin"',
  '-DGST_USE_UNSTABLE_API',
]

plugin_sources = files(
  'tracer/gstpipelinemonitor.c',
  'tracer/gstpipelinemonitortracer.c',
)

gstpipelinemonitor = shared_module('gstpipelinemonitor',
  plugin_sources,
  c_args : plugin_c_args,
  dependencies : [gst_dep],
  install : true,
  install_dir : plugins_install_dir)

plugin_build_dir = meson.current_build_dir()
