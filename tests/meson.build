test_env = environment()
test_env.set('GST_PLUGIN_PATH', plugin_build_dir)
test_env.set('GST_REGISTRY', meson.current_build_dir() / 'registry.bin')
test_env.set('GST_TRACERS', 'pipeline-monitor')

test_load = executable('test_load', 'test_load.c',
  c_args : ['-DGST_USE_UNSTABLE_API'],
  dependencies : [gst_dep])

test('load', test_load,
  env : test_env,
  depends : [gstpipelinemonitor],
  timeout : 120)
