#ifndef VMAF_H
#define VMAF_H

#ifdef __cplusplus
extern "C" {
#endif

// Structure pour décodage vidéo (utilisée en interne)
typedef struct DecodingContext DecodingContext;

float assess_vmaf(const char *reference_file, const char *distorted_file);

// Fonctions auxiliaires (utilisées uniquement à l'intérieur du module)
#if defined(HAVE_LIBVMAF) && defined(HAVE_LIBAVFORMAT) && defined(HAVE_LIBAVCODEC) && defined(HAVE_LIBAVUTIL)
static int init_decoding_context(DecodingContext *ctx, const char *file_path);
static AVFrame* get_next_frame(DecodingContext *ctx, AVPacket *pkt, AVFrame *frame);
static void close_decoding_context(DecodingContext *ctx);
#endif

#ifdef __cplusplus
}
#endif

#endif /* VMAF_H */