/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */

/* The pipeline-monitor command: opens recordings in the browser, without
 * a program. The tracer (GST_TRACERS=pipeline-monitor) watches programs;
 * this command only shows what was recorded. */

#include "pmmodel.h"
#include "pmrecorder.h"
#include "pmserver.h"

#include <gio/gio.h>
#include <gst/gst.h>
#ifdef G_OS_UNIX
#include <glib-unix.h>
#endif

static const gchar *usage =
    "[open [FILE]]\n\n"
    "Shows pipeline-monitor recordings in the browser, without a program:\n"
    "  pipeline-monitor              the recordings in the folder\n"
    "  pipeline-monitor open FILE    that recording, opened\n\n"
    "To watch a running program, use the tracer instead:\n"
    "  GST_TRACERS=pipeline-monitor your-program";

static gboolean
quit (gpointer loop)
{
  g_main_loop_quit (loop);
  return G_SOURCE_REMOVE;
}

int
main (int argc, char **argv)
{
  gchar *recordings = NULL, *file = NULL, *url;
  /* Next to the tracer's 8765; the next free one if taken. */
  gint port = 8766;
  gboolean version = FALSE, no_browser = FALSE;
  GOptionEntry entries[] = {
    {"recordings", 0, 0, G_OPTION_ARG_FILENAME, &recordings,
        "The recordings folder (default: the tracer's)", "DIR"},
    {"port", 0, 0, G_OPTION_ARG_INT, &port,
        "Port to serve on (default 8766, or the next free one)", "PORT"},
    {"no-browser", 0, 0, G_OPTION_ARG_NONE, &no_browser,
        "Only print the address", NULL},
    {"version", 0, 0, G_OPTION_ARG_NONE, &version, "Show the version", NULL},
    {NULL, 0, 0, 0, NULL, NULL, NULL}
  };
  GOptionContext *context = g_option_context_new (usage);
  PmRecordOptions record = { 0, };
  GError *error = NULL;
  PmServer *server;
  PmModel *model;
  GMainLoop *loop;

  g_option_context_add_main_entries (context, entries, NULL);
  if (!g_option_context_parse (context, &argc, &argv, &error)) {
    g_printerr ("pipeline-monitor: %s\n", error->message);
    return 2;
  }
  g_option_context_free (context);
  if (version) {
    g_print ("pipeline-monitor %s\n", VERSION);
    return 0;
  }
  if (argc > 1 && g_strcmp0 (argv[1], "open") != 0) {
    g_printerr ("pipeline-monitor: unknown command '%s' (see --help)\n",
        argv[1]);
    return 2;
  }
  if (argc > 3) {
    g_printerr ("pipeline-monitor: open takes one file (see --help)\n");
    return 2;
  }
  if (argc == 3) {
    file = g_canonicalize_filename (argv[2], NULL);
    if (!g_file_test (file, G_FILE_TEST_IS_REGULAR)) {
      g_printerr ("pipeline-monitor: cannot open %s\n", argv[2]);
      return 1;
    }
  }

  /* No tracer here: this program has no pipeline to watch. */
  g_unsetenv ("GST_TRACERS");
  gst_init (NULL, NULL);
  model = pm_model_new ();
  server = pm_server_new (model, port, NULL, &error);
  if (server == NULL) {
    g_printerr ("pipeline-monitor: cannot start: %s\n", error->message);
    return 1;
  }
  record.dir = recordings;
  record.file_max = PM_RECORD_FILE_MAX;
  record.total_max = PM_RECORD_TOTAL_MAX;
  pm_server_set_recordings (server, &record);
  pm_server_set_viewer (server, file);

  url = g_strdup_printf ("%s#viewer%s", pm_server_get_url (server),
      file ? "=opened" : "");
  g_print ("pipeline-monitor: %s\nPress Ctrl+C to stop.\n", url);
  if (!no_browser && !g_app_info_launch_default_for_uri (url, NULL, &error)) {
    g_printerr ("pipeline-monitor: cannot start a browser (%s): open the "
        "address yourself\n", error->message);
    g_clear_error (&error);
  }

  loop = g_main_loop_new (NULL, FALSE);
#ifdef G_OS_UNIX
  g_unix_signal_add (SIGINT, quit, loop);
  g_unix_signal_add (SIGTERM, quit, loop);
#endif
  g_main_loop_run (loop);

  g_main_loop_unref (loop);
  pm_server_free (server);
  pm_model_free (model);
  g_free (url);
  g_free (file);
  g_free (recordings);
  gst_deinit ();
  return 0;
}
