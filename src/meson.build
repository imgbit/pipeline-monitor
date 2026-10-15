plugin_c_args = [
  '-DPACKAGE="pipeline-monitor"',
  '-DVERSION="@0@"'.format(meson.project_version()),
  '-DPACKAGE_ORIGIN="Unknown package origin"',
  '-DGST_USE_UNSTABLE_API',
]

plugin_sources = files(
  'core/pmmodel.c',
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
  dependencies : [gst_dep, soup_dep],
  install : true,
  install_dir : plugins_install_dir)

plugin_build_dir = meson.current_build_dir()
