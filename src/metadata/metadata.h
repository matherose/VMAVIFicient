#ifndef METADATA_H
#define METADATA_H

/**
 * Préserve les métadonnées du fichier source dans le fichier de destination
 * 
 * @param input_file Chemin vers le fichier source
 * @param output_file Chemin vers le fichier de destination
 * @return 0 en cas de succès, code d'erreur sinon
 */
int preserve_metadata(const char *input_file, const char *output_file);

#endif /* METADATA_H */