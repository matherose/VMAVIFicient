/**
 * @file media_analysis.c
 * @brief Video grain analysis via grav1synth diff.
 *
 * For each of a handful of positions through the film, extracts a short
 * lossless sample plus an hqdn3d-denoised copy, invokes grav1synth's "diff"
 * command to produce an AV1 film-grain table, and parses the per-scene Y
 * scaling points.  The reported score is the max across windows — we want
 * the grainiest passage to drive film_grain_denoise_strength, not the mean,
 * and sampling multiple windows avoids bias from whatever happens to sit at
 * the film's midpoint.
 */

#include "media_analysis.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>

#include "ui.h"

#ifndef VMAV_GRAV1SYNTH_BIN
#error "VMAV_GRAV1SYNTH_BIN must be defined by the build system"
#endif

extern char **environ;

/** Duration (seconds) of each sample window. */
#define SAMPLE_DURATION_SEC 15
/** Number of windows sampled across the film in the initial pass. */
#define NUM_SAMPLES 4
/** Upper bound on total windows including any adaptive refinement pass. */
#define MAX_SAMPLE_WINDOWS 7
/** Initial-pass per-window Y-score variance above which we add refinement
 *  samples between the original positions. Variance is in the same units as
 *  the Y score (0..1), so std-dev ~0.05 across windows clears the bar. */
#define GRAIN_VARIANCE_REFINE_THRESHOLD 0.0025
/** Centered crop dimensions applied to the sample to bound temp-file size. */
#define CROP_W 1920
#define CROP_H 1080
/** Max bytes of the grain-table file we'll parse (safety bound). */
#define MAX_TABLE_BYTES (4 * 1024 * 1024)

/** Positions (0..1) through the file at which to extract the initial windows.
 */
static const double SAMPLE_POSITIONS[NUM_SAMPLES] = {0.20, 0.40, 0.60, 0.80};
/** Midpoints between the initial positions, used when refinement is triggered.
 */
static const double REFINE_SAMPLE_POSITIONS[MAX_SAMPLE_WINDOWS - NUM_SAMPLES] = {0.30, 0.50, 0.70};

/* ------------------------------------------------------------------------- */
/*  Sample-extraction pipeline                                               */
/* ------------------------------------------------------------------------- */

typedef struct {
  AVFormatContext *fmt;
  AVCodecContext *enc;
  AVStream *stream;
} OutCtx;

/**
 * Open a single FFV1-in-MKV output context matching a decoded filter frame.
 */
static int out_open(const char *path, AVFrame *ref, AVRational time_base, OutCtx *o) {
  int ret = avformat_alloc_output_context2(&o->fmt, NULL, "matroska", path);
  if (ret < 0)
    return ret;

  const AVCodec *enc = avcodec_find_encoder(AV_CODEC_ID_FFV1);
  if (!enc)
    return AVERROR_ENCODER_NOT_FOUND;

  o->stream = avformat_new_stream(o->fmt, enc);
  if (!o->stream)
    return AVERROR(ENOMEM);

  o->enc = avcodec_alloc_context3(enc);
  if (!o->enc)
    return AVERROR(ENOMEM);

  o->enc->width = ref->width;
  o->enc->height = ref->height;
  o->enc->pix_fmt = ref->format;
  o->enc->time_base = time_base;
  o->enc->color_range = ref->color_range;
  o->enc->colorspace = ref->colorspace;
  o->enc->color_primaries = ref->color_primaries;
  o->enc->color_trc = ref->color_trc;
  /* level 3 = modern FFV1 with slicing; required for multi-threaded decode. */
  av_opt_set_int(o->enc->priv_data, "level", 3, 0);

  if (o->fmt->oformat->flags & AVFMT_GLOBALHEADER)
    o->enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

  ret = avcodec_open2(o->enc, enc, NULL);
  if (ret < 0)
    return ret;

  ret = avcodec_parameters_from_context(o->stream->codecpar, o->enc);
  if (ret < 0)
    return ret;
  o->stream->time_base = time_base;

  ret = avio_open(&o->fmt->pb, path, AVIO_FLAG_WRITE);
  if (ret < 0)
    return ret;

  return avformat_write_header(o->fmt, NULL);
}

static void out_close(OutCtx *o) {
  if (o->enc) {
    avcodec_send_frame(o->enc, NULL);
    AVPacket *pkt = av_packet_alloc();
    while (pkt && avcodec_receive_packet(o->enc, pkt) == 0) {
      pkt->stream_index = o->stream->index;
      av_packet_rescale_ts(pkt, o->enc->time_base, o->stream->time_base);
      av_interleaved_write_frame(o->fmt, pkt);
      av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
  }
  if (o->fmt) {
    if (o->fmt->pb)
      av_write_trailer(o->fmt);
    if (o->fmt->pb)
      avio_closep(&o->fmt->pb);
    avformat_free_context(o->fmt);
    o->fmt = NULL;
  }
  avcodec_free_context(&o->enc);
  o->stream = NULL;
}

/** Encode one filtered frame to one output. */
static int write_frame(OutCtx *o, AVFrame *f) {
  if (!o->enc || !o->stream || !o->fmt)
    return AVERROR(EINVAL);
  int ret = avcodec_send_frame(o->enc, f);
  if (ret < 0)
    return ret;
  AVPacket *pkt = av_packet_alloc();
  if (!pkt)
    return AVERROR(ENOMEM);
  while ((ret = avcodec_receive_packet(o->enc, pkt)) == 0) {
    pkt->stream_index = o->stream->index;
    av_packet_rescale_ts(pkt, o->enc->time_base, o->stream->time_base);
    ret = av_interleaved_write_frame(o->fmt, pkt);
    av_packet_unref(pkt);
    if (ret < 0)
      break;
  }
  av_packet_free(&pkt);
  if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
    ret = 0;
  return ret;
}

/**
 * Build the filter graph: buffer -> crop -> split -> [a]null / [b]hqdn3d.
 * The two sinks expose the source-cropped and denoised-cropped streams.
 */
static int build_graph(AVFilterGraph **graph, AVFilterContext **src_ctx, AVFilterContext **sink_a,
                       AVFilterContext **sink_b, AVCodecContext *dec) {
  *graph = avfilter_graph_alloc();
  if (!*graph)
    return AVERROR(ENOMEM);

  char args[512];
  snprintf(args, sizeof(args),
           "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d"
           ":colorspace=%d:range=%d",
           dec->width, dec->height, dec->pix_fmt, dec->pkt_timebase.num,
           dec->pkt_timebase.den ? dec->pkt_timebase.den : 1,
           dec->sample_aspect_ratio.num ? dec->sample_aspect_ratio.num : 1,
           dec->sample_aspect_ratio.den ? dec->sample_aspect_ratio.den : 1, dec->colorspace,
           dec->color_range);

  int ret = avfilter_graph_create_filter(src_ctx, avfilter_get_by_name("buffer"), "in", args, NULL,
                                         *graph);
  if (ret < 0)
    return ret;

  ret = avfilter_graph_create_filter(sink_a, avfilter_get_by_name("buffersink"), "out_a", NULL,
                                     NULL, *graph);
  if (ret < 0)
    return ret;
  ret = avfilter_graph_create_filter(sink_b, avfilter_get_by_name("buffersink"), "out_b", NULL,
                                     NULL, *graph);
  if (ret < 0)
    return ret;

  int cw = dec->width < CROP_W ? dec->width : CROP_W;
  int ch = dec->height < CROP_H ? dec->height : CROP_H;

  AVFilterInOut *inputs = avfilter_inout_alloc();
  AVFilterInOut *input_b = avfilter_inout_alloc();
  AVFilterInOut *outputs = avfilter_inout_alloc();
  if (!inputs || !input_b || !outputs) {
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&input_b);
    avfilter_inout_free(&outputs);
    return AVERROR(ENOMEM);
  }

  outputs->name = av_strdup("in");
  outputs->filter_ctx = *src_ctx;
  outputs->pad_idx = 0;
  outputs->next = NULL;

  inputs->name = av_strdup("out_a");
  inputs->filter_ctx = *sink_a;
  inputs->pad_idx = 0;
  inputs->next = input_b;

  input_b->name = av_strdup("out_b");
  input_b->filter_ctx = *sink_b;
  input_b->pad_idx = 0;
  input_b->next = NULL;

  char spec[512];
  /* hqdn3d with mild spatial + strong temporal (ls:cs:lt:ct).
   * Temporal denoise is what isolates grain (temporally uncorrelated), while
   * low spatial values preserve image detail so the diff fed to grav1synth
   * reflects real grain rather than destroyed content. */
  snprintf(spec, sizeof(spec),
           "crop=%d:%d,split[a][b];[a]null[out_a];"
           "[b]hqdn3d=1.5:1.5:6:6[out_b]",
           cw, ch);

  ret = avfilter_graph_parse_ptr(*graph, spec, &inputs, &outputs, NULL);
  avfilter_inout_free(&inputs);
  avfilter_inout_free(&outputs);
  if (ret < 0)
    return ret;

  return avfilter_graph_config(*graph, NULL);
}

/**
 * Extract a short sample of the source and a denoised twin to two FFV1/MKV
 * files.  Seeks to @p pos_ratio of the file's duration and runs for
 * @p duration_sec.
 */
static int extract_samples(const char *in_path, const char *out_src_path,
                           const char *out_denoised_path, double pos_ratio, int duration_sec) {
  AVFormatContext *fmt = NULL;
  AVCodecContext *dec = NULL;
  AVFilterGraph *graph = NULL;
  AVFilterContext *src_ctx = NULL, *sink_a = NULL, *sink_b = NULL;
  OutCtx out_a = {0}, out_b = {0};
  AVPacket *pkt = NULL;
  AVFrame *frame = NULL, *filt_a = NULL, *filt_b = NULL;
  int outputs_open = 0;
  int ret;

  ret = avformat_open_input(&fmt, in_path, NULL, NULL);
  if (ret < 0)
    goto done;
  ret = avformat_find_stream_info(fmt, NULL);
  if (ret < 0)
    goto done;

  int vidx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
  if (vidx < 0) {
    ret = vidx;
    goto done;
  }
  AVStream *stream = fmt->streams[vidx];

  const AVCodec *decoder = avcodec_find_decoder(stream->codecpar->codec_id);
  if (!decoder) {
    ret = AVERROR_DECODER_NOT_FOUND;
    goto done;
  }
  dec = avcodec_alloc_context3(decoder);
  if (!dec) {
    ret = AVERROR(ENOMEM);
    goto done;
  }
  avcodec_parameters_to_context(dec, stream->codecpar);
  dec->pkt_timebase = stream->time_base;
  ret = avcodec_open2(dec, decoder, NULL);
  if (ret < 0)
    goto done;

  ret = build_graph(&graph, &src_ctx, &sink_a, &sink_b, dec);
  if (ret < 0)
    goto done;

  pkt = av_packet_alloc();
  frame = av_frame_alloc();
  filt_a = av_frame_alloc();
  filt_b = av_frame_alloc();
  if (!pkt || !frame || !filt_a || !filt_b) {
    ret = AVERROR(ENOMEM);
    goto done;
  }

  /* Seek to the requested position. */
  int64_t dur = fmt->duration > 0 ? fmt->duration : 600 * AV_TIME_BASE;
  if (pos_ratio < 0.0)
    pos_ratio = 0.0;
  if (pos_ratio > 0.95)
    pos_ratio = 0.95;
  int64_t target = (int64_t)((double)dur * pos_ratio);
  av_seek_frame(fmt, -1, target, AVSEEK_FLAG_BACKWARD);
  avcodec_flush_buffers(dec);

  /* Stop after ~duration_sec of decoded frames (in stream time). */
  int64_t start_pts = AV_NOPTS_VALUE;
  int64_t stop_after =
      av_rescale_q((int64_t)duration_sec * AV_TIME_BASE, AV_TIME_BASE_Q, stream->time_base);

  int done_reading = 0;
  while (!done_reading) {
    ret = av_read_frame(fmt, pkt);
    if (ret < 0)
      break;
    if (pkt->stream_index != vidx) {
      av_packet_unref(pkt);
      continue;
    }
    ret = avcodec_send_packet(dec, pkt);
    av_packet_unref(pkt);
    if (ret < 0)
      goto done;

    while (1) {
      ret = avcodec_receive_frame(dec, frame);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        break;
      if (ret < 0)
        goto done;

      if (start_pts == AV_NOPTS_VALUE)
        start_pts = frame->pts;

      if (av_buffersrc_add_frame(src_ctx, frame) < 0) {
        av_frame_unref(frame);
        continue;
      }
      av_frame_unref(frame);

      while (av_buffersink_get_frame(sink_a, filt_a) == 0) {
        if (!outputs_open) {
          /* Lazily open once we know the filtered pixel format. */
          AVRational tb = av_buffersink_get_time_base(sink_a);
          if ((ret = out_open(out_src_path, filt_a, tb, &out_a)) < 0 ||
              (ret = out_open(out_denoised_path, filt_a, tb, &out_b)) < 0) {
            av_frame_unref(filt_a);
            goto done;
          }
          outputs_open = 1;
        }
        ret = write_frame(&out_a, filt_a);
        av_frame_unref(filt_a);
        if (ret < 0)
          goto done;
      }

      while (av_buffersink_get_frame(sink_b, filt_b) == 0) {
        ret = write_frame(&out_b, filt_b);
        int64_t cur = filt_b->pts;
        av_frame_unref(filt_b);
        if (ret < 0)
          goto done;
        if (start_pts != AV_NOPTS_VALUE && cur != AV_NOPTS_VALUE && cur - start_pts > stop_after) {
          done_reading = 1;
          break;
        }
      }
    }
  }

  /* Flush decoder. */
  avcodec_send_packet(dec, NULL);
  while (avcodec_receive_frame(dec, frame) == 0) {
    (void)av_buffersrc_add_frame(src_ctx, frame);
    av_frame_unref(frame);
  }
  (void)av_buffersrc_add_frame(src_ctx, NULL);

  /* Drain filter sinks. */
  if (outputs_open) {
    while (av_buffersink_get_frame(sink_a, filt_a) == 0) {
      write_frame(&out_a, filt_a);
      av_frame_unref(filt_a);
    }
    while (av_buffersink_get_frame(sink_b, filt_b) == 0) {
      write_frame(&out_b, filt_b);
      av_frame_unref(filt_b);
    }
  }

  ret = outputs_open ? 0 : AVERROR(EIO);

done:
  av_packet_free(&pkt);
  av_frame_free(&frame);
  av_frame_free(&filt_a);
  av_frame_free(&filt_b);
  if (outputs_open) {
    out_close(&out_a);
    out_close(&out_b);
  }
  avfilter_graph_free(&graph);
  avcodec_free_context(&dec);
  avformat_close_input(&fmt);
  return ret;
}

/* ------------------------------------------------------------------------- */
/*  grav1synth invocation                                                    */
/* ------------------------------------------------------------------------- */

static int spawn_grav1synth(const char *src, const char *denoised, const char *table_out) {
  /* grav1synth diff <src> <denoised> -o <table_out> */
  char *argv[] = {
      (char *)VMAV_GRAV1SYNTH_BIN, "diff", (char *)src, (char *)denoised, "-o",
      (char *)table_out,           NULL,
  };
  pid_t pid;
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  /* Silence grav1synth's chatter; we only care about exit status + table. */
  posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
  /* posix_spawnp does PATH lookup when the program name has no slash —
     so packagers can pass -DVMAV_GRAV1SYNTH_BIN_RUNTIME=grav1synth and
     ship the helper alongside vmavificient in bin/. Absolute paths
     (the dev-build default) still work because the `p` variant only
     consults PATH when the name is unqualified. */
  int ret = posix_spawnp(&pid, VMAV_GRAV1SYNTH_BIN, &actions, NULL, argv, environ);
  posix_spawn_file_actions_destroy(&actions);
  if (ret != 0)
    return -ret;

  int status = 0;
  if (waitpid(pid, &status, 0) < 0)
    return -errno;
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    return -1;
  return 0;
}

/* ------------------------------------------------------------------------- */
/*  Grain-table parser                                                       */
/* ------------------------------------------------------------------------- */

/**
 * Parsed grain table scores for luma and chroma planes.
 */
typedef struct {
  double y;  /**< Mean Y (luma) scaling, normalized 0..1. */
  double cb; /**< Mean Cb scaling, normalized 0..1. */
  double cr; /**< Mean Cr scaling, normalized 0..1. */
} GrainTableScores;

/**
 * Parse scaling values for a single plane prefix ("sY", "sCb", or "sCr")
 * from a grain table line.  Returns the sum and count of scaling values found.
 */
static void parse_plane_scalings(const char *line, const char *prefix, double *sum, long *count) {
  const char *p = line;
  while (*p == '\t' || *p == ' ')
    p++;
  size_t plen = strlen(prefix);
  if (strncmp(p, prefix, plen) != 0)
    return;
  if (p[plen] != ' ' && p[plen] != '\t')
    return;
  p += plen;

  char *end;
  long pt_count = strtol(p, &end, 10);
  if (end == p || pt_count <= 0)
    return;
  p = end;
  for (long i = 0; i < pt_count; i++) {
    (void)strtol(p, &end, 10); /* value */
    if (end == p)
      break;
    p = end;
    long scaling = strtol(p, &end, 10);
    if (end == p)
      break;
    p = end;
    *sum += (double)scaling;
    (*count)++;
  }
}

static double normalize_plane_score(double sum, long count) {
  if (count == 0)
    return 0.0;
  double mean = sum / (double)count;
  if (mean < 0.0)
    mean = 0.0;
  if (mean > 255.0)
    mean = 255.0;
  return mean / 255.0;
}

/**
 * Parse a grav1synth grain table and return 0..1 grain strength per plane.
 *
 * Format (tab-indented under each "E" line):
 *   filmgrn1
 *   E <start_ts> <end_ts> 1 <seed> 1
 *       p <12 params>
 *       sY <count> v1 s1 v2 s2 ...
 *       sCb <count> ...
 *       sCr <count> ...
 *       cY <coeffs>
 *       ...
 *
 * We average the scaling values (s1, s2, ...) across all scenes per plane.
 * Each scaling is in [0, 255]; the final score is mean / 255.
 */
static GrainTableScores parse_grain_table(const char *path) {
  GrainTableScores scores = {0};
  FILE *f = fopen(path, "r");
  if (!f)
    return scores;

  double sum_y = 0.0, sum_cb = 0.0, sum_cr = 0.0;
  long count_y = 0, count_cb = 0, count_cr = 0;
  char *line = NULL;
  size_t cap = 0;

  while (getline(&line, &cap, f) >= 0) {
    parse_plane_scalings(line, "sY", &sum_y, &count_y);
    parse_plane_scalings(line, "sCb", &sum_cb, &count_cb);
    parse_plane_scalings(line, "sCr", &sum_cr, &count_cr);
  }

  free(line);
  (void)fclose(f); /* read-only stream */

  scores.y = normalize_plane_score(sum_y, count_y);
  scores.cb = normalize_plane_score(sum_cb, count_cb);
  scores.cr = normalize_plane_score(sum_cr, count_cr);
  return scores;
}

/* ------------------------------------------------------------------------- */
/*  Public entry point                                                       */
/* ------------------------------------------------------------------------- */

/** Extract one sample window and parse its grain table.
 *  Returns 0 on success (scores filled), or negative AVERROR on failure.
 *  The tmp-file naming is keyed by @p window_idx so concurrent passes do not
 *  collide.
 *
 *  Cleans up all intermediate files (src, denoised, table) unless
 *  @p keep_tmp is set — we used to preserve the grainiest table for SVT's
 *  fgs_table, but feeding measured grain params back to the encoder
 *  produced over-faithful (mushy + expensive) reproduction at HDLight
 *  bitrates, so we now let SVT do its own grain analysis instead. */
static int sample_grain_window(const char *path, const char *label, int window_idx, double position,
                               int keep_tmp, GrainTableScores *out_scores) {
  char src_tmp[4096], denoised_tmp[4096], table_tmp[4096];
  snprintf(src_tmp, sizeof(src_tmp), "%s.grav1_src_%d.mkv", path, window_idx);
  snprintf(denoised_tmp, sizeof(denoised_tmp), "%s.grav1_denoised_%d.mkv", path, window_idx);
  snprintf(table_tmp, sizeof(table_tmp), "%s.grav1_table_%d.txt", path, window_idx);

  time_t window_t0 = time(NULL);
  int ret = extract_samples(path, src_tmp, denoised_tmp, position, SAMPLE_DURATION_SEC);
  if (ret >= 0) {
    ret = spawn_grav1synth(src_tmp, denoised_tmp, table_tmp);
    if (ret >= 0) {
      struct stat st;
      if (stat(table_tmp, &st) == 0 && st.st_size > 0 && st.st_size <= MAX_TABLE_BYTES) {
        *out_scores = parse_grain_table(table_tmp);
      } else {
        ret = AVERROR(EIO);
      }
    }
  }

  if (!keep_tmp) {
    unlink(src_tmp);
    unlink(denoised_tmp);
    unlink(table_tmp);
  } else {
    (void)fprintf(stderr, "[grain] window %d (pos %.2f): kept %s\n", window_idx, position,
                  table_tmp);
  }

  /* Per-window status — gives the user something to watch during the long
     analysis pass instead of a silent 10+ minute wait. */
  if (ret >= 0) {
    char detail[64];
    snprintf(detail, sizeof(detail), "pos %.0f%%, luma %.4f  %s", position * 100.0, out_scores->y,
             ui_fmt_duration(difftime(time(NULL), window_t0)));
    ui_stage_ok(label, detail);
  } else {
    char err[64];
    snprintf(err, sizeof(err), "pos %.0f%%, error %d", position * 100.0, ret);
    ui_stage_fail(label, err);
  }

  return ret;
}

/** Population variance of the first @p n entries of @p values. */
static double score_variance(const double *values, int n) {
  if (n <= 1)
    return 0.0;
  double mean = 0.0;
  for (int i = 0; i < n; i++)
    mean += values[i];
  mean /= n;
  double var_sum = 0.0;
  for (int i = 0; i < n; i++) {
    double d = values[i] - mean;
    var_sum += d * d;
  }
  return var_sum / n;
}

GrainScore get_grain_score(const char *path) {
  GrainScore result = {0};
  const int keep_tmp = getenv("VMAV_KEEP_GRAIN_TMP") != NULL;

  double max_score = 0.0;
  double max_chroma = 0.0;
  int windows_succeeded = 0;
  int first_error = 0;

  /* Holds Y scores for every window attempted (initial + refine). Failed
   * windows leave the slot at its zero init, matching the prior variance
   * behavior where failed windows contributed 0.0 to the distribution. */
  double y_scores[MAX_SAMPLE_WINDOWS] = {0};
  int total_attempts = 0;

  for (int i = 0; i < NUM_SAMPLES; i++, total_attempts++) {
    GrainTableScores scores = {0};
    char label[32];
    snprintf(label, sizeof(label), "Window %d/%d", i + 1, NUM_SAMPLES);
    int ret =
        sample_grain_window(path, label, total_attempts, SAMPLE_POSITIONS[i], keep_tmp, &scores);
    if (ret >= 0) {
      result.per_window_scores[i] = scores.y;
      y_scores[total_attempts] = scores.y;
      if (scores.y > max_score)
        max_score = scores.y;
      double chroma_max = scores.cb > scores.cr ? scores.cb : scores.cr;
      if (chroma_max > max_chroma)
        max_chroma = chroma_max;
      windows_succeeded++;
    } else if (first_error == 0) {
      first_error = ret;
    }
  }

  if (windows_succeeded == 0) {
    result.error = first_error ? first_error : AVERROR(EIO);
    return result;
  }

  /* Initial-pass variance gates the refinement decision. Computed over the
   * full NUM_SAMPLES slot set (failed windows as 0.0) to match the signal
   * previously reported to callers. */
  double initial_variance = score_variance(y_scores, NUM_SAMPLES);

  /* Adaptive refinement: if the initial pass shows inconsistent grain across
   * the film, sample at midpoints between the original positions so the final
   * max aggregation sees any grainier passage lurking between them. */
  int refined = 0;
  if (initial_variance > GRAIN_VARIANCE_REFINE_THRESHOLD) {
    const int refine_count =
        (int)(sizeof(REFINE_SAMPLE_POSITIONS) / sizeof(REFINE_SAMPLE_POSITIONS[0]));
    for (int j = 0; j < refine_count && total_attempts < MAX_SAMPLE_WINDOWS;
         j++, total_attempts++) {
      GrainTableScores scores = {0};
      char label[32];
      snprintf(label, sizeof(label), "Refine %d/%d", j + 1, refine_count);
      int ret = sample_grain_window(path, label, total_attempts, REFINE_SAMPLE_POSITIONS[j],
                                    keep_tmp, &scores);
      if (ret >= 0) {
        y_scores[total_attempts] = scores.y;
        if (scores.y > max_score)
          max_score = scores.y;
        double chroma_max = scores.cb > scores.cr ? scores.cb : scores.cr;
        if (chroma_max > max_chroma)
          max_chroma = chroma_max;
        windows_succeeded++;
        refined = 1;
      } else if (first_error == 0) {
        first_error = ret;
      }
    }
  }

  result.grain_score = max_score;
  result.avg_noise = max_score * 255.0;
  result.frames_analyzed = windows_succeeded * SAMPLE_DURATION_SEC * 30;
  result.windows_succeeded = windows_succeeded;
  result.chroma_grain_score = max_chroma;
  /* When refinement ran, report variance over the combined sample set so the
   * signal reflects the full picture; otherwise keep the initial-pass value. */
  result.grain_variance = refined ? score_variance(y_scores, total_attempts) : initial_variance;

  return result;
}
