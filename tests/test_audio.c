/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <gst/gst.h>
#include <gst/audio/audio.h>
#include <math.h>
#include <string.h>

#include "pmaudio.h"

#define RATE 48000
#define FRAMES 4800

/* A buffer of FORMAT holding, per channel, values given as fractions of
 * full scale; packed by GStreamer's own pack functions. */
static GstBuffer *
make_buffer (GstAudioInfo * info, GstAudioFormat format, guint channels,
    gboolean planar, gdouble (*value) (guint channel, guint frame))
{
  const GstAudioFormatInfo *finfo = gst_audio_format_get_info (format);
  gsize n = (gsize) FRAMES * channels, width;
  GstBuffer *buffer;
  GstMapInfo map;
  guint c, f;

  gst_audio_info_set_format (info, format, RATE, channels, NULL);
  if (planar)
    info->layout = GST_AUDIO_LAYOUT_NON_INTERLEAVED;
  width = GST_AUDIO_FORMAT_INFO_WIDTH (finfo) / 8;
  buffer = gst_buffer_new_allocate (NULL, n * width, NULL);
  gst_buffer_map (buffer, &map, GST_MAP_WRITE);

  for (c = 0; c < channels; c++) {
    gpointer tmp;

    if (GST_AUDIO_FORMAT_INFO_IS_FLOAT (finfo)) {
      gdouble *d = g_new (gdouble, FRAMES);

      for (f = 0; f < FRAMES; f++)
        d[f] = value (c, f);
      tmp = d;
    } else {
      gint32 *d = g_new (gint32, FRAMES);

      for (f = 0; f < FRAMES; f++) {
        gdouble v = CLAMP (value (c, f) * 2147483648.0, G_MININT32,
            G_MAXINT32);

        d[f] = (gint32) v;
      }
      tmp = d;
    }
    if (planar) {
      finfo->pack_func (finfo, 0, tmp, map.data + c * FRAMES * width,
          FRAMES);
    } else {
      guint8 *one = g_malloc (FRAMES * width);

      finfo->pack_func (finfo, 0, tmp, one, FRAMES);
      for (f = 0; f < FRAMES; f++)
        memcpy (map.data + (f * channels + c) * width, one + f * width,
            width);
      g_free (one);
    }
    g_free (tmp);
  }
  gst_buffer_unmap (buffer, &map);
  if (planar)
    gst_buffer_add_audio_meta (buffer, info, FRAMES, NULL);
  return buffer;
}

/* Channel 0: a sine at half scale; channel 1: a quarter; others silent. */
static gdouble
sines (guint channel, guint frame)
{
  gdouble amp = channel == 0 ? 0.5 : channel == 1 ? 0.25 : 0.0;

  return amp * sin (2 * G_PI * 1000.0 * frame / RATE);
}

static void
check_format (GstAudioFormat format, gboolean planar)
{
  GstAudioInfo info;
  GstBuffer *buffer = make_buffer (&info, format, 2, planar, sines);
  PmAudioSum sums[2];
  gdouble tolerance = GST_AUDIO_FORMAT_INFO_DEPTH (info.finfo) <= 8 ?
      0.35 : 0.02;

  g_assert_true (pm_audio_measure (&info, 2, buffer, sums));
  g_assert_cmpuint (sums[0].samples, ==, FRAMES);
  /* A sine's peak is its amplitude; its RMS is 3.01 dB lower. */
  g_assert_cmpfloat_with_epsilon (pm_audio_db (sums[0].peak), -6.02,
      tolerance);
  g_assert_cmpfloat_with_epsilon (pm_audio_db (sqrt (sums[0].sum_squares /
              sums[0].samples)), -9.03, tolerance);
  g_assert_cmpfloat_with_epsilon (pm_audio_db (sums[1].peak), -12.04,
      tolerance);
  g_assert_cmpuint (sums[0].clipped, ==, 0);
  gst_buffer_unref (buffer);
}

static void
test_formats (void)
{
  static const GstAudioFormat formats[] = {
    GST_AUDIO_FORMAT_S8, GST_AUDIO_FORMAT_U8,
    GST_AUDIO_FORMAT_S16LE, GST_AUDIO_FORMAT_S16BE,
    GST_AUDIO_FORMAT_U16LE, GST_AUDIO_FORMAT_U16BE,
    GST_AUDIO_FORMAT_S24_32LE, GST_AUDIO_FORMAT_S24_32BE,
    GST_AUDIO_FORMAT_S24LE, GST_AUDIO_FORMAT_S24BE,
    GST_AUDIO_FORMAT_S20LE, GST_AUDIO_FORMAT_S18LE,
    GST_AUDIO_FORMAT_S32LE, GST_AUDIO_FORMAT_S32BE,
    GST_AUDIO_FORMAT_U32LE, GST_AUDIO_FORMAT_U24_32LE,
    GST_AUDIO_FORMAT_F32LE, GST_AUDIO_FORMAT_F32BE,
    GST_AUDIO_FORMAT_F64LE, GST_AUDIO_FORMAT_F64BE,
  };

  for (guint i = 0; i < G_N_ELEMENTS (formats); i++) {
    g_test_message ("%s", gst_audio_format_to_string (formats[i]));
    check_format (formats[i], FALSE);
    check_format (formats[i], TRUE);
  }
}

static gdouble
full_scale (guint channel, guint frame)
{
  /* Channel 0 holds three samples at the limits. */
  if (channel == 0 && frame < 3)
    return frame == 1 ? -1.0 : 1.0;
  return 0.1;
}

static void
test_clipping (void)
{
  static const GstAudioFormat formats[] = {
    GST_AUDIO_FORMAT_S16, GST_AUDIO_FORMAT_S24LE, GST_AUDIO_FORMAT_S32,
    GST_AUDIO_FORMAT_F32, GST_AUDIO_FORMAT_U8,
  };

  for (guint i = 0; i < G_N_ELEMENTS (formats); i++) {
    GstAudioInfo info;
    GstBuffer *buffer = make_buffer (&info, formats[i], 2, FALSE,
        full_scale);
    PmAudioSum sums[2];

    g_test_message ("%s", gst_audio_format_to_string (formats[i]));
    g_assert_true (pm_audio_measure (&info, 2, buffer, sums));
    g_assert_cmpuint (sums[0].clipped, ==, 3);
    g_assert_cmpuint (sums[1].clipped, ==, 0);
    g_assert_cmpfloat_with_epsilon (pm_audio_db (sums[0].peak), 0.0, 0.01);
    gst_buffer_unref (buffer);
  }
}

static gdouble
zero (guint channel, guint frame)
{
  return 0.0;
}

static gdouble
quiet (guint channel, guint frame)
{
  /* About -70 dBFS: quiet, but not digital silence. */
  return 0.0003 * sin (2 * G_PI * 440.0 * frame / RATE);
}

static gdouble
mixed (guint channel, guint frame)
{
  return channel == 0 ? sines (0, frame) : 0.0;
}

static const GstAudioChannelPosition surround[6] = {
  GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT, GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
  GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER, GST_AUDIO_CHANNEL_POSITION_LFE1,
  GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT, GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
};

static PmAudioMeter *
meter_for (GstAudioFormat format, guint channels)
{
  GstAudioInfo info;
  GstCaps *caps;
  PmAudioMeter *meter;
  gchar *reason;

  gst_audio_info_set_format (&info, format, RATE, channels,
      channels == 6 ? surround : NULL);
  caps = gst_audio_info_to_caps (&info);
  meter = pm_audio_meter_new (caps, &reason);
  gst_caps_unref (caps);
  g_assert_null (reason);
  g_assert_nonnull (meter);
  return meter;
}

/* Feeds the meter 100 ms windows of VALUE from time *NOW on, for DURATION. */
static void
feed (PmAudioMeter * meter, gdouble (*value) (guint, guint),
    GstClockTime * now, GstClockTime duration, PmAudioLevel * levels)
{
  GstClockTime end = *now + duration;
  guint channels = GST_AUDIO_INFO_CHANNELS (pm_audio_meter_info (meter));

  while (*now < end) {
    GstAudioInfo info;
    GstBuffer *buffer = make_buffer (&info,
        GST_AUDIO_INFO_FORMAT (pm_audio_meter_info (meter)), channels, FALSE,
        value);
    PmAudioSum sums[PM_AUDIO_SHOWN];

    g_assert_true (pm_audio_measure (pm_audio_meter_info (meter),
            pm_audio_meter_shown (meter), buffer, sums));
    pm_audio_meter_add (meter, sums);
    *now += 100 * GST_MSECOND;
    g_assert_true (pm_audio_meter_take (meter, *now, levels));
    gst_buffer_unref (buffer);
  }
}

static void
test_silence (void)
{
  PmAudioMeter *meter = meter_for (GST_AUDIO_FORMAT_S16, 2);
  PmAudioLevel levels[2];
  GstClockTime now = GST_SECOND;

  /* Nothing arrived: nothing to send. */
  g_assert_false (pm_audio_meter_take (meter, now, levels));

  feed (meter, sines, &now, GST_SECOND, levels);
  g_assert_false (GST_CLOCK_TIME_IS_VALID (levels[0].silent_since));

  /* Zeros: silent only after 2 s, and then from when they started. */
  feed (meter, zero, &now, 1900 * GST_MSECOND, levels);
  g_assert_false (GST_CLOCK_TIME_IS_VALID (levels[0].silent_since));
  g_assert_true (isinf (levels[0].peak_db) && levels[0].peak_db < 0);
  feed (meter, zero, &now, 200 * GST_MSECOND, levels);
  g_assert_cmpuint (levels[0].silent_since, ==, 2 * GST_SECOND);
  g_assert_true (levels[0].digital_silence);

  /* Quiet noise after zeros: still silent, no longer digital silence. */
  feed (meter, quiet, &now, 300 * GST_MSECOND, levels);
  g_assert_cmpuint (levels[0].silent_since, ==, 2 * GST_SECOND);
  g_assert_false (levels[0].digital_silence);
  g_assert_cmpfloat (levels[0].rms_db, <, PM_AUDIO_SILENT_DB);
  g_assert_cmpfloat (levels[0].rms_db, >, -80.0);

  /* Sound ends the silence; one channel can be silent alone. */
  feed (meter, mixed, &now, 2500 * GST_MSECOND, levels);
  g_assert_false (GST_CLOCK_TIME_IS_VALID (levels[0].silent_since));
  g_assert_true (GST_CLOCK_TIME_IS_VALID (levels[1].silent_since));
  pm_audio_meter_free (meter);
}

static void
test_clipped_total (void)
{
  PmAudioMeter *meter = meter_for (GST_AUDIO_FORMAT_S16, 2);
  PmAudioLevel levels[2];
  GstClockTime now = 0;

  feed (meter, full_scale, &now, 300 * GST_MSECOND, levels);
  g_assert_cmpuint (levels[0].clipped, ==, 3);
  g_assert_cmpuint (levels[0].clipped_total, ==, 9);
  feed (meter, sines, &now, 100 * GST_MSECOND, levels);
  g_assert_cmpuint (levels[0].clipped, ==, 0);
  g_assert_cmpuint (levels[0].clipped_total, ==, 9);
  pm_audio_meter_free (meter);
}

static void
test_channels (void)
{
  PmAudioMeter *meter = meter_for (GST_AUDIO_FORMAT_F32, 6);
  PmAudioLevel levels[PM_AUDIO_SHOWN];
  GstClockTime now = 0;
  const GstAudioInfo *info = pm_audio_meter_info (meter);

  g_assert_cmpuint (pm_audio_meter_shown (meter), ==, 2);
  g_assert_cmpint (GST_AUDIO_INFO_CHANNELS (info), ==, 6);
  g_assert_cmpint (info->position[2], ==,
      GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER);
  feed (meter, sines, &now, 100 * GST_MSECOND, levels);
  g_assert_cmpfloat_with_epsilon (levels[0].peak_db, -6.02, 0.02);
  g_assert_cmpfloat_with_epsilon (levels[1].peak_db, -12.04, 0.02);
  pm_audio_meter_free (meter);

  meter = meter_for (GST_AUDIO_FORMAT_S16, 1);
  g_assert_cmpuint (pm_audio_meter_shown (meter), ==, 1);
  pm_audio_meter_free (meter);
}

static void
check_reason (const gchar * caps_text, const gchar * expected)
{
  GstCaps *caps = gst_caps_from_string (caps_text);
  gchar *reason;

  g_assert_null (pm_audio_meter_new (caps, &reason));
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
  check_reason ("audio/mpeg, mpegversion=4", NULL);
  check_reason ("audio/x-opus", NULL);
  check_reason ("audio/x-raw(memory:SomeDevice), format=S16LE, rate=48000, "
      "channels=2, layout=interleaved",
      "samples not in system memory (memory:SomeDevice)");
  check_reason ("audio/x-raw, rate=48000", "audio caps not complete yet");
  check_reason ("video/x-raw, format=I420, width=320, height=240", NULL);
  check_reason ("application/x-rtp", NULL);
}

int
main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/audio/formats", test_formats);
  g_test_add_func ("/audio/clipping", test_clipping);
  g_test_add_func ("/audio/silence", test_silence);
  g_test_add_func ("/audio/clipped-total", test_clipped_total);
  g_test_add_func ("/audio/channels", test_channels);
  g_test_add_func ("/audio/reasons", test_reasons);
  return g_test_run ();
}
