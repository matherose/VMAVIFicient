/**
 * @file media_tracks.c
 * @brief Implementation of audio and subtitle track enumeration.
 */

#include "media_tracks.h"
#include "utils.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/codec_id.h>
#include <libavcodec/defs.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>

/**
 * @brief Language name to ISO 639-2/B code mapping.
 *
 * Maps common title strings (in multiple languages) to 3-letter codes.
 * Checked with case-insensitive substring match against the track title.
 * Entries are ordered longest-first within each language to avoid
 * partial matches (e.g. "fran" matching before "francais").
 */
static const struct {
  const char *pattern;
  const char *code;
} lang_map[] = {
    /* French */
    {
        "fran\xc3\xa7"
        "ais",
        "fre",
    }, /* français (UTF-8) */
    {"francais", "fre"},
    {"french", "fre"},
    /* English */
    {"english", "eng"},
    {"anglais", "eng"},
    /* German */
    {"deutsch", "ger"},
    {"german", "ger"},
    {"allemand", "ger"},
    /* Spanish */
    {
        "espa\xc3\xb1"
        "ol",
        "spa",
    }, /* español (UTF-8) */
    {"espanol", "spa"},
    {"spanish", "spa"},
    {"castillan", "spa"},
    /* Italian */
    {"italiano", "ita"},
    {"italian", "ita"},
    {"italien", "ita"},
    /* Portuguese */
    {
        "portugu\xc3\xaa"
        "s",
        "por",
    }, /* português (UTF-8) */
    {"portugues", "por"},
    {"portuguese", "por"},
    /* Dutch */
    {"nederlands", "dut"},
    {"dutch", "dut"},
    /* Russian */
    {"russian", "rus"},
    {"russe", "rus"},
    /* Japanese */
    {"japanese", "jpn"},
    {"japonais", "jpn"},
    /* Chinese */
    {"chinese", "chi"},
    {"chinois", "chi"},
    /* Korean */
    {"korean", "kor"},
    /* Arabic */
    {"arabic", "ara"},
    {"arabe", "ara"},
    /* Polish */
    {"polish", "pol"},
    {"polonais", "pol"},
    /* Swedish */
    {"swedish", "swe"},
    /* Norwegian */
    {"norwegian", "nor"},
    /* Danish */
    {"danish", "dan"},
    /* Finnish */
    {"finnish", "fin"},
    /* Turkish */
    {"turkish", "tur"},
    /* Hindi */
    {"hindi", "hin"},
    {NULL, NULL},
};

/**
 * @brief Guess ISO 639-2/B language code from a track title string.
 *
 * @return Language code (static string) or NULL if no match.
 */
static const char *guess_language_from_title(const char *title) {
  if (!title || !title[0])
    return NULL;
  for (int i = 0; lang_map[i].pattern; i++) {
    if (str_contains_ci(title, lang_map[i].pattern))
      return lang_map[i].code;
  }
  return NULL;
}

/**
 * @brief Detect if a track title indicates forced subtitles.
 *
 * Forced subtitles are typically shown only when the on-screen dialogue
 * is in a different language than the main audio track. They should only
 * be marked as forced if they explicitly contain "forced" in the title.
 * VFF/VFQ/VFI are audio variants, NOT forced subtitles.
 */
static bool title_indicates_forced(const char *title) {
  if (!title || !title[0])
    return false;

  /* Standard forced keywords */
  if (str_contains_ci(title, "forced") || str_contains_ci(title, "forc\xc3\xa9") || /* forcé */
      str_contains_ci(title, "force"))
    return true;

  return false;
}

/**
 * @brief Detect if a track title indicates SDH / closed captions.
 */
static bool title_indicates_sdh(const char *title) {
  if (!title || !title[0])
    return false;
  return (str_contains_ci(title, "sdh") || str_contains_ci(title, "closed caption") ||
          str_contains_ci(title, "hearing impaired") || str_contains_ci(title, "malentendant")) !=
         0;
}

/**
 * @brief Detect if a track title indicates karaoke content.
 */
static bool title_indicates_karaoke(const char *title) {
  if (!title || !title[0])
    return false;
  return (str_contains_ci(title, "karao") || str_contains_ci(title, "karaok") ||
          str_contains_ci(title, "chanson") || str_contains_ci(title, "karaoké")) != 0;
}

/**
 * @brief Fill a TrackInfo from an AVStream.
 */
static void fill_track(TrackInfo *t, AVStream *stream) {
  t->index = stream->index;

  AVDictionaryEntry *tag;

  tag = av_dict_get(stream->metadata, "title", NULL, 0);
  if (tag)
    snprintf(t->name, sizeof(t->name), "%s", tag->value);
  else
    t->name[0] = '\0';

  tag = av_dict_get(stream->metadata, "language", NULL, 0);
  if (tag && strcmp(tag->value, "und") != 0)
    snprintf(t->language, sizeof(t->language), "%s", tag->value);
  else
    t->language[0] = '\0';

  /* If no language tag, try to guess from title. */
  if (!t->language[0]) {
    const char *guessed = guess_language_from_title(t->name);
    if (guessed)
      snprintf(t->language, sizeof(t->language), "%s", guessed);
  }

  snprintf(t->codec, sizeof(t->codec), "%s", avcodec_get_name(stream->codecpar->codec_id));

  t->channels = stream->codecpar->ch_layout.nb_channels;
  t->bitrate = stream->codecpar->bit_rate;
  t->codec_id = (int)stream->codecpar->codec_id;
  t->profile = stream->codecpar->profile;

  /* Forced: check disposition flag first, then title keywords. */
  t->is_forced = (stream->disposition & AV_DISPOSITION_FORCED) ? 1 : 0;
  if (!t->is_forced && stream->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE)
    t->is_forced = (int)title_indicates_forced(t->name) ? 1 : 0;

  /* SDH: check disposition and title keywords. */
  t->is_sdh = (stream->disposition & AV_DISPOSITION_HEARING_IMPAIRED) ? 1 : 0;
  if (!t->is_sdh && stream->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE)
    t->is_sdh = (int)title_indicates_sdh(t->name) ? 1 : 0;

  /* Karaoke: title keyword only. */
  // clang-format off
  t->is_karaoke =
      (stream->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE &&
       title_indicates_karaoke(t->name))
          ? 1
          : 0;
  // clang-format on
}

MediaTracks get_media_tracks(const char *path) {
  MediaTracks result = {0};
  AVFormatContext *fmt_ctx = NULL;
  char errbuf[AV_ERROR_MAX_STRING_SIZE];
  int ret;

  ret = avformat_open_input(&fmt_ctx, path, NULL, NULL);
  if (ret < 0) {
    av_make_error_string(errbuf, sizeof(errbuf), ret);
    (void)fprintf(stderr, "Error: cannot open '%s': %s\n", path, errbuf);
    result.error = ret;
    return result;
  }

  ret = avformat_find_stream_info(fmt_ctx, NULL);
  if (ret < 0) {
    av_make_error_string(errbuf, sizeof(errbuf), ret);
    (void)fprintf(stderr, "Error: cannot read stream info from '%s': %s\n", path, errbuf);
    result.error = ret;
    avformat_close_input(&fmt_ctx);
    return result;
  }

  /* Count tracks first to allocate arrays. */
  int n_audio = 0, n_sub = 0;
  for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
    if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
      n_audio++;
    else if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE)
      n_sub++;
  }

  if (n_audio > 0) {
    result.audio = calloc((size_t)n_audio, sizeof(TrackInfo));
    if (!result.audio) {
      result.error = AVERROR(ENOMEM);
      avformat_close_input(&fmt_ctx);
      return result;
    }
  }

  if (n_sub > 0) {
    result.subtitles = calloc((size_t)n_sub, sizeof(TrackInfo));
    if (!result.subtitles) {
      free(result.audio);
      result.audio = NULL;
      result.error = AVERROR(ENOMEM);
      avformat_close_input(&fmt_ctx);
      return result;
    }
  }

  int ai = 0, si = 0;
  for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
    AVStream *s = fmt_ctx->streams[i];
    if (s->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
      fill_track(&result.audio[ai++], s);
    else if (s->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE)
      fill_track(&result.subtitles[si++], s);
  }

  result.audio_count = n_audio;
  result.subtitle_count = n_sub;

  avformat_close_input(&fmt_ctx);
  return result;
}

void free_media_tracks(MediaTracks *tracks) {
  if (!tracks)
    return;
  free(tracks->audio);
  tracks->audio = NULL;
  tracks->audio_count = 0;
  free(tracks->subtitles);
  tracks->subtitles = NULL;
  tracks->subtitle_count = 0;
}

static int codec_quality_rank(int codec_id, int profile) {
  switch (codec_id) {
  case AV_CODEC_ID_TRUEHD:
    return 60;
  case AV_CODEC_ID_DTS:
    if (profile == AV_PROFILE_DTS_HD_MA || profile == AV_PROFILE_DTS_HD_MA_X ||
        profile == AV_PROFILE_DTS_HD_MA_X_IMAX)
      return 50;
    if (profile == AV_PROFILE_DTS_HD_HRA)
      return 35;
    return 20;
  case AV_CODEC_ID_FLAC:
    return 45;
  case AV_CODEC_ID_PCM_S16LE:
  case AV_CODEC_ID_PCM_S24LE:
  case AV_CODEC_ID_PCM_S32LE:
    return 40;
  case AV_CODEC_ID_EAC3:
    return 30;
  case AV_CODEC_ID_AC3:
    return 25;
  case AV_CODEC_ID_AAC:
    return 15;
  case AV_CODEC_ID_MP3:
    return 10;
  case AV_CODEC_ID_MP2:
    return 5;
  default:
    return 0;
  }
}

/* ====================================================================== */
/*  Track naming                                                          */
/* ====================================================================== */

/**
 * @brief Map ISO 639-2/B code to human-readable language name.
 */
static const char *language_display_name(const char *code) {
  if (!code || !code[0])
    return "Unknown";

  static const struct {
    const char *code;
    const char *name;
  } names[] = {
      {"eng", "English"},
      {
          "fre",
          "Fran\xc3\xa7"
          "ais",
      },
      {
          "fra",
          "Fran\xc3\xa7"
          "ais",
      },
      {"ger", "German"},
      {"deu", "German"},
      {"spa", "Spanish"},
      {"ita", "Italian"},
      {"por", "Portuguese"},
      {"dut", "Dutch"},
      {"nld", "Dutch"},
      {"rus", "Russian"},
      {"jpn", "Japanese"},
      {"chi", "Chinese"},
      {"zho", "Chinese"},
      {"kor", "Korean"},
      {"ara", "Arabic"},
      {"pol", "Polish"},
      {"swe", "Swedish"},
      {"nor", "Norwegian"},
      {"dan", "Danish"},
      {"fin", "Finnish"},
      {"tur", "Turkish"},
      {"hin", "Hindi"},
      {"cze", "Czech"},
      {"ces", "Czech"},
      {"hun", "Hungarian"},
      {"rum", "Romanian"},
      {"ron", "Romanian"},
      {"tha", "Thai"},
      {"vie", "Vietnamese"},
      {"gre", "Greek"},
      {"ell", "Greek"},
      {"heb", "Hebrew"},
      {"ind", "Indonesian"},
      {"may", "Malay"},
      {"msa", "Malay"},
      {"ukr", "Ukrainian"},
      {"bul", "Bulgarian"},
      {"hrv", "Croatian"},
      {"slo", "Slovak"},
      {"slk", "Slovak"},
      {NULL, NULL},
  };

  for (int i = 0; names[i].code; i++) {
    if (strcmp(code, names[i].code) == 0)
      return names[i].name;
  }
  return code; /* fallback: return the raw code */
}

/**
 * @brief Convert channel count to layout string.
 */
static const char *channels_to_layout(int channels) {
  switch (channels) {
  case 1:
    return "1.0";
  case 2:
    return "2.0";
  case 3:
    return "2.1";
  case 6:
    return "5.1";
  case 8:
    return "7.1";
  default: {
    static char buf[16];
    snprintf(buf, sizeof(buf), "%dch", channels);
    return buf;
  }
  }
}

void build_audio_track_name(char *buf, size_t bufsize, const char *language, int channels,
                            FrenchAudioOrigin fr_origin) {
  const char *layout = channels_to_layout(channels);

  if (language && (strcmp(language, "fre") == 0 || strcmp(language, "fra") == 0)) {
    const char *variant;
    switch (fr_origin) {
    case FRENCH_AUDIO_VFQ:
      variant = "VFQ";
      break;
    case FRENCH_AUDIO_VFI:
      variant = "VFI";
      break;
    case FRENCH_AUDIO_VO:
      variant = "VO";
      break;
    default:
      variant = "VFF";
      break;
    }
    snprintf(buf, bufsize,
             "Fran\xc3\xa7"
             "ais [%s] %s",
             variant, layout);
  } else {
    const char *name = language_display_name(language);
    snprintf(buf, bufsize, "%s %s", name, layout);
  }
}

void build_subtitle_track_name(char *buf, size_t bufsize, const char *language, int is_srt,
                               int is_forced, int is_sdh, FrenchAudioOrigin fr_origin) {
  const char *format = is_srt ? "SRT" : "Text";
  const char *type_label = "";
  if (is_forced)
    type_label = " (forced)";
  else if (is_sdh)
    type_label = " (sdh)";

  int is_french = language && (strcmp(language, "fre") == 0 || strcmp(language, "fra") == 0);
  if (is_french) {
    const char *variant;
    switch (fr_origin) {
    case FRENCH_AUDIO_VFQ:
      variant = "VFQ";
      break;
    case FRENCH_AUDIO_VFI:
      variant = "VFI";
      break;
    case FRENCH_AUDIO_VO:
      variant = "VO";
      break;
    default:
      variant = "VFF";
      break;
    }
    const char *lang_label =
        (fr_origin == FRENCH_AUDIO_VFQ) ? "Français (Québec)" : "Français (France)";
    snprintf(buf, bufsize, "%s [%s]%s | %s", lang_label, variant, type_label, format);
  } else {
    const char *name = language_display_name(language);
    snprintf(buf, bufsize, "%s | %s%s", name, format, type_label);
  }
}

/* ====================================================================== */
/*  Track selection                                                       */
/* ====================================================================== */

static bool track_is_better(const TrackInfo *a, const TrackInfo *b) {
  int ra = codec_quality_rank(a->codec_id, a->profile);
  int rb = codec_quality_rank(b->codec_id, b->profile);
  if (ra != rb)
    return ra > rb;
  if (a->channels != b->channels)
    return a->channels > b->channels;
  return a->bitrate > b->bitrate;
}

/* Values must match FrenchVariant in media_naming.h (0=UNKNOWN, 1=VFF, 2=VFQ,
   3=VFI). Kept numeric here to avoid a circular include. */
int detect_track_french_variant(const TrackInfo *track) {
  if (!track)
    return 0; /* FRENCH_VARIANT_UNKNOWN */
  const char *t = track->name;
  if (!t || !t[0])
    return 0;
  /* Check VFQ/VFI before VFF so "VFF" doesn't shadow more specific tokens. */
  if (str_contains_ci(t, "VFQ") ||
      str_contains_ci(t, "qu\xc3\xa9"
                         "becois") ||
      str_contains_ci(t, "quebec"))
    return 2; /* FRENCH_VARIANT_VFQ */
  if (str_contains_ci(t, "VFI"))
    return 3; /* FRENCH_VARIANT_VFI */
  /* Match VFF with word boundary - "VFF " ensures we don't match "VFQ" or "VFI"
     and avoids false positives from partial matches like "VF - " */
  if (str_contains_ci(t, "VFF ") || str_contains_ci(t, "VFF]"))
    return 1; /* FRENCH_VARIANT_VFF */
  return 0;
}

static int is_french_lang(const char *lang) {
  return lang && (strcmp(lang, "fre") == 0 || strcmp(lang, "fra") == 0);
}

TrackInfo *select_best_audio_per_language(const MediaTracks *tracks, int split_french_variants,
                                          int *out_count) {
  *out_count = 0;
  if (!tracks || tracks->audio_count == 0)
    return NULL;

  TrackInfo *best = calloc((size_t)tracks->audio_count, sizeof(TrackInfo));
  if (!best)
    return NULL;

  /* Parallel array of French variant per kept slot (0 for non-French or when
     splitting is disabled). Heap-allocated rather than a VLA so MSVC and
     strict static analyzers stay happy on the future Windows port. */
  int *best_variant = calloc((size_t)tracks->audio_count, sizeof(int));
  if (!best_variant) {
    free(best);
    return NULL;
  }

  int count = 0;
  for (int i = 0; i < tracks->audio_count; i++) {
    const TrackInfo *t = &tracks->audio[i];
    const char *lang = t->language[0] ? t->language : "und";
    int variant = 0;
    if (split_french_variants && is_french_lang(lang))
      variant = detect_track_french_variant(t);

    /* Check if we already have an entry for this (language, variant). */
    int found = -1;
    for (int j = 0; j < count; j++) {
      const char *blang = best[j].language[0] ? best[j].language : "und";
      if (strcmp(lang, blang) == 0 && best_variant[j] == variant) {
        found = j;
        break;
      }
    }

    if (found < 0) {
      best[count] = *t;
      best_variant[count] = variant;
      count++;
    } else if (track_is_better(t, &best[found])) {
      best[found] = *t;
    }
  }

  free(best_variant);
  *out_count = count;
  return best;
}
