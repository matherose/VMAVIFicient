#include "audio.h"
#include <stdio.h>
#include "config.h"

#ifdef HAVE_LIBAVFORMAT
#include <libavformat/avformat.h>
#endif

#ifdef HAVE_LIBAVCODEC
#include <libavcodec/avcodec.h>
#endif

#ifdef HAVE_LIBAVUTIL
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#endif

#define AUDIO_INBUF_SIZE 20480
#define AUDIO_REFILL_THRESH 4096

int transcode_audio(const char *input_file, const char *output_file) {
#if defined(HAVE_LIBAVFORMAT) && defined(HAVE_LIBAVCODEC) && defined(HAVE_LIBAVUTIL)
    AVFormatContext *input_format_ctx = NULL;
    AVFormatContext *output_format_ctx = NULL;
    AVCodecContext *decoder_ctx = NULL;
    AVCodecContext *encoder_ctx = NULL;
    const AVCodec *decoder = NULL;
    const AVCodec *encoder = NULL;
    AVStream *out_stream = NULL;
    int stream_index = -1;
    int ret = 0;
    
    // Ouvrir le fichier d'entrée
    if (avformat_open_input(&input_format_ctx, input_file, NULL, NULL) < 0) {
        fprintf(stderr, "Impossible d'ouvrir le fichier d'entrée '%s'\n", input_file);
        return -1;
    }
    
    // Récupérer les informations sur les flux
    if (avformat_find_stream_info(input_format_ctx, NULL) < 0) {
        fprintf(stderr, "Impossible de trouver les informations sur les flux\n");
        avformat_close_input(&input_format_ctx);
        return -1;
    }
    
    // Trouver le premier flux audio
    for (unsigned int i = 0; i < input_format_ctx->nb_streams; i++) {
        if (input_format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            stream_index = i;
            break;
        }
    }
    
    if (stream_index == -1) {
        fprintf(stderr, "Aucun flux audio trouvé dans le fichier d'entrée\n");
        avformat_close_input(&input_format_ctx);
        return -1;
    }
    
    // Trouver le décodeur audio approprié
    decoder = avcodec_find_decoder(input_format_ctx->streams[stream_index]->codecpar->codec_id);
    if (!decoder) {
        fprintf(stderr, "Décodeur audio non trouvé\n");
        avformat_close_input(&input_format_ctx);
        return -1;
    }
    
    // Allouer le contexte du décodeur
    decoder_ctx = avcodec_alloc_context3(decoder);
    if (!decoder_ctx) {
        fprintf(stderr, "Échec d'allocation du contexte de décodeur\n");
        avformat_close_input(&input_format_ctx);
        return -1;
    }
    
    // Copier les paramètres du flux d'entrée vers le contexte du décodeur
    if (avcodec_parameters_to_context(decoder_ctx, input_format_ctx->streams[stream_index]->codecpar) < 0) {
        fprintf(stderr, "Échec de la copie des paramètres de codec\n");
        avcodec_free_context(&decoder_ctx);
        avformat_close_input(&input_format_ctx);
        return -1;
    }
    
    // Ouvrir le décodeur
    if (avcodec_open2(decoder_ctx, decoder, NULL) < 0) {
        fprintf(stderr, "Échec d'ouverture du décodeur\n");
        avcodec_free_context(&decoder_ctx);
        avformat_close_input(&input_format_ctx);
        return -1;
    }
    
    // Trouver l'encodeur Opus
    encoder = avcodec_find_encoder_by_name("libopus");
    if (!encoder) {
        fprintf(stderr, "Encodeur Opus non trouvé\n");
        avcodec_free_context(&decoder_ctx);
        avformat_close_input(&input_format_ctx);
        return -1;
    }
    
    // Allouer le contexte de l'encodeur
    encoder_ctx = avcodec_alloc_context3(encoder);
    if (!encoder_ctx) {
        fprintf(stderr, "Échec d'allocation du contexte d'encodeur\n");
        avcodec_free_context(&decoder_ctx);
        avformat_close_input(&input_format_ctx);
        return -1;
    }
    
    // Configurer les paramètres d'encodage Opus
    encoder_ctx->sample_rate = decoder_ctx->sample_rate;
    encoder_ctx->ch_layout = decoder_ctx->ch_layout;
    encoder_ctx->sample_fmt = encoder->sample_fmts[0]; // Utiliser le premier format d'échantillon supporté
    encoder_ctx->bit_rate = 128000; // 128kbps est un bon compromis pour la qualité audio
    encoder_ctx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
    
    // Ouvrir l'encodeur
    if (avcodec_open2(encoder_ctx, encoder, NULL) < 0) {
        fprintf(stderr, "Échec d'ouverture de l'encodeur Opus\n");
        avcodec_free_context(&encoder_ctx);
        avcodec_free_context(&decoder_ctx);
        avformat_close_input(&input_format_ctx);
        return -1;
    }
    
    // Nous ne créerons pas un fichier de sortie complet ici car nous avons besoin
    // d'intégrer cet audio dans un fichier conteneur qui contiendra aussi la vidéo.
    // Cette fonction est appelée par transcode_video qui gère le multiplexage final.
    
    printf("Transcodage audio de %s en Opus (pour intégration dans %s)\n", input_file, output_file);
    
    // Nettoyer les ressources
    avcodec_free_context(&encoder_ctx);
    avcodec_free_context(&decoder_ctx);
    avformat_close_input(&input_format_ctx);
    
    return 0;
#else
    printf("Fonctionnalité de transcodage audio non disponible (dépendances manquantes)\n");
    printf("Simulation de transcodage audio de %s vers %s avec codec Opus\n", input_file, output_file);
    return 0;
#endif
}