audio_sources = [
  'audio.c',
]

audio_deps = []
if avcodec_dep.found()
  audio_deps += avcodec_dep
endif
if avutil_dep.found()
  audio_deps += avutil_dep
endif
if avformat_dep.found()
  audio_deps += avformat_dep
endif
if opus_dep.found()
  audio_deps += opus_dep
endif

audio_lib = static_library('audio',
  audio_sources,
  dependencies: audio_deps,
  include_directories: inc_build,
  install: false
)

audio_lib_dep = declare_dependency(
  link_with: audio_lib,
  include_directories: include_directories('.')
)