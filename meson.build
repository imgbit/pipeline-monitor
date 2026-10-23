project('pipeline-monitor', 'c',
  version : '0.6.0',
  license : 'LGPL-2.1-or-later',
  meson_version : '>= 0.63',
  default_options : ['c_std=gnu11', 'warning_level=2', 'buildtype=debugoptimized'])

cc = meson.get_compiler('c')
add_project_arguments(cc.get_supported_arguments(['-Wno-unused-parameter']),
  language : 'c')

gst_req = '>= 1.22'
gst_dep = dependency('gstreamer-1.0', version : gst_req)
soup_dep = dependency('libsoup-3.0', version : '>= 3.0')
json_glib_dep = dependency('json-glib-1.0')

gnome = import('gnome')

plugins_install_dir = get_option('libdir') / 'gstreamer-1.0'

subdir('src')
subdir('examples')
subdir('tests')
