project('FFmpeg-wrap', 'c',
  version : '7.1.1',
  license : 'LGPL-2.1+'
)

# Chemins pour les bibliothèques et en-têtes compilés
ffmpeg_source_dir = meson.current_source_dir() / '..' / 'FFmpeg'
ffmpeg_build_dir = meson.current_build_dir() / 'ffmpeg_build'
ffmpeg_prefix = ffmpeg_build_dir / 'install'
ffmpeg_lib_dir = ffmpeg_prefix / 'lib'
ffmpeg_inc_dir = ffmpeg_prefix / 'include'

# Vérifier si FFmpeg a déjà été compilé
is_built = run_command('test', '-d', ffmpeg_prefix, check: false).returncode() == 0

# Configurer et compiler FFmpeg si nécessaire
if not is_built
  message('Compilation de FFmpeg...')
  
  # Créer le répertoire de build
  run_command('mkdir', '-p', ffmpeg_build_dir, check: false)
  
  # Arguments de configuration pour FFmpeg
  ffmpeg_conf_args = [
    '--prefix=' + ffmpeg_prefix,
    '--enable-shared',
    '--enable-pic',
    '--enable-gpl',
    '--enable-version3',
    '--disable-doc',
    '--disable-programs',
    '--enable-avformat',
    '--enable-avcodec',
    '--enable-avutil',
    '--enable-swscale',
    '--enable-swresample',
    '--disable-vaapi',
    '--disable-vdpau',
    '--disable-videotoolbox',
  ]
  
  # Exécuter le script configure de FFmpeg
  configure_cmd = run_command('sh', '-c', 
    'cd @0@ && ./configure @1@'.format(
      ffmpeg_source_dir, ' '.join(ffmpeg_conf_args)
    ), 
    check: false
  )
  
  if configure_cmd.returncode() != 0
    error('Échec de configuration de FFmpeg: ' + configure_cmd.stderr())
  endif
  
  # Compiler FFmpeg
  compile_cmd = run_command('sh', '-c', 
    'cd @0@ && make -j@1@'.format(
      ffmpeg_source_dir, 
      host_machine.cpu_count()
    ), 
    check: false
  )
  
  if compile_cmd.returncode() != 0
    error('Échec de compilation de FFmpeg: ' + compile_cmd.stderr())
  endif
  
  # Installer FFmpeg dans le préfixe local
  install_cmd = run_command('sh', '-c', 
    'cd @0@ && make install'.format(ffmpeg_source_dir), 
    check: false
  )
  
  if install_cmd.returncode() != 0
    error('Échec d\'installation de FFmpeg: ' + install_cmd.stderr())
  endif
  
  message('FFmpeg compilé avec succès')
endif

# Créer des dépendances pour chaque bibliothèque FFmpeg
compiler = meson.get_compiler('c')

avformat_lib = compiler.find_library('avformat', dirs: [ffmpeg_lib_dir], required: true)
avcodec_lib = compiler.find_library('avcodec', dirs: [ffmpeg_lib_dir], required: true)
avutil_lib = compiler.find_library('avutil', dirs: [ffmpeg_lib_dir], required: true)
swscale_lib = compiler.find_library('swscale', dirs: [ffmpeg_lib_dir], required: true)
swresample_lib = compiler.find_library('swresample', dirs: [ffmpeg_lib_dir], required: true)

# Créer et exporter les dépendances
ffmpeg_inc = include_directories(ffmpeg_inc_dir)

avformat_dep = declare_dependency(
  include_directories: ffmpeg_inc,
  dependencies: avformat_lib,
  link_args: ['-L' + ffmpeg_lib_dir]
)

avcodec_dep = declare_dependency(
  include_directories: ffmpeg_inc,
  dependencies: avcodec_lib,
  link_args: ['-L' + ffmpeg_lib_dir]
)

avutil_dep = declare_dependency(
  include_directories: ffmpeg_inc,
  dependencies: avutil_lib,
  link_args: ['-L' + ffmpeg_lib_dir]
)

swscale_dep = declare_dependency(
  include_directories: ffmpeg_inc,
  dependencies: swscale_lib,
  link_args: ['-L' + ffmpeg_lib_dir]
)

swresample_dep = declare_dependency(
  include_directories: ffmpeg_inc,
  dependencies: swresample_lib,
  link_args: ['-L' + ffmpeg_lib_dir]
)