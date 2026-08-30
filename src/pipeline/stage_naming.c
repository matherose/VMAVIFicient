/**
 * @file stage_naming.c
 * @brief Naming stage: resolve output filename/title via TMDB or --blind.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "media_naming.h"
#include "media_tracks.h"
#include "pipeline.h"
#include "tmdb.h"
#include "ui.h"

/**
 * @brief Do two paths name the same file on disk?
 *
 * Compared by device and inode rather than by string, so "./a.mkv" and
 * "a.mkv", a symlink and its target, and relative versus absolute spellings
 * all resolve correctly. A path that does not exist cannot collide, so a
 * failed stat answers false.
 */
static bool same_file(const char *a, const char *b) {
  struct stat sa;
  struct stat sb;
  if (stat(a, &sa) != 0 || stat(b, &sb) != 0)
    return false;
  return (bool)(sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino);
}

/**
 * @brief Prompt user to choose a language tag interactively.
 *
 * Lists available audio languages from the source file, then presents
 * all language tag options.
 */
static LanguageTag ask_language_tag(const MediaTracks *tracks) {
  if (tracks && tracks->error == 0 && tracks->audio_count > 0) {
    printf("\nAudio languages found in source:\n");
    char seen[32][8];
    int seen_count = 0;
    for (int i = 0; i < tracks->audio_count; i++) {
      const char *lang = tracks->audio[i].language[0] ? tracks->audio[i].language : "und";
      bool dup = false;
      for (int j = 0; j < seen_count; j++) {
        if (strcmp(seen[j], lang) == 0) {
          dup = true;
          break;
        }
      }
      if (!dup && seen_count < 32) {
        snprintf(seen[seen_count], sizeof(seen[0]), "%s", lang);
        seen_count++;
        printf("  - %s\n", lang);
      }
    }
  }

  printf("\nSelect language tag:\n"
         "   1) MULTi          2) MULTi.VFI       3) MULTi.VFF\n"
         "   4) MULTi.VFQ      5) MULTi.VF2       6) MULTi.VOF\n"
         "   7) DUAL.VFI       8) DUAL.VFF        9) DUAL.VFQ\n"
         "  10) FRENCH        11) VFF            12) VOF\n"
         "  13) TRUEFRENCH    14) VO             15) VOST\n"
         "  16) FANSUB\n"
         "Choice [1-16]: ");
  (void)fflush(stdout);

  char line[16];
  if (!fgets(line, sizeof(line), stdin))
    return LANG_TAG_MULTI;

  switch (vmav_parse_int_or_zero(line)) {
  case 1:
    return LANG_TAG_MULTI;
  case 2:
    return LANG_TAG_MULTI_VFI;
  case 3:
    return LANG_TAG_MULTI_VFF;
  case 4:
    return LANG_TAG_MULTI_VFQ;
  case 5:
    return LANG_TAG_MULTI_VF2;
  case 6:
    return LANG_TAG_MULTI_VOF;
  case 7:
    return LANG_TAG_DUAL_VFI;
  case 8:
    return LANG_TAG_DUAL_VFF;
  case 9:
    return LANG_TAG_DUAL_VFQ;
  case 10:
    return LANG_TAG_FRENCH;
  case 11:
    return LANG_TAG_VFF;
  case 12:
    return LANG_TAG_VOF;
  case 13:
    return LANG_TAG_TRUEFRENCH;
  case 14:
    return LANG_TAG_VO;
  case 15:
    return LANG_TAG_VOST;
  case 16:
    return LANG_TAG_FANSUB;
  default:
    return LANG_TAG_MULTI;
  }
}

/**
 * @brief Prompt user to choose a source type interactively.
 */
static SourceType ask_source(void) {
  printf("\nSource not detected from filename. Select source:\n"
         "   1) BDRip          2) BluRay          3) REMUX\n"
         "   4) DVDRip         5) DVDRemux        6) WEBRip\n"
         "   7) WEB-DL        8) WEB             9) HDTV\n"
         "  10) HDRip         11) TVRip          12) VHSRip\n"
         "Choice [1-12]: ");
  (void)fflush(stdout);

  char line[16];
  if (!fgets(line, sizeof(line), stdin))
    return SOURCE_BLURAY;

  switch (vmav_parse_int_or_zero(line)) {
  case 1:
    return SOURCE_BDRIP;
  case 2:
    return SOURCE_BLURAY;
  case 3:
    return SOURCE_REMUX;
  case 4:
    return SOURCE_DVDRIP;
  case 5:
    return SOURCE_DVDREMUX;
  case 6:
    return SOURCE_WEBRIP;
  case 7:
    return SOURCE_WEBDL;
  case 8:
    return SOURCE_WEB;
  case 9:
    return SOURCE_HDTV;
  case 10:
    return SOURCE_HDRIP;
  case 11:
    return SOURCE_TVRIP;
  case 12:
    return SOURCE_VHSRIP;
  default:
    return SOURCE_BLURAY;
  }
}

/**
 * @brief Prompt for a positive integer on stdin (fgets + parse, no scanf).
 * @return 0 on success, -1 on EOF/non-interactive stdin or 3 bad answers.
 */
static int ask_positive_int(const char *prompt, int *out) {
  char line[16];
  for (int tries = 0; tries < 3; tries++) {
    printf("%s", prompt);
    (void)fflush(stdout);
    if (!fgets(line, sizeof(line), stdin))
      return -1;
    int v = vmav_parse_int_or_zero(line);
    if (v > 0) {
      *out = v;
      return 0;
    }
    printf("Please enter a positive number.\n");
  }
  return -1;
}

StageStatus stage_naming(PipelineCtx *ctx) {
  const char *filepath = ctx->opt.filepath;

  /* ---- Naming setup (TMDB or --blind) ---- */
  ctx->source = ctx->opt.source;
  ctx->fv = FRENCH_VARIANT_UNKNOWN;
  ctx->fr_audio_origin = FRENCH_AUDIO_VFF;
  ctx->resolved_lang_tag = ctx->opt.lang_tag;
  ctx->video_language = "und";

  if (ctx->opt.blind) {
    /* Derive the output name straight from the input filepath: no TMDB,
       no release-group suffix — just `<input-stem>.mkv` next to the
       source file. Used by CI and anyone without a config.ini. */
    const char *fname = strrchr(filepath, '/');
    fname = fname ? fname + 1 : filepath;
    snprintf(ctx->base_name, sizeof(ctx->base_name), "%s", fname);
    char *dot = strrchr(ctx->base_name, '.');
    if (dot)
      *dot = '\0';
    snprintf(ctx->output_name, sizeof(ctx->output_name), "%s.mkv", ctx->base_name);
    snprintf(ctx->mkv_title, sizeof(ctx->mkv_title), "%s", ctx->base_name);

    snprintf(ctx->output_dir, sizeof(ctx->output_dir), "%s", filepath);
    char *slash = strrchr(ctx->output_dir, '/');
    if (slash)
      *(slash + 1) = '\0';
    else
      snprintf(ctx->output_dir, sizeof(ctx->output_dir), "./");

    /* `<stem>.mkv` next to the source collides with the source itself when
       the input is already a .mkv. Every later stage then sees its output
       "already exists" and skips, so the run finishes reporting success
       while writing nothing — the input is left untouched and no encode
       happens. Fail here instead of silently doing nothing. */
    char blind_out[sizeof(ctx->output_dir) + sizeof(ctx->output_name)];
    snprintf(blind_out, sizeof(blind_out), "%s%s", ctx->output_dir, ctx->output_name);
    if (same_file(blind_out, filepath)) {
      ui_stage_fail("Naming", "--blind output would overwrite the input");
      ui_hint("--blind names the output <input-stem>.mkv beside the source, "
              "which collides when the input is itself a .mkv — rename the "
              "input, move it, or use --tmdb <id> to name the release");
      return STAGE_EXIT_FAIL;
    }

    if (ctx->tracks.error == 0 && ctx->tracks.audio_count > 0 && ctx->tracks.audio[0].language[0])
      ctx->video_language = ctx->tracks.audio[0].language;

    return STAGE_CONTINUE;
  }
  if (ctx->opt.tmdb_id > 0) {
    /* Common metadata, filled from the movie or TV endpoint. */
    char meta_title[512] = "";
    int meta_year = 0;
    char meta_lang[8] = "";
    bool meta_ok = false;
    EpisodeInfo ep = {0};

    if (ctx->opt.tv_mode) {
      /* S/E resolution: flags > filename parse > interactive prompt. */
      int season = ctx->opt.season;
      int episode = ctx->opt.episode;
      if (season <= 0 || episode <= 0) {
        const char *fname = strrchr(filepath, '/');
        fname = fname ? fname + 1 : filepath;
        int ps = 0, pe = 0;
        if (parse_season_episode(fname, &ps, &pe) == 0) {
          if (season <= 0)
            season = ps;
          if (episode <= 0)
            episode = pe;
        }
      }
      if (season <= 0 &&
          ask_positive_int("\nSeason not detected from filename. Season: ", &season) != 0) {
        ui_stage_fail("Naming", "season unknown; pass --season <N>");
        return STAGE_EXIT_FAIL;
      }
      if (episode <= 0 &&
          ask_positive_int("Episode not detected from filename. Episode: ", &episode) != 0) {
        ui_stage_fail("Naming", "episode unknown; pass --episode <N>");
        return STAGE_EXIT_FAIL;
      }

      ui_section("TMDB lookup");
      ui_kv("TV ID", "%d", ctx->opt.tmdb_id);
      ui_kv("Episode", "S%02dE%02d", season, episode);
      TmdbTvInfo tv = tmdb_fetch_tv(ctx->opt.tmdb_id);
      if (tv.error != 0) {
        ui_stage_fail("TMDB fetch", "could not fetch TV show info");
        ui_hint("verify TMDB_API_KEY is set in config.ini and the ID is a "
                "TV series ID (tmdb.org/tv/<id>, not /movie/)");
      } else {
        snprintf(meta_title, sizeof(meta_title), "%s", tv.original_name);
        meta_year = tv.first_air_year;
        snprintf(meta_lang, sizeof(meta_lang), "%s", tv.original_language);
        ep.season = season;
        ep.episode = episode;
        TmdbEpisodeInfo epi = tmdb_fetch_episode(ctx->opt.tmdb_id, season, episode);
        if (epi.error == 0)
          snprintf(ep.title, sizeof(ep.title), "%s", epi.name);
        else
          ui_hint("episode title unavailable on TMDB; filename will omit it");
        meta_ok = true;
      }
    } else {
      ui_section("TMDB lookup");
      ui_kv("Movie ID", "%d", ctx->opt.tmdb_id);
      TmdbMovieInfo tmdb = tmdb_fetch_movie(ctx->opt.tmdb_id);
      if (tmdb.error != 0) {
        ui_stage_fail("TMDB fetch", "could not fetch movie info");
        ui_hint("verify TMDB_API_KEY is set in config.ini and the ID is "
                "correct (e.g. tmdb.org/movie/<id>)");
      } else {
        snprintf(meta_title, sizeof(meta_title), "%s", tmdb.original_title);
        meta_year = tmdb.release_year;
        snprintf(meta_lang, sizeof(meta_lang), "%s", tmdb.original_language);
        meta_ok = true;
      }
    }

    if (meta_ok) {
      ui_kv("Title", "%s", meta_title);
      if (ctx->opt.tv_mode) {
        if (ep.title[0])
          ui_kv("Episode title", "%s", ep.title);
      } else {
        ui_kv("Year", "%d", meta_year);
      }
      ui_kv("Language", "%s", meta_lang);

      /* Source: CLI flag > filename detection > interactive prompt. */
      if (ctx->source == SOURCE_UNKNOWN)
        ctx->source = detect_source_from_filename(filepath);
      if (ctx->source == SOURCE_UNKNOWN)
        ctx->source = ask_source();

      /* Determine French variant for OPUS naming. */
      bool has_french = false;
      if (ctx->tracks.error == 0) {
        for (int i = 0; i < ctx->tracks.audio_count; i++) {
          if (strcmp(ctx->tracks.audio[i].language, "fre") == 0 ||
              strcmp(ctx->tracks.audio[i].language, "fra") == 0) {
            has_french = true;
            break;
          }
        }
      }
      if (has_french)
        ctx->fv = detect_french_variant_from_filename(filepath);

      /* Language: CLI flag > auto-detection > interactive prompt. */
      if (ctx->opt.lang_tag != LANG_TAG_NONE) {
        ctx->resolved_lang_tag = ctx->opt.lang_tag;
      } else {
        LanguageTag auto_tag = determine_language_tag(&ctx->tracks, meta_lang, ctx->fv);

        /* If auto-detection produced a definitive result, use it.
           Otherwise ask the user interactively. */
        if (auto_tag != LANG_TAG_VO || ctx->tracks.audio_count <= 1) {
          ctx->resolved_lang_tag = auto_tag;
        } else {
          ctx->resolved_lang_tag = ask_language_tag(&ctx->tracks);
        }
      }

      snprintf(ctx->saved_tmdb_title, sizeof(ctx->saved_tmdb_title), "%s", meta_title);
      ctx->saved_tmdb_year = meta_year;
      ctx->saved_episode = ep;
      ctx->saved_is_tv = ctx->opt.tv_mode;

      build_output_filename(ctx->output_name, sizeof(ctx->output_name), meta_title, meta_year,
                            ctx->resolved_lang_tag, &ctx->info, &ctx->hdr, ctx->source,
                            (int)ctx->opt.tv_mode ? &ep : NULL);

      /* Strip .mkv to get base name. */
      snprintf(ctx->base_name, sizeof(ctx->base_name), "%s", ctx->output_name);
      char *ext = strrchr(ctx->base_name, '.');
      if (ext && strcmp(ext, ".mkv") == 0)
        *ext = '\0';

      /* Output dir: same directory as input file. */
      snprintf(ctx->output_dir, sizeof(ctx->output_dir), "%s", filepath);
      char *last_slash = strrchr(ctx->output_dir, '/');
      if (last_slash)
        *(last_slash + 1) = '\0';
      else
        snprintf(ctx->output_dir, sizeof(ctx->output_dir), "./");

      /* ---- Resolve FrenchAudioOrigin ----
         The CLI language tag wins over the filename-derived French variant
         so that e.g. --multivfi on a source with no VFI marker still labels
         tracks as VFI. */
      switch (ctx->resolved_lang_tag) {
      case LANG_TAG_MULTI_VFI:
      case LANG_TAG_DUAL_VFI:
        ctx->fv = FRENCH_VARIANT_VFI;
        break;
      case LANG_TAG_MULTI_VFQ:
      case LANG_TAG_DUAL_VFQ:
        ctx->fv = FRENCH_VARIANT_VFQ;
        break;
      case LANG_TAG_MULTI_VFF:
      case LANG_TAG_DUAL_VFF:
      case LANG_TAG_VFF:
      case LANG_TAG_TRUEFRENCH:
      case LANG_TAG_FRENCH:
        ctx->fv = FRENCH_VARIANT_VFF;
        break;
      default:
        /* Keep filename-detected fv as-is for MULTI / VO / VOST / etc. */
        break;
      }

      if (strcmp(meta_lang, "fr") == 0) {
        ctx->fr_audio_origin = FRENCH_AUDIO_VO;
      } else {
        switch (ctx->fv) {
        case FRENCH_VARIANT_VFQ:
          ctx->fr_audio_origin = FRENCH_AUDIO_VFQ;
          break;
        case FRENCH_VARIANT_VFI:
          ctx->fr_audio_origin = FRENCH_AUDIO_VFI;
          break;
        default:
          ctx->fr_audio_origin = FRENCH_AUDIO_VFF;
          break;
        }
      }

      if (ctx->opt.tv_mode) {
        if (ep.title[0])
          snprintf(ctx->mkv_title, sizeof(ctx->mkv_title), "%s - S%02dE%02d - %s", meta_title,
                   ep.season, ep.episode, ep.title);
        else
          snprintf(ctx->mkv_title, sizeof(ctx->mkv_title), "%s - S%02dE%02d", meta_title, ep.season,
                   ep.episode);
      } else {
        snprintf(ctx->mkv_title, sizeof(ctx->mkv_title), "%s (%d)", meta_title, meta_year);
      }
      {
        const char *vlang = iso639_1_to_2b(meta_lang);
        if (vlang)
          ctx->video_language = vlang;
      }

      return STAGE_CONTINUE;
    }

    /* TMDB fetch failed: today main just skips the whole encode body and
       exits 0 at the bottom — preserve that behavior. */
    return STAGE_EXIT_OK;
  }
  /* Neither --blind nor --tmdb: without this branch the pipeline body
     below is silently skipped and the program exits 0 after grain
     analysis, having encoded nothing. */
  ui_stage_fail("Naming", "no naming source: pass --tmdb <id> or --blind");
  ui_hint("--tmdb <id> names from TMDB metadata; --blind names the "
          "output <input-stem>.mkv next to the source");
  return STAGE_EXIT_FAIL;
}
