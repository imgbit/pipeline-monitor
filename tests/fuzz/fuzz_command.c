/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */

/* Feeds any bytes to the server as a page's command, on the server's own
 * thread as in real use. Nothing may crash or corrupt memory (ASan,
 * UBSan). The page's connection is real but goes nowhere: replies to it
 * are dropped. The server is built in, to reach its command handler. */

#include "pmserver.c"

#include <stdint.h>

static PmServer *server;
static SoupWebsocketConnection *conn;

typedef struct
{
  GBytes *bytes;
  gboolean done;
  GMutex lock;
  GCond cond;
} Call;

static gboolean
make_conn (gpointer data)
{
  Call *call = data;
  GInputStream *in = g_memory_input_stream_new ();
  GOutputStream *out = g_memory_output_stream_new_resizable ();
  GIOStream *stream = g_simple_io_stream_new (in, out);
  GUri *uri = g_uri_parse ("ws://127.0.0.1/ws", G_URI_FLAGS_NONE, NULL);

  conn = soup_websocket_connection_new (stream, uri,
      SOUP_WEBSOCKET_CONNECTION_SERVER, NULL, NULL, NULL);
  g_uri_unref (uri);
  g_object_unref (stream);
  g_object_unref (in);
  g_object_unref (out);
  g_mutex_lock (&call->lock);
  call->done = TRUE;
  g_cond_signal (&call->cond);
  g_mutex_unlock (&call->lock);
  return G_SOURCE_REMOVE;
}

static gboolean
deliver (gpointer data)
{
  Call *call = data;

  on_ws_message (conn, SOUP_WEBSOCKET_DATA_TEXT, call->bytes, server);
  g_mutex_lock (&call->lock);
  call->done = TRUE;
  g_cond_signal (&call->cond);
  g_mutex_unlock (&call->lock);
  return G_SOURCE_REMOVE;
}

/* Runs func on the server's thread and waits for it. */
static void
on_server (GSourceFunc func, GBytes * bytes)
{
  Call call = { 0, };

  call.bytes = bytes;
  g_mutex_init (&call.lock);
  g_cond_init (&call.cond);
  g_main_context_invoke (server->context, func, &call);
  g_mutex_lock (&call.lock);
  while (!call.done)
    g_cond_wait (&call.cond, &call.lock);
  g_mutex_unlock (&call.lock);
  g_mutex_clear (&call.lock);
  g_cond_clear (&call.cond);
}

int
LLVMFuzzerInitialize (int *argc, char ***argv)
{
  gchar *dir = g_dir_make_tmp ("pm-fuzz-XXXXXX", NULL);
  PmRecordOptions record = { 0, };

  /* A GLib warning or critical in the program is a failure too. */
  g_log_set_always_fatal (G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL |
      G_LOG_LEVEL_WARNING);
  gst_init (NULL, NULL);
  server = pm_server_new (pm_model_new (), 0, NULL, NULL);
  g_assert_nonnull (server);
  /* Recording commands write into a folder of their own. */
  record.dir = dir;
  record.file_max = 1 << 20;
  record.total_max = 1 << 20;
  pm_server_set_recordings (server, &record);
  on_server (make_conn, NULL);
  return 0;
}

int
LLVMFuzzerTestOneInput (const uint8_t * data, size_t size)
{
  GBytes *bytes = g_bytes_new (data, size);

  on_server (deliver, bytes);
  g_bytes_unref (bytes);
  return 0;
}
