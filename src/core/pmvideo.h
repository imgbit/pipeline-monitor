/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef __PM_VIDEO_H__
#define __PM_VIDEO_H__

#include <gst/gst.h>
#include <gst/video/video.h>

G_BEGIN_DECLS

/* The longest side of a picture in the free version. */
#define PM_VIDEO_SIZE 160
/* At most one picture per this time. */
#define PM_VIDEO_EVERY GST_SECOND
/* Brighter than this nowhere: a black picture (luma, 0..1). */
#define PM_VIDEO_BLACK 0.1

/* A small copy of one frame, RGB, and what it shows. */
typedef struct
{
  gint width, height;
  guint8 *rgb;
  GstClockTime pts;
  /* Luma 0..1 over the picture. */
  gdouble brightness_avg, brightness_max;
  gboolean black;
  /* Since when the picture is the same (arrival time). */
  GstClockTime unchanged_since;
} PmPicture;

typedef struct _PmVideoThumb PmVideoThumb;

/* NULL, with a reason (or NULL reason when there is nothing to say), when
 * the caps are not raw video in system memory. */
PmVideoThumb *pm_video_thumb_new (GstCaps * caps, gchar ** reason);
void pm_video_thumb_free (PmVideoThumb * thumb);

const GstVideoInfo *pm_video_thumb_info (PmVideoThumb * thumb);

/* A picture of BUFFER, which arrived at NOW; the buffer is not kept. NULL
 * if it cannot be read. */
PmPicture *pm_video_thumb_take (PmVideoThumb * thumb, GstBuffer * buffer,
    GstClockTime now);
void pm_picture_free (PmPicture * picture);

G_END_DECLS

#endif /* __PM_VIDEO_H__ */
