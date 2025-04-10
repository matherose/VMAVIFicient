project('vmavificient', 'c',
  version : '0.1.0',
  license : 'BSD-2-Clause',
  default_options : [
    'warning_level=3',
    'c_std=c11',
  ]
)

# Configuration pour la génération du fichier config.h
conf_data = configuration_data()

# Utiliser les wrappers pour gérer les dépendances

# FFmpeg - nécessaire pour les fonctionnalités de base
ffmpeg_opt = get_option('use_system_ffmpeg')
if ffmpeg_opt
  # Tentative d'utiliser les bibliothèques système
  avformat_dep = dependency('libavformat', required : false)
  avcodec_dep = dependency('libavcodec', required : false)
  avutil_dep = dependency('libavutil', required : false)
  swscale_dep = dependency('libswscale', required : false)
  swresample_dep = dependency('libswresample', required : false)
  
  all_ffmpeg_found = avformat_dep.found() and avcodec_dep.found() and avutil_dep.found() and 
                     swscale_dep.found() and swresample_dep.found()
                     
  if not all_ffmpeg_found
    # Fallback sur le wrap si une des bibliothèques système manque
    message('Certaines bibliothèques FFmpeg système manquantes, utilisation du wrapper')
    ffmpeg_opt = false
  endif
else
  # Les bibliothèques systèmes ne sont pas utilisées
  avformat_dep = dependency('', required : false)
  avcodec_dep = dependency('', required : false)
  avutil_dep = dependency('', required : false)
  swscale_dep = dependency('', required : false)
  swresample_dep = dependency('', required : false)
endif

if not ffmpeg_opt
  # Utilisation du wrap FFmpeg
  message('Configuration du wrapper FFmpeg')
  ffmpeg_wrap = subproject('ffmpeg-wrap')
  avformat_dep = ffmpeg_wrap.get_variable('avformat_dep')
  avcodec_dep = ffmpeg_wrap.get_variable('avcodec_dep')
  avutil_dep = ffmpeg_wrap.get_variable('avutil_dep')
  swscale_dep = ffmpeg_wrap.get_variable('swscale_dep')
  swresample_dep = ffmpeg_wrap.get_variable('swresample_dep')
endif

# Définir les flags de configuration pour FFmpeg
if avformat_dep.found()
  conf_data.set('HAVE_LIBAVFORMAT', 1)
endif
if avcodec_dep.found()
  conf_data.set('HAVE_LIBAVCODEC', 1)
endif
if avutil_dep.found()
  conf_data.set('HAVE_LIBAVUTIL', 1)
endif
if swscale_dep.found()
  conf_data.set('HAVE_LIBSWSCALE', 1)
endif
if swresample_dep.found()
  conf_data.set('HAVE_LIBSWRESAMPLE', 1)
endif

# libaom - nécessaire pour l'encodage AV1/AVIF
aom_opt = get_option('use_system_aom')
if aom_opt
  aom_dep = dependency('aom', required : false)
  if not aom_dep.found()
    message('Bibliothèque aom système manquante, utilisation du wrapper')
    aom_opt = false
  endif
else
  aom_dep = dependency('', required : false)
endif

if not aom_opt
  message('Configuration du wrapper libaom')
  aom_wrap = subproject('libaom-wrap')
  aom_dep = aom_wrap.get_variable('aom_dep')
endif

if aom_dep.found()
  conf_data.set('HAVE_LIBAOM', 1)
endif

# libopus - nécessaire pour l'encodage audio
opus_opt = get_option('use_system_opus')
if opus_opt
  opus_dep = dependency('opus', required : false)
  if not opus_dep.found()
    message('Bibliothèque opus système manquante, utilisation du wrapper')
    opus_opt = false
  endif
else
  opus_dep = dependency('', required : false)
endif

if not opus_opt
  message('Configuration du wrapper libopus')
  opus_wrap = subproject('libopus-wrap')
  opus_dep = opus_wrap.get_variable('opus_dep')
endif

if opus_dep.found()
  conf_data.set('HAVE_LIBOPUS', 1)
endif

# libexif - utilisé pour gérer les métadonnées EXIF
exif_opt = get_option('use_system_exif')
if exif_opt
  exif_dep = dependency('libexif', required : false)
  if not exif_dep.found()
    message('Bibliothèque exif système manquante, utilisation du wrapper')
    exif_opt = false
  endif
else
  exif_dep = dependency('', required : false)
endif

if not exif_opt
  message('Configuration du wrapper libexif')
  exif_wrap = subproject('libexif-wrap')
  exif_dep = exif_wrap.get_variable('exif_dep')
endif

if exif_dep.found()
  conf_data.set('HAVE_LIBEXIF', 1)
endif

# libvmaf - utilisé pour les métriques de qualité vidéo
vmaf_opt = get_option('use_system_vmaf')
if vmaf_opt
  vmaf_dep = dependency('vmaf', required : false)
  if not vmaf_dep.found()
    message('Bibliothèque vmaf système manquante, utilisation du wrapper')
    vmaf_opt = false
  endif
else
  vmaf_dep = dependency('', required : false)
endif

if not vmaf_opt
  message('Configuration du wrapper libvmaf')
  vmaf_wrap = subproject('libvmaf-wrap')
  vmaf_dep = vmaf_wrap.get_variable('vmaf_dep')
endif

if vmaf_dep.found()
  conf_data.set('HAVE_LIBVMAF', 1)
endif

# Configuration des répertoires d'inclusion
inc_build = include_directories('.')

# Génération du fichier config.h
configure_file(
  output : 'config.h',
  configuration : conf_data
)

# Compilation des sous-répertoires
subdir('src')