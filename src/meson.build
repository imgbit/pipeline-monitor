plugin_c_args = [
  '-DPACKAGE="pipeline-monitor"',
  '-DVERSION="@0@"'.format(meson.project_version()),
  '-DPACKAGE_ORIGIN="https://github.com/imgbit/pipeline-monitor"',
  '-DGST_USE_UNSTABLE_API',
]

plugin_sources = files(
  'core/pmmodel.c',
  'core/pmmonitor.c',
  'core/pmlevels.c',
  'core/pmprops.c',
  'core/pmlatency.c',
  'core/pmfindings.c',
  'server/pmserver.c',
  'tracer/gstpipelinemonitor.c',
  'tracer/gstpipelinemonitortracer.c',
)

ui_resources = gnome.compile_resources('pmui', 'server/pmui.gresource.xml',
  source_dir : meson.project_source_root() / 'web',
  c_name : 'pm_ui')

gstpipelinemonitor = shared_module('gstpipelinemonitor',
  plugin_sources, ui_resources,
  c_args : plugin_c_args,
  include_directories : include_directories('core', 'server'),
  dependencies : [gst_dep, soup_dep, json_glib_dep],
  install : true,
  install_dir : plugins_install_dir)

plugin_build_dir = meson.current_build_dir()
