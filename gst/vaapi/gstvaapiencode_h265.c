/*
 *  gstvaapiencode_h265.c - VA-API H.265 encoder
 *
 *  Copyright (C) 2015 Intel Corporation
 *    Author: Sreerenj Balachandran <sreerenj.balachandran@intel.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public License
 *  as published by the Free Software Foundation; either version 2.1
 *  of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free
 *  Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA 02110-1301 USA
 */

/**
 * SECTION:element-vaapih265enc
 * @short_description: A VA-API based HEVC video encoder
 *
 * Encodes raw video streams into HEVC bitstreams.
 *
 * ## Example launch line
 *
 * |[
 *  gst-launch-1.0 -ev videotestsrc num-buffers=60 ! timeoverlay ! vaapih265enc ! h265parse ! matroskamux ! filesink location=test.mkv
 * ]|
 */

#include "gstcompat.h"
#include <gst/vaapi/gstvaapidisplay.h>
#include <gst/vaapi/gstvaapiencoder_h265.h>
#include <gst/vaapi/gstvaapiutils_h265.h>
#include "gstvaapiencode_h265.h"
#include "gstvaapipluginutil.h"
#include "gstvaapivideomemory.h"

#define GST_PLUGIN_NAME "vaapih265enc"
#define GST_PLUGIN_DESC "A VA-API based H265 video encoder"

GST_DEBUG_CATEGORY_STATIC (gst_vaapi_h265_encode_debug);
#define GST_CAT_DEFAULT gst_vaapi_h265_encode_debug

#define GST_CODEC_CAPS                              \
  "video/x-h265, "                                  \
  "stream-format = (string) { hvc1, byte-stream }, " \
  "alignment = (string) au"

#define EXTRA_FORMATS {}

/* h265 encode */
GST_VAAPI_ENCODE_REGISTER_TYPE (h265, H265, H265, EXTRA_FORMATS,
    gst_vaapi_utils_h265_get_profile_string);

static void
gst_vaapiencode_h265_init (GstVaapiEncodeH265 * encode)
{
  /* nothing to do here */
}

static void
gst_vaapiencode_h265_finalize (GObject * object)
{
  G_OBJECT_CLASS (gst_vaapiencode_h265_parent_class)->finalize (object);
}

static GArray *
gst_vaapiencode_h265_get_allowed_profiles (GstVaapiEncode * encode,
    GstCaps * allowed)
{
  return gst_vaapi_h26x_encoder_get_profiles_from_caps (allowed,
      gst_vaapi_utils_h265_get_profile_from_string);
}

typedef struct
{
  GstVaapiProfile best_profile;
  guint best_score;
} FindBestProfileData;

static void
find_best_profile_value (FindBestProfileData * data, const GValue * value)
{
  const gchar *str;
  GstVaapiProfile profile;
  guint score;

  if (!value || !G_VALUE_HOLDS_STRING (value))
    return;

  str = g_value_get_string (value);
  if (!str)
    return;
  profile = gst_vaapi_utils_h265_get_profile_from_string (str);
  if (!profile)
    return;
  score = gst_vaapi_utils_h265_get_profile_score (profile);
  if (score < data->best_score)
    return;
  data->best_profile = profile;
  data->best_score = score;
}

static GstVaapiProfile
find_best_profile (GstCaps * caps)
{
  FindBestProfileData data;
  guint i, j, num_structures, num_values;

  data.best_profile = GST_VAAPI_PROFILE_UNKNOWN;
  data.best_score = 0;

  num_structures = gst_caps_get_size (caps);
  for (i = 0; i < num_structures; i++) {
    GstStructure *const structure = gst_caps_get_structure (caps, i);
    const GValue *const value = gst_structure_get_value (structure, "profile");

    if (!value)
      continue;
    if (G_VALUE_HOLDS_STRING (value))
      find_best_profile_value (&data, value);
    else if (GST_VALUE_HOLDS_LIST (value)) {
      num_values = gst_value_list_get_size (value);
      for (j = 0; j < num_values; j++)
        find_best_profile_value (&data, gst_value_list_get_value (value, j));
    }
  }
  return data.best_profile;
}

static gboolean
gst_vaapiencode_h265_set_config (GstVaapiEncode * base_encode)
{
  GstVaapiEncoderH265 *const encoder =
      GST_VAAPI_ENCODER_H265 (base_encode->encoder);
  GstCaps *allowed_caps;
  GstVaapiProfile profile;

  /* Check for the largest profile that is supported */
  allowed_caps =
      gst_pad_get_allowed_caps (GST_VAAPI_PLUGIN_BASE_SRC_PAD (base_encode));
  if (!allowed_caps)
    return TRUE;

  profile = find_best_profile (allowed_caps);
  gst_caps_unref (allowed_caps);
  if (profile) {
    GST_INFO ("using %s profile as target decoder constraints",
        gst_vaapi_utils_h265_get_profile_string (profile));
    if (!gst_vaapi_encoder_h265_set_max_profile (encoder, profile))
      return FALSE;
  }
  return TRUE;
}

static GstCaps *
gst_vaapiencode_h265_get_caps (GstVaapiEncode * base_encode)
{
  GstVaapiEncodeH265 *const encode = GST_VAAPIENCODE_H265_CAST (base_encode);
  GstVaapiEncoderH265 *const encoder =
      GST_VAAPI_ENCODER_H265 (base_encode->encoder);
  GstCaps *caps, *allowed_caps;
  GstVaapiProfile profile = GST_VAAPI_PROFILE_UNKNOWN;
  GstVaapiLevelH265 level = 0;
  GstVaapiTierH265 tier = GST_VAAPI_TIER_H265_UNKNOWN;

  caps = gst_caps_from_string (GST_CODEC_CAPS);

  /* Check whether "stream-format" is hvcC mode */
  allowed_caps =
      gst_pad_get_allowed_caps (GST_VAAPI_PLUGIN_BASE_SRC_PAD (encode));
  if (allowed_caps) {
    const char *stream_format = NULL;
    GstStructure *structure;
    guint i, num_structures;

    num_structures = gst_caps_get_size (allowed_caps);
    for (i = 0; !stream_format && i < num_structures; i++) {
      structure = gst_caps_get_structure (allowed_caps, i);
      if (!gst_structure_has_field_typed (structure, "stream-format",
              G_TYPE_STRING))
        continue;
      stream_format = gst_structure_get_string (structure, "stream-format");
    }
    encode->is_hvc = stream_format && strcmp (stream_format, "hvc1") == 0;
    gst_caps_unref (allowed_caps);
  }
  gst_caps_set_simple (caps, "stream-format", G_TYPE_STRING,
      encode->is_hvc ? "hvc1" : "byte-stream", NULL);

  base_encode->need_codec_data = encode->is_hvc;

  gst_vaapi_encoder_h265_get_profile_tier_level (encoder,
      &profile, &tier, &level);
  if (profile != GST_VAAPI_PROFILE_UNKNOWN) {
    gst_caps_set_simple (caps, "profile", G_TYPE_STRING,
        gst_vaapi_utils_h265_get_profile_string (profile), NULL);

    if (level) {
      gst_caps_set_simple (caps, "level", G_TYPE_STRING,
          gst_vaapi_utils_h265_get_level_string (level), NULL);

      if (tier != GST_VAAPI_TIER_H265_UNKNOWN)
        gst_caps_set_simple (caps, "tier", G_TYPE_STRING,
            gst_vaapi_utils_h265_get_tier_string (tier), NULL);
    }
  }

  return caps;
}

static GstVaapiEncoder *
gst_vaapiencode_h265_alloc_encoder (GstVaapiEncode * base,
    GstVaapiDisplay * display)
{
  return gst_vaapi_encoder_h265_new (display);
}

/* h265 NAL byte stream operations */
static guint8 *
_h265_byte_stream_next_nal (guint8 * buffer, guint32 len, guint32 * nal_size)
{
  const guint8 *cur = buffer;
  const guint8 *const end = buffer + len;
  guint8 *nal_start = NULL;
  guint32 flag = 0xFFFFFFFF;
  guint32 nal_start_len = 0;

  g_assert (len != 0U && buffer && nal_size);
  if (len < 3) {
    *nal_size = len;
    nal_start = (len ? buffer : NULL);
    return nal_start;
  }

  /*locate head postion */
  if (!buffer[0] && !buffer[1]) {
    if (buffer[2] == 1) {       /* 0x000001 */
      nal_start_len = 3;
    } else if (!buffer[2] && len >= 4 && buffer[3] == 1) {      /* 0x00000001 */
      nal_start_len = 4;
    }
  }
  nal_start = buffer + nal_start_len;
  cur = nal_start;

  /*find next nal start position */
  while (cur < end) {
    flag = ((flag << 8) | ((*cur++) & 0xFF));
    if ((flag & 0x00FFFFFF) == 0x00000001) {
      if (flag == 0x00000001)
        *nal_size = cur - 4 - nal_start;
      else
        *nal_size = cur - 3 - nal_start;
      break;
    }
  }
  if (cur >= end) {
    *nal_size = end - nal_start;
    if (nal_start >= end) {
      nal_start = NULL;
    }
  }
  return nal_start;
}

static inline void
_start_code_to_size (guint8 nal_start_code[4], guint32 nal_size)
{
  nal_start_code[0] = ((nal_size >> 24) & 0xFF);
  nal_start_code[1] = ((nal_size >> 16) & 0xFF);
  nal_start_code[2] = ((nal_size >> 8) & 0xFF);
  nal_start_code[3] = (nal_size & 0xFF);
}

static gboolean
_h265_convert_byte_stream_to_hvc (GstBuffer * buf)
{
  GstMapInfo info;
  guint32 nal_size;
  guint8 *nal_start_code, *nal_body;
  guint8 *frame_end;

  g_assert (buf);

  if (!gst_buffer_map (buf, &info, GST_MAP_READ | GST_MAP_WRITE))
    return FALSE;

  nal_start_code = info.data;
  frame_end = info.data + info.size;
  nal_size = 0;

  while ((frame_end > nal_start_code) &&
      (nal_body = _h265_byte_stream_next_nal (nal_start_code,
              frame_end - nal_start_code, &nal_size)) != NULL) {
    if (!nal_size)
      goto error;

    g_assert (nal_body - nal_start_code == 4);
    _start_code_to_size (nal_start_code, nal_size);
    nal_start_code = nal_body + nal_size;
  }
  gst_buffer_unmap (buf, &info);
  return TRUE;

  /* ERRORS */
error:
  {
    gst_buffer_unmap (buf, &info);
    return FALSE;
  }
}

static GstFlowReturn
gst_vaapiencode_h265_alloc_buffer (GstVaapiEncode * base_encode,
    GstVaapiCodedBuffer * coded_buf, GstBuffer ** out_buffer_ptr)
{
  GstVaapiEncodeH265 *const encode = GST_VAAPIENCODE_H265_CAST (base_encode);
  GstVaapiEncoderH265 *const encoder =
      GST_VAAPI_ENCODER_H265 (base_encode->encoder);
  GstFlowReturn ret;

  g_return_val_if_fail (encoder != NULL, GST_FLOW_ERROR);

  ret =
      GST_VAAPIENCODE_CLASS (gst_vaapiencode_h265_parent_class)->alloc_buffer
      (base_encode, coded_buf, out_buffer_ptr);
  if (ret != GST_FLOW_OK)
    return ret;

  if (!encode->is_hvc)
    return GST_FLOW_OK;

  /* Convert to hvcC format */
  if (!_h265_convert_byte_stream_to_hvc (*out_buffer_ptr))
    goto error_convert_buffer;
  return GST_FLOW_OK;

  /* ERRORS */
error_convert_buffer:
  {
    GST_ERROR ("failed to convert from bytestream format to hvcC format");
    gst_buffer_replace (out_buffer_ptr, NULL);
    return GST_FLOW_ERROR;
  }
}

static void
gst_vaapiencode_h265_class_init (GstVaapiEncodeH265Class * klass, gpointer data)
{
  GObjectClass *const object_class = G_OBJECT_CLASS (klass);
  GstElementClass *const element_class = GST_ELEMENT_CLASS (klass);
  GstVaapiEncodeClass *const encode_class = GST_VAAPIENCODE_CLASS (klass);
  GstCaps *sink_caps = ((GstVaapiEncodeInitData *) data)->sink_caps;
  GstCaps *src_caps = ((GstVaapiEncodeInitData *) data)->src_caps;
  GstPadTemplate *templ;
  GstCaps *static_caps;
  gpointer encoder_class;

  object_class->finalize = gst_vaapiencode_h265_finalize;
  object_class->set_property = gst_vaapiencode_set_property_subclass;
  object_class->get_property = gst_vaapiencode_get_property_subclass;

  encode_class->get_allowed_profiles =
      gst_vaapiencode_h265_get_allowed_profiles;
  encode_class->set_config = gst_vaapiencode_h265_set_config;
  encode_class->get_caps = gst_vaapiencode_h265_get_caps;
  encode_class->alloc_encoder = gst_vaapiencode_h265_alloc_encoder;
  encode_class->alloc_buffer = gst_vaapiencode_h265_alloc_buffer;

  gst_element_class_set_static_metadata (element_class,
      "VA-API H265 encoder",
      "Codec/Encoder/Video/Hardware",
      GST_PLUGIN_DESC,
      "Sreerenj Balachandran <sreerenj.balachandran@intel.com>");

  /* sink pad */
  g_assert (sink_caps);
  static_caps = gst_caps_from_string (GST_VAAPI_ENCODE_STATIC_SINK_CAPS);
  templ =
      gst_pad_template_new ("sink", GST_PAD_SINK, GST_PAD_ALWAYS, sink_caps);
  gst_pad_template_set_documentation_caps (templ, static_caps);
  gst_element_class_add_pad_template (element_class, templ);
  gst_caps_unref (static_caps);
  gst_caps_unref (sink_caps);

  /* src pad */
  g_assert (src_caps);
  static_caps = gst_caps_from_string (GST_CODEC_CAPS);
  templ = gst_pad_template_new ("src", GST_PAD_SRC, GST_PAD_ALWAYS, src_caps);
  gst_pad_template_set_documentation_caps (templ, static_caps);
  gst_element_class_add_pad_template (element_class, templ);
  gst_caps_unref (static_caps);
  gst_caps_unref (src_caps);

  encoder_class = g_type_class_ref (GST_TYPE_VAAPI_ENCODER_H265);
  g_assert (encoder_class);
  gst_vaapiencode_class_install_properties (encode_class, encoder_class);
  g_type_class_unref (encoder_class);
}