test_env = environment()
test_env.set('GST_PLUGIN_PATH', plugin_build_dir)
test_env.set('GST_REGISTRY', meson.current_build_dir() / 'registry.bin')
# Recordings go to the build folder, not the user's. In GST_TRACERS a
# backslash escapes, so Windows paths use "/".
test_env.set('GST_TRACERS', 'pipeline-monitor(port=0,recordings="@0@")'.format(
  (meson.current_build_dir() / 'recordings').replace('\\', '/')))
# The tests only connect to 127.0.0.1: keep the system's proxy settings
# (read through libproxy, which fails on some macOS exception lists) out.
test_env.set('GIO_USE_PROXY_RESOLVER', 'dummy')

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
  'test_core.c', '../src/core/pmjson.c', '../src/core/pmmodel.c', '../src/core/pmmonitor.c',
  '../src/core/pmlevels.c', '../src/core/pmprops.c', '../src/core/pmlatency.c',
  '../src/core/pmfindings.c',
  c_args : core_args,
  include_directories : include_directories('../src/core'),
  dependencies : [gst_dep, json_glib_dep])

test('core', test_core, timeout : 60)

test_server = executable('test_server',
  'test_server.c', '../src/core/pmjson.c', '../src/core/pmmodel.c', '../src/core/pmmonitor.c',
  '../src/core/pmlevels.c', '../src/core/pmprops.c', '../src/core/pmlatency.c',
  '../src/core/pmfindings.c',
  '../src/server/pmrecorder.c',
  '../src/server/pmserver.c',
  ui_resources,
  c_args : ['-DGST_USE_UNSTABLE_API',
    '-DVERSION="@0@"'.format(meson.project_version())],
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
    args : [files('test_ui.py'), serve, '--tool', pipeline_monitor_tool],
    env : test_env,
    depends : [gstpipelinemonitor, serve, pipeline_monitor_tool],
    timeout : 120)
endif

# Fuzz targets (tests/fuzz). With -Dfuzz=true they are libFuzzer programs
# (clang); otherwise a small main runs each seed input once, as a test.
fuzz_args = get_option('fuzz') ? ['-fsanitize=fuzzer'] : []
fuzz_main = get_option('fuzz') ? [] : ['fuzz/standalone.c']

fuzz_command = executable('fuzz_command',
  'fuzz/fuzz_command.c', fuzz_main,
  '../src/core/pmjson.c', '../src/core/pmmodel.c', '../src/core/pmmonitor.c',
  '../src/core/pmlevels.c', '../src/core/pmprops.c', '../src/core/pmlatency.c',
  '../src/core/pmfindings.c', '../src/server/pmrecorder.c',
  ui_resources,
  c_args : ['-DGST_USE_UNSTABLE_API',
    '-DVERSION="@0@"'.format(meson.project_version())] + fuzz_args,
  link_args : fuzz_args,
  include_directories : include_directories('../src/core', '../src/server'),
  dependencies : [gst_dep, soup_dep, json_glib_dep])

fuzz_recording = executable('fuzz_recording',
  'fuzz/fuzz_recording.c', fuzz_main,
  '../src/core/pmjson.c', '../src/server/pmrecorder.c',
  c_args : fuzz_args,
  link_args : fuzz_args,
  include_directories : include_directories('../src/core', '../src/server'),
  dependencies : [gst_dep, json_glib_dep])

# The seeds once ("-runs=0" makes a libFuzzer build do the same).
test('fuzz-command', fuzz_command,
  args : ['-runs=0', meson.current_source_dir() / 'fuzz/corpus/command'],
  env : test_env, timeout : 60)
test('fuzz-recording', fuzz_recording,
  args : ['-runs=0', meson.current_source_dir() / 'fuzz/corpus/recording'],
  env : test_env, timeout : 60)
