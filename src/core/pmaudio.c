/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pmaudio.h"

#include <math.h>
#include <string.h>

typedef struct
{
  PmAudioSum window;
  guint64 clipped_total;
  GstClockTime quiet_since;
  gboolean quiet_zero;
} PmChannel;

struct _PmAudioMeter
{
  GstAudioInfo info;
  guint shown;
  PmChannel *channels;
  GstClockTime window_begin;
};

gdouble
pm_audio_db (gdouble value)
{
  return value > 0 ? 20.0 * log10 (value) : -INFINITY;
}

PmAudioMeter *
pm_audio_meter_new (GstCaps * caps, gchar ** reason)
{
  GstStructure *s;
  GstCapsFeatures *features;
  const gchar *name;
  PmAudioMeter *meter;

  *reason = NULL;
  if (caps == NULL || gst_caps_get_size (caps) == 0)
    return NULL;
  s = gst_caps_get_structure (caps, 0);
  name = gst_structure_get_name (s);
  if (!g_str_has_prefix (name, "audio/"))
    return NULL;
  if (strcmp (name, "audio/x-raw") != 0) {
    *reason = g_strdup_printf ("compressed (%s): no levels without decoding",
        name);
    return NULL;
  }
  features = gst_caps_get_features (caps, 0);
  if (features != NULL && !gst_caps_features_is_any (features) &&
      !gst_caps_features_contains (features,
          GST_CAPS_FEATURE_MEMORY_SYSTEM_MEMORY)) {
    gchar *text = gst_caps_features_to_string (features);

    *reason = g_strdup_printf ("samples not in system memory (%s)", text);
    g_free (text);
    return NULL;
  }

  meter = g_new0 (PmAudioMeter, 1);
  if (!gst_audio_info_from_caps (&meter->info, caps) ||
      GST_AUDIO_INFO_CHANNELS (&meter->info) == 0) {
    g_free (meter);
    *reason = g_strdup ("audio caps not complete yet");
    return NULL;
  }
  meter->shown = MIN (GST_AUDIO_INFO_CHANNELS (&meter->info), PM_AUDIO_SHOWN);
  meter->channels = g_new0 (PmChannel, meter->shown);
  for (guint c = 0; c < meter->shown; c++)
    meter->channels[c].quiet_since = GST_CLOCK_TIME_NONE;
  meter->window_begin = GST_CLOCK_TIME_NONE;
  return meter;
}

void
pm_audio_meter_free (PmAudioMeter * meter)
{
  if (meter == NULL)
    return;
  g_free (meter->channels);
  g_free (meter);
}

const GstAudioInfo *
pm_audio_meter_info (PmAudioMeter * meter)
{
  return &meter->info;
}

guint
pm_audio_meter_shown (PmAudioMeter * meter)
{
  return meter->shown;
}

static inline void
add_sample (PmAudioSum * s, gdouble x, gboolean clipped, gboolean zero)
{
  gdouble a = fabs (x);

  if (a > s->peak)
    s->peak = a;
  s->sum_squares += x * x;
  s->clipped += clipped;
  s->nonzero += !zero;
}

/* Native formats, read in place. */
static gboolean
sum_native (GstAudioFormat format, gconstpointer data, gsize step,
    gsize n, PmAudioSum * s)
{
  gsize i;

  switch (format) {
    case GST_AUDIO_FORMAT_S16:{
      const gint16 *p = data;

      for (i = 0; i < n; i++) {
        gint16 v = p[i * step];

        add_sample (s, v / 32768.0, v == G_MININT16 || v == G_MAXINT16,
            v == 0);
      }
      return TRUE;
    }
    case GST_AUDIO_FORMAT_S32:{
      const gint32 *p = data;

      for (i = 0; i < n; i++) {
        gint32 v = p[i * step];

        add_sample (s, v / 2147483648.0, v == G_MININT32 || v == G_MAXINT32,
            v == 0);
      }
      return TRUE;
    }
    case GST_AUDIO_FORMAT_F32:{
      const gfloat *p = data;

      for (i = 0; i < n; i++) {
        gfloat v = p[i * step];

        add_sample (s, v, fabsf (v) >= 1.0f, v == 0.0f);
      }
      return TRUE;
    }
    case GST_AUDIO_FORMAT_F64:{
      const gdouble *p = data;

      for (i = 0; i < n; i++) {
        gdouble v = p[i * step];

        add_sample (s, v, fabs (v) >= 1.0, v == 0.0);
      }
      return TRUE;
    }
    default:
      return FALSE;
  }
}

/* Other formats: GStreamer unpacks them to S32 (integers, left-aligned)
 * or F64. */
static void
sum_unpacked (const GstAudioFormatInfo * finfo, gconstpointer data,
    guint channels, guint shown, gsize frames, PmAudioSum * sums)
{
  gsize n = frames * channels, i;
  guint c;

  if (GST_AUDIO_FORMAT_INFO_IS_FLOAT (finfo)) {
    gdouble *tmp = g_new (gdouble, n);

    finfo->unpack_func (finfo, 0, tmp, data, n);
    for (c = 0; c < shown; c++)
      for (i = 0; i < frames; i++) {
        gdouble v = tmp[i * channels + c];

        add_sample (&sums[c], v, fabs (v) >= 1.0, v == 0.0);
      }
    g_free (tmp);
  } else {
    gint32 *tmp = g_new (gint32, n);
    guint unused = 32 - GST_AUDIO_FORMAT_INFO_DEPTH (finfo);
    gint32 top = (gint32) (((guint32) G_MAXINT32 >> unused) << unused);

    finfo->unpack_func (finfo, 0, tmp, data, n);
    for (c = 0; c < shown; c++)
      for (i = 0; i < frames; i++) {
        gint32 v = tmp[i * channels + c];

        add_sample (&sums[c], v / 2147483648.0, v == G_MININT32 || v >= top,
            v == 0);
      }
    g_free (tmp);
  }
}

gboolean
pm_audio_measure (const GstAudioInfo * info, guint shown, GstBuffer * buffer,
    PmAudioSum * sums)
{
  GstAudioBuffer ab;
  GstAudioFormat format = GST_AUDIO_INFO_FORMAT (info);
  guint channels = GST_AUDIO_INFO_CHANNELS (info), c;
  gboolean interleaved =
      GST_AUDIO_INFO_LAYOUT (info) == GST_AUDIO_LAYOUT_INTERLEAVED;

  memset (sums, 0, shown * sizeof (PmAudioSum));
  if (!gst_audio_buffer_map (&ab, info, buffer, GST_MAP_READ))
    return FALSE;

  for (c = 0; c < shown; c++) {
    gconstpointer data;
    gsize width = GST_AUDIO_INFO_WIDTH (info) / 8;

    if (interleaved)
      data = (const guint8 *) ab.planes[0] + c * width;
    else
      data = ab.planes[c];
    if (!sum_native (format, data, interleaved ? channels : 1, ab.n_samples,
            &sums[c]))
      break;
    sums[c].samples = ab.n_samples;
  }
  if (c < shown) {
    memset (sums, 0, shown * sizeof (PmAudioSum));
    if (interleaved) {
      sum_unpacked (info->finfo, ab.planes[0], channels, shown, ab.n_samples,
          sums);
    } else {
      for (c = 0; c < shown; c++)
        sum_unpacked (info->finfo, ab.planes[c], 1, 1, ab.n_samples,
            &sums[c]);
    }
    for (c = 0; c < shown; c++)
      sums[c].samples = ab.n_samples;
  }
  gst_audio_buffer_unmap (&ab);
  return TRUE;
}

void
pm_audio_meter_add (PmAudioMeter * meter, const PmAudioSum * sums)
{
  for (guint c = 0; c < meter->shown; c++) {
    PmAudioSum *w = &meter->channels[c].window;

    w->peak = MAX (w->peak, sums[c].peak);
    w->sum_squares += sums[c].sum_squares;
    w->samples += sums[c].samples;
    w->clipped += sums[c].clipped;
    w->nonzero += sums[c].nonzero;
  }
}

gboolean
pm_audio_meter_take (PmAudioMeter * meter, GstClockTime now,
    PmAudioLevel * levels)
{
  GstClockTime begin = GST_CLOCK_TIME_IS_VALID (meter->window_begin) ?
      meter->window_begin : now;
  guint c;

  if (meter->channels[0].window.samples == 0)
    return FALSE;
  meter->window_begin = now;

  for (c = 0; c < meter->shown; c++) {
    PmChannel *ch = &meter->channels[c];
    PmAudioSum *w = &ch->window;
    PmAudioLevel *l = &levels[c];

    l->peak_db = pm_audio_db (w->peak);
    l->rms_db = pm_audio_db (sqrt (w->sum_squares / w->samples));
    l->clipped = w->clipped;
    ch->clipped_total += w->clipped;
    l->clipped_total = ch->clipped_total;

    if (l->rms_db < PM_AUDIO_SILENT_DB) {
      if (!GST_CLOCK_TIME_IS_VALID (ch->quiet_since)) {
        ch->quiet_since = begin;
        ch->quiet_zero = TRUE;
      }
      ch->quiet_zero &= w->nonzero == 0;
    } else {
      ch->quiet_since = GST_CLOCK_TIME_NONE;
    }
    if (GST_CLOCK_TIME_IS_VALID (ch->quiet_since) &&
        now - ch->quiet_since >= PM_AUDIO_SILENT_AFTER) {
      l->silent_since = ch->quiet_since;
      l->digital_silence = ch->quiet_zero;
    } else {
      l->silent_since = GST_CLOCK_TIME_NONE;
      l->digital_silence = FALSE;
    }
    memset (w, 0, sizeof (PmAudioSum));
  }
  return TRUE;
}
