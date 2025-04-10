vmaf_sources = [
  'vmaf.c',
]

vmaf_deps = []
if avformat_dep.found()
  vmaf_deps += avformat_dep
endif
if avcodec_dep.found()
  vmaf_deps += avcodec_dep
endif
if avutil_dep.found()
  vmaf_deps += avutil_dep
endif
if swscale_dep.found()
  vmaf_deps += swscale_dep
endif
if vmaf_dep.found()
  vmaf_deps += vmaf_dep
endif

vmaf_lib = static_library('vmaf',
  vmaf_sources,
  dependencies: vmaf_deps,
  include_directories: inc_build,
  install: false
)

vmaf_dep = declare_dependency(
  link_with: vmaf_lib,
  include_directories: include_directories('.')
)