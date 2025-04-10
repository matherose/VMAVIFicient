project('FFmpeg', 'c',
  version : '7.1.1',
  license : 'LGPL-2.1+'
)

# Variables pour stocker les chemins des bibliothèques et des en-têtes
ffmpeg_libdir = meson.current_source_dir() / 'lib'
ffmpeg_incdir = meson.current_source_dir() / 'include'

# Exécuter la configuration et la compilation de FFmpeg si nécessaire
configure_script = find_program('configure', required : false)
if not configure_script.found()
  # Générer le script configure si besoin
  configure_gen = find_program('configure', dirs : [meson.current_source_dir()])
endif

# Déterminer les arguments de configuration pour FFmpeg
ffmpeg_conf_args = [
  '--disable-doc',
  '--disable-programs',
  '--enable-shared',
  '--enable-pic',
  '--enable-gpl',
  '--enable-version3',
  '--prefix=' + meson.current_build_dir() / 'install'
]

# Options supplémentaires pour les codecs
if get_option('buildtype') == 'debug'
  ffmpeg_conf_args += '--enable-debug=3'
endif

# Exécution du script configure et make
configure_cmd = run_command('sh', '-c', 'cd @0@ && ./configure @1@'.format(
  meson.current_source_dir(),
  ' '.join(ffmpeg_conf_args)
), check : false)

if configure_cmd.returncode() != 0
  warning('La configuration de FFmpeg a échoué. Utilisez les bibliothèques système si disponibles.')
else
  make_cmd = run_command('sh', '-c', 'cd @0@ && make -j@1@'.format(
    meson.current_source_dir(),
    host_machine.cpu_count()
  ), check : false)
  
  if make_cmd.returncode() != 0
    warning('La compilation de FFmpeg a échoué. Utilisez les bibliothèques système si disponibles.')
  else
    install_cmd = run_command('sh', '-c', 'cd @0@ && make install'.format(
      meson.current_source_dir()
    ), check : false)
  endif
endif

# Créer des dépendances pour chaque bibliothèque FFmpeg
avformat_lib = meson.get_compiler('c').find_library('avformat', 
  dirs : [ffmpeg_libdir], 
  required : false
)
avcodec_lib = meson.get_compiler('c').find_library('avcodec', 
  dirs : [ffmpeg_libdir], 
  required : false
)
avutil_lib = meson.get_compiler('c').find_library('avutil', 
  dirs : [ffmpeg_libdir], 
  required : false
)
swscale_lib = meson.get_compiler('c').find_library('swscale', 
  dirs : [ffmpeg_libdir], 
  required : false
)

# Déclarer les dépendances
avformat_dep = declare_dependency(
  include_directories : include_directories(ffmpeg_incdir),
  dependencies : avformat_lib
)

avcodec_dep = declare_dependency(
  include_directories : include_directories(ffmpeg_incdir),
  dependencies : avcodec_lib
)

avutil_dep = declare_dependency(
  include_directories : include_directories(ffmpeg_incdir),
  dependencies : avutil_lib
)

swscale_dep = declare_dependency(
  include_directories : include_directories(ffmpeg_incdir),
  dependencies : swscale_lib
)