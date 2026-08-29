/**
 * @file video_encode.c
 * @brief AV1 video encoding via SVT-AV1-HDR with Dolby Vision / HDR10+ support.
 *
 * Pipeline: FFmpeg decode → crop → SVT-AV1-HDR encode → FFmpeg MKV mux.
 * Dolby Vision RPU metadata is read from a .rpu.bin file and attached
 * per-frame as ITU-T T.35 metadata OBUs.
 */

#include "video_encode.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>

#include <EbSvtAv1.h>
#include <EbSvtAv1Enc.h>
#include <EbSvtAv1Formats.h>
#include <EbSvtAv1Metadata.h>

#include <libdovi/rpu_parser.h>

#include "encoder_config.h"
#include "ui.h"

/* ====================================================================== */
/*  SVT-AV1 log routing                                                  */
/*                                                                       */
/* Default: silent — we render our own progress bar and don't want SVT's */
/* per-frame chatter clobbering it. With --verbose (ui_is_verbose), we   */
/* forward everything to stderr so the user sees the encoder's diagnostic*/
/* output (rate control decisions, GOP layout, warnings, etc.).         */
/* ====================================================================== */

__attribute__((format(printf, 4, 0))) static void svt_log_callback(void *context,
                                                                   SvtAv1LogLevel level,
                                                                   const char *tag, const char *fmt,
                                                                   va_list args) {
  (void)context;
  /* SVT-AV1 levels: 0=fatal, 1=error, 2=warn, 3=info, 4=debug. Fatal,
     error, and warn always reach stderr — suppressing them behind
     --verbose masked a FATAL misconfiguration for a month (the v1.5.0
     single-keyframe defect). Info and debug stay verbose-gated. */
  if (level > SVT_AV1_LOG_WARN && !ui_is_verbose())
    return;
  const char *level_str = "info";
  switch (level) {
  case SVT_AV1_LOG_FATAL:
    level_str = "fatal";
    break;
  case SVT_AV1_LOG_ERROR:
    level_str = "error";
    break;
  case SVT_AV1_LOG_WARN:
    level_str = "warn";
    break;
  case SVT_AV1_LOG_INFO:
    level_str = "info";
    break;
  case SVT_AV1_LOG_DEBUG:
    level_str = "debug";
    break;
  default:
    break;
  }
  (void)fprintf(stderr, "[svt-av1 %s%s%s] ", level_str, tag ? " " : "", tag ? tag : "");
  (void)vfprintf(stderr, fmt, args);
  /* SVT messages may or may not end with \n; don't double up. */
  size_t fmtlen = fmt ? strlen(fmt) : 0;
  if (fmtlen == 0 || fmt[fmtlen - 1] != '\n')
    (void)fputc('\n', stderr);
}

/* ====================================================================== */
/*  RPU file reader — reads length-prefixed RPU entries                   */
/* ====================================================================== */

typedef struct {
  FILE *fp;
  int eof;
} RpuReader;

static RpuReader rpu_reader_open(const char *path) {
  RpuReader r = {.fp = NULL, .eof = 1};
  if (!path || !path[0])
    return r;
  r.fp = fopen(path, "rb");
  if (r.fp)
    r.eof = 0;
  return r;
}

static void rpu_reader_close(RpuReader *r) {
  if (r->fp) {
    (void)fclose(r->fp); /* read-only stream */
    r->fp = NULL;
  }
}

/**
 * Read the next RPU entry. Caller must free *out_data with free().
 * Returns the size or 0 on EOF/error.
 */
static size_t rpu_reader_next(RpuReader *r, uint8_t **out_data) {
  *out_data = NULL;
  if (!r->fp || r->eof)
    return 0;

  uint8_t len_be[4];
  if (fread(len_be, 1, 4, r->fp) != 4) {
    r->eof = 1;
    return 0;
  }

  uint32_t len = ((uint32_t)len_be[0] << 24) | ((uint32_t)len_be[1] << 16) |
                 ((uint32_t)len_be[2] << 8) | (uint32_t)len_be[3];

  if (len == 0 || len > 64 * 1024) {
    r->eof = 1;
    return 0;
  }

  uint8_t *buf = malloc(len);
  if (!buf) {
    r->eof = 1;
    return 0;
  }

  if (fread(buf, 1, len, r->fp) != len) {
    free(buf);
    r->eof = 1;
    return 0;
  }

  *out_data = buf;
  return len;
}

/* ====================================================================== */
/*  Progress display                                                      */
/* ====================================================================== */

/** Format the per-update middle string for the video progress bar. */
static void fmt_video_middle(char *out, size_t cap, int64_t frames_done, time_t start_time) {
  double elapsed = difftime(time(NULL), start_time);
  double fps = (elapsed > 0.5) ? (double)frames_done / elapsed : 0;
  snprintf(out, cap, "%lld frames  %.1f fps", (long long)frames_done, fps);
}

/**
 * @brief Offset plane pointers so sws_scale reads the crop window.
 *
 * Byte offsets must use the SOURCE format's per-plane sample step
 * (desc->comp[].step: 1 for 8-bit planar, 2 for 10-bit planar, and already
 * including UV interleaving on semi-planar layouts) — the encode target is
 * always 10-bit, so using the target's bytes-per-sample here doubles the
 * horizontal offset on 8-bit sources and shifts the picture.
 */
static void apply_crop_offset(const AVFrame *frame, enum AVPixelFormat pix_fmt, int crop_top,
                              int crop_left, const uint8_t *src_data[4]) {
  const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(pix_fmt);
  int h_shift = desc->log2_chroma_w;
  int v_shift = desc->log2_chroma_h;

  src_data[0] = frame->data[0] + crop_top * frame->linesize[0] + crop_left * desc->comp[0].step;
  if (frame->data[1])
    src_data[1] = frame->data[1] + (crop_top >> v_shift) * frame->linesize[1] +
                  (crop_left >> h_shift) * desc->comp[1].step;
  if (frame->data[2])
    src_data[2] = frame->data[2] + (crop_top >> v_shift) * frame->linesize[2] +
                  (crop_left >> h_shift) * desc->comp[2].step;
}

/**
 * @brief Carry HDR10 static metadata from the source stream to the output.
 *
 * set_hdr10_metadata() hands the mastering display and content light values
 * to SVT-AV1 so they land in the bitstream, but the muxer builds Matroska's
 * Colour element from the *output* stream's coded side data. Copying only
 * color_primaries/trc/space/range leaves that side data empty, so MaxCLL,
 * MaxFALL and the mastering display luminance/primaries silently disappear
 * from the remuxed file even though the source carried them.
 */
static void copy_hdr_static_metadata(AVStream *out_stream, const AVStream *in_stream) {
  static const enum AVPacketSideDataType hdr_types[] = {
      AV_PKT_DATA_MASTERING_DISPLAY_METADATA,
      AV_PKT_DATA_CONTENT_LIGHT_LEVEL,
  };
  const AVCodecParameters *in_par = in_stream->codecpar;

  for (size_t k = 0; k < sizeof(hdr_types) / sizeof(hdr_types[0]); k++) {
    for (int i = 0; i < in_par->nb_coded_side_data; i++) {
      if (in_par->coded_side_data[i].type != hdr_types[k])
        continue;

      /* av_packet_side_data_add() takes ownership on success, so hand it a
         copy and free that copy ourselves if it refuses. */
      uint8_t *copy = av_memdup(in_par->coded_side_data[i].data, in_par->coded_side_data[i].size);
      if (!copy)
        break;
      if (!av_packet_side_data_add(&out_stream->codecpar->coded_side_data,
                                   &out_stream->codecpar->nb_coded_side_data, hdr_types[k], copy,
                                   in_par->coded_side_data[i].size, 0))
        av_free(copy);
      break;
    }
  }
}

/* ====================================================================== */
/*  Main encode function                                                  */
/* ====================================================================== */

VideoEncodeResult encode_video(const VideoEncodeConfig *config) {
  VideoEncodeResult result = {.error = 0, .skipped = 0, .frames_encoded = 0, .bytes_written = 0};

  /* Skip if output already exists. */
  struct stat st;
  if (stat(config->output_path, &st) == 0 && st.st_size > 0) {
    result.skipped = 1;
    return result;
  }

  /* State variables */
  AVFormatContext *ifmt_ctx = NULL;
  AVCodecContext *dec_ctx = NULL;
  AVFormatContext *ofmt_ctx = NULL;
  AVStream *out_stream = NULL;
  struct SwsContext *sws_ctx = NULL;
  EbComponentType *svt_handle = NULL;
  EbSvtAv1EncConfiguration svt_config;
  AVFrame *frame = NULL;
  AVFrame *cropped_frame = NULL;
  AVPacket *pkt = NULL;
  RpuReader rpu_reader = {.fp = NULL, .eof = 1};
  int video_idx = -1;
  char errbuf[AV_ERROR_MAX_STRING_SIZE];
  int ret;

  /* Computed dimensions after crop */
  int crop_top = 0, crop_bottom = 0, crop_left = 0, crop_right = 0;
  if (config->crop && config->crop->error == 0) {
    crop_top = config->crop->top;
    crop_bottom = config->crop->bottom;
    crop_left = config->crop->left;
    crop_right = config->crop->right;
  }

  int src_w = config->info->width;
  int src_h = config->info->height;
  int out_w = src_w - crop_left - crop_right;
  int out_h = src_h - crop_top - crop_bottom;

  /* Ensure dimensions are even for YUV420 */
  out_w &= ~1;
  out_h &= ~1;

  /* Downscale target: if scale_width/height set, encoder receives a smaller
     frame; sws_scale converts crop_w×crop_h → dst_w×dst_h in one pass. */
  int dst_w = (config->scale_width > 0) ? (config->scale_width & ~1) : out_w;
  int dst_h = (config->scale_height > 0) ? (config->scale_height & ~1) : out_h;

  /* SVT-AV1 internally pads dimensions up to superblock alignment (up to
     64 pixels). Allocate frame buffers with this padding so the encoder
     never reads past the end. */
  int padded_h = (dst_h + 63) & ~63;

  if (dst_w < 64 || dst_h < 64) {
    (void)fprintf(stderr, "  Video Error: output dimensions %dx%d too small\n", dst_w, dst_h);
    result.error = -1;
    return result;
  }

  /* ---- Open input ---- */
  ret = avformat_open_input(&ifmt_ctx, config->input_path, NULL, NULL);
  if (ret < 0) {
    av_make_error_string(errbuf, sizeof(errbuf), ret);
    (void)fprintf(stderr, "  Video Error: cannot open '%s': %s\n", config->input_path, errbuf);
    result.error = ret;
    return result;
  }

  ret = avformat_find_stream_info(ifmt_ctx, NULL);
  if (ret < 0) {
    av_make_error_string(errbuf, sizeof(errbuf), ret);
    (void)fprintf(stderr, "  Video Error: cannot read streams: %s\n", errbuf);
    result.error = ret;
    goto cleanup;
  }

  /* Find video stream */
  const AVCodec *decoder = NULL;
  video_idx = av_find_best_stream(ifmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
  if (video_idx < 0) {
    (void)fprintf(stderr, "  Video Error: no video stream found\n");
    result.error = -1;
    goto cleanup;
  }

  AVStream *in_stream = ifmt_ctx->streams[video_idx];

  /* Setup decoder */
  dec_ctx = avcodec_alloc_context3(decoder);
  if (!dec_ctx) {
    result.error = -1;
    goto cleanup;
  }
  avcodec_parameters_to_context(dec_ctx, in_stream->codecpar);
  dec_ctx->pkt_timebase = in_stream->time_base;

  ret = avcodec_open2(dec_ctx, decoder, NULL);
  if (ret < 0) {
    av_make_error_string(errbuf, sizeof(errbuf), ret);
    (void)fprintf(stderr, "  Video Error: cannot open decoder: %s\n", errbuf);
    result.error = ret;
    goto cleanup;
  }

  /* Always encode in 10-bit for best quality and SVT-AV1 compatibility.
     swscale handles the conversion from any source format. */
  enum AVPixelFormat target_pix_fmt = AV_PIX_FMT_YUV420P10LE;
  int bit_depth = 10;
  int bytes_per_sample = 2;

  /* Always use swscale to copy frames into our padded buffer.
     This guarantees the buffer is large enough for SVT-AV1's internal
     alignment regardless of how FFmpeg allocated the decoder frame. */
  /* Input: cropped source dims. Output: dst dims (== cropped if no downscale,
     or smaller when --companion-hd / --scale-to-hd is active). */
  sws_ctx = sws_getContext(out_w, out_h, dec_ctx->pix_fmt, dst_w, dst_h, target_pix_fmt,
                           SWS_LANCZOS, NULL, NULL, NULL);
  if (!sws_ctx) {
    (void)fprintf(stderr, "  Video Error: cannot create swscale context\n");
    result.error = -1;
    goto cleanup;
  }
  int need_crop = (crop_top || crop_bottom || crop_left || crop_right);

  /* ---- Initialize SVT-AV1-HDR encoder ---- */
  svt_av1_set_log_callback(svt_log_callback, NULL);
  ret = svt_av1_enc_init_handle(&svt_handle, &svt_config);
  if (ret != EB_ErrorNone) {
    (void)fprintf(stderr, "  Video Error: svt_av1_enc_init_handle failed (%d)\n", ret);
    result.error = -1;
    goto cleanup;
  }

  /* Set dimensions and framerate */
  svt_config.source_width = (uint32_t)dst_w;
  svt_config.source_height = (uint32_t)dst_h;
  svt_config.encoder_bit_depth = (uint32_t)bit_depth;
  svt_config.encoder_color_format = EB_YUV420;

  /* Frame rate from source */
  if (in_stream->avg_frame_rate.num > 0 && in_stream->avg_frame_rate.den > 0) {
    svt_config.frame_rate_numerator = (uint32_t)in_stream->avg_frame_rate.num;
    svt_config.frame_rate_denominator = (uint32_t)in_stream->avg_frame_rate.den;
  } else if (in_stream->r_frame_rate.num > 0 && in_stream->r_frame_rate.den > 0) {
    svt_config.frame_rate_numerator = (uint32_t)in_stream->r_frame_rate.num;
    svt_config.frame_rate_denominator = (uint32_t)in_stream->r_frame_rate.den;
  }

  /* Color info passthrough */
  copy_color_info(&svt_config, in_stream->codecpar);

  /* HDR10 static metadata (must be set before encoder init) */
  set_hdr10_metadata(&svt_config, in_stream);

  /* Apply quality preset */
  apply_preset_to_config(&svt_config, config->preset, config->film_grain, config->target_bitrate,
                         config->crf);

  /* PQ content adjustments (HDR10 / HDR10+ / Dolby Vision) */
  if (in_stream->codecpar->color_trc == AVCOL_TRC_SMPTE2084) {
    svt_config.variance_boost_curve = 3;
    svt_config.luminance_qp_bias = 0;

    /* Boost ac_bias for HDR's wider dynamic range (tune 5 handles this
     * internally via its own HDR ac_bias escalation). */
    if (svt_config.tune != 5)
      svt_config.ac_bias = fmin(svt_config.ac_bias * 1.5, 8.0);

    /* Gentler temporal filtering — HDR highlight/shadow detail is more
     * visible and more sensitive to TF smearing. */
    if (svt_config.tf_strength > 0)
      svt_config.tf_strength--;
    if (svt_config.kf_tf_strength > 1)
      svt_config.kf_tf_strength--;

    /* Tighter QP compression for HDR temporal consistency */
    if (svt_config.qp_scale_compress_strength < 2.5)
      svt_config.qp_scale_compress_strength += 0.5;
  }

  /* ---- Grain-adaptive encoder tuning ----
   *
   * Adjusts multiple encoder knobs based on the measured grain score to
   * extract the most quality at the target bitrate.  The preset provides
   * the baseline; these overrides fine-tune for the actual content.
   *
   * Bands:
   *   Low  (<0.08): clean digital — keep preset defaults, let CDEF and
   *                  restoration do their job.
   *   Med  (0.08–0.15): moderate noise/texture — ease off filters that
   *                  would blur grain, gently boost texture preservation.
   *   High (>0.15): heavy grain — aggressively preserve texture, disable
   *                  restoration, minimize temporal filtering smear.
   */
  double gs = config->grain_score;

  if (gs > 0.15) {
    /* --- High grain — emulate tune 5 (the Film Grain tune) ---
     *
     * SVT-AV1's tune 5 is shorthand for: --tune 0 --enable-tf 0
     * --enable-restoration 0 --enable-cdef 0 --complex-hvs 1 --tx-bias 1
     * --ac-bias 4.00. For grainy content, juliobbv-p's fork docs explicitly
     * recommend it. Live-action presets ship with tune 0 by default; here
     * we apply the same effect dynamically when we measure heavy grain on
     * any preset. The analog film presets (Super 35 Analog / IMAX Analog)
     * already use tune 5 in their preset config, so this block stacks
     * harmlessly on them. */

    /* CDEF: disable entirely (tune 5). */
    svt_config.cdef_scaling = 4; /* min "active" value; SVT clamps to off
                                    above */
    /* Temporal filter: off (tune 5). */
    svt_config.enable_tf = 0;
    svt_config.tf_strength = 0;
    svt_config.kf_tf_strength = 0;

    /* Restoration filter: OFF (tune 5). */
    svt_config.enable_restoration_filtering = 0;

    /* Complex HVS: on (tune 5). */
    svt_config.complex_hvs = 1;

    /* AC bias: pin to 4.0 (tune 5's exact value). */
    if (svt_config.ac_bias < 4.0)
      svt_config.ac_bias = 4.0;

    /* Noise normalization: boost to preserve AC detail in RD decisions. */
    if (svt_config.noise_norm_strength < 4)
      svt_config.noise_norm_strength++;

    /* Noise-adaptive filtering: CDEF-only mode (skip restoration). */
    svt_config.noise_adaptive_filtering = 3;

    /* QP compression: tighten for temporal consistency on grainy content. */
    svt_config.qp_scale_compress_strength = fmin(svt_config.qp_scale_compress_strength + 0.5, 8.0);

  } else if (gs > 0.08) {
    /* --- Medium grain --- */

    /* CDEF: moderate reduction. */
    int cd = (int)svt_config.cdef_scaling - 3;
    svt_config.cdef_scaling = (uint8_t)(cd < 4 ? 4 : cd);

    /* Temporal filter: reduce by one step if active. */
    if (svt_config.enable_tf && svt_config.tf_strength > 0)
      svt_config.tf_strength--;
    if (svt_config.kf_tf_strength > 1)
      svt_config.kf_tf_strength--;

    /* Noise normalization: slight boost. */
    if (svt_config.noise_norm_strength < 3)
      svt_config.noise_norm_strength++;

    /* AC bias: gentle boost. */
    svt_config.ac_bias = fmin(svt_config.ac_bias + 0.5, 8.0);

    /* QP compression: slight tighten. */
    svt_config.qp_scale_compress_strength = fmin(svt_config.qp_scale_compress_strength + 0.3, 8.0);
  }
  /* Low grain (<0.08): keep preset defaults — no adjustments needed. */

  ret = svt_av1_enc_set_parameter(svt_handle, &svt_config);
  if (ret != EB_ErrorNone) {
    (void)fprintf(stderr, "  Video Error: svt_av1_enc_set_parameter failed (%d)\n", ret);
    result.error = -1;
    goto cleanup;
  }

  ret = svt_av1_enc_init(svt_handle);
  if (ret != EB_ErrorNone) {
    (void)fprintf(stderr, "  Video Error: svt_av1_enc_init failed (%d)\n", ret);
    result.error = -1;
    goto cleanup;
  }

  /* ---- Set up output MKV muxer ---- */
  ret = avformat_alloc_output_context2(&ofmt_ctx, NULL, "matroska", config->output_path);
  if (ret < 0 || !ofmt_ctx) {
    (void)fprintf(stderr, "  Video Error: cannot create output context\n");
    result.error = -1;
    goto cleanup;
  }

  out_stream = avformat_new_stream(ofmt_ctx, NULL);
  if (!out_stream) {
    result.error = -1;
    goto cleanup;
  }

  out_stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
  out_stream->codecpar->codec_id = AV_CODEC_ID_AV1;
  out_stream->codecpar->width = dst_w;
  out_stream->codecpar->height = dst_h;
  out_stream->codecpar->format = target_pix_fmt;
  out_stream->codecpar->color_primaries = in_stream->codecpar->color_primaries;
  out_stream->codecpar->color_trc = in_stream->codecpar->color_trc;
  out_stream->codecpar->color_space = in_stream->codecpar->color_space;
  out_stream->codecpar->color_range = in_stream->codecpar->color_range;
  copy_hdr_static_metadata(out_stream, in_stream);

  /* Time base: use the encoder's frame rate */
  out_stream->time_base =
      (AVRational){(int)svt_config.frame_rate_denominator, (int)svt_config.frame_rate_numerator};

  if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
    ret = avio_open(&ofmt_ctx->pb, config->output_path, AVIO_FLAG_WRITE);
    if (ret < 0) {
      av_make_error_string(errbuf, sizeof(errbuf), ret);
      (void)fprintf(stderr, "  Video Error: cannot open output '%s': %s\n", config->output_path,
                    errbuf);
      result.error = ret;
      goto cleanup;
    }
  }

  /* We'll write the header after getting the first encoded packet
     (to extract the sequence header for extradata). */
  int header_written = 0;

  /* ---- Open RPU reader ---- */
  if (config->rpu_path)
    rpu_reader = rpu_reader_open(config->rpu_path);

  /* ---- Allocate frames & packets ---- */
  frame = av_frame_alloc();
  cropped_frame = av_frame_alloc();
  pkt = av_packet_alloc();
  if (!frame || !cropped_frame || !pkt) {
    result.error = -1;
    goto cleanup;
  }

  /* Allocate output frame buffer (padded to SVT-AV1 alignment).
     Width/height reflect the final dst dimensions (after any downscale). */
  cropped_frame->format = target_pix_fmt;
  cropped_frame->width = dst_w;
  cropped_frame->height = padded_h;
  ret = av_frame_get_buffer(cropped_frame, 32);
  if (ret < 0) {
    result.error = ret;
    goto cleanup;
  }

  /* Total frames estimate for progress */
  int64_t total_frames = 0;
  if (config->info->duration > 0 && config->info->framerate > 0)
    total_frames = (int64_t)(config->info->duration * config->info->framerate);

  time_t start_time = time(NULL);
  UiProgress progress;
  ui_progress_start(&progress, (long long)total_frames);
  time_t last_progress = 0;
  int64_t frame_number = 0;
  int pic_send_done = 0;

  /* ---- Decode → Encode loop ---- */
  while (av_read_frame(ifmt_ctx, pkt) >= 0) {
    if (pkt->stream_index != video_idx) {
      av_packet_unref(pkt);
      continue;
    }

    ret = avcodec_send_packet(dec_ctx, pkt);
    av_packet_unref(pkt);
    if (ret < 0)
      continue;

    while (avcodec_receive_frame(dec_ctx, frame) == 0) {
      /* Copy frame into padded buffer, applying crop + format conversion */
      {
        const uint8_t *src_data[4] = {NULL};
        int src_linesize[4] = {0};
        for (int p = 0; p < 4; p++) {
          src_data[p] = frame->data[p];
          src_linesize[p] = frame->linesize[p];
        }

        if (need_crop && (crop_top || crop_left))
          apply_crop_offset(frame, dec_ctx->pix_fmt, crop_top, crop_left, src_data);

        sws_scale(sws_ctx, src_data, src_linesize, 0, out_h, cropped_frame->data,
                  cropped_frame->linesize);
      }
      AVFrame *enc_input = cropped_frame;

      /* Build SVT-AV1 input buffer */
      EbBufferHeaderType input_buf;
      memset(&input_buf, 0, sizeof(input_buf));
      input_buf.size = sizeof(EbBufferHeaderType);

      EbSvtIOFormat io_fmt;
      io_fmt.luma = enc_input->data[0];
      io_fmt.cb = enc_input->data[1];
      io_fmt.cr = enc_input->data[2];
      io_fmt.y_stride = (uint32_t)(enc_input->linesize[0] / bytes_per_sample);
      io_fmt.cb_stride = (uint32_t)(enc_input->linesize[1] / bytes_per_sample);
      io_fmt.cr_stride = (uint32_t)(enc_input->linesize[2] / bytes_per_sample);

      input_buf.p_buffer = (uint8_t *)&io_fmt;
      input_buf.n_filled_len = (uint32_t)(dst_w * dst_h * bytes_per_sample * 3 / 2);
      input_buf.pts = frame_number;
      input_buf.pic_type = EB_AV1_INVALID_PICTURE;
      input_buf.flags = 0;
      input_buf.metadata = NULL;

      /* Attach Dolby Vision RPU as T.35 metadata */
      uint8_t *rpu_data = NULL;
      size_t rpu_size = rpu_reader_next(&rpu_reader, &rpu_data);
      if (rpu_data && rpu_size > 0) {
        /* Parse RPU, convert to AV1 T.35 OBU, then attach */
        DoviRpuOpaque *rpu = dovi_parse_rpu(rpu_data, rpu_size);
        if (rpu) {
          const char *err = dovi_rpu_get_error(rpu);
          if (!err) {
            /* Convert RPU to profile 8.1 for AV1 */
            dovi_convert_rpu_with_mode(rpu, 2);

            const DoviData *t35 = dovi_write_av1_rpu_metadata_obu_t35_complete(rpu);
            if (t35 && t35->data && t35->len > 0) {
              svt_add_metadata(&input_buf, EB_AV1_METADATA_TYPE_ITUT_T35, t35->data, t35->len);
              dovi_data_free(t35);
            }
          }
          dovi_rpu_free(rpu);
        }
        free(rpu_data);
      }

      /* Send frame to encoder */
      ret = svt_av1_enc_send_picture(svt_handle, &input_buf);

      /* SVT-AV1 takes ownership of metadata via send_picture — do not free */

      if (ret != EB_ErrorNone) {
        (void)fprintf(stderr, "  Video Error: send_picture failed (%d)\n", ret);
        av_frame_unref(frame);
        result.error = -1;
        goto flush_encoder;
      }

      frame_number++;

      /* Drain available output packets (non-blocking) */
      EbBufferHeaderType *out_pkt = NULL;
      while (svt_av1_enc_get_packet(svt_handle, &out_pkt, 0) == EB_ErrorNone) {
        if (out_pkt->n_filled_len > 0) {
          /* Write MKV header before first packet */
          if (!header_written) {
            /* Copy sequence header as extradata if present */
            if (out_pkt->flags & EB_BUFFERFLAG_HAS_TD) {
              /* The first packet with TD contains the sequence header */
              out_stream->codecpar->extradata =
                  av_mallocz(out_pkt->n_filled_len + AV_INPUT_BUFFER_PADDING_SIZE);
              if (out_stream->codecpar->extradata) {
                memcpy(out_stream->codecpar->extradata, out_pkt->p_buffer, out_pkt->n_filled_len);
                out_stream->codecpar->extradata_size = (int)out_pkt->n_filled_len;
              }
            }

            ret = avformat_write_header(ofmt_ctx, NULL);
            if (ret < 0) {
              av_make_error_string(errbuf, sizeof(errbuf), ret);
              (void)fprintf(stderr, "  Video Error: cannot write output header: %s\n", errbuf);
              svt_av1_enc_release_out_buffer(&out_pkt);
              result.error = ret;
              goto cleanup;
            }
            header_written = 1;
          }

          /* Write encoded packet */
          AVPacket *out_av_pkt = av_packet_alloc();
          if (out_av_pkt) {
            out_av_pkt->data = out_pkt->p_buffer;
            out_av_pkt->size = (int)out_pkt->n_filled_len;
            out_av_pkt->pts = out_pkt->pts;
            out_av_pkt->dts = out_pkt->dts;
            out_av_pkt->stream_index = out_stream->index;

            if (out_pkt->pic_type == EB_AV1_KEY_PICTURE ||
                out_pkt->pic_type == EB_AV1_INTRA_ONLY_PICTURE)
              out_av_pkt->flags |= AV_PKT_FLAG_KEY;

            /* Rescale timestamps */
            AVRational svt_tb = {
                (int)svt_config.frame_rate_denominator,
                (int)svt_config.frame_rate_numerator,
            };
            av_packet_rescale_ts(out_av_pkt, svt_tb, out_stream->time_base);

            av_interleaved_write_frame(ofmt_ctx, out_av_pkt);
            result.bytes_written += out_pkt->n_filled_len;
            result.frames_encoded++;

            av_packet_free(&out_av_pkt);
          }
        }

        svt_av1_enc_release_out_buffer(&out_pkt);
      }

      av_frame_unref(frame);

      /* Progress */
      time_t now = time(NULL);
      if (now != last_progress) {
        char middle[64];
        fmt_video_middle(middle, sizeof(middle), frame_number, start_time);
        ui_progress_update(&progress, (long long)frame_number, middle);
        last_progress = now;
      }
    }
  }

  /* Flush decoder */
  avcodec_send_packet(dec_ctx, NULL);
  while (avcodec_receive_frame(dec_ctx, frame) == 0) {
    /* Copy frame into padded buffer, applying crop + format conversion */
    {
      const uint8_t *src_data[4] = {NULL};
      int src_linesize[4] = {0};
      for (int p = 0; p < 4; p++) {
        src_data[p] = frame->data[p];
        src_linesize[p] = frame->linesize[p];
      }
      if (need_crop && (crop_top || crop_left))
        apply_crop_offset(frame, dec_ctx->pix_fmt, crop_top, crop_left, src_data);
      sws_scale(sws_ctx, src_data, src_linesize, 0, out_h, cropped_frame->data,
                cropped_frame->linesize);
    }
    AVFrame *enc_input = cropped_frame;

    EbBufferHeaderType input_buf;
    memset(&input_buf, 0, sizeof(input_buf));
    input_buf.size = sizeof(EbBufferHeaderType);

    EbSvtIOFormat io_fmt;
    io_fmt.luma = enc_input->data[0];
    io_fmt.cb = enc_input->data[1];
    io_fmt.cr = enc_input->data[2];
    io_fmt.y_stride = (uint32_t)(enc_input->linesize[0] / bytes_per_sample);
    io_fmt.cb_stride = (uint32_t)(enc_input->linesize[1] / bytes_per_sample);
    io_fmt.cr_stride = (uint32_t)(enc_input->linesize[2] / bytes_per_sample);

    input_buf.p_buffer = (uint8_t *)&io_fmt;
    input_buf.n_filled_len = (uint32_t)(out_w * out_h * bytes_per_sample * 3 / 2);
    input_buf.pts = frame_number;
    input_buf.pic_type = EB_AV1_INVALID_PICTURE;

    /* RPU for flushed frames too */
    uint8_t *rpu_data = NULL;
    size_t rpu_size = rpu_reader_next(&rpu_reader, &rpu_data);
    if (rpu_data && rpu_size > 0) {
      DoviRpuOpaque *rpu = dovi_parse_rpu(rpu_data, rpu_size);
      if (rpu) {
        const char *err = dovi_rpu_get_error(rpu);
        if (!err) {
          dovi_convert_rpu_with_mode(rpu, 2);
          const DoviData *t35 = dovi_write_av1_rpu_metadata_obu_t35_complete(rpu);
          if (t35 && t35->data && t35->len > 0) {
            svt_add_metadata(&input_buf, EB_AV1_METADATA_TYPE_ITUT_T35, t35->data, t35->len);
            dovi_data_free(t35);
          }
        }
        dovi_rpu_free(rpu);
      }
      free(rpu_data);
    }

    svt_av1_enc_send_picture(svt_handle, &input_buf);
    /* SVT-AV1 takes ownership of metadata — do not free */
    frame_number++;
    av_frame_unref(frame);
  }

flush_encoder:
  /* Signal end of stream to SVT-AV1 */
  {
    EbBufferHeaderType eos_buf;
    memset(&eos_buf, 0, sizeof(eos_buf));
    eos_buf.size = sizeof(EbBufferHeaderType);
    eos_buf.flags = EB_BUFFERFLAG_EOS;
    eos_buf.pic_type = EB_AV1_INVALID_PICTURE;
    svt_av1_enc_send_picture(svt_handle, &eos_buf);
  }
  pic_send_done = 1;

  /* Drain all remaining encoded packets */
  {
    EbBufferHeaderType *out_pkt = NULL;
    while (svt_av1_enc_get_packet(svt_handle, &out_pkt, (uint8_t)pic_send_done) == EB_ErrorNone) {
      if (out_pkt->flags & EB_BUFFERFLAG_EOS) {
        svt_av1_enc_release_out_buffer(&out_pkt);
        break;
      }

      if (out_pkt->n_filled_len > 0) {
        if (!header_written) {
          if (out_pkt->flags & EB_BUFFERFLAG_HAS_TD) {
            out_stream->codecpar->extradata =
                av_mallocz(out_pkt->n_filled_len + AV_INPUT_BUFFER_PADDING_SIZE);
            if (out_stream->codecpar->extradata) {
              memcpy(out_stream->codecpar->extradata, out_pkt->p_buffer, out_pkt->n_filled_len);
              out_stream->codecpar->extradata_size = (int)out_pkt->n_filled_len;
            }
          }
          ret = avformat_write_header(ofmt_ctx, NULL);
          if (ret < 0) {
            svt_av1_enc_release_out_buffer(&out_pkt);
            result.error = ret;
            goto cleanup;
          }
          header_written = 1;
        }

        AVPacket *out_av_pkt = av_packet_alloc();
        if (out_av_pkt) {
          out_av_pkt->data = out_pkt->p_buffer;
          out_av_pkt->size = (int)out_pkt->n_filled_len;
          out_av_pkt->pts = out_pkt->pts;
          out_av_pkt->dts = out_pkt->dts;
          out_av_pkt->stream_index = out_stream->index;

          if (out_pkt->pic_type == EB_AV1_KEY_PICTURE ||
              out_pkt->pic_type == EB_AV1_INTRA_ONLY_PICTURE)
            out_av_pkt->flags |= AV_PKT_FLAG_KEY;

          AVRational svt_tb = {
              (int)svt_config.frame_rate_denominator,
              (int)svt_config.frame_rate_numerator,
          };
          av_packet_rescale_ts(out_av_pkt, svt_tb, out_stream->time_base);

          av_interleaved_write_frame(ofmt_ctx, out_av_pkt);
          result.bytes_written += out_pkt->n_filled_len;
          result.frames_encoded++;

          av_packet_free(&out_av_pkt);
        }
      }

      svt_av1_enc_release_out_buffer(&out_pkt);
    }
  }

  /* Write MKV trailer */
  if (header_written)
    av_write_trailer(ofmt_ctx);

  /* Final progress */
  {
    char middle[64];
    snprintf(middle, sizeof(middle), "%lld frames", (long long)result.frames_encoded);
    ui_progress_done(&progress, (long long)result.frames_encoded, middle);
  }

cleanup:
  rpu_reader_close(&rpu_reader);
  av_frame_free(&frame);
  av_frame_free(&cropped_frame);
  av_packet_free(&pkt);

  if (svt_handle) {
    svt_av1_enc_deinit(svt_handle);
    svt_av1_enc_deinit_handle(svt_handle);
  }

  if (sws_ctx)
    sws_freeContext(sws_ctx);

  avcodec_free_context(&dec_ctx);

  if (ofmt_ctx) {
    if (ofmt_ctx->pb && !(ofmt_ctx->oformat->flags & AVFMT_NOFILE))
      avio_closep(&ofmt_ctx->pb);
    avformat_free_context(ofmt_ctx);
  }

  if (ifmt_ctx)
    avformat_close_input(&ifmt_ctx);

  /* Remove output on failure */
  if (result.error != 0)
    (void)remove(config->output_path); /* best-effort cleanup */

  return result;
}
