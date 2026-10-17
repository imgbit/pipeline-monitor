executable('print-graph', 'print-graph.c',
  c_args : ['-DGST_USE_UNSTABLE_API'],
  dependencies : [gst_dep, soup_dep])

serve = executable('serve', 'serve.c',
  dependencies : [gst_dep])
