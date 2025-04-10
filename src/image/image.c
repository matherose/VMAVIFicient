#include "image.h"
#include <stdio.h>
#include <stdlib.h>
#include "config.h"

#ifdef HAVE_LIBAVFORMAT
#include <libavformat/avformat.h>
#endif

#ifdef HAVE_LIBAVCODEC
#include <libavcodec/avcodec.h>
#endif

#ifdef HAVE_LIBAVUTIL
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#endif

#ifdef HAVE_LIBSWSCALE
#include <libswscale/swscale.h>
#endif

int convert_image(const char *input_file, const char *output_file) {
#if defined(HAVE_LIBAVFORMAT) && defined(HAVE_LIBAVCODEC) && defined(HAVE_LIBAVUTIL) && defined(HAVE_LIBSWSCALE)
    AVFormatContext *input_ctx = NULL;
    AVFormatContext *output_ctx = NULL;
    AVCodecContext *dec_ctx = NULL;
    AVCodecContext *enc_ctx = NULL;
    const AVCodec *decoder = NULL;
    const AVCodec *encoder = NULL;
    AVStream *in_stream = NULL;
    AVStream *out_stream = NULL;
    AVFrame *frame = NULL;
    AVPacket *pkt_in = NULL;
    AVPacket *pkt_out = NULL;
    struct SwsContext *sws_ctx = NULL;
    int ret = 0;
    int stream_index = -1;
    
    // Ouvrir le fichier d'entrée
    if ((ret = avformat_open_input(&input_ctx, input_file, NULL, NULL)) < 0) {
        fprintf(stderr, "Impossible d'ouvrir le fichier d'entrée '%s'\n", input_file);
        return ret;
    }
    
    // Récupérer les informations sur les flux
    if ((ret = avformat_find_stream_info(input_ctx, NULL)) < 0) {
        fprintf(stderr, "Impossible de trouver les informations sur les flux\n");
        goto end;
    }
    
    // Trouver le flux vidéo
    for (int i = 0; i < input_ctx->nb_streams; i++) {
        if (input_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            stream_index = i;
            break;
        }
    }
    
    if (stream_index == -1) {
        fprintf(stderr, "Aucun flux vidéo trouvé dans le fichier d'entrée\n");
        ret = AVERROR_STREAM_NOT_FOUND;
        goto end;
    }
    
    in_stream = input_ctx->streams[stream_index];
    
    // Trouver le décodeur approprié
    decoder = avcodec_find_decoder(in_stream->codecpar->codec_id);
    if (!decoder) {
        fprintf(stderr, "Décodeur non trouvé\n");
        ret = AVERROR_DECODER_NOT_FOUND;
        goto end;
    }
    
    // Allouer le contexte du décodeur
    dec_ctx = avcodec_alloc_context3(decoder);
    if (!dec_ctx) {
        fprintf(stderr, "Échec d'allocation du contexte de décodeur\n");
        ret = AVERROR(ENOMEM);
        goto end;
    }
    
    // Copier les paramètres du flux d'entrée vers le contexte du décodeur
    if ((ret = avcodec_parameters_to_context(dec_ctx, in_stream->codecpar)) < 0) {
        fprintf(stderr, "Échec de la copie des paramètres de codec\n");
        goto end;
    }
    
    // Ouvrir le décodeur
    if ((ret = avcodec_open2(dec_ctx, decoder, NULL)) < 0) {
        fprintf(stderr, "Échec d'ouverture du décodeur\n");
        goto end;
    }
    
    // Créer le contexte de sortie
    if ((ret = avformat_alloc_output_context2(&output_ctx, NULL, "avif", output_file)) < 0) {
        fprintf(stderr, "Impossible de créer le contexte de sortie\n");
        goto end;
    }
    
    // Trouver l'encodeur AV1 pour AVIF
    encoder = avcodec_find_encoder_by_name("libaom-av1");
    if (!encoder) {
        fprintf(stderr, "Encodeur AV1 non trouvé\n");
        ret = AVERROR_ENCODER_NOT_FOUND;
        goto end;
    }
    
    // Créer le flux de sortie
    out_stream = avformat_new_stream(output_ctx, NULL);
    if (!out_stream) {
        fprintf(stderr, "Échec lors de la création du flux de sortie\n");
        ret = AVERROR(ENOMEM);
        goto end;
    }
    
    // Allouer le contexte de l'encodeur
    enc_ctx = avcodec_alloc_context3(encoder);
    if (!enc_ctx) {
        fprintf(stderr, "Échec d'allocation du contexte d'encodeur\n");
        ret = AVERROR(ENOMEM);
        goto end;
    }
    
    // Configurer l'encodeur
    enc_ctx->width = dec_ctx->width;
    enc_ctx->height = dec_ctx->height;
    enc_ctx->sample_aspect_ratio = dec_ctx->sample_aspect_ratio;
    enc_ctx->time_base = (AVRational){1, 25};
    enc_ctx->pix_fmt = AV_PIX_FMT_YUV420P; // Format standard pour AVIF
    
    // Paramètres de qualité pour AVIF
    enc_ctx->bit_rate = 0; // Mode CRF
    
    // Options spécifiques pour AVIF
    if (av_opt_set(enc_ctx->priv_data, "cpu-used", "3", 0) < 0 ||
        av_opt_set(enc_ctx->priv_data, "crf", "18", 0) < 0 ||
        av_opt_set(enc_ctx->priv_data, "usage", "allintra", 0) < 0 ||
        av_opt_set_int(enc_ctx->priv_data, "tile-columns", 2, 0) < 0 ||
        av_opt_set_int(enc_ctx->priv_data, "tile-rows", 2, 0) < 0) {
        fprintf(stderr, "Échec de la configuration des options d'encodage\n");
    }
    
    // Options pour le conteneur AVIF
    if (output_ctx->oformat->flags & AVFMT_GLOBALHEADER)
        enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    
    // Ouvrir l'encodeur
    if ((ret = avcodec_open2(enc_ctx, encoder, NULL)) < 0) {
        fprintf(stderr, "Échec d'ouverture de l'encodeur AV1\n");
        goto end;
    }
    
    // Copier les paramètres de l'encodeur au flux de sortie
    if ((ret = avcodec_parameters_from_context(out_stream->codecpar, enc_ctx)) < 0) {
        fprintf(stderr, "Échec de la copie des paramètres de l'encodeur\n");
        goto end;
    }
    
    // Ouvrir le fichier de sortie
    if (!(output_ctx->oformat->flags & AVFMT_NOFILE)) {
        if ((ret = avio_open(&output_ctx->pb, output_file, AVIO_FLAG_WRITE)) < 0) {
            fprintf(stderr, "Impossible d'ouvrir le fichier de sortie '%s'\n", output_file);
            goto end;
        }
    }
    
    // Écrire l'en-tête
    if ((ret = avformat_write_header(output_ctx, NULL)) < 0) {
        fprintf(stderr, "Erreur lors de l'écriture de l'en-tête\n");
        goto end;
    }
    
    // Allouer les tampons
    frame = av_frame_alloc();
    if (!frame) {
        fprintf(stderr, "Échec d'allocation du frame\n");
        ret = AVERROR(ENOMEM);
        goto end;
    }
    
    pkt_in = av_packet_alloc();
    if (!pkt_in) {
        fprintf(stderr, "Échec d'allocation du paquet d'entrée\n");
        ret = AVERROR(ENOMEM);
        goto end;
    }
    
    pkt_out = av_packet_alloc();
    if (!pkt_out) {
        fprintf(stderr, "Échec d'allocation du paquet de sortie\n");
        ret = AVERROR(ENOMEM);
        goto end;
    }
    
    // Lire la première trame (puisque nous traitons une image unique)
    if ((ret = av_read_frame(input_ctx, pkt_in)) < 0) {
        fprintf(stderr, "Erreur lors de la lecture de l'image\n");
        goto end;
    }
    
    if (pkt_in->stream_index != stream_index) {
        fprintf(stderr, "Le paquet lu n'appartient pas au flux vidéo\n");
        ret = AVERROR_INVALIDDATA;
        goto end;
    }
    
    // Envoyer le paquet au décodeur
    if ((ret = avcodec_send_packet(dec_ctx, pkt_in)) < 0) {
        fprintf(stderr, "Erreur lors de l'envoi du paquet au décodeur\n");
        goto end;
    }
    
    // Recevoir la trame du décodeur
    if ((ret = avcodec_receive_frame(dec_ctx, frame)) < 0) {
        fprintf(stderr, "Erreur lors de la réception du frame du décodeur\n");
        goto end;
    }
    
    // Si nécessaire, convertir le format de pixel
    if (frame->format != enc_ctx->pix_fmt) {
        AVFrame *converted_frame = av_frame_alloc();
        if (!converted_frame) {
            fprintf(stderr, "Échec d'allocation du frame converti\n");
            ret = AVERROR(ENOMEM);
            goto end;
        }
        
        converted_frame->width = frame->width;
        converted_frame->height = frame->height;
        converted_frame->format = enc_ctx->pix_fmt;
        
        if ((ret = av_frame_get_buffer(converted_frame, 0)) < 0) {
            fprintf(stderr, "Échec d'allocation du tampon pour le frame converti\n");
            av_frame_free(&converted_frame);
            goto end;
        }
        
        // Initialiser le contexte de conversion
        sws_ctx = sws_getContext(
            frame->width, frame->height, frame->format,
            converted_frame->width, converted_frame->height, enc_ctx->pix_fmt,
            SWS_BICUBIC, NULL, NULL, NULL
        );
        
        if (!sws_ctx) {
            fprintf(stderr, "Échec de l'initialisation du contexte de conversion\n");
            av_frame_free(&converted_frame);
            ret = AVERROR(EINVAL);
            goto end;
        }
        
        // Effectuer la conversion
        sws_scale(sws_ctx, (const uint8_t * const *)frame->data, frame->linesize,
                 0, frame->height, converted_frame->data, converted_frame->linesize);
        
        av_frame_copy_props(converted_frame, frame);
        av_frame_free(&frame);
        frame = converted_frame;
        sws_freeContext(sws_ctx);
        sws_ctx = NULL;
    }
    
    // Envoyer la trame à l'encodeur
    if ((ret = avcodec_send_frame(enc_ctx, frame)) < 0) {
        fprintf(stderr, "Erreur lors de l'envoi du frame à l'encodeur\n");
        goto end;
    }
    
    // Recevoir le paquet encodé
    ret = avcodec_receive_packet(enc_ctx, pkt_out);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        ret = 0; // Ce n'est pas une erreur, juste pas de paquet disponible
    } else if (ret < 0) {
        fprintf(stderr, "Erreur lors de la réception du paquet encodé\n");
        goto end;
    } else {
        // Ajuster les valeurs du paquet
        pkt_out->stream_index = 0;
        
        // Écrire le paquet dans le fichier de sortie
        if ((ret = av_interleaved_write_frame(output_ctx, pkt_out)) < 0) {
            fprintf(stderr, "Erreur lors de l'écriture du paquet\n");
            goto end;
        }
    }
    
    // Finaliser l'encodage
    if ((ret = avcodec_send_frame(enc_ctx, NULL)) < 0) {
        fprintf(stderr, "Erreur lors de la finalisation de l'encodage\n");
        goto end;
    }
    
    while (ret >= 0) {
        ret = avcodec_receive_packet(enc_ctx, pkt_out);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            fprintf(stderr, "Erreur lors de la réception du paquet encodé\n");
            goto end;
        }
        
        pkt_out->stream_index = 0;
        if ((ret = av_interleaved_write_frame(output_ctx, pkt_out)) < 0) {
            fprintf(stderr, "Erreur lors de l'écriture du paquet\n");
            goto end;
        }
    }
    
    // Écrire la fin du fichier
    if ((ret = av_write_trailer(output_ctx)) < 0) {
        fprintf(stderr, "Erreur lors de l'écriture du trailer\n");
        goto end;
    }
    
    printf("Conversion de l'image %s vers AVIF %s complétée\n", input_file, output_file);
    ret = 0;
    
end:
    // Nettoyage
    if (dec_ctx)
        avcodec_free_context(&dec_ctx);
    if (enc_ctx)
        avcodec_free_context(&enc_ctx);
    if (input_ctx)
        avformat_close_input(&input_ctx);
    if (output_ctx && !(output_ctx->oformat->flags & AVFMT_NOFILE))
        avio_closep(&output_ctx->pb);
    if (output_ctx)
        avformat_free_context(output_ctx);
    if (frame)
        av_frame_free(&frame);
    if (pkt_in)
        av_packet_free(&pkt_in);
    if (pkt_out)
        av_packet_free(&pkt_out);
    if (sws_ctx)
        sws_freeContext(sws_ctx);
    
    return ret < 0 ? ret : 0;
#else
    printf("Fonctionnalité de conversion d'image non disponible (dépendances manquantes)\n");
    printf("Simulation de conversion de l'image %s vers AVIF %s\n", input_file, output_file);
    return 0;
#endif
}