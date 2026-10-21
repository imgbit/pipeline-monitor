test_env = environment()
test_env.set('GST_PLUGIN_PATH', plugin_build_dir)
test_env.set('GST_REGISTRY', meson.current_build_dir() / 'registry.bin')
test_env.set('GST_TRACERS', 'pipeline-monitor(port=0)')

test_load = executable('test_load', 'test_load.c',
  c_args : ['-DGST_USE_UNSTABLE_API'],
  dependencies : [gst_dep])

test('load', test_load,
  env : test_env,
  depends : [gstpipelinemonitor],
  timeout : 120)

test_model = executable('test_model', 'test_model.c',
  c_args : ['-DGST_USE_UNSTABLE_API'],
  dependencies : [gst_dep, json_glib_dep, soup_dep])

test('model', test_model,
  env : test_env,
  depends : [gstpipelinemonitor],
  timeout : 120)

core_args = []
if get_option('b_sanitize') != 'none'
  core_args += '-DPM_SANITIZED'
endif

test_core = executable('test_core',
  'test_core.c', '../src/core/pmmodel.c', '../src/core/pmmonitor.c',
  c_args : core_args,
  include_directories : include_directories('../src/core'),
  dependencies : [gst_dep, json_glib_dep])

test('core', test_core, timeout : 60)

test_server = executable('test_server',
  'test_server.c', '../src/core/pmmodel.c', '../src/core/pmmonitor.c',
  '../src/server/pmserver.c',
  ui_resources,
  c_args : ['-DGST_USE_UNSTABLE_API'],
  include_directories : include_directories('../src/core', '../src/server'),
  dependencies : [gst_dep, soup_dep, json_glib_dep])

test('server', test_server,
  env : test_env,
  depends : [gstpipelinemonitor],
  timeout : 120)

test_options = executable('test_options', 'test_options.c',
  c_args : ['-DGST_USE_UNSTABLE_API'],
  dependencies : [gst_dep, soup_dep])

test('options', test_options,
  env : test_env,
  depends : [gstpipelinemonitor],
  timeout : 120)

python = find_program('python3', required : false)
if python.found()
  test('ui', python,
    args : [files('test_ui.py'), serve],
    env : test_env,
    depends : [gstpipelinemonitor, serve],
    timeout : 120)
endif
