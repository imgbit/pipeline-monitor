project('pipeline-monitor', 'c',
  version : '0.1.0',
  license : 'MIT',
  meson_version : '>= 0.63',
  default_options : ['c_std=gnu11', 'warning_level=2', 'buildtype=debugoptimized'])

cc = meson.get_compiler('c')
add_project_arguments(cc.get_supported_arguments(['-Wno-unused-parameter']),
  language : 'c')

gst_req = '>= 1.22'
gst_dep = dependency('gstreamer-1.0', version : gst_req)

plugins_install_dir = get_option('libdir') / 'gstreamer-1.0'

subdir('src')
subdir('tests')
