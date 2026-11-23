/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <gst/gst.h>
#include <gst/video/video.h>
#include <string.h>

#include "pmvideo.h"

static void
on_handoff (GstElement * sink, GstBuffer * buffer, GstPad * pad,
    GstBuffer ** out)
{
  if (*out == NULL)
    *out = gst_buffer_ref (buffer);
}

/* One frame of videotestsrc in CAPS, and the caps it had. */
static GstBuffer *
frame_of (const gchar * pattern, const gchar * caps_text, GstCaps ** caps)
{
  gchar *desc = g_strdup_printf ("videotestsrc num-buffers=1 pattern=%s ! "
      "%s ! fakesink name=sink signal-handoffs=true", pattern, caps_text);
  GstElement *pipeline = gst_parse_launch (desc, NULL);
  GstElement *sink = gst_bin_get_by_name (GST_BIN (pipeline), "sink");
  GstPad *pad = gst_element_get_static_pad (sink, "sink");
  GstBuffer *buffer = NULL;
  GstMessage *msg;

  g_signal_connect (sink, "handoff", G_CALLBACK (on_handoff), &buffer);
  gst_element_set_state (pipeline, GST_STATE_PLAYING);
  msg = gst_bus_timed_pop_filtered (GST_ELEMENT_BUS (pipeline),
      5 * GST_SECOND, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
  g_assert_nonnull (msg);
  g_assert_cmpint (GST_MESSAGE_TYPE (msg), ==, GST_MESSAGE_EOS);
  gst_message_unref (msg);
  *caps = gst_pad_get_current_caps (pad);
  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (pad);
  gst_object_unref (sink);
  gst_object_unref (pipeline);
  g_free (desc);
  g_assert_nonnull (buffer);
  return buffer;
}

static PmPicture *
picture_of (const gchar * pattern, const gchar * caps_text,
    GstClockTime now)
{
  GstCaps *caps;
  GstBuffer *buffer = frame_of (pattern, caps_text, &caps);
  gchar *reason;
  PmVideoThumb *thumb = pm_video_thumb_new (caps, &reason);
  PmPicture *p;

  g_assert_null (reason);
  g_assert_nonnull (thumb);
  p = pm_video_thumb_take (thumb, buffer, now);
  g_assert_nonnull (p);
  /* The frame is not kept. */
  g_assert_cmpint (GST_MINI_OBJECT_REFCOUNT_VALUE (buffer), ==, 1);
  pm_video_thumb_free (thumb);
  gst_buffer_unref (buffer);
  gst_caps_unref (caps);
  return p;
}

static void
test_formats (void)
{
  static const gchar *formats[] = {
    "I420", "YV12", "NV12", "NV21", "YUY2", "UYVY", "Y444", "AYUV",
    "RGB", "BGR", "RGBx", "BGRx", "RGBA", "ARGB", "GRAY8", "GRAY16_LE",
    "I420_10LE", "P010_10LE", "v210", "Y42B",
  };

  for (guint i = 0; i < G_N_ELEMENTS (formats); i++) {
    gchar *caps = g_strdup_printf ("video/x-raw,format=%s,width=320,"
        "height=240", formats[i]);
    PmPicture *white = picture_of ("white", caps, 0);
    PmPicture *black = picture_of ("black", caps, 0);
    PmPicture *bars = picture_of ("smpte", caps, 0);

    g_test_message ("%s", formats[i]);
    g_assert_cmpint (white->width, ==, 160);
    g_assert_cmpint (white->height, ==, 120);
    /* videotestsrc writes white as 235 and black as 16 in the full-range
     * gray formats. */
    g_assert_cmpfloat (white->brightness_avg, >, 0.9);
    g_assert_false (white->black);
    g_assert_cmpfloat (black->brightness_max, <, 0.07);
    g_assert_true (black->black);
    g_assert_cmpfloat (bars->brightness_avg, >, 0.2);
    g_assert_cmpfloat (bars->brightness_avg, <, 0.8);
    pm_picture_free (white);
    pm_picture_free (black);
    pm_picture_free (bars);
    g_free (caps);
  }
}

static void
test_colours (void)
{
  PmPicture *red = picture_of ("red",
      "video/x-raw,format=I420,width=64,height=64", 0);
  guint8 *px = red->rgb + (32 * red->width + 32) * 3;

  g_assert_cmpint (red->width, ==, 160);
  g_assert_cmpint (px[0], >, 220);
  g_assert_cmpint (px[1], <, 40);
  g_assert_cmpint (px[2], <, 40);
  pm_picture_free (red);
}

static void
check_size (const gchar * caps, gint width, gint height)
{
  PmPicture *p = picture_of ("black", caps, 0);

  g_test_message ("%s", caps);
  g_assert_cmpint (p->width, ==, width);
  g_assert_cmpint (p->height, ==, height);
  pm_picture_free (p);
}

static void
test_sizes (void)
{
  check_size ("video/x-raw,format=I420,width=1920,height=1080", 160, 90);
  check_size ("video/x-raw,format=I420,width=1080,height=1920", 90, 160);
  check_size ("video/x-raw,format=I420,width=3840,height=2160", 160, 90);
  /* PAL 4:3 with its pixel aspect ratio. */
  check_size ("video/x-raw,format=I420,width=720,height=576,"
      "pixel-aspect-ratio=16/15", 160, 120);
  check_size ("video/x-raw,format=I420,width=16,height=16", 160, 160);
}

static void
test_unchanged (void)
{
  GstCaps *caps;
  GstBuffer *ball = frame_of ("ball",
      "video/x-raw,format=I420,width=320,height=240", &caps);
  GstCaps *caps2;
  GstBuffer *bars = frame_of ("smpte",
      "video/x-raw,format=I420,width=320,height=240", &caps2);
  gchar *reason;
  PmVideoThumb *thumb = pm_video_thumb_new (caps, &reason);
  PmPicture *p;

  p = pm_video_thumb_take (thumb, ball, 1 * GST_SECOND);
  g_assert_cmpuint (p->unchanged_since, ==, 1 * GST_SECOND);
  pm_picture_free (p);
  p = pm_video_thumb_take (thumb, ball, 2 * GST_SECOND);
  g_assert_cmpuint (p->unchanged_since, ==, 1 * GST_SECOND);
  pm_picture_free (p);
  p = pm_video_thumb_take (thumb, bars, 3 * GST_SECOND);
  g_assert_cmpuint (p->unchanged_since, ==, 3 * GST_SECOND);
  pm_picture_free (p);

  pm_video_thumb_free (thumb);
  gst_buffer_unref (ball);
  gst_buffer_unref (bars);
  gst_caps_unref (caps);
  gst_caps_unref (caps2);
}

static void
check_reason (const gchar * caps_text, const gchar * expected)
{
  GstCaps *caps = gst_caps_from_string (caps_text);
  gchar *reason;

  g_assert_null (pm_video_thumb_new (caps, &reason));
  if (expected == NULL)
    g_assert_null (reason);
  else
    g_assert_cmpstr (reason, ==, expected);
  g_free (reason);
  gst_caps_unref (caps);
}

static void
test_reasons (void)
{
  check_reason ("video/x-h264, stream-format=byte-stream",
      "compressed (video/x-h264): no picture without decoding");
  check_reason ("image/jpeg", "compressed (image/jpeg): no picture without "
      "decoding");
  check_reason ("video/x-raw(memory:GLMemory), format=RGBA, width=320, "
      "height=240, framerate=30/1",
      "frames in GPU or device memory (memory:GLMemory): no picture");
  check_reason ("video/x-raw, format=I420", "video caps not complete yet");
  check_reason ("audio/x-raw, format=S16LE, rate=48000, channels=2", NULL);
}

int
main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/video/formats", test_formats);
  g_test_add_func ("/video/colours", test_colours);
  g_test_add_func ("/video/sizes", test_sizes);
  g_test_add_func ("/video/unchanged", test_unchanged);
  g_test_add_func ("/video/reasons", test_reasons);
  return g_test_run ();
}
