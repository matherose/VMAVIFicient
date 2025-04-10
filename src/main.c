#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "audio/audio.h"
#include "video/video.h"
#include "image/image.h"
#include "vmaf/vmaf.h"
#include "metadata/metadata.h"

// Prototypes des fonctions
long get_file_size(const char *filename);

int main(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Usage: %s input_file output_file [options]\n", argv[0]);
        printf("Options:\n");
        printf("  --no-vmaf     Disable VMAF quality assessment\n");
        return 1;
    }

    const char *input_file = argv[1];
    const char *output_file = argv[2];
    int use_vmaf = 1;  // Par défaut, utiliser VMAF

    // Traitement des options
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--no-vmaf") == 0) {
            use_vmaf = 0;
        }
    }

    // Déterminer le type de fichier selon l'extension
    const char *ext = strrchr(input_file, '.');
    if (!ext) {
        printf("Erreur: Impossible de déterminer le type de fichier d'entrée\n");
        return 1;
    }

    // Conversion d'une image en AVIF
    if (strcasecmp(ext, ".png") == 0 || 
        strcasecmp(ext, ".jpg") == 0 || 
        strcasecmp(ext, ".jpeg") == 0) {
        printf("Conversion d'image en AVIF...\n");
        if (convert_image(input_file, output_file) != 0) {
            printf("Erreur lors de la conversion d'image\n");
            return 1;
        }
        
        // Préserver les métadonnées
        if (preserve_metadata(input_file, output_file) != 0) {
            printf("Avertissement: Échec de la préservation des métadonnées\n");
        }
    } 
    // Transcoder une vidéo en AV1/Opus dans MKV
    else if (strcasecmp(ext, ".mp4") == 0 || 
             strcasecmp(ext, ".mkv") == 0 || 
             strcasecmp(ext, ".webm") == 0 ||
             strcasecmp(ext, ".mov") == 0) {
        printf("Transcodage de vidéo en AV1/Opus (MKV)...\n");
        
        // Transcoder vidéo
        if (transcode_video(input_file, output_file) != 0) {
            printf("Erreur lors du transcodage vidéo\n");
            return 1;
        }
        
        // Transcoder audio
        if (transcode_audio(input_file, output_file) != 0) {
            printf("Erreur lors du transcodage audio\n");
            return 1;
        }
        
        // Évaluer la qualité avec VMAF si activé
        if (use_vmaf) {
            float vmaf_score = assess_vmaf(input_file, output_file);
            printf("Score VMAF: %.2f\n", vmaf_score);
        }
    } else {
        printf("Format de fichier non pris en charge: %s\n", ext);
        return 1;
    }

    // Afficher les statistiques de taille des fichiers
    long original_size = get_file_size(input_file);
    long new_size = get_file_size(output_file);
    float reduction = 100.0f * (1.0f - (float)new_size / original_size);
    
    printf("Conversion terminée: taille originale %ld Mo, nouvelle taille %ld Mo (%.1f%% de réduction).\n",
           original_size / (1024 * 1024), new_size / (1024 * 1024), reduction);

    return 0;
}

// Fonction utilitaire pour obtenir la taille d'un fichier
long get_file_size(const char *filename) {
    FILE *file = fopen(filename, "rb");
    if (!file) return 0;
    
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fclose(file);
    
    return size;
}