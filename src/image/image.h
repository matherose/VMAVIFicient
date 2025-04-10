#ifndef IMAGE_H
#define IMAGE_H

/**
 * Convertit une image en format AVIF
 * 
 * @param input_file Chemin vers l'image source
 * @param output_file Chemin vers le fichier AVIF de destination
 * @return 0 en cas de succès, code d'erreur sinon
 */
int convert_image(const char *input_file, const char *output_file);

#endif /* IMAGE_H */