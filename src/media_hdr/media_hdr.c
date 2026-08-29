/**
 * @file media_hdr.c
 * @brief Implementation of Dolby Vision and HDR10+ detection.
 */

#include "media_hdr.h"

#include <stdio.h>

#include <libavcodec/avcodec.h>
#include <libavcodec/codec_par.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/dovi_meta.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>

/** Number of frames to decode when probing for HDR10+ side data. */
#define HDR10PLUS_PROBE_FRAMES 30

/**
 * @brief Check coded side data for Dolby Vision configuration.
 */
static void detect_dovi(AVCodecParameters *codecpar, HdrInfo *info) {
  for (int i = 0; i < codecpar->nb_coded_side_data; i++) {
    if (codecpar->coded_side_data[i].type == AV_PKT_DATA_DOVI_CONF &&
        codecpar->coded_side_data[i].size >= sizeof(AVDOVIDecoderConfigurationRecord)) {
      const AVDOVIDecoderConfigurationRecord *dovi =
          (const AVDOVIDecoderConfigurationRecord *)codecpar->coded_side_data[i].data;
      info->has_dolby_vision = 1;
      info->dv_profile = dovi->dv_profile;
      info->dv_level = dovi->dv_level;
      return;
    }
  }
}

/**
 * @brief Decode a few frames and look for HDR10+ dynamic metadata.
 *
 * Only called when the video uses PQ transfer characteristics.
 */
static void detect_hdr10plus(AVFormatContext *fmt_ctx, int stream_idx, HdrInfo *info) {
  AVStream *stream = fmt_ctx->streams[stream_idx];
  const AVCodec *decoder = avcodec_find_decoder(stream->codecpar->codec_id);
  if (!decoder)
    return;

  AVCodecContext *dec_ctx = avcodec_alloc_context3(decoder);
  if (!dec_ctx)
    return;

  avcodec_parameters_to_context(dec_ctx, stream->codecpar);
  if (avcodec_open2(dec_ctx, decoder, NULL) < 0) {
    avcodec_free_context(&dec_ctx);
    return;
  }

  AVPacket *pkt = av_packet_alloc();
  AVFrame *frame = av_frame_alloc();
  if (!pkt || !frame) {
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&dec_ctx);
    return;
  }

  int probed = 0;
  while (probed < HDR10PLUS_PROBE_FRAMES && av_read_frame(fmt_ctx, pkt) >= 0) {
    if (pkt->stream_index != stream_idx) {
      av_packet_unref(pkt);
      continue;
    }
    avcodec_send_packet(dec_ctx, pkt);
    av_packet_unref(pkt);

    while (avcodec_receive_frame(dec_ctx, frame) == 0) {
      probed++;
      for (int i = 0; i < frame->nb_side_data; i++) {
        if (frame->side_data[i]->type == AV_FRAME_DATA_DYNAMIC_HDR_PLUS) {
          info->has_hdr10plus = 1;
          goto done;
        }
      }
      av_frame_unref(frame);
    }
  }

done:
  av_packet_free(&pkt);
  av_frame_free(&frame);
  avcodec_free_context(&dec_ctx);
}

HdrInfo get_hdr_info(const char *path) {
  HdrInfo info = {
      .error = 0,
      .has_dolby_vision = 0,
      .dv_profile = -1,
      .dv_level = -1,
      .has_hdr10 = 0,
      .has_hdr10plus = 0,
  };
  AVFormatContext *fmt_ctx = NULL;
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
    avformat_close_input(&fmt_ctx);
    return info;
  }

  ret = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
  if (ret < 0) {
    av_make_error_string(errbuf, sizeof(errbuf), ret);
    (void)fprintf(stderr, "Error: no video stream found in '%s': %s\n", path, errbuf);
    info.error = ret;
    avformat_close_input(&fmt_ctx);
    return info;
  }

  int video_idx = ret;
  AVCodecParameters *codecpar = fmt_ctx->streams[video_idx]->codecpar;

  /* Dolby Vision: codec-level side data, no decoding required. */
  detect_dovi(codecpar, &info);

  /* HDR10 base: PQ transfer characteristics indicate HDR10 or higher. */
  if (codecpar->color_trc == AVCOL_TRC_SMPTE2084) {
    info.has_hdr10 = 1;
    /* HDR10+: probe decoded frames for dynamic metadata. */
    detect_hdr10plus(fmt_ctx, video_idx, &info);
  }

  avformat_close_input(&fmt_ctx);
  return info;
}
