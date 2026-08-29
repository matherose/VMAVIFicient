/**
 * @file crf_search.c
 * @brief In-process CRF search via libSvtAv1Enc + libvmaf.
 *
 * For each trial CRF, picks short samples of the source, encodes them through
 * the vendored SVT-AV1-HDR encoder using the exact same configuration as the
 * final encode (apply_preset_to_config), decodes the AV1 back to YUV, and
 * scores against the reference with libvmaf using harmonic-mean pooling.
 *
 * Sampling is adaptive: starts with 1 sample for speed, escalates to 3 if the
 * 1-sample result lands outside the convergence band around the target.
 *
 * Replaces the legacy ab-av1 subprocess so the tool no longer needs a
 * matching system libsvtav1 on PATH.
 */

#include "crf_search.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>

#include <EbSvtAv1.h>
#include <EbSvtAv1Enc.h>
#include <EbSvtAv1Formats.h>

#include <libvmaf/libvmaf.h>

#include <stdarg.h>

#include "../video_encode/encoder_config.h"
#include "ui.h"

/* Silence the SVT-AV1 banner spam during probe trials unless --verbose.
 * Each trial calls svt_av1_enc_init_handle which re-emits the banner; with
 * 4 trials × 3 samples that's a lot of noise. */
__attribute__((format(printf, 4, 0))) static void
svt_probe_log_callback(void *ctx, SvtAv1LogLevel level, const char *tag, const char *fmt,
                       va_list args) {
  (void)ctx;
  (void)level;
  (void)tag;
  if (!ui_is_verbose())
    return;
  (void)vfprintf(stderr, fmt, args);
}

/* ====================================================================== */
/*  Tuning                                                                 */
/* ====================================================================== */

#define PROBE_SAMPLE_FRAMES 480 /* ~20s @ 24fps (matches ab-av1 default). */
#define PROBE_INITIAL_CRF 30
#define PROBE_MAX_TRIALS 4
#define PROBE_CONVERGE_VMAF 0.5 /* |vmaf - target| <= this => done */
#define PROBE_ESCALATE_VMAF 2.0 /* 1-sample acceptance band */
#define PROBE_DEFAULT_SLOPE 0.6 /* VMAF drop per +1 CRF, initial guess */
#define PROBE_LIBVMAF_THREADS 4
#define PROBE_CRF_MIN 18
#define PROBE_CRF_MAX 50

/* ====================================================================== */
/*  Helpers                                                                */
/* ====================================================================== */

static int round_clamp(double x, int lo, int hi) {
  int v = (int)lround(x);
  if (v < lo)
    v = lo;
  if (v > hi)
    v = hi;
  return v;
}

/* ====================================================================== */
/*  Source decode pipeline                                                 */
/* ====================================================================== */

typedef struct {
  AVFormatContext *ifmt;
  AVCodecContext *dec;
  int video_idx;
  AVRational fps;
  int64_t total_frames;
  int width;
  int height;
} Source;

static void close_source(Source *s) {
  if (!s)
    return;
  if (s->dec)
    avcodec_free_context(&s->dec);
  if (s->ifmt)
    avformat_close_input(&s->ifmt);
  memset(s, 0, sizeof(*s));
}

static int open_source(const char *path, Source *s) {
  memset(s, 0, sizeof(*s));
  if (avformat_open_input(&s->ifmt, path, NULL, NULL) < 0)
    return -1;
  if (avformat_find_stream_info(s->ifmt, NULL) < 0)
    return -1;

  s->video_idx = -1;
  for (unsigned i = 0; i < s->ifmt->nb_streams; i++) {
    if (s->ifmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      s->video_idx = (int)i;
      break;
    }
  }
  if (s->video_idx < 0)
    return -1;

  AVStream *st = s->ifmt->streams[s->video_idx];
  const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
  if (!codec)
    return -1;

  s->dec = avcodec_alloc_context3(codec);
  if (!s->dec)
    return -1;
  if (avcodec_parameters_to_context(s->dec, st->codecpar) < 0)
    return -1;
  s->dec->thread_count = 0;
  if (avcodec_open2(s->dec, codec, NULL) < 0)
    return -1;

  s->fps = av_guess_frame_rate(s->ifmt, st, NULL);
  if (s->fps.num == 0)
    s->fps = (AVRational){24000, 1001};
  s->width = s->dec->width;
  s->height = s->dec->height;
  s->total_frames =
      st->nb_frames > 0
          ? st->nb_frames
          : (int64_t)(av_q2d(s->fps) * ((double)s->ifmt->duration / (double)AV_TIME_BASE));
  return 0;
}

static int seek_to_frame(Source *s, int64_t target_frame) {
  AVStream *st = s->ifmt->streams[s->video_idx];
  double seconds = (double)target_frame / av_q2d(s->fps);
  int64_t ts = (int64_t)(seconds / av_q2d(st->time_base));
  avcodec_flush_buffers(s->dec);
  if (av_seek_frame(s->ifmt, s->video_idx, ts, AVSEEK_FLAG_BACKWARD) < 0)
    return -1;
  return 0;
}

/* ====================================================================== */
/*  Filter graph (vfilter)                                                 */
/* ====================================================================== */

typedef struct {
  AVFilterGraph *graph;
  AVFilterContext *src;
  AVFilterContext *sink;
  int out_w;
  int out_h;
} VFilter;

static void close_vfilter(VFilter *vf) {
  if (vf && vf->graph)
    avfilter_graph_free(&vf->graph);
  if (vf)
    memset(vf, 0, sizeof(*vf));
}

/**
 * Build:  buffer (from source codec) → user vfilter (or pass-through) → buffersink (yuv420p10le).
 */
static int build_vfilter(Source *s, const char *vfilter_expr, VFilter *vf) {
  memset(vf, 0, sizeof(*vf));

  vf->graph = avfilter_graph_alloc();
  if (!vf->graph)
    return -1;

  AVStream *st = s->ifmt->streams[s->video_idx];
  AVRational tb = st->time_base;
  AVRational sar =
      s->dec->sample_aspect_ratio.num ? s->dec->sample_aspect_ratio : (AVRational){1, 1};

  /* Use the codecpar's pix_fmt rather than dec_ctx->pix_fmt because the
   * decoder may not have decoded a frame yet (we just seeked) so dec_ctx
   * may report AV_PIX_FMT_NONE until the first frame comes out. */
  enum AVPixelFormat in_fmt = s->dec->pix_fmt != AV_PIX_FMT_NONE
                                  ? s->dec->pix_fmt
                                  : (enum AVPixelFormat)st->codecpar->format;
  if (in_fmt == AV_PIX_FMT_NONE) {
    (void)fprintf(stderr, "crf_search: build_vfilter: unknown source pix_fmt (dec=%d, par=%d)\n",
                  s->dec->pix_fmt, st->codecpar->format);
    return -1;
  }

  char src_args[256];
  snprintf(src_args, sizeof(src_args),
           "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d", s->dec->width,
           s->dec->height, in_fmt, tb.num, tb.den, sar.num, sar.den);

  const AVFilter *bsrc = avfilter_get_by_name("buffer");
  const AVFilter *bsink = avfilter_get_by_name("buffersink");
  if (!bsrc || !bsink) {
    (void)fprintf(stderr, "crf_search: build_vfilter: buffer/buffersink filter not found\n");
    return -1;
  }

  int r = avfilter_graph_create_filter(&vf->src, bsrc, "in", src_args, NULL, vf->graph);
  if (r < 0) {
    (void)fprintf(stderr, "crf_search: build_vfilter: create buffer failed (%d) args=%s\n", r,
                  src_args);
    return -1;
  }
  /* buffersink: alloc uninitialized → set pix_fmts → init. Options can't
   * be set on an already-initialized filter. */
  vf->sink = avfilter_graph_alloc_filter(vf->graph, bsink, "out");
  if (!vf->sink) {
    (void)fprintf(stderr, "crf_search: build_vfilter: alloc buffersink failed\n");
    return -1;
  }

  enum AVPixelFormat pixfmts[] = {AV_PIX_FMT_YUV420P10LE};
  r = av_opt_set_array(vf->sink, "pixel_formats", AV_OPT_SEARCH_CHILDREN, 0, 1,
                       AV_OPT_TYPE_PIXEL_FMT, pixfmts);
  if (r < 0) {
    (void)fprintf(stderr, "crf_search: build_vfilter: set pix_fmts failed (%d)\n", r);
    return -1;
  }
  r = avfilter_init_str(vf->sink, NULL);
  if (r < 0) {
    (void)fprintf(stderr, "crf_search: build_vfilter: init buffersink failed (%d)\n", r);
    return -1;
  }

  if (!vfilter_expr || !*vfilter_expr) {
    r = avfilter_link(vf->src, 0, vf->sink, 0);
    if (r < 0) {
      (void)fprintf(stderr, "crf_search: build_vfilter: avfilter_link failed (%d, in_fmt=%d %s)\n",
                    r, in_fmt, av_get_pix_fmt_name(in_fmt));
      return -1;
    }
  } else {
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs = avfilter_inout_alloc();
    if (!outputs || !inputs) {
      avfilter_inout_free(&outputs);
      avfilter_inout_free(&inputs);
      return -1;
    }
    outputs->name = av_strdup("in");
    outputs->filter_ctx = vf->src;
    outputs->pad_idx = 0;
    outputs->next = NULL;
    inputs->name = av_strdup("out");
    inputs->filter_ctx = vf->sink;
    inputs->pad_idx = 0;
    inputs->next = NULL;
    r = avfilter_graph_parse_ptr(vf->graph, vfilter_expr, &inputs, &outputs, NULL);
    avfilter_inout_free(&outputs);
    avfilter_inout_free(&inputs);
    if (r < 0) {
      (void)fprintf(stderr, "crf_search: build_vfilter: graph_parse_ptr failed (%d, expr=%s)\n", r,
                    vfilter_expr);
      return -1;
    }
  }

  r = avfilter_graph_config(vf->graph, NULL);
  if (r < 0) {
    char errbuf[256] = {0};
    av_strerror(r, errbuf, sizeof(errbuf));
    (void)fprintf(stderr, "crf_search: build_vfilter: graph_config failed (%d: %s) args=%s\n", r,
                  errbuf, src_args);
    return -1;
  }

  vf->out_w = av_buffersink_get_w(vf->sink);
  vf->out_h = av_buffersink_get_h(vf->sink);
  return 0;
}

/* ====================================================================== */
/*  Sample selection                                                       */
/* ====================================================================== */

static int pick_samples(int64_t total_frames, int n, int frames_per_sample, int64_t *out_starts) {
  if (n < 1)
    n = 1;
  int64_t margin = total_frames / 10;
  int64_t lo = margin;
  int64_t hi = total_frames - margin - frames_per_sample;
  if (hi <= lo) {
    out_starts[0] = total_frames > frames_per_sample ? (total_frames - frames_per_sample) / 2 : 0;
    return 1;
  }
  if (n == 1) {
    out_starts[0] = (lo + hi) / 2;
    return 1;
  }
  int64_t step = (hi - lo) / (n - 1);
  for (int i = 0; i < n; i++)
    out_starts[i] = lo + step * i;
  return n;
}

/* ====================================================================== */
/*  AV1 packet store — each entry is one SVT output buffer                 */
/* ====================================================================== */

typedef struct {
  AVPacket **pkts;
  int num;
  int cap;
} PktList;

static int pl_append(PktList *pl, const uint8_t *data, size_t len, int64_t pts) {
  if (pl->num == pl->cap) {
    int new_cap = pl->cap ? pl->cap * 2 : 64;
    AVPacket **p = realloc(pl->pkts, (size_t)new_cap * sizeof(*p));
    if (!p)
      return -1;
    pl->pkts = p;
    pl->cap = new_cap;
  }
  AVPacket *pkt = av_packet_alloc();
  if (!pkt)
    return -1;
  if (av_new_packet(pkt, (int)len) < 0) {
    av_packet_free(&pkt);
    return -1;
  }
  memcpy(pkt->data, data, len);
  pkt->pts = pts;
  pkt->dts = pts;
  pl->pkts[pl->num++] = pkt;
  return 0;
}

static void pl_free(PktList *pl) {
  for (int i = 0; i < pl->num; i++)
    av_packet_free(&pl->pkts[i]);
  free(pl->pkts);
  memset(pl, 0, sizeof(*pl));
}

/* ====================================================================== */
/*  VMAF picture copy                                                      */
/* ====================================================================== */

static int copy_avframe_to_vmaf(const AVFrame *f, VmafPicture *pic) {
  if (vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, 10, (unsigned)f->width, (unsigned)f->height) <
      0)
    return -1;
  const int planes_h[3] = {f->height, f->height / 2, f->height / 2};
  const int planes_w[3] = {f->width, f->width / 2, f->width / 2};
  for (int p = 0; p < 3; p++) {
    const uint8_t *src = f->data[p];
    uint8_t *dst = pic->data[p];
    size_t row_bytes = (size_t)planes_w[p] * 2; /* 10-bit packed in 16 */
    for (int y = 0; y < planes_h[p]; y++) {
      memcpy(dst + (size_t)y * (size_t)pic->stride[p], src + (size_t)y * (size_t)f->linesize[p],
             row_bytes);
    }
  }
  return 0;
}

/* ====================================================================== */
/*  Probe encode — one sample → list of AV1 packets                        */
/* ====================================================================== */

static int encode_sample(const char *input_path, int64_t start_frame, int frames,
                         const EncodePreset *p, int film_grain, int crf, const char *vfilter_expr,
                         PktList *out) {
  int rc = -1;
  Source src = {0};
  VFilter vf = {0};
  AVPacket *pkt = NULL;
  AVFrame *raw = NULL;
  AVFrame *flt = NULL;
  EbComponentType *svt = NULL;
  bool svt_inited = false;

  if (open_source(input_path, &src) != 0) {
    (void)fprintf(stderr, "crf_search: encode_sample: open_source failed\n");
    goto done;
  }
  if (seek_to_frame(&src, start_frame) != 0) {
    (void)fprintf(stderr, "crf_search: encode_sample: seek_to_frame %lld failed\n",
                  (long long)start_frame);
    goto done;
  }
  if (build_vfilter(&src, vfilter_expr, &vf) != 0) {
    (void)fprintf(stderr, "crf_search: encode_sample: build_vfilter failed (expr=%s)\n",
                  vfilter_expr ? vfilter_expr : "(null)");
    goto done;
  }

  EbSvtAv1EncConfiguration cfg;
  memset(&cfg, 0, sizeof(cfg));
  EbErrorType svt_rc = svt_av1_enc_init_handle(&svt, &cfg);
  if (svt_rc != EB_ErrorNone) {
    (void)fprintf(stderr, "crf_search: svt_av1_enc_init_handle failed (%d)\n", svt_rc);
    goto done;
  }
  svt_inited = true;

  cfg.source_width = (uint32_t)vf.out_w;
  cfg.source_height = (uint32_t)vf.out_h;
  cfg.encoder_bit_depth = 10;
  cfg.encoder_color_format = EB_YUV420;
  cfg.frame_rate_numerator = (uint32_t)src.fps.num;
  cfg.frame_rate_denominator = (uint32_t)src.fps.den;

  apply_preset_to_config(&cfg, p, film_grain, 0, crf);
  copy_color_info(&cfg, src.ifmt->streams[src.video_idx]->codecpar);
  set_hdr10_metadata(&cfg, src.ifmt->streams[src.video_idx]);

  svt_rc = svt_av1_enc_set_parameter(svt, &cfg);
  if (svt_rc != EB_ErrorNone) {
    (void)fprintf(stderr, "crf_search: svt_av1_enc_set_parameter failed (%d) — %dx%d, crf=%d\n",
                  svt_rc, vf.out_w, vf.out_h, crf);
    goto done;
  }
  svt_rc = svt_av1_enc_init(svt);
  if (svt_rc != EB_ErrorNone) {
    (void)fprintf(stderr, "crf_search: svt_av1_enc_init failed (%d)\n", svt_rc);
    goto done;
  }

  pkt = av_packet_alloc();
  raw = av_frame_alloc();
  flt = av_frame_alloc();
  if (!pkt || !raw || !flt)
    goto done;

  int frames_pushed = 0;
  bool src_eof = false;

  while (frames_pushed < frames && !src_eof) {
    int r = av_read_frame(src.ifmt, pkt);
    if (r == AVERROR_EOF) {
      src_eof = true;
    } else if (r < 0) {
      goto done;
    }

    if (r >= 0 && pkt->stream_index != src.video_idx) {
      av_packet_unref(pkt);
      continue;
    }

    int sent =
        (int)src_eof ? avcodec_send_packet(src.dec, NULL) : avcodec_send_packet(src.dec, pkt);
    av_packet_unref(pkt);
    if (sent < 0 && sent != AVERROR_EOF)
      goto done;

    while (frames_pushed < frames) {
      int dr = avcodec_receive_frame(src.dec, raw);
      if (dr == AVERROR(EAGAIN))
        break;
      if (dr == AVERROR_EOF) {
        src_eof = true;
        break;
      }
      if (dr < 0)
        goto done;

      if (av_buffersrc_add_frame(vf.src, raw) < 0) {
        av_frame_unref(raw);
        goto done;
      }
      av_frame_unref(raw);

      while (frames_pushed < frames) {
        int fr = av_buffersink_get_frame(vf.sink, flt);
        if (fr == AVERROR(EAGAIN) || fr == AVERROR_EOF)
          break;
        if (fr < 0)
          goto done;

        EbBufferHeaderType input_buf;
        memset(&input_buf, 0, sizeof(input_buf));
        input_buf.size = sizeof(EbBufferHeaderType);

        EbSvtIOFormat io_fmt;
        io_fmt.luma = flt->data[0];
        io_fmt.cb = flt->data[1];
        io_fmt.cr = flt->data[2];
        io_fmt.y_stride = (uint32_t)(flt->linesize[0] / 2);
        io_fmt.cb_stride = (uint32_t)(flt->linesize[1] / 2);
        io_fmt.cr_stride = (uint32_t)(flt->linesize[2] / 2);

        input_buf.p_buffer = (uint8_t *)&io_fmt;
        input_buf.n_filled_len = (uint32_t)(flt->width * flt->height * 2 * 3 / 2);
        input_buf.pts = frames_pushed;
        input_buf.pic_type = EB_AV1_INVALID_PICTURE;
        input_buf.flags = 0;
        input_buf.metadata = NULL;

        if (svt_av1_enc_send_picture(svt, &input_buf) != EB_ErrorNone) {
          av_frame_unref(flt);
          goto done;
        }
        frames_pushed++;
        av_frame_unref(flt);

        /* Non-blocking drain — pick up whatever the encoder is willing to
         * release so far. */
        for (;;) {
          EbBufferHeaderType *out_pkt = NULL;
          EbErrorType e = svt_av1_enc_get_packet(svt, &out_pkt, 0);
          if (e != EB_ErrorNone || !out_pkt)
            break;
          if (out_pkt->n_filled_len > 0)
            pl_append(out, out_pkt->p_buffer, out_pkt->n_filled_len, out_pkt->pts);
          svt_av1_enc_release_out_buffer(&out_pkt);
        }
      }
    }
  }

  /* EOS + blocking drain. */
  EbBufferHeaderType eos;
  memset(&eos, 0, sizeof(eos));
  eos.size = sizeof(EbBufferHeaderType);
  eos.flags = EB_BUFFERFLAG_EOS;
  eos.pic_type = EB_AV1_INVALID_PICTURE;
  svt_av1_enc_send_picture(svt, &eos);

  for (;;) {
    EbBufferHeaderType *out_pkt = NULL;
    EbErrorType e = svt_av1_enc_get_packet(svt, &out_pkt, 1);
    if (e != EB_ErrorNone || !out_pkt)
      break;
    bool last = (out_pkt->flags & EB_BUFFERFLAG_EOS) != 0;
    if (out_pkt->n_filled_len > 0)
      pl_append(out, out_pkt->p_buffer, out_pkt->n_filled_len, out_pkt->pts);
    svt_av1_enc_release_out_buffer(&out_pkt);
    if (last)
      break;
  }

  rc = 0;

done:
  if (svt_inited) {
    svt_av1_enc_deinit(svt);
    svt_av1_enc_deinit_handle(svt);
  }
  if (flt)
    av_frame_free(&flt);
  if (raw)
    av_frame_free(&raw);
  if (pkt)
    av_packet_free(&pkt);
  close_vfilter(&vf);
  close_source(&src);
  return rc;
}

/* ====================================================================== */
/*  Score — open AV1 decoder, decode in lockstep with source, feed VMAF    */
/* ====================================================================== */

static int score_sample(const char *input_path, int64_t start_frame, int frames,
                        const char *vfilter_expr, const PktList *av1, VmafContext *vmaf,
                        unsigned base_index, int *frames_scored_out) {
  int rc = -1;
  Source src = {0};
  VFilter vf = {0};
  AVCodecContext *av1_dec = NULL;
  AVPacket *pkt_src = NULL;
  AVFrame *raw_src = NULL;
  AVFrame *dec_av1 = NULL;
  AVFrame **src_buf = NULL;
  int src_count = 0;
  int frames_scored = 0;

  if (open_source(input_path, &src) != 0) {
    (void)fprintf(stderr, "crf_search: score_sample: open_source failed\n");
    goto done;
  }
  if (seek_to_frame(&src, start_frame) != 0) {
    (void)fprintf(stderr, "crf_search: score_sample: seek failed\n");
    goto done;
  }
  if (build_vfilter(&src, vfilter_expr, &vf) != 0) {
    (void)fprintf(stderr, "crf_search: score_sample: build_vfilter failed\n");
    goto done;
  }

  const AVCodec *av1_codec = avcodec_find_decoder(AV_CODEC_ID_AV1);
  if (!av1_codec) {
    (void)fprintf(stderr, "crf_search: avcodec_find_decoder(AV_CODEC_ID_AV1) returned NULL\n");
    goto done;
  }
  av1_dec = avcodec_alloc_context3(av1_codec);
  if (!av1_dec) {
    (void)fprintf(stderr, "crf_search: avcodec_alloc_context3(AV1) failed\n");
    goto done;
  }
  av1_dec->thread_count = 0;
  int oc = avcodec_open2(av1_dec, av1_codec, NULL);
  if (oc < 0) {
    (void)fprintf(stderr, "crf_search: avcodec_open2(AV1) failed (%d)\n", oc);
    goto done;
  }

  pkt_src = av_packet_alloc();
  raw_src = av_frame_alloc();
  dec_av1 = av_frame_alloc();
  if (!pkt_src || !raw_src || !dec_av1)
    goto done;

  /* ---- Pass A: pull `frames` filtered source frames into src_buf. ---- */
  src_buf = calloc((size_t)frames, sizeof(AVFrame *));
  if (!src_buf)
    goto done;

  bool src_eof = false;
  while (src_count < frames && !src_eof) {
    int r = av_read_frame(src.ifmt, pkt_src);
    if (r == AVERROR_EOF)
      src_eof = true;
    else if (r < 0)
      goto done;

    if (r >= 0 && pkt_src->stream_index != src.video_idx) {
      av_packet_unref(pkt_src);
      continue;
    }

    int sent =
        (int)src_eof ? avcodec_send_packet(src.dec, NULL) : avcodec_send_packet(src.dec, pkt_src);
    av_packet_unref(pkt_src);
    if (sent < 0 && sent != AVERROR_EOF)
      goto done;

    while (src_count < frames) {
      int dr = avcodec_receive_frame(src.dec, raw_src);
      if (dr == AVERROR(EAGAIN))
        break;
      if (dr == AVERROR_EOF) {
        src_eof = true;
        break;
      }
      if (dr < 0)
        goto done;

      if (av_buffersrc_add_frame(vf.src, raw_src) < 0) {
        av_frame_unref(raw_src);
        goto done;
      }
      av_frame_unref(raw_src);

      while (src_count < frames) {
        AVFrame *out_frame = av_frame_alloc();
        if (!out_frame)
          goto done;
        int fr = av_buffersink_get_frame(vf.sink, out_frame);
        if (fr == AVERROR(EAGAIN) || fr == AVERROR_EOF) {
          av_frame_free(&out_frame);
          break;
        }
        if (fr < 0) {
          av_frame_free(&out_frame);
          goto done;
        }
        src_buf[src_count++] = out_frame;
      }
    }
  }

  /* ---- Pass B: feed all AV1 packets, then receive frames in display order ---- */
  for (int i = 0; i < av1->num; i++) {
    if (avcodec_send_packet(av1_dec, av1->pkts[i]) < 0)
      goto done;
    /* Drain whatever frames are ready already. */
    while (frames_scored < src_count) {
      int dr = avcodec_receive_frame(av1_dec, dec_av1);
      if (dr == AVERROR(EAGAIN))
        break;
      if (dr == AVERROR_EOF)
        goto flush_score;
      if (dr < 0)
        goto done;

      VmafPicture pic_ref = {0};
      VmafPicture pic_dist = {0};
      if (copy_avframe_to_vmaf(src_buf[frames_scored], &pic_ref) != 0 ||
          copy_avframe_to_vmaf(dec_av1, &pic_dist) != 0) {
        vmaf_picture_unref(&pic_ref);
        vmaf_picture_unref(&pic_dist);
        av_frame_unref(dec_av1);
        goto done;
      }
      if (vmaf_read_pictures(vmaf, &pic_ref, &pic_dist, base_index + (unsigned)frames_scored) < 0) {
        av_frame_unref(dec_av1);
        goto done;
      }
      av_frame_unref(dec_av1);
      frames_scored++;
    }
  }

flush_score:
  /* Flush the AV1 decoder. */
  avcodec_send_packet(av1_dec, NULL);
  while (frames_scored < src_count) {
    int dr = avcodec_receive_frame(av1_dec, dec_av1);
    if (dr == AVERROR(EAGAIN) || dr == AVERROR_EOF)
      break;
    if (dr < 0)
      goto done;

    VmafPicture pic_ref = {0};
    VmafPicture pic_dist = {0};
    if (copy_avframe_to_vmaf(src_buf[frames_scored], &pic_ref) != 0 ||
        copy_avframe_to_vmaf(dec_av1, &pic_dist) != 0) {
      vmaf_picture_unref(&pic_ref);
      vmaf_picture_unref(&pic_dist);
      av_frame_unref(dec_av1);
      goto done;
    }
    if (vmaf_read_pictures(vmaf, &pic_ref, &pic_dist, base_index + (unsigned)frames_scored) < 0) {
      av_frame_unref(dec_av1);
      goto done;
    }
    av_frame_unref(dec_av1);
    frames_scored++;
  }

  rc = 0;

done:
  if (frames_scored_out)
    *frames_scored_out = frames_scored;
  if (src_buf) {
    for (int i = 0; i < src_count; i++)
      av_frame_free(&src_buf[i]);
    free(src_buf);
  }
  if (dec_av1)
    av_frame_free(&dec_av1);
  if (raw_src)
    av_frame_free(&raw_src);
  if (pkt_src)
    av_packet_free(&pkt_src);
  if (av1_dec)
    avcodec_free_context(&av1_dec);
  close_vfilter(&vf);
  close_source(&src);
  return rc;
}

/* ====================================================================== */
/*  One trial: encode + score N samples at a given CRF                     */
/* ====================================================================== */

static int run_trial(const char *input_path, const EncodePreset *p, int film_grain, int crf,
                     const char *vfilter, const int64_t *sample_starts, int nsamples,
                     int frames_per_sample, double *out_vmaf) {
  VmafContext *vmaf = NULL;
  VmafModel *model = NULL;
  VmafConfiguration cfg = {
      .log_level = VMAF_LOG_LEVEL_ERROR,
      .n_threads = PROBE_LIBVMAF_THREADS,
      .n_subsample = 1,
  };
  int vr = vmaf_init(&vmaf, cfg);
  if (vr < 0) {
    (void)fprintf(stderr, "crf_search: vmaf_init failed (%d)\n", vr);
    return -1;
  }

  VmafModelConfig mcfg = {.name = "vmaf"};
  vr = vmaf_model_load(&model, &mcfg, "vmaf_v0.6.1");
  if (vr < 0) {
    (void)fprintf(stderr, "crf_search: vmaf_model_load(vmaf_v0.6.1) failed (%d)\n", vr);
    vmaf_close(vmaf);
    return -1;
  }
  vr = vmaf_use_features_from_model(vmaf, model);
  if (vr < 0) {
    (void)fprintf(stderr, "crf_search: vmaf_use_features_from_model failed (%d)\n", vr);
    vmaf_model_destroy(model);
    vmaf_close(vmaf);
    return -1;
  }

  unsigned total_scored = 0;
  int trial_rc = 0;
  for (int s = 0; s < nsamples; s++) {
    if (nsamples > 1)
      ui_kv("  Sample", "%d/%d  start frame %lld — encoding…", s + 1, nsamples,
            (long long)sample_starts[s]);
    else
      ui_kv("  Encode", "sample at frame %lld…", (long long)sample_starts[s]);
    PktList av1 = {0};
    if (encode_sample(input_path, sample_starts[s], frames_per_sample, p, film_grain, crf, vfilter,
                      &av1) != 0) {
      pl_free(&av1);
      trial_rc = -1;
      break;
    }
    if (av1.num == 0) {
      (void)fprintf(stderr,
                    "crf_search: encode_sample produced 0 packets at sample %d (start=%lld)\n", s,
                    (long long)sample_starts[s]);
      pl_free(&av1);
      trial_rc = -1;
      break;
    }
    ui_kv("  Score", "%d AV1 packets, computing VMAF…", av1.num);
    int scored = 0;
    int sr = score_sample(input_path, sample_starts[s], frames_per_sample, vfilter, &av1, vmaf,
                          total_scored, &scored);
    if (sr != 0)
      (void)fprintf(stderr,
                    "crf_search: score_sample failed at sample %d (%d packets, scored %d)\n", s,
                    av1.num, scored);
    pl_free(&av1);
    if (sr != 0) {
      trial_rc = -1;
      break;
    }
    total_scored += (unsigned)scored;
  }

  if (trial_rc == 0) {
    vr = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    if (vr < 0) {
      (void)fprintf(stderr, "crf_search: vmaf_read_pictures(flush) failed (%d)\n", vr);
      trial_rc = -1;
    }
  }

  double score = 0;
  if (trial_rc == 0 && total_scored > 0) {
    vr =
        vmaf_score_pooled(vmaf, model, VMAF_POOL_METHOD_HARMONIC_MEAN, &score, 0, total_scored - 1);
    if (vr < 0) {
      (void)fprintf(stderr, "crf_search: vmaf_score_pooled failed (%d, scored=%u)\n", vr,
                    total_scored);
      trial_rc = -1;
    }
  }

  vmaf_model_destroy(model);
  vmaf_close(vmaf);

  if (trial_rc != 0 || total_scored == 0) {
    if (total_scored == 0)
      (void)fprintf(stderr, "crf_search: no frames scored across %d sample(s)\n", nsamples);
    return -1;
  }
  *out_vmaf = score;
  return 0;
}

/* ====================================================================== */
/*  Binary search driver                                                   */
/* ====================================================================== */

static int next_crf_guess(int crf_a, double vmaf_a, int crf_b, double vmaf_b, int target) {
  double slope;
  if (crf_b != crf_a && fabs(vmaf_b - vmaf_a) > 0.1)
    slope = (vmaf_b - vmaf_a) / (double)(crf_b - crf_a);
  else
    slope = -PROBE_DEFAULT_SLOPE;
  if (slope > -0.05)
    slope = -PROBE_DEFAULT_SLOPE;
  double next = (double)crf_b + ((double)target - vmaf_b) / slope;
  return round_clamp(next, PROBE_CRF_MIN, PROBE_CRF_MAX);
}

static int search_with_n_samples(const char *input_path, const EncodePreset *p, int film_grain,
                                 int vmaf_target, const char *vfilter, const int64_t *sample_starts,
                                 int nsamples, int frames_per_sample, int *out_crf,
                                 double *out_vmaf) {
  int history_crf[PROBE_MAX_TRIALS] = {0};
  double history_vmaf[PROBE_MAX_TRIALS] = {0};
  int n_history = 0;

  int next_crf = PROBE_INITIAL_CRF;
  int best_crf = -1;
  double best_vmaf = 0.0;
  double best_dist = 1e9;
  bool best_meets = false;

  for (int trial = 0; trial < PROBE_MAX_TRIALS; trial++) {
    bool dup = false;
    for (int i = 0; i < n_history; i++) {
      if (history_crf[i] == next_crf) {
        dup = true;
        break;
      }
    }
    if (dup)
      break;

    ui_kv("Trial", "%d/%d  CRF %d  (%d sample%s, %d frames each) — encoding+scoring…", trial + 1,
          PROBE_MAX_TRIALS, next_crf, nsamples, nsamples == 1 ? "" : "s", frames_per_sample);

    double vmaf = 0.0;
    if (run_trial(input_path, p, film_grain, next_crf, vfilter, sample_starts, nsamples,
                  frames_per_sample, &vmaf) != 0)
      return -1;

    ui_kv("Result", "CRF %d → VMAF %.2f  (target %d)", next_crf, vmaf, vmaf_target);

    history_crf[n_history] = next_crf;
    history_vmaf[n_history] = vmaf;
    n_history++;

    bool meets = vmaf >= (double)vmaf_target;
    double dist = fabs(vmaf - (double)vmaf_target);

    /* Best CRF preference: highest CRF that still meets target; otherwise
     * (no trial met target yet) closest to target from below. */
    if (meets) {
      if (!best_meets || next_crf > best_crf) {
        best_meets = true;
        best_crf = next_crf;
        best_vmaf = vmaf;
        best_dist = dist;
      }
    } else if (!best_meets && dist < best_dist) {
      best_crf = next_crf;
      best_vmaf = vmaf;
      best_dist = dist;
    }

    if (dist <= PROBE_CONVERGE_VMAF) {
      *out_crf = next_crf;
      *out_vmaf = vmaf;
      return 0;
    }

    if (n_history >= 2) {
      next_crf =
          next_crf_guess(history_crf[n_history - 2], history_vmaf[n_history - 2],
                         history_crf[n_history - 1], history_vmaf[n_history - 1], vmaf_target);
    } else {
      double delta = (vmaf - (double)vmaf_target) / PROBE_DEFAULT_SLOPE;
      next_crf = round_clamp((double)next_crf + delta, PROBE_CRF_MIN, PROBE_CRF_MAX);
    }
  }

  if (best_crf < 0)
    return -1;
  *out_crf = best_crf;
  *out_vmaf = best_vmaf;
  return 0;
}

CrfSearchResult run_crf_search(const char *input_path, int vmaf_target, const EncodePreset *preset,
                               int film_grain, const char *vfilter) {
  CrfSearchResult result = {
      .crf = -1,
      .vmaf_target = vmaf_target,
      .vmaf_result = 0.0,
      .error = NULL,
  };
  svt_av1_set_log_callback(svt_probe_log_callback, NULL);

  Source src = {0};
  if (open_source(input_path, &src) != 0) {
    result.error = "failed to open input";
    return result;
  }
  int64_t total_frames = src.total_frames;
  close_source(&src);
  if (total_frames < PROBE_SAMPLE_FRAMES) {
    result.error = "input too short for CRF search (< 480 frames)";
    return result;
  }

  /* Pass 1 — 1 sample. */
  int64_t starts1[1];
  pick_samples(total_frames, 1, PROBE_SAMPLE_FRAMES, starts1);

  int crf1 = -1;
  double vmaf1 = 0.0;
  if (search_with_n_samples(input_path, preset, film_grain, vmaf_target, vfilter, starts1, 1,
                            PROBE_SAMPLE_FRAMES, &crf1, &vmaf1) != 0) {
    result.error = "probe encode or scoring failed";
    return result;
  }

  /* Accept if within escalation tolerance — escalate to 3 samples only when
   * the 1-sample number is clearly off-target. */
  if (fabs(vmaf1 - (double)vmaf_target) <= PROBE_ESCALATE_VMAF) {
    result.crf = crf1;
    result.vmaf_result = vmaf1;
    return result;
  }

  /* Pass 2 — 3 samples. */
  int64_t starts3[3];
  int n3 = pick_samples(total_frames, 3, PROBE_SAMPLE_FRAMES, starts3);
  int crf3 = -1;
  double vmaf3 = 0.0;
  if (search_with_n_samples(input_path, preset, film_grain, vmaf_target, vfilter, starts3, n3,
                            PROBE_SAMPLE_FRAMES, &crf3, &vmaf3) != 0) {
    result.error = "probe encode or scoring failed (3-sample refinement)";
    return result;
  }

  result.crf = crf3;
  result.vmaf_result = vmaf3;
  return result;
}
