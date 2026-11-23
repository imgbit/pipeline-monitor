/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef __PM_AUDIO_H__
#define __PM_AUDIO_H__

#include <gst/gst.h>
#include <gst/audio/audio.h>

G_BEGIN_DECLS

/* Channels measured in the free version. */
#define PM_AUDIO_SHOWN 2
/* Below this RMS for PM_AUDIO_SILENT_AFTER, a channel is silent. */
#define PM_AUDIO_SILENT_DB (-60.0)
#define PM_AUDIO_SILENT_AFTER (2 * GST_SECOND)

/* One channel over some samples; values are scaled to full scale 1.0. */
typedef struct
{
  gdouble peak;
  gdouble sum_squares;
  guint64 samples;
  guint64 clipped;
  /* Samples that were not exactly 0. */
  guint64 nonzero;
} PmAudioSum;

/* One channel over one window, as sent. */
typedef struct
{
  /* -INFINITY for no signal at all. */
  gdouble peak_db, rms_db;
  guint64 clipped, clipped_total;
  /* Since when the channel is silent (arrival time), or NONE. */
  GstClockTime silent_since;
  /* Whether every sample since then was exactly 0. */
  gboolean digital_silence;
} PmAudioLevel;

typedef struct _PmAudioMeter PmAudioMeter;

/* NULL, with a reason (or NULL reason when there is nothing to say), when
 * the caps are not raw audio in system memory. */
PmAudioMeter *pm_audio_meter_new (GstCaps * caps, gchar ** reason);
void pm_audio_meter_free (PmAudioMeter * meter);

const GstAudioInfo *pm_audio_meter_info (PmAudioMeter * meter);
guint pm_audio_meter_shown (PmAudioMeter * meter);

/* Sums of one buffer per shown channel, without touching the meter's
 * state: call it outside any lock, then pm_audio_meter_add. */
gboolean pm_audio_measure (const GstAudioInfo * info, guint shown,
    GstBuffer * buffer, PmAudioSum * sums);
void pm_audio_meter_add (PmAudioMeter * meter, const PmAudioSum * sums);

/* Ends the window at NOW: fills one level per shown channel and starts
 * the next window. FALSE if no samples arrived in it. */
gboolean pm_audio_meter_take (PmAudioMeter * meter, GstClockTime now,
    PmAudioLevel * levels);

gdouble pm_audio_db (gdouble value);

G_END_DECLS

#endif /* __PM_AUDIO_H__ */
