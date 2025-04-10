metadata_sources = [
  'metadata.c',
]

metadata_deps = []
if avcodec_dep.found()
  metadata_deps += avcodec_dep
endif
if avutil_dep.found()
  metadata_deps += avutil_dep
endif
if avformat_dep.found()
  metadata_deps += avformat_dep
endif
if exif_dep.found()
  metadata_deps += exif_dep
endif

metadata_lib = static_library('metadata',
  metadata_sources,
  dependencies: metadata_deps,
  include_directories: inc_build,
  install: false
)

metadata_lib_dep = declare_dependency(
  link_with: metadata_lib,
  include_directories: include_directories('.')
)