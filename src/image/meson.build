image_sources = [
  'image.c',
]

image_deps = []
if avcodec_dep.found()
  image_deps += avcodec_dep
endif
if avutil_dep.found()
  image_deps += avutil_dep
endif
if avformat_dep.found()
  image_deps += avformat_dep
endif
if swscale_dep.found()
  image_deps += swscale_dep
endif
if aom_dep.found()
  image_deps += aom_dep
endif

image_lib = static_library('image',
  image_sources,
  dependencies: image_deps,
  include_directories: inc_build,
  install: false
)

image_dep = declare_dependency(
  link_with: image_lib,
  include_directories: include_directories('.')
)