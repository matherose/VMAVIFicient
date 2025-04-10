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

# Vérification des dépendances disponibles sur le système
avformat_dep = dependency('libavformat', required : false)
avcodec_dep = dependency('libavcodec', required : false)
avutil_dep = dependency('libavutil', required : false)
swscale_dep = dependency('libswscale', required : false)
aom_dep = dependency('aom', required : false)
opus_dep = dependency('opus', required : false)
vmaf_dep = dependency('libvmaf', required : false)
exif_dep = dependency('libexif', required : false)

# Définir les macros de configuration
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
if aom_dep.found()
  conf_data.set('HAVE_LIBAOM', 1)
endif
if opus_dep.found()
  conf_data.set('HAVE_LIBOPUS', 1)
endif
if vmaf_dep.found()
  conf_data.set('HAVE_LIBVMAF', 1)
endif
if exif_dep.found()
  conf_data.set('HAVE_LIBEXIF', 1)
endif

# Générer le fichier config.h
configure_file(
  output: 'config.h',
  configuration: conf_data
)

# Avertissements pour les dépendances manquantes
if not avformat_dep.found()
  warning('libavformat non trouvé. Les fonctionnalités avancées seront désactivées.')
endif
if not avcodec_dep.found()
  warning('libavcodec non trouvé. Les fonctionnalités avancées seront désactivées.')
endif
if not avutil_dep.found()
  warning('libavutil non trouvé. Les fonctionnalités avancées seront désactivées.')
endif
if not swscale_dep.found()
  warning('libswscale non trouvé. Les fonctionnalités avancées seront désactivées.')
endif
if not aom_dep.found()
  warning('libaom non trouvé. L\'encodage AV1 sera désactivé.')
endif
if not opus_dep.found()
  warning('libopus non trouvé. L\'encodage Opus sera désactivé.')
endif
if not vmaf_dep.found()
  warning('libvmaf non trouvé. L\'évaluation VMAF sera désactivée.')
endif
if not exif_dep.found()
  warning('libexif non trouvé. La préservation des métadonnées sera limitée.')
endif

# Inclure le répertoire de build pour que config.h soit trouvé
inc_build = include_directories('.')

# Configuration des sous-répertoires
subdir('src')