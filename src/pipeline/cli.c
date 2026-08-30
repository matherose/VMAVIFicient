/**
 * @file cli.c
 * @brief CLI argument parsing for vmavificient.
 *
 * Moved verbatim from src/main/main.c (print_usage, option enum,
 * long_options[], getopt_long loop, post-loop validation, prescan loop).
 */

#include <getopt.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "pipeline.h"
#include "ui.h"

void vmav_print_usage(const char *prog) {
  (void)fprintf(stderr,
                "Usage: %s [options] <input_file>\n"
                "\n"
                "Options:\n"
                "  --tmdb <id>      TMDB movie ID for naming (requires TMDB_API_KEY)\n"
                "  --tv             TV mode: --tmdb <id> is a TMDB series ID\n"
                "                   (themoviedb.org/tv/<id>). Output is named\n"
                "                   Show.SxxEyy.Episode.Title.<...> — no year.\n"
                "  --mv             Movie mode (the default; explicit form)\n"
                "  --season <N>     Season number (with --tv; overrides filename\n"
                "                   parsing, prompts if still unknown)\n"
                "  --episode <N>    Episode number (with --tv; same resolution\n"
                "                   order as --season)\n"
                "  --blind          Skip TMDB lookup; name output as <input-stem>.mkv\n"
                "                   (no config required)\n"
                "  --config         Run interactive setup once; writes\n"
                "                   $HOME/.config/vmavificient/config.ini with the TMDB\n"
                "                   API key and release group. Subsequent runs read it\n"
                "                   automatically.\n"
                "  --crf <N>        Skip CRF search; encode at this CRF directly\n"
                "                   (1–63, lower = higher quality)\n"
                "  --vmaf-target <N> Override the VMAF target for CRF search\n"
                "                   (default: per-preset, 90–96)\n"
                "  --bitrate <kbps> Skip CRF search; encode VBR at this bitrate\n"
                "  --srt <path>     Additional SRT subtitle file (can be repeated)\n"
                "  --force          Redo stages whose output already exists. Without\n"
                "                   it a leftover file is reused, which silently hides\n"
                "                   the effect of a changed binary or settings\n"
                "  --dry-run        Run analysis + CRF search + naming, print the\n"
                "                   encoding plan, then exit. No files written.\n"
                "  --quiet          Compact output: hide informational sections, keep\n"
                "                   only stage status lines + the Plan / Done blocks.\n"
                "  --verbose        Forward SVT-AV1 encoder log messages to stderr\n"
                "                   (rate control, GOP layout, warnings). Composes\n"
                "                   with --quiet.\n"
                "  --grain-only     Like --dry-run, plus dump every encoder knob the\n"
                "                   resolved preset configures (grain mech, tune,\n"
                "                   ac-bias, filters, QMs). For sanity-checking what\n"
                "                   each preset actually does without a full encode.\n"
                "  --companion-hd   After the 4K encode, produce a second 1080p HDLight\n"
                "                   release from the same REMUX source. Requires a 4K\n"
                "                   source. Audio and subtitles are shared between both\n"
                "                   outputs. Dolby Vision is stripped from the HD "
                "output.\n"
                "  --scale-to-hd    Produce only a 1080p HDLight release (no 4K "
                "output).\n"
                "                   Requires a 4K source. Full independent pipeline.\n"
                "                   Mutually exclusive with --companion-hd.\n"
                "  --cache-dir <path>\n"
                "                   Use specified directory for intermediate files\n"
                "                   (grain analysis, CRF search results, extracted\n"
                "                   audio/subtitles). Cache is deleted after successful\n"
                "                   encode. Defaults to a hidden .vmavificient-cache folder\n"
                "                   in the project root.\n"
                "  --help           Show this help\n"
                "\n"
                "Language flags (override auto-detection):\n"
                "  --multi          MULTi\n"
                "  --multivfi       MULTi.VFI\n"
                "  --multivff       MULTi.VFF\n"
                "  --multivfq       MULTi.VFQ\n"
                "  --multivf2       MULTi.VF2\n"
                "  --multivof       MULTi.VOF\n"
                "  --dual_vfi       DUAL.VFI\n"
                "  --dual_vff       DUAL.VFF\n"
                "  --dual_vfq       DUAL.VFQ\n"
                "  --french         FRENCH\n"
                "  --vff            VFF\n"
                "  --vof            VOF\n"
                "  --truefrench     TRUEFRENCH\n"
                "  --vo             VO\n"
                "  --vost           VOST\n"
                "  --fansub         FANSUB\n"
                "\n"
                "Source flags (override auto-detection):\n"
                "  --bdrip          BDRip\n"
                "  --bluray         BluRay\n"
                "  --remux          REMUX\n"
                "  --dvdrip         DVDRip\n"
                "  --dvdremux       DVDRemux\n"
                "  --webrip         WEBRip\n"
                "  --webdl          WEB-DL\n"
                "  --web            WEB\n"
                "  --hdtv           HDTV\n"
                "  --hdrip          HDRip\n"
                "  --tvrip          TVRip\n"
                "  --vhsrip         VHSRip\n"
                "\n"
                "Quality presets (default: live-action):\n"
                "  --animation       Animation content\n"
                "  --super35_analog  Super 35mm analog film\n"
                "  --super35_digital Super 35mm digital\n"
                "  --imax_analog     IMAX analog film\n"
                "  --imax_digital    IMAX digital\n",
                prog);
}

int vmav_cli_prescan(int argc, char *argv[], VmavOptions *opt) {
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      vmav_print_usage(argv[0]);
      return 0;
    }
    if (strcmp(argv[i], "--config") == 0)
      return config_interactive_setup();
    if (strcmp(argv[i], "--blind") == 0)
      opt->blind = true;
  }
  return -1;
}

int vmav_cli_parse(int argc, char *argv[], VmavOptions *opt) {
  /* Initialize non-zero defaults (calloc zeros everything else). */
  opt->lang_tag = LANG_TAG_NONE;
  opt->source = SOURCE_UNKNOWN;
  opt->quality = QUALITY_LIVEACTION;

  enum {
    OPT_TMDB = 't',
    OPT_HELP = 'h',
    OPT_BITRATE = 'b',
    OPT_SRT = 's',
    /* Language flags (start at 256 to avoid ASCII collision). */
    OPT_BLIND = 255,
    OPT_MULTI = 256,
    OPT_MULTIVFI = 257,
    OPT_MULTIVFF = 258,
    OPT_MULTIVFQ = 259,
    OPT_MULTIVF2 = 260,
    OPT_MULTIVOF = 261,
    OPT_DUAL_VFI = 262,
    OPT_DUAL_VFF = 263,
    OPT_DUAL_VFQ = 264,
    OPT_FRENCH = 265,
    OPT_VFF = 266,
    OPT_VOF = 267,
    OPT_TRUEFRENCH = 268,
    OPT_VO = 269,
    OPT_VOST = 270,
    OPT_FANSUB = 271,
    /* Source flags. */
    OPT_BDRIP = 272,
    OPT_BLURAY = 273,
    OPT_REMUX = 274,
    OPT_DVDRIP = 275,
    OPT_DVDREMUX = 276,
    OPT_WEBRIP = 277,
    OPT_WEBDL = 278,
    OPT_WEB = 279,
    OPT_HDTV = 280,
    OPT_HDRIP = 281,
    OPT_TVRIP = 282,
    OPT_VHSRIP = 283,
    /* Quality preset flags. */
    OPT_ANIMATION = 284,
    OPT_SUPER35_ANALOG = 285,
    OPT_SUPER35_DIGITAL = 286,
    OPT_IMAX_ANALOG = 287,
    OPT_IMAX_DIGITAL = 288,
    /* Auxiliary flags appended at the end so they get the next free
       sequential value without colliding with the explicit OPT_MULTI=256
       anchor above. */
    OPT_DRY_RUN = 289,
    OPT_QUIET = 290,
    OPT_VERBOSE = 291,
    OPT_GRAIN_ONLY = 292,
    /* --config is pre-scanned and dispatched before getopt_long runs;
       it's still registered with getopt so the parser doesn't reject it
       when it shows up alongside other flags. Placed at the end so its
       auto-incremented value can't collide with OPT_MULTI = 256 anchor. */
    OPT_CONFIG_SETUP = 293,
    OPT_CRF = 294,
    OPT_VMAF_TARGET = 295,
    OPT_COMPANION_HD = 296,
    OPT_SCALE_TO_HD = 297,
    OPT_CACHE_DIR = 298,
    OPT_TV = 299,
    OPT_MV = 300,
    OPT_SEASON = 301,
    OPT_EPISODE = 302,
    OPT_FORCE = 303,
  };

  static struct option long_options[] = {
      {"tmdb", required_argument, 0, OPT_TMDB},
      {"bitrate", required_argument, 0, OPT_BITRATE},
      {"crf", required_argument, 0, OPT_CRF},
      {"vmaf-target", required_argument, 0, OPT_VMAF_TARGET},
      {"srt", required_argument, 0, OPT_SRT},
      {"help", no_argument, 0, OPT_HELP},
      {"blind", no_argument, 0, OPT_BLIND},
      {"config", no_argument, 0, OPT_CONFIG_SETUP},
      {"dry-run", no_argument, 0, OPT_DRY_RUN},
      {"force", no_argument, 0, OPT_FORCE},
      {"quiet", no_argument, 0, OPT_QUIET},
      {"verbose", no_argument, 0, OPT_VERBOSE},
      {"grain-only", no_argument, 0, OPT_GRAIN_ONLY},
      /* Language flags. */
      {"multi", no_argument, 0, OPT_MULTI},
      {"multivfi", no_argument, 0, OPT_MULTIVFI},
      {"multivff", no_argument, 0, OPT_MULTIVFF},
      {"multivfq", no_argument, 0, OPT_MULTIVFQ},
      {"multivf2", no_argument, 0, OPT_MULTIVF2},
      {"multivof", no_argument, 0, OPT_MULTIVOF},
      {"dual_vfi", no_argument, 0, OPT_DUAL_VFI},
      {"dual_vff", no_argument, 0, OPT_DUAL_VFF},
      {"dual_vfq", no_argument, 0, OPT_DUAL_VFQ},
      {"french", no_argument, 0, OPT_FRENCH},
      {"vff", no_argument, 0, OPT_VFF},
      {"vof", no_argument, 0, OPT_VOF},
      {"truefrench", no_argument, 0, OPT_TRUEFRENCH},
      {"vo", no_argument, 0, OPT_VO},
      {"vost", no_argument, 0, OPT_VOST},
      {"fansub", no_argument, 0, OPT_FANSUB},
      /* Source flags. */
      {"bdrip", no_argument, 0, OPT_BDRIP},
      {"bluray", no_argument, 0, OPT_BLURAY},
      {"remux", no_argument, 0, OPT_REMUX},
      {"dvdrip", no_argument, 0, OPT_DVDRIP},
      {"dvdremux", no_argument, 0, OPT_DVDREMUX},
      {"webrip", no_argument, 0, OPT_WEBRIP},
      {"webdl", no_argument, 0, OPT_WEBDL},
      {"web", no_argument, 0, OPT_WEB},
      {"hdtv", no_argument, 0, OPT_HDTV},
      {"hdrip", no_argument, 0, OPT_HDRIP},
      {"tvrip", no_argument, 0, OPT_TVRIP},
      {"vhsrip", no_argument, 0, OPT_VHSRIP},
      /* Quality preset flags. */
      {"animation", no_argument, 0, OPT_ANIMATION},
      {"super35_analog", no_argument, 0, OPT_SUPER35_ANALOG},
      {"super35_digital", no_argument, 0, OPT_SUPER35_DIGITAL},
      {"imax_analog", no_argument, 0, OPT_IMAX_ANALOG},
      {"imax_digital", no_argument, 0, OPT_IMAX_DIGITAL},
      {"companion-hd", no_argument, 0, OPT_COMPANION_HD},
      {"scale-to-hd", no_argument, 0, OPT_SCALE_TO_HD},
      {"cache-dir", required_argument, 0, OPT_CACHE_DIR},
      {"tv", no_argument, 0, OPT_TV},
      {"mv", no_argument, 0, OPT_MV},
      {"season", required_argument, 0, OPT_SEASON},
      {"episode", required_argument, 0, OPT_EPISODE},
      {0, 0, 0, 0},
  };

  int o;
  while ((o = getopt_long(argc, argv, "hb:s:", long_options, NULL)) != -1) {
    switch (o) {
    case OPT_TMDB:
      opt->tmdb_id = vmav_parse_int_or_zero(optarg);
      break;
    case OPT_BITRATE:
      opt->bitrate = vmav_parse_int_or_zero(optarg);
      if (opt->bitrate <= 0) {
        (void)fprintf(stderr, "Error: --bitrate must be a positive integer (kbps)\n");
        return 1;
      }
      break;
    case OPT_CRF:
      opt->crf = vmav_parse_int_or_zero(optarg);
      if (opt->crf < 1 || opt->crf > 63) {
        (void)fprintf(stderr, "Error: --crf must be in range 1–63\n");
        return 1;
      }
      break;
    case OPT_VMAF_TARGET:
      opt->vmaf_target = vmav_parse_int_or_zero(optarg);
      if (opt->vmaf_target < 1 || opt->vmaf_target > 100) {
        (void)fprintf(stderr, "Error: --vmaf-target must be in range 1–100\n");
        return 1;
      }
      break;
    case OPT_SRT:
      if (opt->extra_srt_count < 16)
        opt->extra_srt_paths[opt->extra_srt_count++] = optarg;
      else
        (void)fprintf(stderr, "Warning: too many --srt files, ignoring '%s'\n", optarg);
      break;
    case OPT_HELP:
      vmav_print_usage(argv[0]);
      return 0;
    case OPT_BLIND:
      /* Already detected in the pre-scan above; nothing more to do here. */
      break;
    case OPT_CONFIG_SETUP:
      /* Pre-scan dispatches this; reaching here means a flag came before
         it that getopt processed first. Run setup and exit anyway. */
      return config_interactive_setup();
    case OPT_DRY_RUN:
      opt->dry_run = true;
      break;
    case OPT_FORCE:
      opt->force = true;
      break;
    case OPT_QUIET:
      opt->quiet = true;
      break;
    case OPT_VERBOSE:
      opt->verbose = true;
      break;
    case OPT_GRAIN_ONLY:
      opt->grain_only = true;
      break;
    /* Language flags. */
    case OPT_MULTI:
      opt->lang_tag = LANG_TAG_MULTI;
      break;
    case OPT_MULTIVFI:
      opt->lang_tag = LANG_TAG_MULTI_VFI;
      break;
    case OPT_MULTIVFF:
      opt->lang_tag = LANG_TAG_MULTI_VFF;
      break;
    case OPT_MULTIVFQ:
      opt->lang_tag = LANG_TAG_MULTI_VFQ;
      break;
    case OPT_MULTIVF2:
      opt->lang_tag = LANG_TAG_MULTI_VF2;
      break;
    case OPT_MULTIVOF:
      opt->lang_tag = LANG_TAG_MULTI_VOF;
      break;
    case OPT_DUAL_VFI:
      opt->lang_tag = LANG_TAG_DUAL_VFI;
      break;
    case OPT_DUAL_VFF:
      opt->lang_tag = LANG_TAG_DUAL_VFF;
      break;
    case OPT_DUAL_VFQ:
      opt->lang_tag = LANG_TAG_DUAL_VFQ;
      break;
    case OPT_FRENCH:
      opt->lang_tag = LANG_TAG_FRENCH;
      break;
    case OPT_VFF:
      opt->lang_tag = LANG_TAG_VFF;
      break;
    case OPT_VOF:
      opt->lang_tag = LANG_TAG_VOF;
      break;
    case OPT_TRUEFRENCH:
      opt->lang_tag = LANG_TAG_TRUEFRENCH;
      break;
    case OPT_VO:
      opt->lang_tag = LANG_TAG_VO;
      break;
    case OPT_VOST:
      opt->lang_tag = LANG_TAG_VOST;
      break;
    case OPT_FANSUB:
      opt->lang_tag = LANG_TAG_FANSUB;
      break;
    /* Source flags. */
    case OPT_BDRIP:
      opt->source = SOURCE_BDRIP;
      break;
    case OPT_BLURAY:
      opt->source = SOURCE_BLURAY;
      break;
    case OPT_REMUX:
      opt->source = SOURCE_REMUX;
      break;
    case OPT_DVDRIP:
      opt->source = SOURCE_DVDRIP;
      break;
    case OPT_DVDREMUX:
      opt->source = SOURCE_DVDREMUX;
      break;
    case OPT_WEBRIP:
      opt->source = SOURCE_WEBRIP;
      break;
    case OPT_WEBDL:
      opt->source = SOURCE_WEBDL;
      break;
    case OPT_WEB:
      opt->source = SOURCE_WEB;
      break;
    case OPT_HDTV:
      opt->source = SOURCE_HDTV;
      break;
    case OPT_HDRIP:
      opt->source = SOURCE_HDRIP;
      break;
    case OPT_TVRIP:
      opt->source = SOURCE_TVRIP;
      break;
    case OPT_VHSRIP:
      opt->source = SOURCE_VHSRIP;
      break;
    /* Quality preset flags. */
    case OPT_ANIMATION:
      opt->quality = QUALITY_ANIMATION;
      break;
    case OPT_SUPER35_ANALOG:
      opt->quality = QUALITY_SUPER35_ANALOG;
      break;
    case OPT_SUPER35_DIGITAL:
      opt->quality = QUALITY_SUPER35_DIGITAL;
      break;
    case OPT_IMAX_ANALOG:
      opt->quality = QUALITY_IMAX_ANALOG;
      break;
    case OPT_IMAX_DIGITAL:
      opt->quality = QUALITY_IMAX_DIGITAL;
      break;
    case OPT_COMPANION_HD:
      opt->companion_hd = true;
      break;
    case OPT_SCALE_TO_HD:
      opt->scale_to_hd = true;
      break;
    case OPT_CACHE_DIR:
      snprintf(opt->cache_dir, sizeof(opt->cache_dir), "%s", optarg);
      if (strlen(opt->cache_dir) == 0) {
        (void)fprintf(stderr, "Error: --cache-dir requires a directory path\n");
        return 1;
      }
      break;
    case OPT_TV:
      opt->tv_mode = true;
      break;
    case OPT_MV:
      opt->tv_mode = false;
      break;
    case OPT_SEASON:
      opt->season = vmav_parse_int_or_zero(optarg);
      if (opt->season <= 0) {
        (void)fprintf(stderr, "Error: --season must be a positive integer\n");
        return 1;
      }
      break;
    case OPT_EPISODE:
      opt->episode = vmav_parse_int_or_zero(optarg);
      if (opt->episode <= 0) {
        (void)fprintf(stderr, "Error: --episode must be a positive integer\n");
        return 1;
      }
      break;
    default:
      vmav_print_usage(argv[0]);
      return 1;
    }
  }

  /* Apply --quiet now that all flags are parsed. Sections that should
     always render (Encoding plan, Done) bracket themselves with
     ui_set_quiet(0) / ui_set_quiet(1). */
  if (opt->quiet)
    ui_set_quiet(1);
  /* --verbose is orthogonal: it forwards SVT-AV1 chatter to stderr.
     Compatible with --quiet (compact our-output, raw encoder log). */
  if (opt->verbose)
    ui_set_verbose(1);

  if (opt->companion_hd && opt->scale_to_hd) {
    (void)fprintf(stderr, "Error: --companion-hd and --scale-to-hd are mutually exclusive\n");
    return 1;
  }
  if (!opt->tv_mode && (opt->season > 0 || opt->episode > 0)) {
    (void)fprintf(stderr, "Error: --season/--episode require --tv\n");
    return 1;
  }

  if (optind < argc) {
    opt->filepath = argv[optind];
  } else {
    vmav_print_usage(argv[0]);
    return 1;
  }
  return -1;
}
