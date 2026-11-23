/* SPDX-FileCopyrightText: 2026 Łukasz Huńczak */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pmvideo.h"

#include <string.h>

struct _PmVideoThumb
{
  GstVideoInfo in, out;
  GstVideoConverter *converter;
  /* The previous picture, to tell whether it changed. */
  guint8 *previous;
  GstClockTime unchanged_since;
};

PmVideoThumb *
pm_video_thumb_new (GstCaps * caps, gchar ** reason)
{
  GstStructure *s;
  GstCapsFeatures *features;
  const gchar *name;
  PmVideoThumb *thumb;
  gint width, height;
  gdouble shape;

  *reason = NULL;
  if (caps == NULL || gst_caps_get_size (caps) == 0)
    return NULL;
  s = gst_caps_get_structure (caps, 0);
  name = gst_structure_get_name (s);
  if (!g_str_has_prefix (name, "video/") && !g_str_has_prefix (name,
          "image/"))
    return NULL;
  if (strcmp (name, "video/x-raw") != 0) {
    *reason = g_strdup_printf ("compressed (%s): no picture without "
        "decoding", name);
    return NULL;
  }
  features = gst_caps_get_features (caps, 0);
  if (features != NULL && !gst_caps_features_is_any (features) &&
      !gst_caps_features_contains (features,
          GST_CAPS_FEATURE_MEMORY_SYSTEM_MEMORY)) {
    gchar *text = gst_caps_features_to_string (features);

    *reason = g_strdup_printf ("frames in GPU or device memory (%s): no "
        "picture", text);
    g_free (text);
    return NULL;
  }

  thumb = g_new0 (PmVideoThumb, 1);
  if (!gst_video_info_from_caps (&thumb->in, caps) ||
      GST_VIDEO_INFO_WIDTH (&thumb->in) <= 0 ||
      GST_VIDEO_INFO_HEIGHT (&thumb->in) <= 0) {
    g_free (thumb);
    *reason = g_strdup ("video caps not complete yet");
    return NULL;
  }

  /* The shape shown, with the pixel aspect ratio; the longest side
   * PM_VIDEO_SIZE. */
  shape = (gdouble) GST_VIDEO_INFO_WIDTH (&thumb->in) *
      MAX (GST_VIDEO_INFO_PAR_N (&thumb->in), 1) /
      (GST_VIDEO_INFO_HEIGHT (&thumb->in) *
      MAX (GST_VIDEO_INFO_PAR_D (&thumb->in), 1));
  if (shape >= 1) {
    width = PM_VIDEO_SIZE;
    height = MAX (1, (gint) (PM_VIDEO_SIZE / shape + 0.5));
  } else {
    height = PM_VIDEO_SIZE;
    width = MAX (1, (gint) (PM_VIDEO_SIZE * shape + 0.5));
  }
  gst_video_info_set_interlaced_format (&thumb->out, GST_VIDEO_FORMAT_RGB,
      GST_VIDEO_INFO_INTERLACE_MODE (&thumb->in), width, height);
  /* The converter wants these alike. */
  GST_VIDEO_INFO_FPS_N (&thumb->out) = GST_VIDEO_INFO_FPS_N (&thumb->in);
  GST_VIDEO_INFO_FPS_D (&thumb->out) = GST_VIDEO_INFO_FPS_D (&thumb->in);
  thumb->converter = gst_video_converter_new (&thumb->in, &thumb->out,
      gst_structure_new ("config",
          GST_VIDEO_CONVERTER_OPT_RESAMPLER_METHOD,
          GST_TYPE_VIDEO_RESAMPLER_METHOD, GST_VIDEO_RESAMPLER_METHOD_NEAREST,
          GST_VIDEO_CONVERTER_OPT_CHROMA_RESAMPLER_METHOD,
          GST_TYPE_VIDEO_RESAMPLER_METHOD, GST_VIDEO_RESAMPLER_METHOD_NEAREST,
          GST_VIDEO_CONVERTER_OPT_DITHER_METHOD, GST_TYPE_VIDEO_DITHER_METHOD,
          GST_VIDEO_DITHER_NONE,
          GST_VIDEO_CONVERTER_OPT_THREADS, G_TYPE_UINT, 1, NULL));
  if (thumb->converter == NULL) {
    g_free (thumb);
    *reason = g_strdup_printf ("format %s: no picture",
        gst_video_format_to_string (GST_VIDEO_INFO_FORMAT (&thumb->in)));
    return NULL;
  }
  thumb->unchanged_since = GST_CLOCK_TIME_NONE;
  return thumb;
}

void
pm_video_thumb_free (PmVideoThumb * thumb)
{
  if (thumb == NULL)
    return;
  gst_video_converter_free (thumb->converter);
  g_free (thumb->previous);
  g_free (thumb);
}

const GstVideoInfo *
pm_video_thumb_info (PmVideoThumb * thumb)
{
  return &thumb->in;
}

void
pm_picture_free (PmPicture * picture)
{
  if (picture == NULL)
    return;
  g_free (picture->rgb);
  g_free (picture);
}

PmPicture *
pm_video_thumb_take (PmVideoThumb * thumb, GstBuffer * buffer,
    GstClockTime now)
{
  GstVideoFrame in, out;
  GstBuffer *small;
  PmPicture *p;
  gsize size = GST_VIDEO_INFO_SIZE (&thumb->out);
  gint x, y, stride;
  gdouble sum = 0, max = 0;

  if (!gst_video_frame_map (&in, &thumb->in, buffer, GST_MAP_READ))
    return NULL;
  small = gst_buffer_new_allocate (NULL, size, NULL);
  if (!gst_video_frame_map (&out, &thumb->out, small, GST_MAP_WRITE)) {
    gst_video_frame_unmap (&in);
    gst_buffer_unref (small);
    return NULL;
  }
  gst_video_converter_frame (thumb->converter, &in, &out);
  gst_video_frame_unmap (&in);

  p = g_new0 (PmPicture, 1);
  p->width = GST_VIDEO_INFO_WIDTH (&thumb->out);
  p->height = GST_VIDEO_INFO_HEIGHT (&thumb->out);
  p->pts = GST_BUFFER_PTS (buffer);
  p->rgb = g_malloc ((gsize) p->width * p->height * 3);
  stride = GST_VIDEO_FRAME_PLANE_STRIDE (&out, 0);
  for (y = 0; y < p->height; y++) {
    const guint8 *row = (const guint8 *) GST_VIDEO_FRAME_PLANE_DATA (&out,
        0) + (gsize) y * stride;

    memcpy (p->rgb + (gsize) y * p->width * 3, row, (gsize) p->width * 3);
    for (x = 0; x < p->width; x++) {
      /* Rec. 709 luma of the shown colours. */
      gdouble luma = (0.2126 * row[x * 3] + 0.7152 * row[x * 3 + 1] +
          0.0722 * row[x * 3 + 2]) / 255.0;

      sum += luma;
      if (luma > max)
        max = luma;
    }
  }
  gst_video_frame_unmap (&out);
  gst_buffer_unref (small);

  p->brightness_avg = sum / (p->width * p->height);
  p->brightness_max = max;
  p->black = max < PM_VIDEO_BLACK;

  size = (gsize) p->width * p->height * 3;
  if (thumb->previous == NULL || memcmp (thumb->previous, p->rgb, size) != 0)
    thumb->unchanged_since = now;
  g_free (thumb->previous);
  thumb->previous = g_memdup2 (p->rgb, size);
  p->unchanged_since = thumb->unchanged_since;
  return p;
}
