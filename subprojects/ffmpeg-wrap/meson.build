project('ffmpeg-wrap', 'c')

# Vérifier si nous devons compiler FFmpeg à partir des sources
avcodec_dep = dependency('libavcodec', required : false)
avutil_dep = dependency('libavutil', required : false)
avformat_dep = dependency('libavformat', required : false)
swscale_dep = dependency('libswscale', required : false)
swresample_dep = dependency('libswresample', required : false)

# Liste des dépendances FFmpeg à vérifier
deps_to_check = [
  ['libavcodec', avcodec_dep], 
  ['libavutil', avutil_dep],
  ['libavformat', avformat_dep],
  ['libswscale', swscale_dep],
  ['libswresample', swresample_dep]
]

use_system_ffmpeg = true
foreach dep : deps_to_check
  if not dep[1].found()
    use_system_ffmpeg = false
    break
  endif
endforeach

if not use_system_ffmpeg
  message('Compilation de FFmpeg à partir des sources')
  
  # Options de configuration
  ffmpeg_options = [
    '-Denable-shared=true',
    '-Denable-static=false',
    '-Ddisable-doc=true',
    '-Ddisable-programs=true',
    '-Denable-gpl=true',
    '-Denable-version3=true',
  ]
  
  # Compilation du sous-projet
  # Note: FFmpeg utilisera la commande configure directement avec ces options
  # via un script d'intégration dans le wrapper
  run_command('sh', '-c', 'cd ../FFmpeg && ./configure --enable-shared --disable-static --disable-doc --disable-programs --enable-gpl --enable-version3', check: true)
  
  # Créer des dépendances internes
  avcodec_dep = declare_dependency(
    dependencies: dependency('libavcodec', required: true, fallback: ['FFmpeg', 'avcodec_dep'])
  )
  
  avutil_dep = declare_dependency(
    dependencies: dependency('libavutil', required: true, fallback: ['FFmpeg', 'avutil_dep'])
  )
  
  avformat_dep = declare_dependency(
    dependencies: dependency('libavformat', required: true, fallback: ['FFmpeg', 'avformat_dep'])
  )
  
  swscale_dep = declare_dependency(
    dependencies: dependency('libswscale', required: true, fallback: ['FFmpeg', 'swscale_dep'])
  )
  
  swresample_dep = declare_dependency(
    dependencies: dependency('libswresample', required: true, fallback: ['FFmpeg', 'swresample_dep'])
  )
else
  message('Utilisation des bibliothèques système FFmpeg')
endif

# Fournir les dépendances au projet principal
meson.override_dependency('libavcodec', avcodec_dep)
meson.override_dependency('libavutil', avutil_dep)
meson.override_dependency('libavformat', avformat_dep)
meson.override_dependency('libswscale', swscale_dep)
meson.override_dependency('libswresample', swresample_dep)