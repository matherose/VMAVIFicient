#ifndef VIDEO_H
#define VIDEO_H

/**
 * Transcode une vidéo source vers un fichier de destination en utilisant AV1
 * 
 * @param input_file Chemin vers le fichier source
 * @param output_file Chemin vers le fichier de destination
 * @return 0 en cas de succès, code d'erreur sinon
 */
int transcode_video(const char *input_file, const char *output_file);

#endif /* VIDEO_H */