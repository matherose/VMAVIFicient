/**
 * @file pipeline.h
 * @brief Shared pipeline context and stage entry points for the
 *        vmavificient driver (src/pipeline/).
 */
#ifndef VMAV_PIPELINE_H
#define VMAV_PIPELINE_H

#include <stdbool.h>
#include <time.h>

#include "crf_search.h"
#include "encode_preset.h"
#include "media_analysis.h"
#include "media_crop.h"
#include "media_hdr.h"
#include "media_info.h"
#include "media_naming.h"
#include "media_tracks.h"

/* How a stage tells the driver to proceed. Mirrors the early-return
   points of the original monolithic main(). */
typedef enum {
  STAGE_CONTINUE = 0, /* keep running the pipeline           */
  STAGE_EXIT_OK,      /* stop now, process exit code 0        */
  STAGE_EXIT_FAIL,    /* stop now, process exit code 1        */
} StageStatus;

/* Everything the CLI parser resolves. Field names match the old
   main() locals (cli_ prefix dropped) so moved code reads naturally. */
typedef struct {
  int tmdb_id;
  int bitrate;     /* 0 = run crf-search (default path) */
  int crf;         /* 0 = run crf-search; >0 = use directly */
  int vmaf_target; /* 0 = per-preset default */
  bool blind;
  bool dry_run;
  bool force; /* redo stages whose output already exists */
  bool quiet;
  bool verbose;
  bool grain_only;
  bool companion_hd;
  bool scale_to_hd;
  bool tv_mode;
  int season;           /* 0 = resolve from filename, then prompt */
  int episode;          /* same resolution order as season */
  char cache_dir[4096]; /* "" = default ./.vmavificient-cache */
  LanguageTag lang_tag;
  SourceType source;
  QualityType quality;
  const char *extra_srt_paths[16];
  int extra_srt_count;
  const char *filepath; /* positional input (required) */
} VmavOptions;

/* Per-pass knobs for the unified rate-control/plan + encode/mux pass.
   The 4K pass and the HD (companion / scale-to-hd) pass differ only
   in these fields. */
typedef struct {
  const EncodePreset *preset;
  int film_grain;
  int crf;                      /* in: cli override or 0; out: resolved by rate_plan */
  int vmaf_used;                /* out: resolved by rate_plan */
  const char *base_name;        /* stem for cache-file names (RPU, video.mkv) */
  const char *output_name;      /* final .mkv filename */
  int scale_width;              /* 0 = encode at source resolution (4K pass) */
  int scale_height;             /* 0 = encode at source resolution */
  const char *crf_scale_filter; /* NULL, or "scale=1920:1080:flags=lanczos" */
  const MediaInfo *plan_info;   /* dims shown in the plan (hd_info for HD) */
  const HdrInfo *enc_hdr;       /* hdr passed to the video encoder */
  bool is_hd;                   /* HD pass: "HD …" labels when companion_hd,
                                   Scale kv in plan, no bitrate-delta receipt */
  bool use_cached_crf;          /* 4K pass only: honor cached_crf from scores */
  bool dry_run_falls_through;   /* 4K pass with --companion-hd: print plan,
                                   do not exit, so the HD plan also renders */
  bool extract_rpu;             /* extract RPU in this pass (4K pass, or HD
                                   pass when --scale-to-hd) */
  bool defer_shared_cleanup;    /* 4K pass with --companion-hd: keep opus/
                                   srt/rpu intermediates for the HD mux */
} EncodePassParams;

/* All cross-stage state. Heap-allocate with calloc(1, sizeof) — the
   embedded path arrays total several hundred kB. */
typedef struct {
  VmavOptions opt;

  char cache_dir[4096]; /* resolved (opt.cache_dir or default) */
  char scores_cache_path[4096];
  double cached_grain_score;
  double cached_grain_variance;
  int cached_crf;
  bool scores_cached;

  MediaInfo info;
  HdrInfo hdr;
  CropInfo crop;
  MediaTracks tracks;

  GrainScore grain;
  int film_grain;
  const EncodePreset *enc_preset;

  /* Naming outputs. */
  char output_name[1024];
  char base_name[1024];
  char output_dir[2048];
  char mkv_title[1024];
  char saved_tmdb_title[512];
  int saved_tmdb_year;
  EpisodeInfo saved_episode;
  bool saved_is_tv;
  const char *video_language;
  SourceType source;
  FrenchVariant fv;
  FrenchAudioOrigin fr_audio_origin;
  LanguageTag resolved_lang_tag;

  /* Audio stage outputs. */
  char opus_paths[32][4096];
  char audio_names[32][256];
  char audio_langs[32][16];
  int opus_count;
  int audio_fail_count;

  /* Subtitle stage outputs. */
  char srt_paths[64][4096];
  char srt_names[64][256];
  char srt_langs[64][64];
  int srt_is_forced[64];
  int srt_is_sdh[64];
  int srt_variant[64];
  int srt_count;

  char rpu_path[4096];

  time_t encode_start_time;
  int pipeline_failed; /* drives the process exit code */
} PipelineCtx;

/* ---- pipeline_util.c ---- */
int vmav_parse_int_or_zero(const char *s);
bool vmav_file_exists(const char *path);
const char *vmav_codec_short(const char *codec);
void vmav_build_cache_path(const PipelineCtx *ctx, char *buf, size_t bufsize,
                           const char *relative_path);
void vmav_cleanup_cache_dir(const PipelineCtx *ctx);
bool vmav_load_cached_scores(const char *cache_path, double *grain_score, double *grain_variance,
                             int *crf);
bool vmav_save_cached_scores(const char *cache_path, double grain_score, double grain_variance,
                             int crf);
int vmav_audio_lang_priority(const char *lang);
int vmav_cmp_audio_order(const void *a, const void *b);
int vmav_sub_sort_key(const char *lang, int is_forced);
void vmav_print_encoder_knobs(const EncodePreset *p, int film_grain);

/* ---- cli.c (Task 2) ---- */
void vmav_print_usage(const char *prog);
/* Pre-getopt scan for --help/--config/--blind (they must act before
   config_init). Returns like a mini-main: -1 = keep going, >=0 = exit
   now with that code. Sets opt->blind. */
int vmav_cli_prescan(int argc, char *argv[], VmavOptions *opt);
/* Full getopt_long pass. Returns -1 = keep going, >=0 = exit code. */
int vmav_cli_parse(int argc, char *argv[], VmavOptions *opt);

/* ---- stage_probe.c (Task 3) ---- */
StageStatus stage_probe(PipelineCtx *ctx);

/* ---- stage_grain.c (Task 4) ---- */
StageStatus stage_grain(PipelineCtx *ctx);

/* ---- stage_naming.c (Task 5) ---- */
StageStatus stage_naming(PipelineCtx *ctx);

/* ---- stage_audio.c (Task 6) ---- */
StageStatus stage_audio(PipelineCtx *ctx);

/* ---- stage_subs.c (Task 7) ---- */
StageStatus stage_subs(PipelineCtx *ctx);

/* ---- encode_pass.c (Tasks 8-9) ---- */
StageStatus stage_rate_plan(PipelineCtx *ctx, EncodePassParams *pass);
StageStatus encode_pass(PipelineCtx *ctx, EncodePassParams *pass);

#endif /* VMAV_PIPELINE_H */
