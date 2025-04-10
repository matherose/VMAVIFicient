# Configuration des sous-répertoires
subdir('audio')
subdir('image') 
subdir('metadata')
subdir('video')
subdir('vmaf')

# Fichiers sources du projet
src_files = [
  'main.c',
]

# Inclure les répertoires
inc_dirs = include_directories(
  '.',
  'audio',
  'image',
  'metadata',
  'video',
  'vmaf'
)

# Liste des dépendances disponibles
project_deps = []
if avformat_dep.found()
  project_deps += avformat_dep
endif
if avcodec_dep.found()
  project_deps += avcodec_dep
endif
if avutil_dep.found()
  project_deps += avutil_dep
endif
if swscale_dep.found()
  project_deps += swscale_dep
endif
if aom_dep.found()
  project_deps += aom_dep
endif
if opus_dep.found()
  project_deps += opus_dep
endif
if vmaf_dep.found()
  project_deps += vmaf_dep
endif
if exif_dep.found()
  project_deps += exif_dep
endif

# Ajouter les dépendances internes des modules
project_deps += audio_dep
project_deps += video_dep
project_deps += image_dep
project_deps += metadata_dep
project_deps += vmaf_dep

# Exécutable principal
executable('vmavificient',
  src_files,
  dependencies : project_deps,
  include_directories : [inc_dirs, inc_build],
  install : true,
)