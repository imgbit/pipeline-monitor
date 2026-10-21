/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <gst/gst.h>
#include <json-glib/json-glib.h>

#include "pmmodel.h"

/* Model tests without a pipeline: objects are created and destroyed by the
 * test itself, in orders the tracer hooks can't easily produce. */

typedef struct
{
  PmModel *model;
  GstElement *element;
  GstPad *pad;
} Fixture;

static void
fixture_setup (Fixture * f, gconstpointer data)
{
  f->model = pm_model_new ();
  f->element = gst_object_ref_sink (gst_bin_new ("core-pipeline"));
  f->pad = gst_object_ref_sink (gst_pad_new ("core-pad", GST_PAD_SRC));
  pm_model_update_element (f->model, f->element, "core-pipeline", "bin",
      "GstBin", TRUE, TRUE);
  pm_model_add_pad (f->model, f->element, f->pad, "core-pad", GST_PAD_SRC,
      NULL, FALSE);
}

static void
fixture_teardown (Fixture * f, gconstpointer data)
{
  pm_model_free (f->model);
  gst_clear_object (&f->pad);
  gst_object_unref (f->element);
}

/* The id the model gave the pad, read from its JSON like the page does. */
static guint64
pad_id (PmModel * model)
{
  gchar *json = pm_model_to_json (model);
  JsonParser *parser = json_parser_new ();
  JsonObject *element;
  guint64 id;

  g_assert_true (json_parser_load_from_data (parser, json, -1, NULL));
  element = json_array_get_object_element (json_object_get_array_member
      (json_node_get_object (json_parser_get_root (parser)), "pipelines"), 0);
  id = json_object_get_int_member (json_array_get_object_element
      (json_object_get_array_member (element, "pads"), 0), "id");
  g_object_unref (parser);
  g_free (json);
  return id;
}

static void
test_get_pad (Fixture * f, gconstpointer data)
{
  guint64 id = pad_id (f->model);
  GstPad *pad = pm_model_get_pad (f->model, id);

  g_assert_true (pad == f->pad);
  /* The caller gets its own reference. */
  g_assert_cmpint (GST_OBJECT_REFCOUNT_VALUE (f->pad), ==, 2);
  gst_object_unref (pad);

  g_assert_null (pm_model_get_pad (f->model, id + 1000));
}

static void
test_removed_pad (Fixture * f, gconstpointer data)
{
  guint64 id = pad_id (f->model);

  pm_model_remove_pad (f->model, f->pad);
  g_assert_null (pm_model_get_pad (f->model, id));
}

/* The pad is destroyed while the model still lists it, as when it goes
 * away just before the tracer's object-destroyed hook updates the model. */
static void
test_destroyed_pad (Fixture * f, gconstpointer data)
{
  guint64 id = pad_id (f->model);

  gst_clear_object (&f->pad);
  g_assert_null (pm_model_get_pad (f->model, id));
}

static void
test_removed_element (Fixture * f, gconstpointer data)
{
  guint64 id = pad_id (f->model);

  pm_model_remove_object (f->model, f->element);
  g_assert_null (pm_model_get_pad (f->model, id));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  gst_init (&argc, &argv);

  g_test_add ("/core/get-pad", Fixture, NULL, fixture_setup, test_get_pad,
      fixture_teardown);
  g_test_add ("/core/removed-pad", Fixture, NULL, fixture_setup,
      test_removed_pad, fixture_teardown);
  g_test_add ("/core/destroyed-pad", Fixture, NULL, fixture_setup,
      test_destroyed_pad, fixture_teardown);
  g_test_add ("/core/removed-element", Fixture, NULL, fixture_setup,
      test_removed_element, fixture_teardown);

  return g_test_run ();
}
