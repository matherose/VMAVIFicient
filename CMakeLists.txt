cmake_minimum_required(VERSION 3.24)

# ---------------------------------------------------------------------------
# Force LLVM/Clang toolchain (must be set before project()).
# Prefers Homebrew LLVM if present, then /usr/local/opt/llvm, then PATH clang.
# ---------------------------------------------------------------------------
if(NOT DEFINED CMAKE_C_COMPILER)
    find_program(LLVM_CLANG clang
        HINTS
            /opt/homebrew/opt/llvm/bin
            /usr/local/opt/llvm/bin
            ENV LLVM_PREFIX
        NO_DEFAULT_PATH
    )
    if(NOT LLVM_CLANG)
        find_program(LLVM_CLANG clang REQUIRED)
    endif()
    set(CMAKE_C_COMPILER "${LLVM_CLANG}" CACHE FILEPATH "C compiler" FORCE)
endif()
if(NOT DEFINED CMAKE_CXX_COMPILER)
    find_program(LLVM_CLANGXX clang++
        HINTS
            /opt/homebrew/opt/llvm/bin
            /usr/local/opt/llvm/bin
            ENV LLVM_PREFIX
        NO_DEFAULT_PATH
    )
    if(NOT LLVM_CLANGXX)
        find_program(LLVM_CLANGXX clang++ REQUIRED)
    endif()
    set(CMAKE_CXX_COMPILER "${LLVM_CLANGXX}" CACHE FILEPATH "C++ compiler" FORCE)
endif()

# C++ is enabled purely so CMake picks the C++ driver when linking the final
# executable (tesseract is C++ and drags in libc++/libstdc++).
project(vmavificient VERSION 2.3.0 LANGUAGES C CXX)

# Enforce Ninja generator for the top-level build.
if(NOT CMAKE_GENERATOR STREQUAL "Ninja")
    message(FATAL_ERROR
        "vmavificient requires the Ninja generator, got '${CMAKE_GENERATOR}'.\n"
        "Reconfigure with:  cmake -G Ninja -B build")
endif()

if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang" AND NOT CMAKE_C_COMPILER_ID STREQUAL "AppleClang")
    message(FATAL_ERROR "vmavificient requires LLVM/Clang, got ${CMAKE_C_COMPILER_ID}")
endif()
message(STATUS "C compiler: ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION} (${CMAKE_C_COMPILER})")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Default to Release if nothing was specified.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Optimization flags (Clang-specific).
#   * -O3 + LTO for release
#   * -march=native so local builds exploit the host CPU (AVX2/NEON)
#   * -fvisibility=hidden to shrink the symbol table
#   * -ffp-contract=fast for cheap FMA in the grain/crop analysis loops
# ---------------------------------------------------------------------------
include(CheckIPOSupported)
check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_err)
if(_ipo_ok)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    message(STATUS "LTO: enabled for Release")
else()
    message(STATUS "LTO: unavailable (${_ipo_err})")
endif()

set(CMAKE_C_FLAGS_RELEASE "-O3 -DNDEBUG")

# Project-wide optimization flags, applied only to *our* target so that
# third-party deps built in-tree (cjson, etc.) don't choke on -Werror +
# -Wunused-command-line-argument when an option doesn't apply.
set(VMAV_OPT_FLAGS
    -fvisibility=hidden
    -ffp-contract=fast
    -fno-math-errno
    -pipe
    -Wall
    -Wextra
    -Wno-unused-parameter
    -Wno-unused-command-line-argument
)
if(NOT CMAKE_CROSSCOMPILING)
    list(APPEND VMAV_OPT_FLAGS -march=native -mtune=native)
endif()
if(NOT APPLE)
    list(APPEND VMAV_OPT_FLAGS -ffunction-sections -fdata-sections)
endif()

# Optional ASAN/UBSAN build for CI and local debugging.
#   cmake -G Ninja -B build-asan -DVMAV_SANITIZE=ON && cmake --build build-asan
# Disables -fvisibility=hidden so sanitizer interceptors resolve cleanly,
# and forces -O1 to keep stack frames intact for useful diagnostics.
option(VMAV_SANITIZE "Build with -fsanitize=address,undefined" OFF)
if(VMAV_SANITIZE)
    list(REMOVE_ITEM VMAV_OPT_FLAGS -fvisibility=hidden)
    list(APPEND VMAV_OPT_FLAGS
        -O1
        -g
        -fno-omit-frame-pointer
        -fsanitize=address,undefined
        -fno-sanitize-recover=all
    )
    set(VMAV_SANITIZE_LINK -fsanitize=address,undefined)
    message(STATUS "Sanitizers: address, undefined")
else()
    set(VMAV_SANITIZE_LINK "")
endif()

# Linker optimizations: dead-strip + use lld if available.
set(VMAV_LINK_FLAGS "")
# lld is faster than the GNU linker on Linux, but Homebrew's lld crashes with
# LTO on macOS arm64 ("Unsupported stack probing method"), so only use it on
# non-Apple platforms.
if(NOT APPLE)
    find_program(LLVM_LLD ld.lld)
    if(LLVM_LLD)
        list(APPEND VMAV_LINK_FLAGS -fuse-ld=lld)
        message(STATUS "Linker: lld")
    endif()
endif()
if(APPLE)
    list(APPEND VMAV_LINK_FLAGS -Wl,-dead_strip)
else()
    list(APPEND VMAV_LINK_FLAGS -Wl,--gc-sections -Wl,-O2)
endif()
if(VMAV_SANITIZE)
    # Sanitizer runtimes must be linked into the final executable too.
    list(APPEND VMAV_LINK_FLAGS ${VMAV_SANITIZE_LINK})
endif()

include(ExternalProject)
include(FetchContent)
include(ProcessorCount)

ProcessorCount(NPROC)
if(NPROC EQUAL 0)
    set(NPROC 4)
endif()

# ---------------------------------------------------------------------------
# Prerequisites
# ---------------------------------------------------------------------------
# cargo + cargo-c are needed unconditionally because libhdr10plus and
# grav1synth stay vendored on every platform — Debian doesn't package
# libhdr10plus, and grav1synth ships only as a CLI binary we invoke as
# a subprocess. SVT-AV1-HDR also stays vendored because the juliobbv-p
# fork (DV/HDR10+ passthrough + --noise) isn't in any distro repo.
find_program(CARGO cargo REQUIRED)
find_program(CARGO_CINSTALL cargo-cinstall)
if(NOT CARGO_CINSTALL)
    message(FATAL_ERROR
        "cargo-c is required but not found.\n"
        "Install it with:  cargo install cargo-c")
endif()
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Dependency mode toggle
# ---------------------------------------------------------------------------
# ON  (default): use system-provided shared libraries via pkg-config /
#                find_package. Fast builds, distro-friendly (.deb), and the
#                only sane mode once the project is packaged.
# OFF:           reproduce the legacy v1.0.x build that vendors and statically
#                links every dependency. Used by the GitHub-release static
#                binary so users without dev headers can still run vmavificient.
#
# Three deps stay vendored regardless of the toggle (see the always-vendored
# section below): SVT-AV1-HDR, libhdr10plus, grav1synth.
option(VMAV_USE_SYSTEM_DEPS
    "Use system-provided dependencies via pkg-config (preferred). Set OFF to vendor everything as static libs."
    ON)

# ===========================================================================
# Always-vendored: SVT-AV1-HDR + libhdr10plus + grav1synth
# ===========================================================================
# These three never come from the system:
#   - SVT-AV1-HDR (juliobbv-p fork) — needs the HDR fork's DV/HDR10+
#     passthrough and --noise (4.1.0+); Debian only ships upstream SVT-AV1.
#   - libhdr10plus — not packaged in Debian; Homebrew ships the binary
#     only, not the cargo-c lib.
#   - grav1synth — only a CLI binary; we invoke it as a subprocess from
#     src/media_analysis.c.
# ---------------------------------------------------------------------------
set(DEPS_PREFIX        "${CMAKE_BINARY_DIR}/deps")
set(HDR10_INSTALL      "${DEPS_PREFIX}/libhdr10plus")
set(SVTAV1_INSTALL     "${DEPS_PREFIX}/svtav1")
set(GRAV1SYNTH_INSTALL "${DEPS_PREFIX}/grav1synth")
file(MAKE_DIRECTORY "${HDR10_INSTALL}/include")
file(MAKE_DIRECTORY "${SVTAV1_INSTALL}/include")
file(MAKE_DIRECTORY "${SVTAV1_INSTALL}/include/svt-av1")

set(LIBHDR10PLUS_GIT_TAG "ec9613eb257bd92130a446e2ce155685a243bd1d") # libhdr10plus-2.1.5
set(LIBHDR10PLUS_VERSION "2.1.5")
set(SVTAV1_GIT_TAG       "8d13912d5e0e6b611e3d482d880b41a7f2e5cd52") # v4.1.0
set(GRAV1SYNTH_GIT_REV   "1044228cd411672b565e5762a9b3597f4dd163b0")

ExternalProject_Add(ep_svtav1
    GIT_REPOSITORY  https://github.com/juliobbv-p/svt-av1-hdr.git
    GIT_TAG         ${SVTAV1_GIT_TAG}
    GIT_SHALLOW     TRUE
    PREFIX          "${DEPS_PREFIX}/build/svtav1"
    CMAKE_GENERATOR "Ninja"
    CMAKE_ARGS
        -DCMAKE_INSTALL_PREFIX=${SVTAV1_INSTALL}
        -DCMAKE_BUILD_TYPE=Release
        -DBUILD_SHARED_LIBS=OFF
        -DBUILD_TESTING=OFF
        -DBUILD_APPS=OFF
    BUILD_COMMAND   cmake --build <BINARY_DIR> --parallel ${NPROC}
    BUILD_BYPRODUCTS "${SVTAV1_INSTALL}/lib/libSvtAv1Enc.a"
    LOG_CONFIGURE   TRUE
    LOG_BUILD       TRUE
    LOG_INSTALL     TRUE
    LOG_OUTPUT_ON_FAILURE TRUE
)

ExternalProject_Add(ep_libhdr10plus
    GIT_REPOSITORY  https://github.com/quietvoid/hdr10plus_tool.git
    GIT_TAG         ${LIBHDR10PLUS_GIT_TAG}
    GIT_SHALLOW     TRUE
    PREFIX          "${DEPS_PREFIX}/build/libhdr10plus"
    CONFIGURE_COMMAND ""
    BUILD_COMMAND     ""
    INSTALL_COMMAND
        ${CARGO} cinstall
            --manifest-path=<SOURCE_DIR>/hdr10plus/Cargo.toml
            --release
            --prefix=${HDR10_INSTALL}
            --libdir=${HDR10_INSTALL}/lib
            --includedir=${HDR10_INSTALL}/include
    BUILD_IN_SOURCE TRUE
    BUILD_BYPRODUCTS "${HDR10_INSTALL}/lib/libhdr10plus-rs.a"
    LOG_INSTALL     TRUE
    LOG_OUTPUT_ON_FAILURE TRUE
)

# grav1synth builds its OWN FFmpeg, statically, via the ffmpeg_static feature.
# It does not use ours and does not use the host's.
#
# Why it has to be this way. grav1synth is version-locked to FFmpeg 8.1
# (ffmpeg-the-third 5.0.0+ffmpeg-8.1, held by its Cargo.lock under --locked);
# against FFmpeg 9 headers it fails to compile, because AVCodec lost
# pix_fmts / ch_layouts / supported_framerates. Steering it at an 8.x tree via
# PKG_CONFIG_PATH does not work on Apple Silicon: ffmpeg-sys-the-third's
# build.rs unconditionally *prepends* /opt/homebrew/lib/pkgconfig ahead of the
# caller's PKG_CONFIG_PATH. Its guard is
#     if !pc_path.contains("/opt/homebrew/lib/pkgconfig/")
# and real pc_path entries carry no trailing slash, so the guard never holds
# and the override always fires. Setting PKG_CONFIG to Homebrew's own binary
# does not help for the same reason (verified both ways).
#
# So the only reliable route is to take pkg-config out of the picture: the
# `build` feature makes ffmpeg-sys compile a private FFmpeg and skip probing
# entirely. software-resampling is required alongside it — grav1synth requests
# only `format`, but the private FFmpeg's opus decoder references swr_*, and
# without swresample on the link line the binary fails to link.
#
# Upside beyond unbreaking the build: the helper no longer depends on host
# FFmpeg at all, so our own FFmpeg pin is free to move independently of it.
set(GRAV1SYNTH_BIN "${GRAV1SYNTH_INSTALL}/bin/grav1synth")
ExternalProject_Add(ep_grav1synth
    DOWNLOAD_COMMAND  ""
    CONFIGURE_COMMAND ""
    BUILD_COMMAND     ""
    PREFIX            "${DEPS_PREFIX}/build/grav1synth"
    INSTALL_COMMAND
        ${CARGO} install
            --git https://github.com/rust-av/grav1synth.git
            --rev ${GRAV1SYNTH_GIT_REV}
            --locked
            --features ffmpeg_static,ffmpeg/software-resampling
            --root ${GRAV1SYNTH_INSTALL}
            grav1synth
    BUILD_BYPRODUCTS "${GRAV1SYNTH_BIN}"
    LOG_INSTALL     TRUE
    LOG_OUTPUT_ON_FAILURE TRUE
)

# IMPORTED targets — always-vendored libs.
add_library(SvtAv1::Enc STATIC IMPORTED GLOBAL)
set_target_properties(SvtAv1::Enc PROPERTIES
    IMPORTED_LOCATION             "${SVTAV1_INSTALL}/lib/libSvtAv1Enc.a"
    INTERFACE_INCLUDE_DIRECTORIES "${SVTAV1_INSTALL}/include/svt-av1"
)
add_dependencies(SvtAv1::Enc ep_svtav1)

add_library(Hdr10Plus::hdr10plus STATIC IMPORTED GLOBAL)
set_target_properties(Hdr10Plus::hdr10plus PROPERTIES
    IMPORTED_LOCATION             "${HDR10_INSTALL}/lib/libhdr10plus-rs.a"
    INTERFACE_INCLUDE_DIRECTORIES "${HDR10_INSTALL}/include"
)
add_dependencies(Hdr10Plus::hdr10plus ep_libhdr10plus)

if(VMAV_USE_SYSTEM_DEPS)
    # =======================================================================
    # System-deps mode: pkg-config / find_package
    # =======================================================================
    # On macOS install with:
    #   brew install ffmpeg opus dovi_tool tesseract jpeg-turbo libpng \
    #       libtiff cjson openssl@3 pkg-config
    #
    # Linux support is deferred — the dependency surface (specifically
    # libdovi-dev) only landed in Debian trixie / Ubuntu 25.04+, and we
    # don't want to ship a half-working Linux story. Revisit when it's
    # in a stable distro most users actually run.
    # -----------------------------------------------------------------------
    # On macOS with both Homebrew and MacPorts installed, CMake's default
    # PATH search picks up MacPorts' pkg-config first, which only knows
    # about /opt/local libraries.  Explicitly prefer Homebrew's copy so
    # that `cmake -G Ninja -B build` works without exporting PKG_CONFIG_PATH.
    if(APPLE)
      find_program(_brew_pkg_config pkg-config
          PATHS /opt/homebrew/bin /usr/local/bin
          NO_DEFAULT_PATH)
      if(_brew_pkg_config)
        set(PKG_CONFIG_EXECUTABLE "${_brew_pkg_config}" CACHE FILEPATH
            "Homebrew pkg-config (preferred over MacPorts)" FORCE)
        set(ENV{PKG_CONFIG_PATH}
            "/opt/homebrew/lib/pkgconfig\
:/opt/homebrew/opt/openssl@3/lib/pkgconfig\
:/opt/homebrew/opt/jpeg-turbo/lib/pkgconfig\
:$ENV{PKG_CONFIG_PATH}")
        if(NOT DEFINED CACHE{OPENSSL_ROOT_DIR})
          set(OPENSSL_ROOT_DIR "/opt/homebrew/opt/openssl@3" CACHE PATH
              "Homebrew OpenSSL root" FORCE)
        endif()
      endif()
    endif()

    find_package(PkgConfig REQUIRED)
    find_package(OpenSSL REQUIRED)
    find_package(ZLIB REQUIRED)

    # FFmpeg — bundle six modules into one IMPORTED target so the link line
    # gets a single dependency to chase instead of six. Floors are the
    # FFmpeg 8.0 module majors (av_opt_set_array + buffersink
    # "pixel_formats" require ≥ 7.1; we standardize on 8+).
    #
    # These stay at the 8.x majors deliberately, even though the vendored
    # build now pins FFmpeg 9. Our sources build clean against both: every
    # libav* entry point we call still exists in 9, and none of the struct
    # fields 9 dropped from AVCodec (pix_fmts, ch_layouts,
    # supported_framerates, sample_fmts, supported_samplerates) is one we
    # touch. Raising the floors to the 9.x majors would therefore lock out
    # FFmpeg 8 hosts for no gain. Tested range is 8.1–9.0.
    pkg_check_modules(VMAV_LIBAV REQUIRED IMPORTED_TARGET
        libavformat>=62
        libavcodec>=62
        libavutil>=60
        libavfilter>=11
        libswscale>=9
        libswresample>=6
    )
    pkg_check_modules(VMAV_OPUS      REQUIRED IMPORTED_TARGET opus)
    pkg_check_modules(VMAV_DOVI      REQUIRED IMPORTED_TARGET dovi)
    pkg_check_modules(VMAV_TESSERACT REQUIRED IMPORTED_TARGET tesseract)
    pkg_check_modules(VMAV_LEPTONICA REQUIRED IMPORTED_TARGET lept)
    pkg_check_modules(VMAV_LIBPNG    REQUIRED IMPORTED_TARGET libpng)
    pkg_check_modules(VMAV_LIBJPEG   REQUIRED IMPORTED_TARGET libjpeg)
    pkg_check_modules(VMAV_LIBTIFF   REQUIRED IMPORTED_TARGET libtiff-4)
    pkg_check_modules(VMAV_LIBCURL   REQUIRED IMPORTED_TARGET libcurl)
    pkg_check_modules(VMAV_CJSON     REQUIRED IMPORTED_TARGET libcjson)
    pkg_check_modules(VMAV_LIBVMAF   REQUIRED IMPORTED_TARGET libvmaf)

    message(STATUS "Dependencies: system (pkg-config / find_package)")
else()
    # =======================================================================
    # Vendored mode: build everything from source as static libs
    # =======================================================================
    set(FF_INSTALL      "${DEPS_PREFIX}/ffmpeg")
    set(OPUS_INSTALL    "${DEPS_PREFIX}/opus")
    set(DOVI_INSTALL    "${DEPS_PREFIX}/libdovi")
    set(ZLIB_INSTALL    "${DEPS_PREFIX}/zlib")
    set(OPENSSL_INSTALL "${DEPS_PREFIX}/openssl")
    set(CURL_INSTALL    "${DEPS_PREFIX}/curl")
    set(PNG_INSTALL     "${DEPS_PREFIX}/libpng")
    set(JPEG_INSTALL    "${DEPS_PREFIX}/libjpeg")
    set(TIFF_INSTALL    "${DEPS_PREFIX}/libtiff")
    set(LEPT_INSTALL    "${DEPS_PREFIX}/leptonica")
    set(TESS_INSTALL    "${DEPS_PREFIX}/tesseract")
    set(VMAF_INSTALL    "${DEPS_PREFIX}/libvmaf")
    set(DAV1D_INSTALL   "${DEPS_PREFIX}/dav1d")

    # CMake validates INTERFACE_INCLUDE_DIRECTORIES at configure time,
    # so the directories must already exist even though headers arrive later.
    file(MAKE_DIRECTORY "${FF_INSTALL}/include")
    file(MAKE_DIRECTORY "${OPUS_INSTALL}/include")
    file(MAKE_DIRECTORY "${DOVI_INSTALL}/include")
    file(MAKE_DIRECTORY "${ZLIB_INSTALL}/include")
    file(MAKE_DIRECTORY "${OPENSSL_INSTALL}/include")
    file(MAKE_DIRECTORY "${CURL_INSTALL}/include")
    file(MAKE_DIRECTORY "${PNG_INSTALL}/include")
    file(MAKE_DIRECTORY "${JPEG_INSTALL}/include")
    file(MAKE_DIRECTORY "${TIFF_INSTALL}/include")
    file(MAKE_DIRECTORY "${LEPT_INSTALL}/include")
    file(MAKE_DIRECTORY "${LEPT_INSTALL}/include/leptonica")
    file(MAKE_DIRECTORY "${TESS_INSTALL}/include")
    file(MAKE_DIRECTORY "${TESS_INSTALL}/include/tesseract")
    file(MAKE_DIRECTORY "${VMAF_INSTALL}/include")
    file(MAKE_DIRECTORY "${DAV1D_INSTALL}/include")

    set(FFMPEG_GIT_TAG    "bf1b838f2ab88b4f8fd83443325c782ea0e0f7fa") # n9.0.1
    set(OPUS_GIT_TAG      "22244de5a79bd1d6d623c32e72bf1954b56235be") # v1.6.1
    set(LIBDOVI_GIT_TAG   "d1abe0e27ff2c7ab3339614d06db9f8a058af6b2") # libdovi-3.4.0
    set(LIBDOVI_VERSION   "3.4.0")
    set(CJSON_GIT_TAG     "c859b25da02955fef659d658b8f324b5cde87be3") # v1.7.19
    set(ZLIB_GIT_TAG      "da607da739fa6047df13e66a2af6b8bec7c2a498") # v1.3.2
    set(OPENSSL_GIT_TAG   "f4dc4d58b48d346a8270183f89acf826d459b0ca") # openssl-3.5.8
    set(CURL_GIT_TAG      "68720b4837284335b2d63cb358f8f6ce65f5bc55") # curl-8_21_0
    set(LIBPNG_GIT_TAG    "3061454d980de7d53608f594194cfac722721d2a") # v1.6.58
    set(LIBJPEG_GIT_TAG   "c85e6b905bf237038faa936dab160ebfc5da0344") # 3.2.0
    set(LIBTIFF_GIT_TAG   "d01a94be176f5f6a87f7ee1c0b32e65416aa2b4d") # v4.7.2
    # Held at 1.86.0 on purpose — do not bump to 1.87.0. 1.87.0 exports a
    # broken CMake package on macOS: LeptonicaTargets.cmake lists
    # $<LINK_ONLY:ZLIB::ZLIB> and $<LINK_ONLY:JPEG::JPEG> in its link
    # interface, but LeptonicaConfig.cmake never find_dependency()s either,
    # so every consumer fails with "target ZLIB::ZLIB not found" — tesseract
    # dies in check_leptonica_tiff_support's try_compile. 1.87.0 also
    # misdetects our static libpng as a macOS framework (png.framework
    # instead of libpng16.a). 1.86.0 exports plain .a paths and is fine.
    set(LEPTONICA_GIT_TAG "dbb48b0fd0e10e943ecd9bab7439701842894733") # 1.86.0
    # 5.5.3 builds clean with DISABLED_LEGACY_ENGINE=ON (verified by trial
    # build against leptonica 1.86.0 with this exact flag set). The previous
    # note here blamed 5.5.2's cmake/SourceLists.cmake for making REMOVE_ITEM
    # a no-op via a relative-vs-absolute path mismatch; that is not what the
    # released tarballs contain — TESSERACT_SRC_CORE and TESSERACT_SRC_LEGACY
    # both use identical relative paths, so REMOVE_ITEM does match.
    set(TESSERACT_GIT_TAG "db0ec62f81b0737fbbe184d8fea40af5738f8eef") # 5.5.3
    set(LIBVMAF_GIT_TAG   "3f9e02af258a5c0e30124fc585a3c3af90126dee") # v3.2.0
    set(DAV1D_GIT_TAG     "54706fc6bc0cdecab7e9593974a4039cc038fca7") # 1.5.4

    # ---- cJSON via FetchContent ----------------------------------------
    FetchContent_Declare(cjson
        GIT_REPOSITORY  https://github.com/DaveGamble/cJSON.git
        GIT_TAG         ${CJSON_GIT_TAG}
        GIT_SHALLOW     TRUE
    )
    set(ENABLE_CJSON_TEST OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_AND_STATIC_LIBS OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    # cJSON 1.7.18 still declares cmake_minimum_required(VERSION 2.x), which
    # modern CMake refuses. Bump the effective minimum for the nested project.
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
    FetchContent_MakeAvailable(cjson)
    unset(CMAKE_POLICY_VERSION_MINIMUM CACHE)

    # ---- zlib ----------------------------------------------------------
    ExternalProject_Add(ep_zlib
        GIT_REPOSITORY  https://github.com/madler/zlib.git
        GIT_TAG         ${ZLIB_GIT_TAG}
        GIT_SHALLOW     TRUE
        PREFIX          "${DEPS_PREFIX}/build/zlib"
        CMAKE_GENERATOR "Ninja"
        CMAKE_ARGS
            -DCMAKE_INSTALL_PREFIX=${ZLIB_INSTALL}
            -DCMAKE_INSTALL_LIBDIR=lib
            -DCMAKE_BUILD_TYPE=Release
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON
            -DZLIB_BUILD_EXAMPLES=OFF
            -DBUILD_SHARED_LIBS=OFF
        INSTALL_COMMAND cmake --install <BINARY_DIR>
        # zlib 1.3.1 unconditionally declares the shared `zlib` target via
        # add_library(... SHARED ...), so BUILD_SHARED_LIBS=OFF doesn't
        # suppress it. Delete the shared library after install so downstream
        # deps link static.
        COMMAND ${CMAKE_COMMAND} -E rm -f
            "${ZLIB_INSTALL}/lib/libz.so"
            "${ZLIB_INSTALL}/lib/libz.so.1"
            "${ZLIB_INSTALL}/lib/libz.so.1.3.1"
            "${ZLIB_INSTALL}/lib/libz.dylib"
            "${ZLIB_INSTALL}/lib/libz.1.dylib"
            "${ZLIB_INSTALL}/lib/libz.1.3.1.dylib"
        BUILD_BYPRODUCTS "${ZLIB_INSTALL}/lib/libz.a"
        LOG_CONFIGURE   TRUE
        LOG_BUILD       TRUE
        LOG_INSTALL     TRUE
        LOG_OUTPUT_ON_FAILURE TRUE
    )

    # ---- OpenSSL -------------------------------------------------------
    ExternalProject_Add(ep_openssl
        GIT_REPOSITORY  https://github.com/openssl/openssl.git
        GIT_TAG         ${OPENSSL_GIT_TAG}
        GIT_SHALLOW     TRUE
        PREFIX          "${DEPS_PREFIX}/build/openssl"
        DEPENDS         ep_zlib
        CONFIGURE_COMMAND
            ${CMAKE_COMMAND} -E env
                "CC=${CMAKE_C_COMPILER}"
            <SOURCE_DIR>/config
                --prefix=${OPENSSL_INSTALL}
                --openssldir=${OPENSSL_INSTALL}/ssl
                --libdir=lib
                no-shared
                no-tests
                no-docs
                no-apps
                no-legacy
                zlib
                --with-zlib-include=${ZLIB_INSTALL}/include
                --with-zlib-lib=${ZLIB_INSTALL}/lib
        BUILD_COMMAND   make -j${NPROC}
        INSTALL_COMMAND make install_sw
        BUILD_IN_SOURCE TRUE
        BUILD_BYPRODUCTS
            "${OPENSSL_INSTALL}/lib/libssl.a"
            "${OPENSSL_INSTALL}/lib/libcrypto.a"
        LOG_CONFIGURE   TRUE
        LOG_BUILD       TRUE
        LOG_INSTALL     TRUE
        LOG_OUTPUT_ON_FAILURE TRUE
    )

    # ---- libcurl -------------------------------------------------------
    ExternalProject_Add(ep_curl
        GIT_REPOSITORY  https://github.com/curl/curl.git
        GIT_TAG         ${CURL_GIT_TAG}
        GIT_SHALLOW     TRUE
        PREFIX          "${DEPS_PREFIX}/build/curl"
        DEPENDS         ep_openssl ep_zlib
        CMAKE_GENERATOR "Ninja"
        CMAKE_ARGS
            -DCMAKE_INSTALL_PREFIX=${CURL_INSTALL}
            -DCMAKE_INSTALL_LIBDIR=lib
            -DCMAKE_BUILD_TYPE=Release
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON
            -DCMAKE_PREFIX_PATH=${OPENSSL_INSTALL}\;${ZLIB_INSTALL}
            -DBUILD_SHARED_LIBS=OFF
            -DBUILD_STATIC_LIBS=ON
            -DBUILD_CURL_EXE=OFF
            -DBUILD_TESTING=OFF
            -DCURL_USE_OPENSSL=ON
            -DCURL_USE_LIBSSH2=OFF
            -DCURL_USE_LIBPSL=OFF
            -DCURL_USE_LIBSSH=OFF
            -DCURL_USE_GSSAPI=OFF
            -DUSE_LIBIDN2=OFF
            -DCURL_BROTLI=OFF
            -DCURL_ZSTD=OFF
            -DUSE_NGHTTP2=OFF
            -DUSE_NGTCP2=OFF
            -DUSE_QUICHE=OFF
            -DUSE_LIBRTMP=OFF
            -DCURL_ZLIB=ON
            -DHTTP_ONLY=ON
            -DCURL_DISABLE_LDAP=ON
            -DCURL_DISABLE_LDAPS=ON
            -DCURL_DISABLE_DICT=ON
            -DCURL_DISABLE_FILE=ON
            -DCURL_DISABLE_FTP=ON
            -DCURL_DISABLE_GOPHER=ON
            -DCURL_DISABLE_IMAP=ON
            -DCURL_DISABLE_MQTT=ON
            -DCURL_DISABLE_POP3=ON
            -DCURL_DISABLE_RTSP=ON
            -DCURL_DISABLE_SMB=ON
            -DCURL_DISABLE_SMTP=ON
            -DCURL_DISABLE_TELNET=ON
            -DCURL_DISABLE_TFTP=ON
            -DOPENSSL_ROOT_DIR=${OPENSSL_INSTALL}
            -DOPENSSL_USE_STATIC_LIBS=ON
            -DZLIB_ROOT=${ZLIB_INSTALL}
            -DZLIB_USE_STATIC_LIBS=ON
        BUILD_COMMAND   cmake --build <BINARY_DIR> --parallel ${NPROC}
        INSTALL_COMMAND cmake --install <BINARY_DIR>
        BUILD_BYPRODUCTS "${CURL_INSTALL}/lib/libcurl.a"
        LOG_CONFIGURE   TRUE
        LOG_BUILD       TRUE
        LOG_INSTALL     TRUE
        LOG_OUTPUT_ON_FAILURE TRUE
    )

    # ---- libpng --------------------------------------------------------
    ExternalProject_Add(ep_libpng
        GIT_REPOSITORY  https://github.com/pnggroup/libpng.git
        GIT_TAG         ${LIBPNG_GIT_TAG}
        GIT_SHALLOW     TRUE
        PREFIX          "${DEPS_PREFIX}/build/libpng"
        DEPENDS         ep_zlib
        CMAKE_GENERATOR "Ninja"
        CMAKE_ARGS
            -DCMAKE_INSTALL_PREFIX=${PNG_INSTALL}
            -DCMAKE_INSTALL_LIBDIR=lib
            -DCMAKE_BUILD_TYPE=Release
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON
            -DBUILD_SHARED_LIBS=OFF
            -DPNG_SHARED=OFF
            -DPNG_STATIC=ON
            -DPNG_TESTS=OFF
            -DPNG_TOOLS=OFF
            -DZLIB_ROOT=${ZLIB_INSTALL}
            -DZLIB_USE_STATIC_LIBS=ON
        BUILD_COMMAND   cmake --build <BINARY_DIR> --parallel ${NPROC}
        INSTALL_COMMAND cmake --install <BINARY_DIR>
        BUILD_BYPRODUCTS "${PNG_INSTALL}/lib/libpng16.a" "${PNG_INSTALL}/lib/libpng.a"
        LOG_CONFIGURE   TRUE
        LOG_BUILD       TRUE
        LOG_INSTALL     TRUE
        LOG_OUTPUT_ON_FAILURE TRUE
    )

    # ---- libjpeg-turbo -------------------------------------------------
    ExternalProject_Add(ep_libjpeg
        GIT_REPOSITORY  https://github.com/libjpeg-turbo/libjpeg-turbo.git
        GIT_TAG         ${LIBJPEG_GIT_TAG}
        GIT_SHALLOW     TRUE
        PREFIX          "${DEPS_PREFIX}/build/libjpeg"
        CMAKE_GENERATOR "Ninja"
        CMAKE_ARGS
            -DCMAKE_INSTALL_PREFIX=${JPEG_INSTALL}
            -DCMAKE_INSTALL_LIBDIR=lib
            -DCMAKE_BUILD_TYPE=Release
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON
            -DENABLE_SHARED=OFF
            -DENABLE_STATIC=ON
            -DWITH_TURBOJPEG=OFF
        BUILD_COMMAND   cmake --build <BINARY_DIR> --parallel ${NPROC}
        INSTALL_COMMAND cmake --install <BINARY_DIR>
        BUILD_BYPRODUCTS "${JPEG_INSTALL}/lib/libjpeg.a"
        LOG_CONFIGURE   TRUE
        LOG_BUILD       TRUE
        LOG_INSTALL     TRUE
        LOG_OUTPUT_ON_FAILURE TRUE
    )

    # ---- libtiff -------------------------------------------------------
    ExternalProject_Add(ep_libtiff
        GIT_REPOSITORY  https://gitlab.com/libtiff/libtiff.git
        GIT_TAG         ${LIBTIFF_GIT_TAG}
        GIT_SHALLOW     TRUE
        PREFIX          "${DEPS_PREFIX}/build/libtiff"
        DEPENDS         ep_zlib ep_libjpeg
        CMAKE_GENERATOR "Ninja"
        CMAKE_ARGS
            -DCMAKE_INSTALL_PREFIX=${TIFF_INSTALL}
            -DCMAKE_INSTALL_LIBDIR=lib
            -DCMAKE_BUILD_TYPE=Release
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON
            -DCMAKE_PREFIX_PATH=${JPEG_INSTALL}\;${ZLIB_INSTALL}
            -DBUILD_SHARED_LIBS=OFF
            -Dtiff-tools=OFF
            -Dtiff-tests=OFF
            -Dtiff-contrib=OFF
            -Dtiff-docs=OFF
            -Dtiff-deprecated=OFF
            -Dtiff-install=ON
            -Djpeg=ON
            -DJPEG_INCLUDE_DIR=${JPEG_INSTALL}/include
            -DJPEG_LIBRARY=${JPEG_INSTALL}/lib/libjpeg.a
            -Djbig=OFF
            -Dlerc=OFF
            -Dlzma=OFF
            -Dwebp=OFF
            -Dzstd=OFF
            -Dlibdeflate=OFF
            -Dzlib=ON
            -DZLIB_ROOT=${ZLIB_INSTALL}
            -DZLIB_USE_STATIC_LIBS=ON
        BUILD_COMMAND   cmake --build <BINARY_DIR> --parallel ${NPROC}
        INSTALL_COMMAND cmake --install <BINARY_DIR>
        BUILD_BYPRODUCTS "${TIFF_INSTALL}/lib/libtiff.a"
        LOG_CONFIGURE   TRUE
        LOG_BUILD       TRUE
        LOG_INSTALL     TRUE
        LOG_OUTPUT_ON_FAILURE TRUE
    )

    # ---- Leptonica ----------------------------------------------------
    ExternalProject_Add(ep_leptonica
        GIT_REPOSITORY  https://github.com/DanBloomberg/leptonica.git
        GIT_TAG         ${LEPTONICA_GIT_TAG}
        GIT_SHALLOW     TRUE
        PREFIX          "${DEPS_PREFIX}/build/leptonica"
        DEPENDS         ep_zlib ep_libpng ep_libjpeg ep_libtiff
        CMAKE_GENERATOR "Ninja"
        CMAKE_ARGS
            -DCMAKE_INSTALL_PREFIX=${LEPT_INSTALL}
            -DCMAKE_INSTALL_LIBDIR=lib
            -DCMAKE_BUILD_TYPE=Release
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON
            -DCMAKE_PREFIX_PATH=${ZLIB_INSTALL}\;${PNG_INSTALL}\;${JPEG_INSTALL}\;${TIFF_INSTALL}
            -DBUILD_SHARED_LIBS=OFF
            -DBUILD_PROG=OFF
            -DSW_BUILD=OFF
            -DENABLE_ZLIB=ON
            -DENABLE_PNG=ON
            -DENABLE_JPEG=ON
            -DENABLE_TIFF=ON
            -DENABLE_WEBP=OFF
            -DENABLE_OPENJPEG=OFF
            -DENABLE_GIF=OFF
            -DJPEG_INCLUDE_DIR=${JPEG_INSTALL}/include
            -DJPEG_LIBRARY=${JPEG_INSTALL}/lib/libjpeg.a
            -DCMAKE_DISABLE_FIND_PACKAGE_WebP=ON
            -DCMAKE_DISABLE_FIND_PACKAGE_WebPMux=ON
            -DCMAKE_DISABLE_FIND_PACKAGE_GIF=ON
            -DCMAKE_DISABLE_FIND_PACKAGE_OpenJPEG=ON
            -DPNG_ROOT=${PNG_INSTALL}
            -DPNG_PNG_INCLUDE_DIR=${PNG_INSTALL}/include
            -DPNG_LIBRARY=${PNG_INSTALL}/lib/libpng16.a
            -DTIFF_ROOT=${TIFF_INSTALL}
            -DTIFF_INCLUDE_DIR=${TIFF_INSTALL}/include
            -DTIFF_LIBRARY=${TIFF_INSTALL}/lib/libtiff.a
            -DZLIB_ROOT=${ZLIB_INSTALL}
            -DZLIB_USE_STATIC_LIBS=ON
        BUILD_COMMAND   cmake --build <BINARY_DIR> --parallel ${NPROC}
        INSTALL_COMMAND cmake --install <BINARY_DIR>
        BUILD_BYPRODUCTS "${LEPT_INSTALL}/lib/libleptonica.a"
        LOG_CONFIGURE   TRUE
        LOG_BUILD       TRUE
        LOG_INSTALL     TRUE
        LOG_OUTPUT_ON_FAILURE TRUE
    )

    # ---- Tesseract -----------------------------------------------------
    ExternalProject_Add(ep_tesseract
        GIT_REPOSITORY  https://github.com/tesseract-ocr/tesseract.git
        GIT_TAG         ${TESSERACT_GIT_TAG}
        GIT_SHALLOW     TRUE
        PREFIX          "${DEPS_PREFIX}/build/tesseract"
        DEPENDS         ep_leptonica
        CMAKE_GENERATOR "Ninja"
        CMAKE_ARGS
            -DCMAKE_INSTALL_PREFIX=${TESS_INSTALL}
            -DCMAKE_INSTALL_LIBDIR=lib
            -DCMAKE_BUILD_TYPE=Release
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON
            -DCMAKE_PREFIX_PATH=${LEPT_INSTALL}\;${ZLIB_INSTALL}
            -DLeptonica_DIR=${LEPT_INSTALL}/lib/cmake/leptonica
            -DBUILD_SHARED_LIBS=OFF
            -DBUILD_TRAINING_TOOLS=OFF
            -DBUILD_TESTS=OFF
            -DDISABLED_LEGACY_ENGINE=ON
            -DDISABLE_CURL=ON
            -DDISABLE_ARCHIVE=ON
            -DGRAPHICS_DISABLED=ON
            -DUSE_SYSTEM_ICU=OFF
            -DCMAKE_DISABLE_FIND_PACKAGE_LibArchive=ON
            -DCMAKE_DISABLE_FIND_PACKAGE_CURL=ON
        BUILD_COMMAND   cmake --build <BINARY_DIR> --parallel ${NPROC}
        INSTALL_COMMAND cmake --install <BINARY_DIR>
        BUILD_BYPRODUCTS "${TESS_INSTALL}/lib/libtesseract.a"
        LOG_CONFIGURE   TRUE
        LOG_BUILD       TRUE
        LOG_INSTALL     TRUE
        LOG_OUTPUT_ON_FAILURE TRUE
    )

    # ---- libopus -------------------------------------------------------
    ExternalProject_Add(ep_opus
        GIT_REPOSITORY  https://github.com/xiph/opus.git
        GIT_TAG         ${OPUS_GIT_TAG}
        GIT_SHALLOW     TRUE
        PREFIX          "${DEPS_PREFIX}/build/opus"
        CMAKE_GENERATOR "Ninja"
        CMAKE_ARGS
            -DCMAKE_INSTALL_PREFIX=${OPUS_INSTALL}
            -DCMAKE_INSTALL_LIBDIR=lib
            -DCMAKE_BUILD_TYPE=Release
            -DBUILD_SHARED_LIBS=OFF
            -DOPUS_BUILD_TESTING=OFF
            -DOPUS_BUILD_PROGRAMS=OFF
        BUILD_COMMAND   cmake --build . --parallel ${NPROC}
        BUILD_BYPRODUCTS "${OPUS_INSTALL}/lib/libopus.a"
        LOG_CONFIGURE   TRUE
        LOG_BUILD       TRUE
        LOG_INSTALL     TRUE
        LOG_OUTPUT_ON_FAILURE TRUE
    )

    # ---- dav1d (meson) --------------------------------------------------
    find_program(MESON_EXECUTABLE meson REQUIRED)
    # CRF search decodes its own AV1 probe encodes in software
    # (crf_search.c, avcodec_find_decoder(AV_CODEC_ID_AV1)). FFmpeg's
    # native av1 decoder is hwaccel-only, so without dav1d the static
    # binary scores 0 frames and CRF search silently fails.
    ExternalProject_Add(ep_dav1d
        GIT_REPOSITORY  https://code.videolan.org/videolan/dav1d.git
        GIT_TAG         ${DAV1D_GIT_TAG}
        GIT_SHALLOW     TRUE
        PREFIX          "${DEPS_PREFIX}/build/dav1d"
        CONFIGURE_COMMAND
            ${MESON_EXECUTABLE} setup
                --prefix=${DAV1D_INSTALL}
                --libdir=${DAV1D_INSTALL}/lib
                --buildtype=release
                --default-library=static
                -Denable_tools=false
                -Denable_tests=false
                <BINARY_DIR>
                <SOURCE_DIR>
        BUILD_COMMAND   ${MESON_EXECUTABLE} compile -C <BINARY_DIR>
        INSTALL_COMMAND ${MESON_EXECUTABLE} install -C <BINARY_DIR>
        BUILD_BYPRODUCTS "${DAV1D_INSTALL}/lib/libdav1d.a"
        LOG_CONFIGURE   TRUE
        LOG_BUILD       TRUE
        LOG_INSTALL     TRUE
        LOG_OUTPUT_ON_FAILURE TRUE
    )

    # ---- FFmpeg --------------------------------------------------------
    ExternalProject_Add(ep_ffmpeg
        GIT_REPOSITORY  https://github.com/FFmpeg/FFmpeg.git
        GIT_TAG         ${FFMPEG_GIT_TAG}
        GIT_SHALLOW     TRUE
        PREFIX          "${DEPS_PREFIX}/build/ffmpeg"
        DEPENDS         ep_opus ep_zlib ep_dav1d
        CONFIGURE_COMMAND
            ${CMAKE_COMMAND} -E env
                "PKG_CONFIG_PATH=${OPUS_INSTALL}/lib/pkgconfig:${ZLIB_INSTALL}/lib/pkgconfig:${ZLIB_INSTALL}/share/pkgconfig:${DAV1D_INSTALL}/lib/pkgconfig"
            <SOURCE_DIR>/configure
                --prefix=${FF_INSTALL}
                --enable-static
                --disable-shared
                --disable-programs
                --disable-doc
                --disable-autodetect
                --enable-gpl
                --enable-filter=cropdetect
                --enable-libopus
                --enable-encoder=libopus
                --enable-decoder=libopus
                --enable-libdav1d
                --enable-decoder=libdav1d
                --enable-zlib
                --extra-cflags=-I${OPUS_INSTALL}/include
                --extra-cflags=-I${ZLIB_INSTALL}/include
                --extra-ldflags=-L${OPUS_INSTALL}/lib
                --extra-ldflags=-L${ZLIB_INSTALL}/lib
        BUILD_COMMAND   make -j${NPROC}
        INSTALL_COMMAND make install
        BUILD_IN_SOURCE TRUE
        BUILD_BYPRODUCTS
            "${FF_INSTALL}/lib/libavutil.a"
            "${FF_INSTALL}/lib/libavcodec.a"
            "${FF_INSTALL}/lib/libavformat.a"
            "${FF_INSTALL}/lib/libavfilter.a"
            "${FF_INSTALL}/lib/libswscale.a"
            "${FF_INSTALL}/lib/libswresample.a"
        LOG_CONFIGURE   TRUE
        LOG_BUILD       TRUE
        LOG_INSTALL     TRUE
        LOG_OUTPUT_ON_FAILURE TRUE
    )

    # ---- libvmaf (meson) ----------------------------------------------
    ExternalProject_Add(ep_libvmaf
        GIT_REPOSITORY  https://github.com/Netflix/vmaf.git
        GIT_TAG         ${LIBVMAF_GIT_TAG}
        GIT_SHALLOW     TRUE
        PREFIX          "${DEPS_PREFIX}/build/libvmaf"
        CONFIGURE_COMMAND
            ${MESON_EXECUTABLE} setup
                --prefix=${VMAF_INSTALL}
                --libdir=${VMAF_INSTALL}/lib
                --buildtype=release
                --default-library=static
                -Denable_tests=false
                -Denable_docs=false
                -Dbuilt_in_models=true
                <BINARY_DIR>
                <SOURCE_DIR>/libvmaf
        BUILD_COMMAND   ${MESON_EXECUTABLE} compile -C <BINARY_DIR>
        INSTALL_COMMAND ${MESON_EXECUTABLE} install -C <BINARY_DIR>
        BUILD_BYPRODUCTS "${VMAF_INSTALL}/lib/libvmaf.a"
        LOG_CONFIGURE   TRUE
        LOG_BUILD       TRUE
        LOG_INSTALL     TRUE
        LOG_OUTPUT_ON_FAILURE TRUE
    )

    # ---- libdovi (Rust / cargo-c) -------------------------------------
    ExternalProject_Add(ep_libdovi
        GIT_REPOSITORY  https://github.com/quietvoid/dovi_tool.git
        GIT_TAG         ${LIBDOVI_GIT_TAG}
        GIT_SHALLOW     TRUE
        PREFIX          "${DEPS_PREFIX}/build/libdovi"
        CONFIGURE_COMMAND ""
        BUILD_COMMAND     ""
        INSTALL_COMMAND
            ${CARGO} cinstall
                --manifest-path=<SOURCE_DIR>/dolby_vision/Cargo.toml
                --release
                --prefix=${DOVI_INSTALL}
                --libdir=${DOVI_INSTALL}/lib
                --includedir=${DOVI_INSTALL}/include
        BUILD_IN_SOURCE TRUE
        BUILD_BYPRODUCTS "${DOVI_INSTALL}/lib/libdovi.a"
        LOG_INSTALL     TRUE
        LOG_OUTPUT_ON_FAILURE TRUE
    )

    # ---- IMPORTED static targets --------------------------------------
    set(_ff_libs avutil avcodec avformat avfilter swscale swresample)
    foreach(_lib IN LISTS _ff_libs)
        add_library(FFmpeg::${_lib} STATIC IMPORTED GLOBAL)
        set_target_properties(FFmpeg::${_lib} PROPERTIES
            IMPORTED_LOCATION             "${FF_INSTALL}/lib/lib${_lib}.a"
            INTERFACE_INCLUDE_DIRECTORIES "${FF_INSTALL}/include"
        )
        add_dependencies(FFmpeg::${_lib} ep_ffmpeg)
    endforeach()

    add_library(Dovi::dovi STATIC IMPORTED GLOBAL)
    set_target_properties(Dovi::dovi PROPERTIES
        IMPORTED_LOCATION             "${DOVI_INSTALL}/lib/libdovi.a"
        INTERFACE_INCLUDE_DIRECTORIES "${DOVI_INSTALL}/include"
    )
    add_dependencies(Dovi::dovi ep_libdovi)

    add_library(Vmav::libvmaf STATIC IMPORTED GLOBAL)
    set_target_properties(Vmav::libvmaf PROPERTIES
        IMPORTED_LOCATION             "${VMAF_INSTALL}/lib/libvmaf.a"
        INTERFACE_INCLUDE_DIRECTORIES "${VMAF_INSTALL}/include"
    )
    add_dependencies(Vmav::libvmaf ep_libvmaf)

    add_library(Vmav::zlib STATIC IMPORTED GLOBAL)
    set_target_properties(Vmav::zlib PROPERTIES
        IMPORTED_LOCATION             "${ZLIB_INSTALL}/lib/libz.a"
        INTERFACE_INCLUDE_DIRECTORIES "${ZLIB_INSTALL}/include"
    )
    add_dependencies(Vmav::zlib ep_zlib)

    add_library(Vmav::ssl STATIC IMPORTED GLOBAL)
    set_target_properties(Vmav::ssl PROPERTIES
        IMPORTED_LOCATION             "${OPENSSL_INSTALL}/lib/libssl.a"
        INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INSTALL}/include"
    )
    add_dependencies(Vmav::ssl ep_openssl)

    add_library(Vmav::crypto STATIC IMPORTED GLOBAL)
    set_target_properties(Vmav::crypto PROPERTIES
        IMPORTED_LOCATION             "${OPENSSL_INSTALL}/lib/libcrypto.a"
        INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INSTALL}/include"
    )
    add_dependencies(Vmav::crypto ep_openssl)

    add_library(Vmav::curl STATIC IMPORTED GLOBAL)
    set_target_properties(Vmav::curl PROPERTIES
        IMPORTED_LOCATION             "${CURL_INSTALL}/lib/libcurl.a"
        INTERFACE_INCLUDE_DIRECTORIES "${CURL_INSTALL}/include"
        INTERFACE_COMPILE_DEFINITIONS "CURL_STATICLIB"
    )
    add_dependencies(Vmav::curl ep_curl)

    add_library(Vmav::jpeg STATIC IMPORTED GLOBAL)
    set_target_properties(Vmav::jpeg PROPERTIES
        IMPORTED_LOCATION             "${JPEG_INSTALL}/lib/libjpeg.a"
        INTERFACE_INCLUDE_DIRECTORIES "${JPEG_INSTALL}/include"
    )
    add_dependencies(Vmav::jpeg ep_libjpeg)

    add_library(Vmav::tiff STATIC IMPORTED GLOBAL)
    set_target_properties(Vmav::tiff PROPERTIES
        IMPORTED_LOCATION             "${TIFF_INSTALL}/lib/libtiff.a"
        INTERFACE_INCLUDE_DIRECTORIES "${TIFF_INSTALL}/include"
    )
    add_dependencies(Vmav::tiff ep_libtiff)

    add_library(Vmav::png STATIC IMPORTED GLOBAL)
    set_target_properties(Vmav::png PROPERTIES
        IMPORTED_LOCATION             "${PNG_INSTALL}/lib/libpng16.a"
        INTERFACE_INCLUDE_DIRECTORIES "${PNG_INSTALL}/include"
    )
    add_dependencies(Vmav::png ep_libpng)

    add_library(Vmav::leptonica STATIC IMPORTED GLOBAL)
    set_target_properties(Vmav::leptonica PROPERTIES
        IMPORTED_LOCATION             "${LEPT_INSTALL}/lib/libleptonica.a"
        INTERFACE_INCLUDE_DIRECTORIES "${LEPT_INSTALL}/include;${LEPT_INSTALL}/include/leptonica"
    )
    add_dependencies(Vmav::leptonica ep_leptonica)

    add_library(Vmav::tesseract STATIC IMPORTED GLOBAL)
    set_target_properties(Vmav::tesseract PROPERTIES
        IMPORTED_LOCATION             "${TESS_INSTALL}/lib/libtesseract.a"
        INTERFACE_INCLUDE_DIRECTORIES "${TESS_INSTALL}/include;${TESS_INSTALL}/include/tesseract"
    )
    add_dependencies(Vmav::tesseract ep_tesseract)

    message(STATUS "Dependencies: vendored static (legacy GitHub-release path)")
endif()

# ===========================================================================
# Executable (per AGENTS.md § 15: modular source structure)
# ===========================================================================
add_executable(vmavificient
    src/audio_encode/audio_encode.c
    src/config/config.c
    src/crf_search/crf_search.c
    src/encode_preset/encode_preset.c
    src/final_mux/final_mux.c
    src/main/main.c
    src/media_analysis/media_analysis.c
    src/media_crop/media_crop.c
    src/media_hdr/media_hdr.c
    src/media_info/media_info.c
    src/media_naming/media_naming.c
    src/media_tracks/media_tracks.c
    src/pipeline/cli.c
    src/pipeline/pipeline_util.c
    src/pipeline/stage_probe.c
    src/pipeline/stage_grain.c
    src/pipeline/stage_naming.c
    src/pipeline/stage_audio.c
    src/pipeline/stage_subs.c
    src/pipeline/encode_pass.c
    src/rpu_extract/rpu_extract.c
    src/subtitle_convert/srt_sanitize.c
    src/subtitle_convert/subtitle_convert.c
    src/tmdb/tmdb.c
    src/ui/ui.c
    src/utils/proc.c
    src/utils/utils.c
    src/video_encode/encoder_config.c
    src/video_encode/video_encode.c
)

target_compile_options(vmavificient PRIVATE ${VMAV_OPT_FLAGS})
target_link_options(vmavificient PRIVATE ${VMAV_LINK_FLAGS})

# Build-time defines:
#   VMAV_GRAV1SYNTH_BIN — path/command for the grav1synth subprocess
#                         (invoked from src/media_analysis.c). Defaults to
#                         the absolute path of the in-tree build, which is
#                         right for `cmake --build` invocations. Packagers
#                         (Homebrew, .deb) override it with a bare command
#                         name so the binary uses PATH lookup, then ship
#                         grav1synth alongside vmavificient in bin/.
#   VMAV_VERSION         — project version string for the CLI banner
set(VMAV_GRAV1SYNTH_BIN_RUNTIME "${GRAV1SYNTH_BIN}" CACHE STRING
    "Path or command name baked into the binary for spawning grav1synth")
target_compile_definitions(vmavificient PRIVATE
    "VMAV_GRAV1SYNTH_BIN=\"${VMAV_GRAV1SYNTH_BIN_RUNTIME}\""
    "VMAV_VERSION=\"${PROJECT_VERSION}\"")
add_dependencies(vmavificient ep_grav1synth)

# tesseract is C++; use the C++ linker driver so libc++/libstdc++ is pulled in.
set_target_properties(vmavificient PROPERTIES LINKER_LANGUAGE CXX)

target_include_directories(vmavificient PRIVATE
    "${CMAKE_SOURCE_DIR}/include/vmavificient"
    "${SVTAV1_INSTALL}/include"
)
if(NOT VMAV_USE_SYSTEM_DEPS)
    # FetchContent's cjson exposes its sources via cjson_SOURCE_DIR; in
    # system mode the PkgConfig::VMAV_CJSON target carries its own include.
    target_include_directories(vmavificient PRIVATE "${cjson_SOURCE_DIR}")
endif()

# ===========================================================================
# Linking
# ===========================================================================
# Always-vendored libs first (they have no order dependency on the rest).
target_link_libraries(vmavificient PRIVATE
    SvtAv1::Enc
    Hdr10Plus::hdr10plus
    Threads::Threads
    m
)

if(VMAV_USE_SYSTEM_DEPS)
    target_link_libraries(vmavificient PRIVATE
        PkgConfig::VMAV_LIBAV
        PkgConfig::VMAV_OPUS
        PkgConfig::VMAV_DOVI
        PkgConfig::VMAV_TESSERACT
        PkgConfig::VMAV_LEPTONICA
        PkgConfig::VMAV_LIBTIFF
        PkgConfig::VMAV_LIBJPEG
        PkgConfig::VMAV_LIBPNG
        PkgConfig::VMAV_LIBCURL
        PkgConfig::VMAV_CJSON
        PkgConfig::VMAV_LIBVMAF
        OpenSSL::SSL
        OpenSSL::Crypto
        ZLIB::ZLIB
    )
else()
    # Link order matters for static archives: dependents before dependencies.
    target_link_libraries(vmavificient PRIVATE
        FFmpeg::avformat
        FFmpeg::avfilter
        FFmpeg::avcodec
        FFmpeg::swresample
        FFmpeg::swscale
        FFmpeg::avutil
        Dovi::dovi
        Vmav::libvmaf
        "${OPUS_INSTALL}/lib/libopus.a"
        "${DAV1D_INSTALL}/lib/libdav1d.a"
        Vmav::tesseract
        Vmav::leptonica
        Vmav::tiff
        Vmav::jpeg
        Vmav::png
        Vmav::curl
        Vmav::ssl
        Vmav::crypto
        Vmav::zlib
        cjson
    )
endif()

# Platform-specific link extras.
if(APPLE)
    # iconv (FFmpeg) and frameworks (Rust std + SecureTransport fallbacks)
    # are needed in both modes.
    target_link_libraries(vmavificient PRIVATE
        iconv
        "-framework CoreFoundation"
        "-framework Security"
        "-framework SystemConfiguration"
    )
elseif(NOT VMAV_USE_SYSTEM_DEPS)
    # Vendored OpenSSL's engine layer references dlopen on Linux; the
    # static .a doesn't carry that transitively. In system mode the
    # distro's openssl.pc lists -ldl already.
    target_link_libraries(vmavificient PRIVATE dl)
endif()

# ===========================================================================
# Install rules
# ===========================================================================
# `cmake --install build` lays out a standard prefix tree for packagers
# (Homebrew formula, .deb / .rpm CPack generators, etc.). vmavificient
# spawns grav1synth via VMAV_GRAV1SYNTH_BIN at runtime, so the helper is
# installed alongside the main binary; if the packager passed
# -DVMAV_GRAV1SYNTH_BIN_RUNTIME=grav1synth, vmavificient does a PATH
# lookup and finds it there.
include(GNUInstallDirs)
# Both rules carry a component so `cmake --install --component vmavificient`
# installs exactly our two binaries. Without it, a vendored-mode install
# also runs cJSON's rules, and those use CMAKE_INSTALL_FULL_INCLUDEDIR —
# an absolute path baked in at configure time that --prefix cannot
# redirect — so the install dies trying to write /usr/local/include/cjson.
install(TARGETS vmavificient
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    COMPONENT vmavificient
)
install(PROGRAMS "${GRAV1SYNTH_BIN}"
    DESTINATION "${CMAKE_INSTALL_BINDIR}"
    COMPONENT vmavificient
)

# ===========================================================================
# Doxygen documentation (optional)
# ===========================================================================
find_package(Doxygen QUIET)
if(DOXYGEN_FOUND)
    set(DOXYGEN_PROJECT_NAME        "${PROJECT_NAME}")
    set(DOXYGEN_PROJECT_NUMBER      "${PROJECT_VERSION}")
    set(DOXYGEN_OUTPUT_DIRECTORY     "${CMAKE_BINARY_DIR}/docs")
    set(DOXYGEN_EXTRACT_ALL          YES)
    set(DOXYGEN_OPTIMIZE_OUTPUT_FOR_C YES)
    doxygen_add_docs(docs
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_SOURCE_DIR}/src"
        COMMENT "Generating API documentation with Doxygen"
    )
endif()

# ---------------------------------------------------------------------------
# Unit tests (pure-logic modules only; no external deps linked).
#   Build:  cmake --build build --target vmav_tests
#   Run:    ./build/vmav_tests
# ---------------------------------------------------------------------------
add_executable(vmav_tests
    tests/test_media_naming.c
    tests/test_srt_sanitize.c
    src/media_naming/media_naming.c
    src/subtitle_convert/srt_sanitize.c
)
target_include_directories(vmav_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/include/vmavificient"
)
target_compile_options(vmav_tests PRIVATE ${VMAV_OPT_FLAGS})
target_link_options(vmav_tests PRIVATE ${VMAV_LINK_FLAGS})
