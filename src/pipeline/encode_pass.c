/**
 * @file encode_pass.c
 * @brief Unified rate-control/plan + encode/mux pass, shared by the 4K
 *        and HD (companion / scale-to-hd) paths.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "crf_search.h"
#include "encode_preset.h"
#include "final_mux.h"
#include "media_analysis.h"
#include "media_info.h"
#include "pipeline.h"
#include "rpu_extract.h"
#include "ui.h"
#include "video_encode.h"

StageStatus stage_rate_plan(PipelineCtx *ctx, EncodePassParams *pass) {
  const char *filepath = ctx->opt.filepath;

  /* ---- Rate control: CRF search or manual override ---- */
  int bitrate = ctx->opt.bitrate; /* 0 if not specified; VBR only when set */
  int vmaf_used = 0;

  if (!ctx->opt.grain_only && pass->crf == 0 && bitrate == 0) {
    /* Check for cached CRF before running search */
    if (pass->use_cached_crf && ctx->scores_cached && ctx->cached_crf > 0) {
      pass->crf = ctx->cached_crf;
      vmaf_used = ctx->opt.vmaf_target > 0
                      ? ctx->opt.vmaf_target
                      : get_vmaf_target(ctx->opt.quality, pass->plan_info->height);
      ui_section("CRF search");
      char detail[64];
      snprintf(detail, sizeof(detail), "using cached CRF %d", pass->crf);
      ui_stage_ok("skip", detail);
    } else {
      /* Default path: probe CRF in-process at source (4K) resolution. */
      vmaf_used = ctx->opt.vmaf_target > 0
                      ? ctx->opt.vmaf_target
                      : get_vmaf_target(ctx->opt.quality, pass->plan_info->height);
      ui_section(pass->is_hd && ctx->opt.companion_hd ? "HD CRF search" : "CRF search");
      CrfSearchResult csr = run_crf_search(filepath, vmaf_used, pass->preset, pass->film_grain,
                                           pass->crf_scale_filter);
      if (csr.crf < 0) {
        ui_stage_fail("crf-search", csr.error ? csr.error : "CRF search failed");
        ui_hint("bypass with --crf <N> or --bitrate <kbps>");
        return STAGE_EXIT_FAIL;
      }
      char detail[96];
      snprintf(detail, sizeof(detail), "CRF %d  (VMAF %.2f, target %d)", csr.crf, csr.vmaf_result,
               vmaf_used);
      ui_stage_ok("crf-search", detail);
      pass->crf = csr.crf;
    }
  } else if (ctx->opt.vmaf_target > 0) {
    vmaf_used = ctx->opt.vmaf_target;
  }
  pass->vmaf_used = vmaf_used;

  /* Plan + Dry-run notice always render. */
  int saved_quiet = ui_is_quiet();
  ui_set_quiet(0);
  ui_section(pass->is_hd && ctx->opt.companion_hd ? "HD encoding plan" : "Encoding plan");
  if (pass->is_hd) {
    ui_kv("Preset", "%s  (HD)", quality_type_to_string(ctx->opt.quality));
  } else {
    ui_kv("Preset", "%s  (%s)", quality_type_to_string(ctx->opt.quality),
          pass->plan_info->height >= 2160 ? "4K" : "HD");
  }
  ui_kv("SVT-AV1", "preset %d, tune %d, keyint %d, ac-bias %.1f", pass->preset->preset,
        pass->preset->tune, pass->preset->keyint, pass->preset->ac_bias);
  if (pass->is_hd) {
    ui_kv("Scale", "%d×%d  (from %d×%d 4K source)", pass->scale_width, pass->scale_height,
          ctx->info.width, ctx->info.height);
  }
  if (ctx->grain.error == 0) {
    int is_anim = (ctx->opt.quality == QUALITY_ANIMATION);
    const char *content_tier_if_live = ctx->grain.grain_score >= 0.08 ? "grainy" : "clean";
    const char *content_tier = is_anim ? "animation" : content_tier_if_live;
    ui_kv("Grain", "level %d  (%s tier)", pass->film_grain, content_tier);
  }
  if (pass->crf > 0) {
    if (vmaf_used > 0)
      ui_kv("CRF", "%d  (VMAF target %d)", pass->crf, vmaf_used);
    else
      ui_kv("CRF", "%d  (manual)", pass->crf);
  } else if (bitrate > 0) {
    ui_kv("Bitrate", "%d kbps VBR  (manual)", bitrate);
  } else {
    ui_kv("CRF", "(use --dry-run to probe, or --crf <N> to set)");
  }
  ui_kv("Output", "%s%s", ctx->output_dir, pass->output_name);

  if (ctx->opt.grain_only)
    vmav_print_encoder_knobs(pass->preset, pass->film_grain);

  /* For companion-hd, fall through so the HD plan section also renders
     before exiting.  For solo dry-run / grain-only, exit here. */
  if ((ctx->opt.dry_run || ctx->opt.grain_only) && !pass->dry_run_falls_through) {
    ui_section((int)ctx->opt.grain_only ? "Grain-only" : "Dry run");
    ui_row("No files written. Re-run without %s to encode.",
           (int)ctx->opt.grain_only ? "--grain-only" : "--dry-run");
    return STAGE_EXIT_OK;
  }
  ui_set_quiet(saved_quiet);

  return STAGE_CONTINUE;
}

StageStatus encode_pass(PipelineCtx *ctx, EncodePassParams *pass) {
  const char *filepath = ctx->opt.filepath;
  int bitrate = ctx->opt.bitrate;

  /* ---- RPU extraction (Dolby Vision) ---- */
  if (pass->extract_rpu && ctx->hdr.error == 0 && ctx->hdr.has_dolby_vision) {
    char rpu_name[2048];
    build_rpu_filename(rpu_name, sizeof(rpu_name), pass->base_name);

    /* Write RPU to cache directory */
    char rpu_cache_path[4096];
    vmav_build_cache_path(ctx, rpu_cache_path, sizeof(rpu_cache_path), rpu_name);
    snprintf(ctx->rpu_path, sizeof(ctx->rpu_path), "%s", rpu_cache_path);

    ui_section("Dolby Vision RPU");
    time_t rpu_t0 = time(NULL);
    RpuExtractResult rpu_res = extract_rpu(filepath, ctx->rpu_path);

    if (rpu_res.skipped) {
      ui_stage_skip(rpu_name, "already exists");
    } else if (rpu_res.error == 0) {
      char detail[64];
      snprintf(detail, sizeof(detail), "%d RPUs in %s", rpu_res.rpu_count,
               ui_fmt_duration(difftime(time(NULL), rpu_t0)));
      ui_stage_ok(rpu_name, detail);
    } else {
      char err[64];
      snprintf(err, sizeof(err), "error %d", rpu_res.error);
      ui_stage_fail(rpu_name, err);
      ui_hint("source claims Dolby Vision but RPU extraction failed; "
              "encode will continue without DV metadata");
      ctx->rpu_path[0] = '\0'; /* Don't use RPU on failure */
    }
  }

  /* ---- AV1 video encoding ---- */
  char av1_video_name[2048];
  snprintf(av1_video_name, sizeof(av1_video_name), "%s.video.mkv", pass->base_name);
  char av1_video_cache_path[4096];
  vmav_build_cache_path(ctx, av1_video_cache_path, sizeof(av1_video_cache_path), av1_video_name);
  char av1_video_path[4096];
  snprintf(av1_video_path, sizeof(av1_video_path), "%s", av1_video_cache_path);
  time_t video_t0 = time(NULL);
  VideoEncodeResult vr = {0};

  {
    ui_section(pass->is_hd && ctx->opt.companion_hd ? "HD video encoding" : "Video encoding");

    VideoEncodeConfig vcfg = {
        .input_path = filepath,
        .output_path = av1_video_path,
        .rpu_path = ctx->rpu_path[0] ? ctx->rpu_path : NULL,
        .preset = pass->preset,
        .film_grain = pass->film_grain,
        .grain_score = ctx->grain.error == 0 ? ctx->grain.grain_score : 0.0,
        .grain_variance = ctx->grain.error == 0 ? ctx->grain.grain_variance : 0.0,
        .target_bitrate = bitrate,
        .crf = pass->crf,
        .info = &ctx->info,
        .crop = (ctx->crop.error == 0) ? &ctx->crop : NULL,
        .hdr = pass->enc_hdr,
        .scale_width = pass->scale_width,
        .scale_height = pass->scale_height,
    };

    vr = encode_video(&vcfg);

    if (vr.skipped) {
      ui_stage_skip("video.mkv", "already exists");
    } else if (vr.error == 0) {
      char detail[128];
      snprintf(detail, sizeof(detail), "%lld frames, %s in %s", (long long)vr.frames_encoded,
               ui_fmt_bytes(vr.bytes_written), ui_fmt_duration(difftime(time(NULL), video_t0)));
      ui_stage_ok("video.mkv", detail);
    } else {
      char err[64];
      snprintf(err, sizeof(err), "error %d after %lld frames", vr.error,
               (long long)vr.frames_encoded);
      ui_stage_fail("video.mkv", err);
      ui_hint("re-run with --verbose to forward SVT-AV1's own log to "
              "stderr (rate control, GOP layout, fatal warnings)");
      ctx->pipeline_failed = 1;
    }
  }

  /* ---- Final MKV muxing ---- */
  {
    char final_path[4096];
    snprintf(final_path, sizeof(final_path), "%s%s", ctx->output_dir, pass->output_name);

    /* Build mux audio descriptors */
    MuxAudioTrack mux_audio[32];
    for (int i = 0; i < ctx->opus_count && i < 32; i++) {
      mux_audio[i].path = ctx->opus_paths[i];
      mux_audio[i].language = ctx->audio_langs[i];
      mux_audio[i].track_name = ctx->audio_names[i];
      mux_audio[i].is_default = (i == 0) ? 1 : 0;
    }

    /* Build mux subtitle descriptors */
    MuxSubtitleTrack mux_subs[64];
    int sub_default_set = 0;
    for (int i = 0; i < ctx->srt_count && i < 64; i++) {
      mux_subs[i].path = ctx->srt_paths[i];
      mux_subs[i].language = ctx->srt_langs[i];
      mux_subs[i].track_name = ctx->srt_names[i];
      mux_subs[i].is_forced = ctx->srt_is_forced[i];
      mux_subs[i].is_sdh = ctx->srt_is_sdh[i];
      /* Only the first French forced subtitle is default */
      if (!sub_default_set && ctx->srt_is_forced[i] &&
          (strcmp(ctx->srt_langs[i], "fre") == 0 || strcmp(ctx->srt_langs[i], "fra") == 0)) {
        mux_subs[i].is_default = 1;
        sub_default_set = 1;
      } else {
        mux_subs[i].is_default = 0;
      }
    }

    FinalMuxConfig mux_cfg = {
        .video_path = av1_video_path,
        .output_path = final_path,
        .audio = mux_audio,
        .audio_count = ctx->opus_count,
        .subs = mux_subs,
        .sub_count = ctx->srt_count,
        .title = ctx->mkv_title,
        .video_title = ctx->mkv_title,
        .video_language = ctx->video_language,
        .chapters_source_path = filepath,
    };

    ui_section(pass->is_hd && ctx->opt.companion_hd ? "HD final mux" : "Final mux");
    ui_kv("Inputs", "1 video + %d audio + %d subtitle track%s", ctx->opus_count, ctx->srt_count,
          ctx->srt_count == 1 ? "" : "s");
    time_t mux_t0 = time(NULL);
    FinalMuxResult mr = {.error = -1, .skipped = 0};

    if (ctx->audio_fail_count > 0) {
      char err[128];
      snprintf(err, sizeof(err), "%d audio track%s failed to encode — mux skipped",
               ctx->audio_fail_count, ctx->audio_fail_count == 1 ? "" : "s");
      ui_stage_fail(pass->output_name, err);
      ui_hint("a release missing an announced audio track is broken; "
              "cached intermediates are kept, fix the source and re-run");
      ctx->pipeline_failed = 1;
    } else if (vr.error != 0) {
      ui_stage_fail(pass->output_name, "video encode failed — mux skipped");
      ctx->pipeline_failed = 1;
    } else {
      mr = final_mux(&mux_cfg);

      if (mr.skipped) {
        ui_stage_skip(pass->output_name, "already exists");
      } else if (mr.error == 0) {
        char detail[64];
        snprintf(detail, sizeof(detail), "%s", ui_fmt_duration(difftime(time(NULL), mux_t0)));
        ui_stage_ok(pass->output_name, detail);
      } else {
        char err[128];
        snprintf(err, sizeof(err), "error %d (%d audio + %d sub inputs)", mr.error, ctx->opus_count,
                 ctx->srt_count);
        ui_stage_fail(pass->output_name, err);
        ui_hint("intermediates kept on disk; inspect them next to the "
                "source file before re-running");
        ctx->pipeline_failed = 1;
      }
    }

    /* Clean up intermediate files on success. Leaving sidecar .srt
       files next to the final MKV causes players to auto-load them
       as external subtitles, overriding the embedded defaults. */
    int removed = 0;
    if (mr.error == 0) {
      /* For --companion-hd and --scale-to-hd, audio/subtitle intermediates
         are reused by the HD mux; defer their removal to the HD cleanup pass.
         Only the pass's video.mkv and RPU (if not DV) are removed here. */
      if (!pass->defer_shared_cleanup) {
        for (int i = 0; i < ctx->opus_count; i++)
          if (remove(ctx->opus_paths[i]) == 0)
            removed++;
        for (int i = 0; i < ctx->srt_count; i++) {
          /* Skip user-supplied --srt files: they live outside ctx->output_dir
             or weren't created by us. Only remove SRTs we wrote into
             the output directory. */
          if (strncmp(ctx->srt_paths[i], ctx->output_dir, strlen(ctx->output_dir)) == 0 &&
              remove(ctx->srt_paths[i]) == 0)
            removed++;
        }
      }
      if (remove(av1_video_path) == 0)
        removed++;
      /* For --companion-hd the RPU is reused by the HD encode;
         defer deletion to the HD cleanup pass. */
      if (ctx->rpu_path[0] && !pass->defer_shared_cleanup && remove(ctx->rpu_path) == 0)
        removed++;
      if (removed > 0) {
        char detail[64];
        snprintf(detail, sizeof(detail), "%d intermediate file%s", removed,
                 removed == 1 ? "" : "s");
        ui_stage_ok("Cleanup", detail);
      }
      /* Clean up cache directory when everything succeeded
       * For companion-hd, defer cleanup to the HD mux pass (audio/subtitle
       * intermediates are shared between 4K and HD muxes). */
      if (mr.error == 0 && !pass->defer_shared_cleanup)
        vmav_cleanup_cache_dir(ctx);
    }

    /* ---- Done receipt ---- */
    if (mr.error == 0 && !mr.skipped) {
      struct stat fst;
      long long final_bytes = 0;
      if (stat(final_path, &fst) == 0)
        final_bytes = (long long)fst.st_size;

      double elapsed = difftime(time(NULL), ctx->encode_start_time);
      double avg_kbps = 0.0;
      if (ctx->info.duration > 0.5 && final_bytes > 0)
        avg_kbps = ((double)final_bytes * 8.0) / (ctx->info.duration * 1000.0);
      double delta_pct = bitrate > 0 ? (avg_kbps - bitrate) / bitrate * 100.0 : 0.0;
      double speed = elapsed > 0.5 ? ctx->info.duration / elapsed : 0.0;

      /* Done receipt always renders — it's the headline result. */
      int saved_quiet_done = ui_is_quiet();
      ui_set_quiet(0);
      ui_section(pass->is_hd && ctx->opt.companion_hd ? "HD Done" : "Done");
      ui_kv("Output", "%s", final_path);
      ui_kv("Size", "%s", ui_fmt_bytes(final_bytes));
      if (!pass->is_hd && bitrate > 0 && avg_kbps > 0)
        ui_kv("Bitrate", "%.0f kbps avg  (%+.1f%% vs %d kbps target)", avg_kbps, delta_pct,
              bitrate);
      ui_kv("Duration", "%s  encoded in %s  (%.2f× realtime)", ui_fmt_duration(ctx->info.duration),
            ui_fmt_duration(elapsed), speed);
      ui_set_quiet(saved_quiet_done);
    }
  }

  return STAGE_CONTINUE;
}
