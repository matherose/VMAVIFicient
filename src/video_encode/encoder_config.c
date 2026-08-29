/**
 * @file encoder_config.c
 * @brief Shared SVT-AV1-HDR encoder configuration helpers.
 *
 * Extracted from video_encode.c so the CRF probe encodes use the exact same
 * configuration mapping as the final encode. Keep all preset-to-config
 * translation logic here.
 */

#include "encoder_config.h"

#include <stdbool.h>

#include <libavutil/mastering_display_metadata.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>

#include <EbSvtAv1Formats.h>

#define UNSET (-1)

void apply_preset_to_config(EbSvtAv1EncConfiguration *cfg, const EncodePreset *p, int film_grain,
                            int target_bitrate_kbps, int crf) {
  cfg->enc_mode = (int8_t)p->preset;
  cfg->intra_period_length = p->keyint;
  cfg->tune = (uint8_t)p->tune;
  if (crf > 0) {
    cfg->rate_control_mode = SVT_AV1_RC_MODE_CQP_OR_CRF;
    cfg->qp = (uint32_t)crf;
  } else {
    cfg->rate_control_mode = SVT_AV1_RC_MODE_VBR;
    cfg->target_bit_rate = (uint32_t)target_bitrate_kbps * 1000;
  }

  cfg->ac_bias = p->ac_bias;
  cfg->enable_variance_boost = true;
  cfg->variance_boost_strength = (uint8_t)p->variance_boost;
  cfg->variance_octile = (uint8_t)p->variance_octile;
  cfg->variance_boost_curve = (uint8_t)p->variance_curve;
  cfg->sharpness = (int8_t)p->sharpness;
  cfg->luminance_qp_bias = (uint8_t)p->luminance_bias;
  cfg->enable_tf = (uint8_t)p->enable_tf;
  cfg->tf_strength = (uint8_t)p->tf_strength;
  cfg->kf_tf_strength = (uint8_t)p->kf_tf_strength;
  cfg->tx_bias = (uint8_t)p->tx_bias;
  cfg->sharp_tx = (uint8_t)p->sharp_tx;
  cfg->complex_hvs = (uint8_t)p->complex_hvs;
  cfg->noise_adaptive_filtering = (uint8_t)p->noise_adaptive_filtering;
  cfg->enable_dlf_flag = (uint8_t)p->enable_dlf;
  cfg->cdef_scaling = (uint8_t)p->cdef_scaling;
  cfg->qp_scale_compress_strength = p->qp_scale_compress_strength;
  cfg->max_tx_size = (uint8_t)p->max_tx_size;
  cfg->hbd_mds = (uint8_t)p->hbd_mds;
  cfg->adaptive_film_grain = (p->adaptive_film_grain == 1);
  cfg->alt_lambda_factors = (p->alt_lambda_factors == 1);

  if (p->noise_norm_strength != UNSET)
    cfg->noise_norm_strength = (uint8_t)p->noise_norm_strength;
  if (p->chroma_qm_min != UNSET)
    cfg->min_chroma_qm_level = (uint8_t)p->chroma_qm_min;
  if (p->chroma_qm_max != UNSET)
    cfg->max_chroma_qm_level = (uint8_t)p->chroma_qm_max;
  if (p->enable_overlays != UNSET)
    cfg->enable_overlays = (p->enable_overlays == 1);

  if (p->enable_qm >= 0)
    cfg->enable_qm = (p->enable_qm == 1);
  if (p->qm_min != UNSET)
    cfg->min_qm_level = (uint8_t)p->qm_min;
  if (p->qm_max != UNSET)
    cfg->max_qm_level = (uint8_t)p->qm_max;

  if (p->undershoot_pct != UNSET)
    cfg->under_shoot_pct = (uint32_t)p->undershoot_pct;
  if (p->overshoot_pct != UNSET)
    cfg->over_shoot_pct = (uint32_t)p->overshoot_pct;

  if (p->min_qp != UNSET)
    cfg->min_qp_allowed = (uint32_t)p->min_qp;
  if (p->max_qp != UNSET)
    cfg->max_qp_allowed = (uint32_t)p->max_qp;

  if (p->enable_mfmv != UNSET)
    cfg->enable_mfmv = (p->enable_mfmv == 1);

  /* Seekability invariant: the output MUST carry periodic IDR keyframes so
   * players can seek. Open GOP (CRA, intra_refresh_type 1) produces
   * un-seekable output (the SVT-AV1 v1.5.0 single-keyframe defect). This is
   * the single funnel both the CRF probe and the final encode pass through,
   * so forcing closed GOP here makes the invariant un-regressable — no preset
   * can reintroduce open GOP. Paired with a positive intra_period_length
   * (set from keyint above) this guarantees periodic, seekable keyframes. */
  cfg->intra_refresh_type = 2; /* closed GOP (IDR) */

  if (p->aq_mode != UNSET)
    cfg->aq_mode = (uint8_t)p->aq_mode;

  if (p->enable_restoration != UNSET)
    cfg->enable_restoration_filtering = (p->enable_restoration == 1);

  if (p->recode_loop != UNSET)
    cfg->recode_loop = (uint32_t)p->recode_loop;

  if (p->look_ahead_distance != UNSET)
    cfg->look_ahead_distance = (uint32_t)p->look_ahead_distance;

  if (p->enable_dg != UNSET)
    cfg->enable_dg = (p->enable_dg == 1);

  if (p->fast_decode != UNSET)
    cfg->fast_decode = (uint8_t)p->fast_decode;

  if (p->scd != UNSET)
    cfg->scene_change_detection = (uint32_t)(p->scd == 1);

  if (p->temporal_layer_chroma_qindex_offset != UNSET)
    for (int i = 0; i < EB_MAX_TEMPORAL_LAYERS; i++)
      cfg->chroma_qindex_offsets[i] = (int32_t)p->temporal_layer_chroma_qindex_offset;

  /* Keyframe QP offsets — mode 2 = apply on top of CRF-derived QP. */
  if (p->key_frame_qindex_offset != 0 || p->key_frame_chroma_qindex_offset != 0) {
    cfg->use_fixed_qindex_offsets = 2;
    cfg->key_frame_qindex_offset = (int32_t)p->key_frame_qindex_offset;
    cfg->key_frame_chroma_qindex_offset = (int32_t)p->key_frame_chroma_qindex_offset;
  }

  if (crf <= 0 && p->vbr_max_section_pct > 0)
    cfg->vbr_max_section_pct = (uint32_t)p->vbr_max_section_pct;

  if (crf > 0) {
    if (p->startup_mg_size > 0)
      cfg->startup_mg_size = (uint8_t)p->startup_mg_size;
    if (p->startup_qp_offset != 0)
      cfg->startup_qp_offset = (int8_t)p->startup_qp_offset;
  }

  if (p->use_noise) {
    cfg->noise_strength = (uint8_t)film_grain;
    cfg->noise_strength_chroma = (int32_t)p->noise_chroma_strength;
    cfg->noise_chroma_from_luma = (uint8_t)p->noise_chroma_from_luma;
    cfg->noise_size = (int8_t)p->noise_size;
    cfg->film_grain_denoise_strength = 0;
    cfg->film_grain_denoise_apply = 0;
  } else {
    cfg->film_grain_denoise_strength = (uint32_t)film_grain;
    cfg->film_grain_denoise_apply = (film_grain > 0) ? 1 : 0;
    cfg->noise_strength = 0;
  }
}

void copy_color_info(EbSvtAv1EncConfiguration *cfg, const AVCodecParameters *codecpar) {
  cfg->color_primaries = (EbColorPrimaries)codecpar->color_primaries;
  cfg->transfer_characteristics = (EbTransferCharacteristics)codecpar->color_trc;
  cfg->matrix_coefficients = (EbMatrixCoefficients)codecpar->color_space;
  cfg->color_range =
      (codecpar->color_range == AVCOL_RANGE_JPEG) ? EB_CR_FULL_RANGE : EB_CR_STUDIO_RANGE;
}

void set_hdr10_metadata(EbSvtAv1EncConfiguration *cfg, const AVStream *stream) {
  const AVCodecParameters *par = stream->codecpar;

  const AVPacketSideData *mdcv_sd = NULL;
  const AVPacketSideData *cll_sd = NULL;
  for (int i = 0; i < par->nb_coded_side_data; i++) {
    if (par->coded_side_data[i].type == AV_PKT_DATA_MASTERING_DISPLAY_METADATA)
      mdcv_sd = &par->coded_side_data[i];
    else if (par->coded_side_data[i].type == AV_PKT_DATA_CONTENT_LIGHT_LEVEL)
      cll_sd = &par->coded_side_data[i];
  }

  if (mdcv_sd && mdcv_sd->size >= (int)sizeof(AVMasteringDisplayMetadata)) {
    const AVMasteringDisplayMetadata *mdcv = (const AVMasteringDisplayMetadata *)mdcv_sd->data;
    if (mdcv->has_primaries) {
      cfg->mastering_display.r.x = (uint16_t)(av_q2d(mdcv->display_primaries[0][0]) * 50000 + 0.5);
      cfg->mastering_display.r.y = (uint16_t)(av_q2d(mdcv->display_primaries[0][1]) * 50000 + 0.5);
      cfg->mastering_display.g.x = (uint16_t)(av_q2d(mdcv->display_primaries[1][0]) * 50000 + 0.5);
      cfg->mastering_display.g.y = (uint16_t)(av_q2d(mdcv->display_primaries[1][1]) * 50000 + 0.5);
      cfg->mastering_display.b.x = (uint16_t)(av_q2d(mdcv->display_primaries[2][0]) * 50000 + 0.5);
      cfg->mastering_display.b.y = (uint16_t)(av_q2d(mdcv->display_primaries[2][1]) * 50000 + 0.5);
      cfg->mastering_display.white_point.x = (uint16_t)(av_q2d(mdcv->white_point[0]) * 50000 + 0.5);
      cfg->mastering_display.white_point.y = (uint16_t)(av_q2d(mdcv->white_point[1]) * 50000 + 0.5);
    }
    if (mdcv->has_luminance) {
      cfg->mastering_display.max_luma = (uint32_t)(av_q2d(mdcv->max_luminance) * 10000 + 0.5);
      cfg->mastering_display.min_luma = (uint32_t)(av_q2d(mdcv->min_luminance) * 10000 + 0.5);
    }
  }

  if (cll_sd && cll_sd->size >= (int)sizeof(AVContentLightMetadata)) {
    const AVContentLightMetadata *cll = (const AVContentLightMetadata *)cll_sd->data;
    cfg->content_light_level.max_cll = (uint16_t)cll->MaxCLL;
    cfg->content_light_level.max_fall = (uint16_t)cll->MaxFALL;
  }
}
