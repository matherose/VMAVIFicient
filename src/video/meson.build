video_sources = [
  'video.c',
]

video_deps = []
if avcodec_dep.found()
  video_deps += avcodec_dep
endif
if avutil_dep.found()
  video_deps += avutil_dep
endif
if avformat_dep.found()
  video_deps += avformat_dep
endif
if swscale_dep.found()
  video_deps += swscale_dep
endif
if swresample_dep.found()
  video_deps += swresample_dep
endif
if aom_dep.found()
  video_deps += aom_dep
endif

video_lib = static_library('video',
  video_sources,
  dependencies: video_deps,
  include_directories: inc_build,
  install: false
)

video_lib_dep = declare_dependency(
  link_with: video_lib,
  include_directories: include_directories('.')
)