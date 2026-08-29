/**
 * @file subtitle_convert.c
 * @brief PGS bitmap subtitle to SRT text conversion using Tesseract OCR.
 *
 * Pipeline: FFmpeg demux → native PGS segment parsing → RLE decode →
 *           palette apply → Tesseract OCR → SRT output.
 *
 * PGS (Presentation Graphic Stream) format is parsed natively rather than
 * using FFmpeg's subtitle decoder, following the approach of pgsrip.
 * Each PGS Display Set consists of segments:
 *   - PCS (0x16): Presentation Composition — timestamps, composition state
 *   - WDS (0x17): Window Definition — position and size
 *   - PDS (0x14): Palette Definition — YCbCrA color table
 *   - ODS (0x15): Object Definition — RLE-encoded bitmap
 *   - END (0x80): End of Display Set
 */

#include "subtitle_convert.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>

#include <zlib.h>

#include "ui.h"

#include <allheaders.h>
#include <tesseract/capi.h>

/* ====================================================================== */
/*  Language mapping                                                      */
/* ====================================================================== */

const char *iso639_to_tesseract_lang(const char *iso639) {
  if (!iso639 || !iso639[0])
    return "eng";

  static const struct {
    const char *iso;
    const char *tess;
  } map[] = {
      {"eng", "eng"}, {"fre", "fra"}, {"fra", "fra"},     {"ger", "deu"},     {"deu", "deu"},
      {"spa", "spa"}, {"ita", "ita"}, {"por", "por"},     {"dut", "nld"},     {"nld", "nld"},
      {"rus", "rus"}, {"jpn", "jpn"}, {"chi", "chi_sim"}, {"zho", "chi_sim"}, {"kor", "kor"},
      {"ara", "ara"}, {"pol", "pol"}, {"swe", "swe"},     {"nor", "nor"},     {"dan", "dan"},
      {"fin", "fin"}, {"tur", "tur"}, {"hin", "hin"},     {"cze", "ces"},     {"ces", "ces"},
      {"hun", "hun"}, {"rum", "ron"}, {"ron", "ron"},     {"tha", "tha"},     {"vie", "vie"},
      {"gre", "ell"}, {"ell", "ell"}, {"heb", "heb"},     {"ukr", "ukr"},     {"bul", "bul"},
      {"hrv", "hrv"}, {NULL, NULL},
  };

  for (int i = 0; map[i].iso; i++) {
    if (strcmp(iso639, map[i].iso) == 0)
      return map[i].tess;
  }
  return iso639;
}

void build_srt_filename(char *buf, size_t bufsize, const char *base_name, const char *language,
                        FrenchVariant fv, int is_forced, int is_sdh) {
  const char *lang = (language && language[0]) ? language : "und";
  const char *lang_suffix = lang;
  const char *type_suffix = ".full";
  const char *variant = "";

  /* French variants — include variant in filename for clarity */
  if (strcmp(lang, "fre") == 0 || strcmp(lang, "fra") == 0) {
    switch (fv) {
    case FRENCH_VARIANT_VFQ:
      lang_suffix = "fre.ca";
      variant = ".";
      break;
    case FRENCH_VARIANT_VFI:
      lang_suffix = "fre.vfi";
      variant = ".";
      break;
    default:
      lang_suffix = "fre.fr";
      variant = ".";
      break;
    }
  }

  /* Type suffix */
  if (is_forced)
    type_suffix = ".forced";
  else if (is_sdh)
    type_suffix = ".sdh";

  snprintf(buf, bufsize, "%s.%s%s%s.srt", base_name, lang_suffix, variant, type_suffix);
}

int is_pgs_subtitle(const TrackInfo *track) {
  if (!track)
    return 0;
  return (track->codec_id == AV_CODEC_ID_HDMV_PGS_SUBTITLE);
}

int is_text_subtitle(const TrackInfo *track) {
  if (!track)
    return 0;
  return (track->codec_id == AV_CODEC_ID_SUBRIP || track->codec_id == AV_CODEC_ID_ASS ||
          track->codec_id == AV_CODEC_ID_SSA || track->codec_id == AV_CODEC_ID_WEBVTT ||
          track->codec_id == AV_CODEC_ID_MOV_TEXT || track->codec_id == AV_CODEC_ID_TEXT);
}

/* ====================================================================== */
/*  SRT timestamp formatting                                              */
/* ====================================================================== */

static void format_srt_time(char *buf, size_t bufsize, int64_t ms) {
  if (ms < 0)
    ms = 0;
  int hours = (int)(ms / 3600000);
  ms %= 3600000;
  int minutes = (int)(ms / 60000);
  ms %= 60000;
  int seconds = (int)(ms / 1000);
  int millis = (int)(ms % 1000);
  snprintf(buf, bufsize, "%02d:%02d:%02d,%03d", hours, minutes, seconds, millis);
}

/* ====================================================================== */
/*  PGS segment types                                                     */
/* ====================================================================== */

#define PGS_PDS 0x14 /* Palette Definition Segment */
#define PGS_ODS 0x15 /* Object Definition Segment */
#define PGS_PCS 0x16 /* Presentation Composition Segment */
#define PGS_WDS 0x17 /* Window Definition Segment */
#define PGS_END 0x80 /* End of Display Set */

/* PCS composition states */
#define PCS_NORMAL 0x00
#define PCS_ACQ_POINT 0x40
#define PCS_EPOCH_START 0x80

/** @brief Palette entry: YCbCr + Alpha */
typedef struct {
  uint8_t y, cr, cb, a;
} PgsPaletteEntry;

/** @brief A complete PGS Display Set ready for OCR */
typedef struct {
  int64_t pts_ms; /**< Presentation timestamp in ms */
  int valid;      /**< 1 if we have bitmap data */

  /* Palette (256 entries max) */
  PgsPaletteEntry palette[256];
  int palette_set;

  /* Object bitmap (RLE-encoded) */
  uint8_t *rle_data;
  size_t rle_size;
  size_t rle_capacity;
  int obj_width;
  int obj_height;

  /* Composition info */
  int composition_state;
} PgsDisplaySet;

static void pgs_ds_init(PgsDisplaySet *ds) {
  memset(ds, 0, sizeof(*ds));
}

static void pgs_ds_reset(PgsDisplaySet *ds) {
  free(ds->rle_data);
  pgs_ds_init(ds);
}

/* ====================================================================== */
/*  PGS segment parsing                                                   */
/* ====================================================================== */

static uint16_t read_be16(const uint8_t *p) {
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/**
 * Parse segments from a demuxed PGS packet.
 *
 * In MKV, each packet contains one or more PGS segments concatenated.
 * Segment format: type(1) + size(2) + data(size).
 * Note: In MKV demux, the 13-byte PG header (magic+PTS+DTS+type+size)
 * is stripped — we get raw segment data starting with the segment type byte.
 */
/**
 * Try to zlib-decompress data. Returns malloc'd buffer + sets *out_size.
 * Returns NULL if data is not zlib-compressed or decompression fails.
 */
static uint8_t *try_zlib_decompress(const uint8_t *data, int data_size, int *out_size) {
  if (data_size < 2 || data[0] != 0x78)
    return NULL;

  /* Start with 8x expansion, grow if needed */
  uLongf buf_size = (uLongf)data_size * 8;
  if (buf_size < 65536)
    buf_size = 65536;

  uint8_t *buf = malloc(buf_size);
  if (!buf)
    return NULL;

  int zret = uncompress(buf, &buf_size, data, (uLong)data_size);
  if (zret == Z_BUF_ERROR) {
    /* Try larger buffer */
    buf_size = (uLongf)data_size * 32;
    uint8_t *tmp = realloc(buf, buf_size);
    if (!tmp) {
      free(buf);
      return NULL;
    }
    buf = tmp;
    zret = uncompress(buf, &buf_size, data, (uLong)data_size);
  }

  if (zret != Z_OK) {
    free(buf);
    return NULL;
  }

  *out_size = (int)buf_size;
  return buf;
}

static void parse_pgs_packet(PgsDisplaySet *ds, const uint8_t *data, int data_size) {
  /* Try zlib decompression (Matroska content compression) */
  int decomp_size = 0;
  uint8_t *decomp = try_zlib_decompress(data, data_size, &decomp_size);
  if (decomp) {
    data = decomp;
    data_size = decomp_size;
  }

  int pos = 0;

  while (pos < data_size) {
    if (pos + 3 > data_size)
      break;

    uint8_t seg_type = data[pos];
    uint16_t seg_size = read_be16(&data[pos + 1]);
    pos += 3;

    if (pos + seg_size > data_size)
      break;

    const uint8_t *seg = &data[pos];

    switch (seg_type) {
    case PGS_PCS:
      /* Presentation Composition Segment */
      if (seg_size >= 11) {
        ds->composition_state = seg[7]; /* composition_state at offset 7 */
        ds->valid = 0;                  /* reset until we get ODS */
      }
      break;

    case PGS_PDS:
      /* Palette Definition Segment: id(1) + version(1) + entries(N*5) */
      if (seg_size >= 2) {
        int entries_size = seg_size - 2;
        const uint8_t *entries = seg + 2;
        for (int i = 0; i + 5 <= entries_size; i += 5) {
          uint8_t idx = entries[i];
          ds->palette[idx].y = entries[i + 1];
          ds->palette[idx].cr = entries[i + 2];
          ds->palette[idx].cb = entries[i + 3];
          ds->palette[idx].a = entries[i + 4];
        }
        ds->palette_set = 1;
      }
      break;

    case PGS_ODS: {
      /* Object Definition Segment:
         id(2) + version(1) + seq_flag(1) + data_length(3) + width(2) +
         height(2) + rle_data(...) seq_flag: 0xC0 = first+last, 0x80 = first,
         0x40 = last, 0x00 = middle */
      if (seg_size < 4)
        break;

      uint8_t seq_flag = seg[3];
      int is_first = (seq_flag & 0x80) != 0;

      if (is_first) {
        /* First (or only) fragment */
        if (seg_size < 11)
          break;
        /* data_length at [4..6] is 3 bytes big-endian */
        ds->obj_width = read_be16(&seg[7]);
        ds->obj_height = read_be16(&seg[9]);

        /* RLE data starts at offset 11 */
        size_t rle_len = seg_size - 11;
        free(ds->rle_data);
        ds->rle_capacity = rle_len + 65536;
        ds->rle_data = malloc(ds->rle_capacity);
        if (ds->rle_data) {
          memcpy(ds->rle_data, seg + 11, rle_len);
          ds->rle_size = rle_len;
        } else {
          ds->rle_size = 0;
        }
      } else {
        /* Continuation fragment — append RLE data (starts at offset 4) */
        if (seg_size > 4 && ds->rle_data) {
          size_t rle_len = seg_size - 4;
          size_t new_size = ds->rle_size + rle_len;
          if (new_size > ds->rle_capacity) {
            ds->rle_capacity = new_size + 65536;
            uint8_t *tmp = realloc(ds->rle_data, ds->rle_capacity);
            if (!tmp)
              break;
            ds->rle_data = tmp;
          }
          memcpy(ds->rle_data + ds->rle_size, seg + 4, rle_len);
          ds->rle_size = new_size;
        }
      }

      if (ds->obj_width > 0 && ds->obj_height > 0 && ds->rle_size > 0)
        ds->valid = 1;
      break;
    }

    case PGS_END: // NOLINT(bugprone-branch-clone) -- kept distinct from default to
                  // document PGS_END as a known, intentional no-op segment type
      /* End of Display Set — nothing to do, ds is now complete */
      break;

    default:
      break;
    }

    pos += seg_size;
  }

  free(decomp);
}

/* ====================================================================== */
/*  PGS RLE decoding                                                      */
/* ====================================================================== */

/**
 * Decode PGS RLE bitmap to a flat palette-indexed buffer.
 *
 * PGS RLE scheme:
 *   - Non-zero byte: 1 pixel of that color index
 *   - 0x00 + 0x00: end of line
 *   - 0x00 + 0LLLLLLL: L transparent pixels (1-63)
 *   - 0x00 + 01LLLLLL LLLLLLLL: L transparent pixels (64-16383)
 *   - 0x00 + 10LLLLLL + CC: L pixels of color CC (1-63)
 *   - 0x00 + 11LLLLLL LLLLLLLL + CC: L pixels of color CC (64-16383)
 */
static uint8_t *decode_pgs_rle(const uint8_t *rle, size_t rle_size, int w, int h) {
  uint8_t *bitmap = calloc((size_t)w * (size_t)h, 1);
  if (!bitmap)
    return NULL;

  int x = 0, y = 0;
  size_t i = 0;

  while (i < rle_size && y < h) {
    uint8_t b = rle[i++];

    if (b != 0) {
      /* Single pixel of color b */
      if (x < w)
        bitmap[y * w + x] = b;
      x++;
    } else {
      /* Control byte follows */
      if (i >= rle_size)
        break;
      uint8_t flags = rle[i++];

      if (flags == 0) {
        /* End of line */
        x = 0;
        y++;
      } else {
        int len;
        uint8_t color;

        if ((flags & 0xC0) == 0x00) {
          /* 00LLLLLL: L transparent pixels */
          len = flags & 0x3F;
          color = 0;
        } else if ((flags & 0xC0) == 0x40) {
          /* 01LLLLLL LLLLLLLL: L transparent pixels */
          if (i >= rle_size)
            break;
          len = ((flags & 0x3F) << 8) | rle[i++];
          color = 0;
        } else if ((flags & 0xC0) == 0x80) {
          /* 10LLLLLL CC: L pixels of color CC */
          if (i >= rle_size)
            break;
          len = flags & 0x3F;
          color = rle[i++];
        } else {
          /* 11LLLLLL LLLLLLLL CC: L pixels of color CC */
          if (i + 1 >= rle_size)
            break;
          len = ((flags & 0x3F) << 8) | rle[i++];
          color = rle[i++];
        }

        for (int j = 0; j < len && x < w; j++)
          bitmap[y * w + (x++)] = color;
      }
    }
  }

  return bitmap;
}

/* ====================================================================== */
/*  Bitmap to Leptonica PIX conversion (from palette-indexed)             */
/* ====================================================================== */

/**
 * Convert a palette-indexed PGS bitmap to an 8-bit grayscale PIX.
 * Binarizes: luminance Y > 127 → black text (0), else white (255).
 * Transparent pixels (alpha < 128) → white (255).
 */
static PIX *pgs_bitmap_to_pix(const uint8_t *bitmap, int w, int h, const PgsPaletteEntry *palette) {
  PIX *pix = pixCreate(w, h, 8);
  if (!pix)
    return NULL;

  l_uint32 *data = pixGetData(pix);
  int wpl = pixGetWpl(pix);

  for (int y = 0; y < h; y++) {
    l_uint32 *line = data + y * wpl;
    for (int x = 0; x < w; x++) {
      uint8_t idx = bitmap[y * w + x];
      const PgsPaletteEntry *pe = &palette[idx];

      /* Transparent → white background */
      if (pe->a < 128) {
        SET_DATA_BYTE(line, x, 255);
        continue;
      }

      /* Binarize: bright text (Y > 127) → dark for OCR, else white */
      if (pe->y > 127)
        SET_DATA_BYTE(line, x, 0); /* dark text */
      else
        SET_DATA_BYTE(line, x, 255); /* background */
    }
  }

  return pix;
}

/**
 * Prepare a PGS bitmap PIX for OCR:
 *  1. Add white padding around all edges (characters at the border confuse
 *     Tesseract — especially "Il" misread as "|" or "[").
 *  2. Scale so the text height is in the 40–80 px sweet spot for Tesseract.
 */
static PIX *prepare_pix_for_ocr(PIX *src) {
  int w = pixGetWidth(src);
  int h = pixGetHeight(src);

  /* Padding: 20 px on each side (or proportional for very small images) */
  int pad = (h < 30) ? 30 : 20;

  int new_w = w + 2 * pad;
  int new_h = h + 2 * pad;

  /* Create padded image filled with white */
  PIX *padded = pixCreate(new_w, new_h, 8);
  if (!padded)
    return src;
  pixSetAll(padded); /* fill white (255 for 8-bit) */

  /* Copy source into center of padded image */
  pixRasterop(padded, pad, pad, w, h, PIX_SRC, src, 0, 0);
  pixDestroy(&src);

  /* Scale: target ~3x for subtitle-sized bitmaps (typically 20-50px tall),
     or 2x for larger ones.  Tesseract is most accurate at ~40-80px x-height. */
  float scale = 1.0F;
  int ph = pixGetHeight(padded);
  if (ph < 60)
    scale = 3.0F;
  else if (ph < 120)
    scale = 2.0F;

  if (scale > 1.0F) {
    PIX *scaled = pixScale(padded, scale, scale);
    if (scaled) {
      pixDestroy(&padded);
      padded = scaled;
    }
  }

  /* Otsu binarization: produces a clean 1bpp image that Tesseract
     handles much better than raw grayscale for glyph discrimination */
  PIX *binary = pixOtsuThreshOnBackgroundNorm(padded, NULL, 10, 15, 100, 50, 255, 2, 2, 0.1F, NULL);
  if (binary) {
    pixDestroy(&padded);

    return binary;
  }

  return padded;
}

/* ====================================================================== */
/*  Progress middle-string formatter                                      */
/* ====================================================================== */

/** Format the per-update middle string for the subtitle OCR progress bar. */
static void fmt_sub_middle(char *out, size_t cap, int current) {
  snprintf(out, cap, "%d subs", current);
}

/* ====================================================================== */
/*  Strip leading/trailing whitespace from OCR output                     */
/* ====================================================================== */

static void strip_whitespace(char *text) {
  if (!text)
    return;

  size_t len = strlen(text);
  while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\n' || text[len - 1] == '\r' ||
                     text[len - 1] == '\t'))
    text[--len] = '\0';

  char *start = text;
  while (*start == ' ' || *start == '\n' || *start == '\r' || *start == '\t')
    start++;

  if (start != text)
    memmove(text, start, strlen(start) + 1);
}

/* ====================================================================== */
/*  Tessdata preflight                                                    */
/* ====================================================================== */

int subtitle_ocr_preflight(const char *tesseract_lang, char *out_dir, size_t out_size) {
  const char *tessdata_path = getenv("TESSDATA_PREFIX");
  if (!tessdata_path || !tessdata_path[0]) {
    /* Common tessdata_best locations */
    static const char *paths[] = {
        "/usr/local/share/tessdata",
        "/opt/homebrew/share/tessdata",
        NULL,
    };
    tessdata_path = NULL;
    for (int i = 0; paths[i]; i++) {
      struct stat tst;
      if (stat(paths[i], &tst) == 0) {
        tessdata_path = paths[i];
        break;
      }
    }
  }
  if (!tessdata_path)
    return -1;
  char model[1024];
  snprintf(model, sizeof(model), "%s/%s.traineddata", tessdata_path, tesseract_lang);
  struct stat mst;
  if (stat(model, &mst) != 0)
    return -1;
  if (out_dir)
    snprintf(out_dir, out_size, "%s", tessdata_path);
  return 0;
}

/* ====================================================================== */
/*  Main conversion function                                              */
/* ====================================================================== */

SubtitleConvertResult convert_pgs_to_srt(const char *input_path, const TrackInfo *track,
                                         const char *output_path, const char *tesseract_lang) {
  SubtitleConvertResult result = {.error = 0, .skipped = 0, .subtitle_count = 0};
  snprintf(result.output_path, sizeof(result.output_path), "%s", output_path);

  /* Skip if output already exists. */
  struct stat st;
  if (stat(output_path, &st) == 0 && st.st_size > 0) {
    result.skipped = 1;
    return result;
  }

  /* Determine Tesseract language */
  const char *tess_lang = tesseract_lang;
  if (!tess_lang || !tess_lang[0])
    tess_lang = iso639_to_tesseract_lang(track->language);

  /* Initialize Tesseract — prefer tessdata_best for higher accuracy. */
  char tessdata_buf[1024];
  const char *tessdata_path = NULL;
  if (subtitle_ocr_preflight(tess_lang, tessdata_buf, sizeof(tessdata_buf)) == 0)
    tessdata_path = tessdata_buf;

  TessBaseAPI *tess = TessBaseAPICreate();
  if (TessBaseAPIInit3(tess, tessdata_path, tess_lang) != 0) {
    (void)fprintf(stderr, "  OCR Error: Tesseract init failed for lang '%s'\n", tess_lang);
    (void)fprintf(stderr, "  Make sure tessdata is installed for this language.\n");
    TessBaseAPIDelete(tess);
    result.error = -1;
    return result;
  }

  TessBaseAPISetPageSegMode(tess, PSM_SINGLE_BLOCK);

  TessBaseAPISetVariable(tess, "preserve_interword_spaces", "1");

  /* Open input with FFmpeg (for demuxing only, no subtitle decoding) */
  AVFormatContext *ifmt_ctx = NULL;
  AVPacket *pkt = NULL;
  FILE *srt_fp = NULL;
  char errbuf[AV_ERROR_MAX_STRING_SIZE];
  int ret;

  /* PGS display set accumulator — initialized here, before the first
     `goto cleanup` below, since the cleanup path always resets it. */
  PgsDisplaySet ds;
  pgs_ds_init(&ds);

  ret = avformat_open_input(&ifmt_ctx, input_path, NULL, NULL);
  if (ret < 0) {
    av_make_error_string(errbuf, sizeof(errbuf), ret);
    (void)fprintf(stderr, "  OCR Error: cannot open '%s': %s\n", input_path, errbuf);
    result.error = ret;
    goto cleanup;
  }

  ret = avformat_find_stream_info(ifmt_ctx, NULL);
  if (ret < 0) {
    result.error = ret;
    goto cleanup;
  }

  if (track->index < 0 || (unsigned)track->index >= ifmt_ctx->nb_streams) {
    (void)fprintf(stderr, "  OCR Error: stream index %d out of range\n", track->index);
    result.error = -1;
    goto cleanup;
  }

  AVStream *sub_stream = ifmt_ctx->streams[track->index];

  /* Open SRT output file */
  srt_fp = fopen(output_path, "w");
  if (!srt_fp) {
    (void)fprintf(stderr, "  OCR Error: cannot create '%s'\n", output_path);
    result.error = -1;
    goto cleanup;
  }

  /* Duration estimate for progress */
  int64_t duration_ms = 0;
  if (ifmt_ctx->duration > 0)
    duration_ms = ifmt_ctx->duration / (AV_TIME_BASE / 1000);

  pkt = av_packet_alloc();
  if (!pkt) {
    result.error = -1;
    goto cleanup;
  }

  UiProgress progress;
  ui_progress_start(&progress, (long long)duration_ms);
  time_t last_progress = 0;
  int sub_index = 0;

  /* Previous display set for timestamps (end time = start of next) */
  int64_t prev_pts_ms = -1;
  PIX *prev_pix = NULL;

  /* Demux and parse loop */
  while (av_read_frame(ifmt_ctx, pkt) >= 0) {
    if (pkt->stream_index != track->index) {
      av_packet_unref(pkt);
      continue;
    }

    /* Get PTS in ms */
    int64_t pts_ms = 0;
    if (pkt->pts != AV_NOPTS_VALUE)
      pts_ms = av_rescale_q(pkt->pts, sub_stream->time_base, (AVRational){1, 1000});

    /* Parse PGS segments from this packet */
    parse_pgs_packet(&ds, pkt->data, pkt->size);

    /* When we have a valid display set with a bitmap, process it */
    if (ds.valid && ds.palette_set && ds.rle_data) {
      /* If there's a previous subtitle pending, it ends now */
      if (prev_pix && prev_pts_ms >= 0) {
        int64_t end_ms = pts_ms;

        /* OCR the previous bitmap */
        TessBaseAPISetImage2(tess, prev_pix);
        char *text = TessBaseAPIGetUTF8Text(tess);

        if (text && text[0]) {
          strip_whitespace(text);
          if (text[0]) {
            sub_index++;
            char start_str[32], end_str[32];
            format_srt_time(start_str, sizeof(start_str), prev_pts_ms);
            format_srt_time(end_str, sizeof(end_str), end_ms);
            (void)fprintf(srt_fp, "%d\n%s --> %s\n%s\n\n", sub_index, start_str, end_str, text);
          }
        }
        if (text)
          TessDeleteText(text);

        pixDestroy(&prev_pix);
        prev_pix = NULL;
      }

      /* Decode RLE bitmap */
      uint8_t *bitmap = decode_pgs_rle(ds.rle_data, ds.rle_size, ds.obj_width, ds.obj_height);
      if (bitmap) {
        PIX *pix = pgs_bitmap_to_pix(bitmap, ds.obj_width, ds.obj_height, ds.palette);
        free(bitmap);

        if (pix) {
          pix = prepare_pix_for_ocr(pix);
          prev_pix = pix;
          prev_pts_ms = pts_ms;
        }
      }

      /* Reset display set for next subtitle */
      free(ds.rle_data);
      ds.rle_data = NULL;
      ds.rle_size = 0;
      ds.rle_capacity = 0;
      ds.valid = 0;
    } else if (!ds.valid && prev_pix && prev_pts_ms >= 0) {
      /* Display set with no bitmap = clearing event. End the previous sub. */
      if (ds.composition_state == PCS_NORMAL || ds.composition_state == PCS_ACQ_POINT ||
          ds.composition_state == PCS_EPOCH_START) {
        int64_t end_ms = pts_ms;

        TessBaseAPISetImage2(tess, prev_pix);
        char *text = TessBaseAPIGetUTF8Text(tess);

        if (text && text[0]) {
          strip_whitespace(text);
          if (text[0]) {
            sub_index++;
            char start_str[32], end_str[32];
            format_srt_time(start_str, sizeof(start_str), prev_pts_ms);
            format_srt_time(end_str, sizeof(end_str), end_ms);
            (void)fprintf(srt_fp, "%d\n%s --> %s\n%s\n\n", sub_index, start_str, end_str, text);
          }
        }
        if (text)
          TessDeleteText(text);

        pixDestroy(&prev_pix);
        prev_pix = NULL;
        prev_pts_ms = -1;
      }
    }

    av_packet_unref(pkt);

    /* Progress update */
    time_t now = time(NULL);
    if (now != last_progress) {
      char middle[32];
      fmt_sub_middle(middle, sizeof(middle), sub_index);
      ui_progress_update(&progress, (long long)pts_ms, middle);
      last_progress = now;
    }
  }

  /* Flush last pending subtitle (use a generous end time) */
  if (prev_pix && prev_pts_ms >= 0) {
    int64_t end_ms = prev_pts_ms + 3000; /* default 3 second duration */

    TessBaseAPISetImage2(tess, prev_pix);
    char *text = TessBaseAPIGetUTF8Text(tess);

    if (text && text[0]) {
      strip_whitespace(text);
      if (text[0]) {
        sub_index++;
        char start_str[32], end_str[32];
        format_srt_time(start_str, sizeof(start_str), prev_pts_ms);
        format_srt_time(end_str, sizeof(end_str), end_ms);
        (void)fprintf(srt_fp, "%d\n%s --> %s\n%s\n\n", sub_index, start_str, end_str, text);
      }
    }
    if (text)
      TessDeleteText(text);
    pixDestroy(&prev_pix);
  }

  result.subtitle_count = sub_index;

  /* Final progress */
  {
    char middle[32];
    fmt_sub_middle(middle, sizeof(middle), sub_index);
    ui_progress_done(&progress, (long long)duration_ms, middle);
  }

cleanup:
  pgs_ds_reset(&ds);

  if (pkt)
    av_packet_free(&pkt);
  if (srt_fp && fclose(srt_fp) != 0 && result.error == 0) {
    (void)fprintf(stderr, "  Subtitle Error: failed to finalize '%s'\n", output_path);
    result.error = -1;
  }
  if (ifmt_ctx)
    avformat_close_input(&ifmt_ctx);

  TessBaseAPIEnd(tess);
  TessBaseAPIDelete(tess);

  /* Remove output on failure */
  if (result.error != 0 && !result.skipped)
    (void)remove(output_path); /* best-effort cleanup */

  return result;
}
