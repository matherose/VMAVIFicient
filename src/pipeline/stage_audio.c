/**
 * @file stage_audio.c
 * @brief Audio stage: select best tracks per language and encode to OPUS.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "audio_encode.h"
#include "media_tracks.h"
#include "pipeline.h"
#include "ui.h"

StageStatus stage_audio(PipelineCtx *ctx) {
  const char *filepath = ctx->opt.filepath;

  /* ---- OPUS audio encoding ---- */
  int enc_best_count = 0;
  int enc_split_fre = (ctx->resolved_lang_tag == LANG_TAG_MULTI_VF2) ? 1 : 0;
  TrackInfo *enc_best =
      select_best_audio_per_language(&ctx->tracks, enc_split_fre, &enc_best_count);

  /* Sort: French first, then English, then others */
  if (enc_best && enc_best_count > 1)
    qsort(enc_best, (size_t)enc_best_count, sizeof(TrackInfo), vmav_cmp_audio_order);

  /* Store OPUS paths, track names and languages for final mux.
     All three are written through the opus_count write-index so a
     failed track never leaves a hole in the mux inputs. */
  if (enc_best && enc_best_count > 0 && !ctx->opt.dry_run && !ctx->opt.grain_only) {
    ui_section("Audio");
    ui_kv("Encode", "%d track%s → OPUS", enc_best_count, enc_best_count == 1 ? "" : "s");
    for (int i = 0; i < enc_best_count && i < 32; i++) {
      /* In VF2 mode, each French track gets its own variant derived from
         the track title ("VFF - DTS-HD…", "VFQ - E-AC3…") so VFF and VFQ
         produce distinct .fre.fr.opus / .fre.ca.opus files. */
      FrenchVariant track_fv = ctx->fv;
      FrenchAudioOrigin track_origin = ctx->fr_audio_origin;
      if (enc_split_fre &&
          (strcmp(enc_best[i].language, "fre") == 0 || strcmp(enc_best[i].language, "fra") == 0)) {
        FrenchVariant detected = (FrenchVariant)detect_track_french_variant(&enc_best[i]);
        if (detected != FRENCH_VARIANT_UNKNOWN) {
          track_fv = detected;
          if (detected == FRENCH_VARIANT_VFQ)
            track_origin = FRENCH_AUDIO_VFQ;
          else if (detected == FRENCH_VARIANT_VFI)
            track_origin = FRENCH_AUDIO_VFI;
          else
            track_origin = FRENCH_AUDIO_VFF;
        }
      }

      char opus_name[2048];
      build_opus_filename(opus_name, sizeof(opus_name), ctx->base_name, enc_best[i].language,
                          track_fv);

      /* Write opus to cache directory */
      char opus_cache_path[4096];
      vmav_build_cache_path(ctx, opus_cache_path, sizeof(opus_cache_path), opus_name);
      snprintf(ctx->opus_paths[ctx->opus_count], sizeof(ctx->opus_paths[0]), "%s", opus_cache_path);

      /* Build display name and language for MKV track */
      build_audio_track_name(ctx->audio_names[ctx->opus_count], sizeof(ctx->audio_names[0]),
                             enc_best[i].language, enc_best[i].channels, track_origin);
      snprintf(ctx->audio_langs[ctx->opus_count], sizeof(ctx->audio_langs[0]), "%s",
               enc_best[i].language);

      ui_row("[%d/%d] %s  %s  %dch  %lld kbps  →  \"%s\"", i + 1, enc_best_count,
             enc_best[i].language, enc_best[i].codec, enc_best[i].channels,
             (long long)(enc_best[i].bitrate / 1000), ctx->audio_names[ctx->opus_count]);

      time_t track_t0 = time(NULL);
      if (ctx->opt.force)
        (void)remove(ctx->opus_paths[ctx->opus_count]);
      OpusEncodeResult r =
          encode_track_to_opus(filepath, &enc_best[i], ctx->opus_paths[ctx->opus_count]);

      if (r.skipped) {
        ui_stage_skip(opus_name, "already exists");
        ctx->opus_count++;
      } else if (r.error == 0) {
        char detail[64];
        snprintf(detail, sizeof(detail), "%s", ui_fmt_duration(difftime(time(NULL), track_t0)));
        ui_stage_ok(opus_name, detail);
        ctx->opus_count++;
      } else {
        char err[128];
        snprintf(err, sizeof(err), "stream #%d (%s, %dch): error %d", enc_best[i].index,
                 enc_best[i].codec, enc_best[i].channels, r.error);
        ui_stage_fail(opus_name, err);
        ui_hint("verify the source stream is decodable; opusenc-style "
                "channel layouts (>2ch) require ffmpeg with libopus");
        ctx->audio_fail_count++;
        ctx->pipeline_failed = 1;
      }
    }
  }

  if (enc_best)
    free(enc_best);

  return STAGE_CONTINUE;
}
