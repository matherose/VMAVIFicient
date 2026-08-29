/**
 * @file media_crop.c
 * @brief Automatic crop detection using FFmpeg's cropdetect filter.
 */

#include "media_crop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavcodec/codec_par.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libavutil/rational.h>

/** Number of sample points for crop detection. */
#define CROP_SAMPLES 6
/** Frames to feed per sample so cropdetect stabilises. */
#define WARMUP_FRAMES 24

static int cmp_int(const void *a, const void *b) {
  return *(const int *)a - *(const int *)b;
}

/**
 * @brief Read cropdetect metadata from a filtered frame.
 *
 * @return 1 if valid crop values were found, 0 otherwise.
 */
static int read_crop_metadata(AVFrame *filt_frame, int frame_w, int frame_h, int *top, int *bottom,
                              int *left, int *right) {
  AVDictionaryEntry *e;
  int cw = 0, ch = 0, cx = 0, cy = 0;

  e = av_dict_get(filt_frame->metadata, "lavfi.cropdetect.w", NULL, 0);
  if (e)
    cw = (int)strtol(e->value, NULL, 10);
  e = av_dict_get(filt_frame->metadata, "lavfi.cropdetect.h", NULL, 0);
  if (e)
    ch = (int)strtol(e->value, NULL, 10);
  e = av_dict_get(filt_frame->metadata, "lavfi.cropdetect.x", NULL, 0);
  if (e)
    cx = (int)strtol(e->value, NULL, 10);
  e = av_dict_get(filt_frame->metadata, "lavfi.cropdetect.y", NULL, 0);
  if (e)
    cy = (int)strtol(e->value, NULL, 10);

  if (cw <= 0 || ch <= 0)
    return 0;

  *top = cy;
  *bottom = frame_h - (cy + ch);
  *left = cx;
  *right = frame_w - (cx + cw);
  return 1;
}

/**
 * @brief Build a buffersrc -> cropdetect -> buffersink filter graph.
 *
 * Uses avfilter_graph_parse_ptr so FFmpeg automatically inserts pixel
 * format conversion when the source format (e.g. yuv420p10le) is not
 * natively supported by cropdetect.
 */
static int build_cropdetect_graph(AVFilterGraph **graph, AVFilterContext **src_ctx,
                                  AVFilterContext **sink_ctx, AVCodecContext *dec_ctx) {
  *graph = avfilter_graph_alloc();
  if (!*graph)
    return AVERROR(ENOMEM);

  const AVFilter *buffersrc = avfilter_get_by_name("buffer");
  const AVFilter *buffersink = avfilter_get_by_name("buffersink");

  char args[512];
  snprintf(args, sizeof(args),
           "video_size=%dx%d:pix_fmt=%d:time_base=1/25:pixel_aspect=1/1"
           ":colorspace=%d:range=%d",
           dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt, dec_ctx->colorspace,
           dec_ctx->color_range);

  int ret;
  ret = avfilter_graph_create_filter(src_ctx, buffersrc, "in", args, NULL, *graph);
  if (ret < 0)
    return ret;

  ret = avfilter_graph_create_filter(sink_ctx, buffersink, "out", NULL, NULL, *graph);
  if (ret < 0)
    return ret;

  AVFilterInOut *inputs = avfilter_inout_alloc();
  AVFilterInOut *outputs = avfilter_inout_alloc();
  if (!inputs || !outputs) {
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    return AVERROR(ENOMEM);
  }

  outputs->name = av_strdup("in");
  outputs->filter_ctx = *src_ctx;
  outputs->pad_idx = 0;
  outputs->next = NULL;

  inputs->name = av_strdup("out");
  inputs->filter_ctx = *sink_ctx;
  inputs->pad_idx = 0;
  inputs->next = NULL;

  ret = avfilter_graph_parse_ptr(*graph, "cropdetect=round=2", &inputs, &outputs, NULL);
  avfilter_inout_free(&inputs);
  avfilter_inout_free(&outputs);
  if (ret < 0)
    return ret;

  return avfilter_graph_config(*graph, NULL);
}

CropInfo get_crop_info(const char *path) {
  CropInfo info = {.error = 0, .top = 0, .bottom = 0, .left = 0, .right = 0};
  AVFormatContext *fmt_ctx = NULL;
  AVCodecContext *dec_ctx = NULL;
  AVPacket *pkt = NULL;
  AVFrame *frame = NULL, *filt_frame = NULL;
  char errbuf[AV_ERROR_MAX_STRING_SIZE];
  int ret;

  ret = avformat_open_input(&fmt_ctx, path, NULL, NULL);
  if (ret < 0) {
    av_make_error_string(errbuf, sizeof(errbuf), ret);
    (void)fprintf(stderr, "Error: cannot open '%s': %s\n", path, errbuf);
    info.error = ret;
    return info;
  }

  ret = avformat_find_stream_info(fmt_ctx, NULL);
  if (ret < 0) {
    av_make_error_string(errbuf, sizeof(errbuf), ret);
    (void)fprintf(stderr, "Error: cannot read stream info from '%s': %s\n", path, errbuf);
    info.error = ret;
    goto cleanup;
  }

  ret = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
  if (ret < 0) {
    av_make_error_string(errbuf, sizeof(errbuf), ret);
    (void)fprintf(stderr, "Error: no video stream found in '%s': %s\n", path, errbuf);
    info.error = ret;
    goto cleanup;
  }

  int video_idx = ret;
  AVStream *stream = fmt_ctx->streams[video_idx];

  const AVCodec *decoder = avcodec_find_decoder(stream->codecpar->codec_id);
  if (!decoder) {
    (void)fprintf(stderr, "Error: unsupported video codec in '%s'.\n", path);
    info.error = AVERROR_DECODER_NOT_FOUND;
    goto cleanup;
  }

  dec_ctx = avcodec_alloc_context3(decoder);
  avcodec_parameters_to_context(dec_ctx, stream->codecpar);
  ret = avcodec_open2(dec_ctx, decoder, NULL);
  if (ret < 0) {
    info.error = ret;
    goto cleanup;
  }

  pkt = av_packet_alloc();
  frame = av_frame_alloc();
  filt_frame = av_frame_alloc();
  if (!pkt || !frame || !filt_frame) {
    info.error = AVERROR(ENOMEM);
    goto cleanup;
  }

  int64_t duration = fmt_ctx->duration;
  if (duration <= 0)
    duration = 600 * AV_TIME_BASE;

  int tops[CROP_SAMPLES], bottoms[CROP_SAMPLES];
  int lefts[CROP_SAMPLES], rights[CROP_SAMPLES];
  int sample_count = 0;

  for (int s = 0; s < CROP_SAMPLES && sample_count < CROP_SAMPLES; s++) {
    int64_t target = duration / 10 + (int64_t)((double)duration * 0.8 / CROP_SAMPLES) * s;

    av_seek_frame(fmt_ctx, -1, target, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(dec_ctx);

    /* Build a fresh filter graph for each sample point so cropdetect
     * state is clean. */
    AVFilterGraph *graph = NULL;
    AVFilterContext *src_ctx = NULL, *sink_ctx = NULL;

    if (build_cropdetect_graph(&graph, &src_ctx, &sink_ctx, dec_ctx) < 0) {
      avfilter_graph_free(&graph);
      continue;
    }

    /* Decode WARMUP_FRAMES frames, take the last cropdetect result. */
    int decoded = 0;
    int last_top = 0, last_bottom = 0, last_left = 0, last_right = 0;
    int got_result = 0;

    while (decoded < WARMUP_FRAMES && av_read_frame(fmt_ctx, pkt) >= 0) {
      if (pkt->stream_index != video_idx) {
        av_packet_unref(pkt);
        continue;
      }
      avcodec_send_packet(dec_ctx, pkt);
      av_packet_unref(pkt);

      while (decoded < WARMUP_FRAMES && avcodec_receive_frame(dec_ctx, frame) == 0) {
        if (av_buffersrc_add_frame(src_ctx, frame) < 0) {
          av_frame_unref(frame);
          break;
        }
        av_frame_unref(frame);

        while (av_buffersink_get_frame(sink_ctx, filt_frame) == 0) {
          if (read_crop_metadata(filt_frame, dec_ctx->width, dec_ctx->height, &last_top,
                                 &last_bottom, &last_left, &last_right))
            got_result = 1;
          av_frame_unref(filt_frame);
          decoded++;
        }
      }
    }

    avfilter_graph_free(&graph);

    if (got_result) {
      tops[sample_count] = last_top;
      bottoms[sample_count] = last_bottom;
      lefts[sample_count] = last_left;
      rights[sample_count] = last_right;
      sample_count++;
    }
  }

  if (sample_count > 0) {
    qsort(tops, (size_t)sample_count, sizeof(int), cmp_int);
    qsort(bottoms, (size_t)sample_count, sizeof(int), cmp_int);
    qsort(lefts, (size_t)sample_count, sizeof(int), cmp_int);
    qsort(rights, (size_t)sample_count, sizeof(int), cmp_int);

    int mid = sample_count / 2;
    info.top = tops[mid];
    info.bottom = bottoms[mid];
    info.left = lefts[mid];
    info.right = rights[mid];
  }

cleanup:
  av_packet_free(&pkt);
  av_frame_free(&frame);
  av_frame_free(&filt_frame);
  avcodec_free_context(&dec_ctx);
  avformat_close_input(&fmt_ctx);
  return info;
}
