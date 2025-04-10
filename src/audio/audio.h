#ifndef AUDIO_H
#define AUDIO_H

/**
 * Transcode l'audio d'un fichier source vers un fichier de destination en utilisant Opus
 * 
 * @param input_file Chemin vers le fichier source
 * @param output_file Chemin vers le fichier de destination
 * @return 0 en cas de succès, code d'erreur sinon
 */
int transcode_audio(const char *input_file, const char *output_file);

#endif /* AUDIO_H */