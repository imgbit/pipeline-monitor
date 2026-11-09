/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */

/* Without libFuzzer: runs a fuzz target once on each file given (or each
 * file in a folder given), so its seed inputs are a normal test. Options
 * starting with "-" (libFuzzer's) are ignored. */

#include <glib.h>
#include <stdint.h>

int LLVMFuzzerInitialize (int *argc, char ***argv);
int LLVMFuzzerTestOneInput (const uint8_t * data, size_t size);

static guint runs;

static void
run_file (const gchar * path)
{
  gchar *data;
  gsize size;

  if (!g_file_get_contents (path, &data, &size, NULL))
    g_error ("cannot read %s", path);
  LLVMFuzzerTestOneInput ((const uint8_t *) data, size);
  g_free (data);
  runs++;
}

int
main (int argc, char **argv)
{
  gint i;

  LLVMFuzzerInitialize (&argc, &argv);
  for (i = 1; i < argc; i++) {
    GDir *dir;
    const gchar *name;

    if (argv[i][0] == '-')
      continue;
    if (!g_file_test (argv[i], G_FILE_TEST_IS_DIR)) {
      run_file (argv[i]);
      continue;
    }
    dir = g_dir_open (argv[i], 0, NULL);
    while ((name = g_dir_read_name (dir)) != NULL) {
      gchar *path = g_build_filename (argv[i], name, NULL);

      run_file (path);
      g_free (path);
    }
    g_dir_close (dir);
  }
  g_print ("%u inputs ran\n", runs);
  return runs > 0 ? 0 : 1;
}
