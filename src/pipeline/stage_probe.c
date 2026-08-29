/**
 * @file stage_probe.c
 * @brief Probe/report stage: media info, color, crop, tracks, early-resume
 *        check, and OCR preflight.
 */

#include <stdio.h>
#include <string.h>

#include "pipeline.h"
#include "subtitle_convert.h"
#include "ui.h"

StageStatus stage_probe(PipelineCtx *ctx) {
  const char *filepath = ctx->opt.filepath;
  int do_hd = ctx->opt.companion_hd || ctx->opt.scale_to_hd;

  /* ---- Source ---- */
  ctx->info = get_media_info(filepath);
  if (ctx->info.error != 0) {
    char err[64];
    snprintf(err, sizeof(err), "could not probe %s (error %d)", filepath, ctx->info.error);
    ui_stage_fail("Source probe", err);
    ui_hint("verify the path and that ffmpeg can read the container");
    return STAGE_EXIT_FAIL;
  }
  ui_section("Source");
  ui_kv("File", "%s", filepath);
  ui_kv("Resolution", "%d\xc3\x97%d", ctx->info.width, ctx->info.height);
  ui_kv("Duration", "%s", ui_fmt_duration(ctx->info.duration));
  ui_kv("Framerate", "%.3f fps", ctx->info.framerate);

  /* ---- Color (HDR) ---- */
  ctx->hdr = get_hdr_info(filepath);
  if (ctx->hdr.error == 0) {
    ui_section("Color");
    ui_kv("HDR10", "%s", ctx->hdr.has_hdr10 ? "yes" : "no");
    if (ctx->hdr.has_dolby_vision)
      ui_kv("Dolby Vision", "yes  (profile %d, level %d)", ctx->hdr.dv_profile, ctx->hdr.dv_level);
    else
      ui_kv("Dolby Vision", "no");
    ui_kv("HDR10+", "%s", ctx->hdr.has_hdr10plus ? "yes" : "no");
  }

  /* ---- Crop ---- */
  ctx->crop = get_crop_info(filepath);
  if (ctx->crop.error == 0 &&
      (ctx->crop.top || ctx->crop.bottom || ctx->crop.left || ctx->crop.right)) {
    ui_section("Crop");
    ui_kv("Detected", "T/B %d/%d   L/R %d/%d", ctx->crop.top, ctx->crop.bottom, ctx->crop.left,
          ctx->crop.right);
  }

  /* ---- Tracks ---- */
  ctx->tracks = get_media_tracks(filepath);
  TrackInfo *best = NULL;
  int best_count = 0;
  if (ctx->tracks.error == 0) {
    ui_section("Tracks");

    int split_fre = (ctx->opt.lang_tag == LANG_TAG_MULTI_VF2) ? 1 : 0;
    best = select_best_audio_per_language(&ctx->tracks, split_fre, &best_count);

    /* ── Audio table ──────────────────────────────────────────────────────
     * Columns: #(2) lng(3) codec(7) ch(3) bitrate(10) title(20)
     * "→" marker (3 UTF-8 bytes, 1 display col) + space sits outside the
     * left border so the │ stays at the same column for all rows.       */
    ui_kv("Audio", "%d source track%s", ctx->tracks.audio_count,
          ctx->tracks.audio_count == 1 ? "" : "s");
    // clang-format off
    ui_row("  \xe2\x94\x8c\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xac\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xac\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xac\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xac\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xac\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x90");
    ui_row("  \xe2\x94\x82 %2s \xe2\x94\x82 %-3s \xe2\x94\x82 %-7s \xe2\x94\x82 %-3s \xe2\x94\x82 %10s \xe2\x94\x82 %-20s \xe2\x94\x82",
           " #", "lng", "codec", " ch", "   bitrate", "title");
    ui_row("  \xe2\x94\x9c\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xbc\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xbc\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xbc\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xbc\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xbc\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xa4");
    // clang-format on
    for (int i = 0; i < ctx->tracks.audio_count; i++) {
      long long kbps = (long long)(ctx->tracks.audio[i].bitrate / 1000);
      char rate_buf[16];
      if (kbps > 0)
        snprintf(rate_buf, sizeof(rate_buf), "%5lld kbps", kbps);
      else
        snprintf(rate_buf, sizeof(rate_buf), "          ");
      char ch_buf[8];
      snprintf(ch_buf, sizeof(ch_buf), "%dch", ctx->tracks.audio[i].channels);
      bool sel = false;
      for (int j = 0; j < best_count && !sel; j++)
        sel = (best[j].index == ctx->tracks.audio[i].index);
      // clang-format off
      ui_row(
          "%s\xe2\x94\x82 %2d \xe2\x94\x82 %-3s \xe2\x94\x82 %-7.7s \xe2\x94\x82 %-3s \xe2\x94\x82 %s \xe2\x94\x82 %-20.20s \xe2\x94\x82",
          (int)sel ? "\xe2\x86\x92 " : "  ", ctx->tracks.audio[i].index,
          ctx->tracks.audio[i].language, ctx->tracks.audio[i].codec, ch_buf, rate_buf,
          ctx->tracks.audio[i].name);
      // clang-format on
    }
    // clang-format off
    ui_row("  \xe2\x94\x94\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xb4\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xb4\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xb4\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xb4\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xb4\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x98");
    // clang-format on

    if (best && best_count > 0) {
      char idx_buf[64] = "";
      size_t pos = 0;
      for (int i = 0; i < best_count; i++) {
        int n =
            snprintf(idx_buf + pos, sizeof(idx_buf) - pos, i > 0 ? "  #%d" : "#%d", best[i].index);
        if (n > 0 && (size_t)n < sizeof(idx_buf) - pos)
          pos += (size_t)n;
      }
      ui_kv("Selected", "%d track%s for encode  (%s)", best_count, best_count == 1 ? "" : "s",
            idx_buf);
    }

    /* ── Subtitle table ───────────────────────────────────────────────────
     * Columns: #(2) lng(3) fmt(3) type(6) title(20)                     */
    int n_karaoke = 0;
    for (int i = 0; i < ctx->tracks.subtitle_count; i++)
      if (ctx->tracks.subtitles[i].is_karaoke)
        n_karaoke++;
    int n_visible = ctx->tracks.subtitle_count - n_karaoke;
    if (n_karaoke > 0)
      ui_kv("Subtitles", "%d source track%s  (%d karaoke excluded)", n_visible,
            n_visible == 1 ? "" : "s", n_karaoke);
    else
      ui_kv("Subtitles", "%d source track%s", ctx->tracks.subtitle_count,
            ctx->tracks.subtitle_count == 1 ? "" : "s");

    // Pre-compute which PGS subtitles will be skipped (already have text SRT)
    // Two-pass: first collect all SRT tracks, then mark PGS as skipped
    bool pgs_skipped[256] = {false}; // max 256 subtitle tracks
    int pgs_skipped_count = 0;

    // Track SRTs we've seen: (lang, variant, forced, sdh) for skip detection
    int srt_seen_count = 0;
    char srt_seen_lang[64][64];
    int srt_seen_variant[64]; // FRENCH_VARIANT_* or 0 for non-French
    int srt_seen_forced[64];
    int srt_seen_sdh[64];

    // PASS 1: Collect all SRT tracks
    for (int i = 0; i < ctx->tracks.subtitle_count; i++) {
      TrackInfo *sub = &ctx->tracks.subtitles[i];
      if (sub->is_karaoke)
        continue;
      const char *lang = sub->language[0] ? sub->language : "und";

      if (is_text_subtitle(sub) && srt_seen_count < 64) {
        snprintf(srt_seen_lang[srt_seen_count], sizeof(srt_seen_lang[0]), "%s", lang);
        srt_seen_variant[srt_seen_count] = detect_track_french_variant(sub);
        srt_seen_forced[srt_seen_count] = sub->is_forced;
        srt_seen_sdh[srt_seen_count] = sub->is_sdh;
        srt_seen_count++;
      }
    }

    // PASS 2: Mark PGS subtitles that have matching SRT (anywhere in list)
    for (int i = 0; i < ctx->tracks.subtitle_count; i++) {
      TrackInfo *sub = &ctx->tracks.subtitles[i];
      if (sub->is_karaoke)
        continue;

      if (is_pgs_subtitle(sub)) {
        const char *lang = sub->language[0] ? sub->language : "und";
        int pgs_variant = detect_track_french_variant(sub);
        for (int j = 0; j < srt_seen_count; j++) {
          if (strcmp(srt_seen_lang[j], lang) == 0 && srt_seen_variant[j] == pgs_variant &&
              srt_seen_forced[j] == sub->is_forced && srt_seen_sdh[j] == sub->is_sdh) {
            pgs_skipped[sub->index] = true;
            pgs_skipped_count++;
            break;
          }
        }
      }
    }

    if (n_visible > 0) {
      // clang-format off
      ui_row("  \xe2\x94\x8c\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xac\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xac\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xac\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xac\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x90");
      ui_row("  \xe2\x94\x82 %2s \xe2\x94\x82 %-3s \xe2\x94\x82 %-3s \xe2\x94\x82 %-6s \xe2\x94\x82 %-20s \xe2\x94\x82",
             " #", "lng", "fmt", " type", "title");
      ui_row("  \xe2\x94\x9c\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xbc\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xbc\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xbc\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xbc\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xa4");
      // clang-format on
      for (int i = 0; i < ctx->tracks.subtitle_count; i++) {
        const char *type_if_not_forced = ctx->tracks.subtitles[i].is_sdh ? "sdh" : "full";
        const char *type = ctx->tracks.subtitles[i].is_forced ? "forced" : type_if_not_forced;
        const char *lang =
            ctx->tracks.subtitles[i].language[0] ? ctx->tracks.subtitles[i].language : "und";
        const char *selection = "  ";

        // Determine selection marker
        if (ctx->tracks.subtitles[i].is_karaoke) {
          // Karaoke tracks: Not selected (×)
          selection = "\xc3\x97 ";
        } else if (is_text_subtitle(&ctx->tracks.subtitles[i])) {
          // Text SRT tracks: Selected for direct extraction (→)
          selection = "\xe2\x86\x92 ";
        } else if (is_pgs_subtitle(&ctx->tracks.subtitles[i])) {
          // PGS tracks: check if skipped or needs OCR
          if (pgs_skipped[ctx->tracks.subtitles[i].index]) {
            // Has matching SRT - not selected (×)
            selection = "\xc3\x97 ";
          } else {
            // Needs OCR conversion (O)
            selection = "O ";
          }
        }
        // clang-format off
        ui_row(
            "%s\xe2\x94\x82 %2d \xe2\x94\x82 %-3s \xe2\x94\x82 %-3s \xe2\x94\x82 %-6s \xe2\x94\x82 %-20.20s \xe2\x94\x82",
            selection, ctx->tracks.subtitles[i].index, lang,
            vmav_codec_short(ctx->tracks.subtitles[i].codec), type,
            ctx->tracks.subtitles[i].name);
        // clang-format on
      }
      // clang-format off
      ui_row("  \xe2\x94\x94\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xb4\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xb4\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xb4\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xb4\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x98");
      // clang-format on

      // Count subtitle types for display:
      // - text_count: SRT tracks (selected for direct extraction)
      // - pgs_count: PGS tracks that need OCR (not skipped)
      int text_count = 0, pgs_ocr_count = 0;
      for (int i = 0; i < ctx->tracks.subtitle_count; i++) {
        if (ctx->tracks.subtitles[i].is_karaoke)
          continue;
        if (is_text_subtitle(&ctx->tracks.subtitles[i]))
          text_count++;
        else if (is_pgs_subtitle(&ctx->tracks.subtitles[i]) &&
                 !pgs_skipped[ctx->tracks.subtitles[i].index])
          pgs_ocr_count++;
      }
      if (text_count > 0 || pgs_ocr_count > 0) {
        char detail[128] = "";
        size_t pos = 0;
        if (text_count > 0) {
          int n = snprintf(detail + pos, sizeof(detail) - pos, "%d direct SRT", text_count);
          if (n > 0 && (size_t)n < sizeof(detail) - pos)
            pos += (size_t)n;
        }
        if (pgs_ocr_count > 0) {
          int n = snprintf(detail + pos, sizeof(detail) - pos,
                           pos > 0 ? ", %d OCR PGS" : "%d OCR PGS", pgs_ocr_count);
          if (n > 0 && (size_t)n < sizeof(detail) - pos)
            pos += (size_t)n;
          if (pgs_skipped_count > 0) {
            (void)snprintf(detail + pos, sizeof(detail) - pos, " (%d already available)",
                           pgs_skipped_count);
          }
        }
        ui_kv("Processing", "%s", detail);
      }
    }
  }

  /* Early resume check: if .video.mkv already exists, skip encoding */
  char video_4k_cache_path[4096] = "";
  char video_hd_cache_path[4096] = "";

  /* Compute base name from filepath for cache lookup */
  char base_for_cache[1024];
  const char *fname = strrchr(filepath, '/');
  fname = fname ? fname + 1 : filepath;
  snprintf(base_for_cache, sizeof(base_for_cache), "%s", fname);
  char *ext = strrchr(base_for_cache, '.');
  if (ext)
    *ext = '\0';

  snprintf(video_4k_cache_path, sizeof(video_4k_cache_path), "%s/%s.video.mkv", ctx->cache_dir,
           base_for_cache);
  if (do_hd && !ctx->opt.scale_to_hd) {
    snprintf(video_hd_cache_path, sizeof(video_hd_cache_path), "%s/%s-HDLight.video.mkv",
             ctx->cache_dir, base_for_cache);
  } else {
    snprintf(video_hd_cache_path, sizeof(video_hd_cache_path), "%s/%s.video.mkv", ctx->cache_dir,
             base_for_cache);
  }

  if (vmav_file_exists(video_4k_cache_path)) {
    ui_section("Resume check");
    ui_stage_ok("skip", "4K video.mkv already present in cache, proceeding to mux");
    return STAGE_EXIT_OK;
  }

  if (vmav_file_exists(video_hd_cache_path)) {
    ui_section("Resume check");
    ui_stage_ok("skip", "HD video.mkv already present in cache, proceeding to mux");
    return STAGE_EXIT_OK;
  }

  /* ---- OCR preflight: verify tessdata before any expensive work ---- */
  if (ctx->tracks.error == 0 && ctx->tracks.subtitle_count > 0 && !ctx->opt.dry_run &&
      !ctx->opt.grain_only) {
    for (int i = 0; i < ctx->tracks.subtitle_count; i++) {
      TrackInfo *sub = &ctx->tracks.subtitles[i];
      if (sub->is_karaoke || !is_pgs_subtitle(sub))
        continue;
      const char *lang = sub->language[0] ? sub->language : "und";
      const char *tess_lang = iso639_to_tesseract_lang(lang);
      if (subtitle_ocr_preflight(tess_lang, NULL, 0) != 0) {
        ui_stage_fail("OCR preflight", "no usable tessdata for PGS subtitle OCR");
        ui_hint("install tessdata_best (eng+fra) and set TESSDATA_PREFIX, or drop PGS tracks");
        return STAGE_EXIT_FAIL;
      }
    }
  }

  return STAGE_CONTINUE;
}
