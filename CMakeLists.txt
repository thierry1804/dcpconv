cmake_minimum_required(VERSION 3.20)
project(dcpconv VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# Build everything statically
set(BUILD_SHARED_LIBS OFF)
set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
set(CMAKE_EXE_LINKER_FLAGS "-static-libgcc -static-libstdc++")

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(DEPS_INSTALL_DIR ${CMAKE_BINARY_DIR}/deps)
set(DEPS_INCLUDE_DIR ${DEPS_INSTALL_DIR}/include)
set(DEPS_LIB_DIR ${DEPS_INSTALL_DIR}/lib)

include(ExternalProject)

# ===========================================================================
# Third-party dependencies (all built as static libraries)
# ===========================================================================

# --- OpenJPEG (JPEG 2000 decoder) ---
ExternalProject_Add(ext_openjpeg
    GIT_REPOSITORY https://github.com/uclouvain/openjpeg.git
    GIT_TAG        v2.5.3
    CMAKE_ARGS
        -DCMAKE_INSTALL_PREFIX=${DEPS_INSTALL_DIR}
        -DCMAKE_BUILD_TYPE=Release
        -DBUILD_SHARED_LIBS=OFF
        -DBUILD_CODEC=OFF
        -DBUILD_TESTING=OFF
    BUILD_BYPRODUCTS ${DEPS_LIB_DIR}/libopenjp2.a
)

# --- xerces-c (XML parser for CPL/ASSETMAP) ---
ExternalProject_Add(ext_xerces
    GIT_REPOSITORY https://github.com/apache/xerces-c.git
    GIT_TAG        v3.2.5
    CMAKE_ARGS
        -DCMAKE_INSTALL_PREFIX=${DEPS_INSTALL_DIR}
        -DCMAKE_BUILD_TYPE=Release
        -DBUILD_SHARED_LIBS=OFF
        -Dnetwork:BOOL=OFF
        -Dtranscoder=iconv
        -Dthreads:BOOL=OFF
        -Dmessage-loader=inmemory
    BUILD_BYPRODUCTS ${DEPS_LIB_DIR}/libxerces-c.a
)

# --- OpenSSL (for asdcplib crypto) ---
ExternalProject_Add(ext_openssl
    GIT_REPOSITORY https://github.com/openssl/openssl.git
    GIT_TAG        openssl-3.2.1
    CONFIGURE_COMMAND <SOURCE_DIR>/Configure
        --prefix=${DEPS_INSTALL_DIR}
        --openssldir=${DEPS_INSTALL_DIR}/ssl
        no-shared
        no-tests
        darwin64-x86_64-cc
    BUILD_COMMAND make -j4
    INSTALL_COMMAND make install_sw
    BUILD_BYPRODUCTS ${DEPS_LIB_DIR}/libssl.a ${DEPS_LIB_DIR}/libcrypto.a
)

# --- asdcplib (MXF demuxer for DCP/IMF) ---
ExternalProject_Add(ext_asdcplib
    GIT_REPOSITORY https://github.com/cinecert/asdcplib.git
    GIT_TAG        rel_2_13_1
    DEPENDS ext_openssl ext_xerces
    CMAKE_ARGS
        -DCMAKE_INSTALL_PREFIX=${DEPS_INSTALL_DIR}
        -DCMAKE_BUILD_TYPE=Release
        -DBUILD_SHARED_LIBS=OFF
        -DOPENSSL_ROOT_DIR=${DEPS_INSTALL_DIR}
        -DXERCES_ROOT=${DEPS_INSTALL_DIR}
    BUILD_BYPRODUCTS ${DEPS_LIB_DIR}/libasdcp.a ${DEPS_LIB_DIR}/libkumu.a ${DEPS_LIB_DIR}/libas02.a
)

# --- x264 (H.264 encoder) ---
ExternalProject_Add(ext_x264
    GIT_REPOSITORY https://code.videolan.org/videolan/x264.git
    GIT_TAG        stable
    CONFIGURE_COMMAND <SOURCE_DIR>/configure
        --prefix=${DEPS_INSTALL_DIR}
        --enable-static
        --disable-shared
        --disable-cli
        --disable-opencl
        --bit-depth=all
    BUILD_COMMAND make -j4
    INSTALL_COMMAND make install
    BUILD_BYPRODUCTS ${DEPS_LIB_DIR}/libx264.a
)

# --- x265 (H.265/HEVC encoder) ---
ExternalProject_Add(ext_x265
    GIT_REPOSITORY https://bitbucket.org/multicoreware/x265_git.git
    GIT_TAG        Release_3.6
    SOURCE_SUBDIR  source
    CMAKE_ARGS
        -DCMAKE_INSTALL_PREFIX=${DEPS_INSTALL_DIR}
        -DCMAKE_BUILD_TYPE=Release
        -DENABLE_SHARED=OFF
        -DENABLE_CLI=OFF
    BUILD_BYPRODUCTS ${DEPS_LIB_DIR}/libx265.a
)

# --- FDK-AAC (AAC audio encoder) ---
ExternalProject_Add(ext_fdkaac
    GIT_REPOSITORY https://github.com/mstorsjo/fdk-aac.git
    GIT_TAG        v2.0.3
    CMAKE_ARGS
        -DCMAKE_INSTALL_PREFIX=${DEPS_INSTALL_DIR}
        -DCMAKE_BUILD_TYPE=Release
        -DBUILD_SHARED_LIBS=OFF
        -DBUILD_PROGRAMS=OFF
    BUILD_BYPRODUCTS ${DEPS_LIB_DIR}/libfdk-aac.a
)

# --- FreeType (text rendering for subtitle burn-in) ---
ExternalProject_Add(ext_freetype
    GIT_REPOSITORY https://gitlab.freedesktop.org/freetype/freetype.git
    GIT_TAG        VER-2-13-3
    CMAKE_ARGS
        -DCMAKE_INSTALL_PREFIX=${DEPS_INSTALL_DIR}
        -DCMAKE_BUILD_TYPE=Release
        -DBUILD_SHARED_LIBS=OFF
        -DFT_DISABLE_HARFBUZZ=ON
        -DFT_DISABLE_BZIP2=ON
        -DFT_DISABLE_BROTLI=ON
        -DFT_DISABLE_PNG=ON
    BUILD_BYPRODUCTS ${DEPS_LIB_DIR}/libfreetype.a
)

# --- HarfBuzz (complex text shaping) ---
ExternalProject_Add(ext_harfbuzz
    GIT_REPOSITORY https://github.com/harfbuzz/harfbuzz.git
    GIT_TAG        8.5.0
    DEPENDS ext_freetype
    CMAKE_ARGS
        -DCMAKE_INSTALL_PREFIX=${DEPS_INSTALL_DIR}
        -DCMAKE_BUILD_TYPE=Release
        -DBUILD_SHARED_LIBS=OFF
        -DHB_HAVE_FREETYPE=ON
        -DFREETYPE_INCLUDE_DIR_freetype2=${DEPS_INCLUDE_DIR}/freetype2
        -DFREETYPE_INCLUDE_DIR_ft2build=${DEPS_INCLUDE_DIR}
        -DFREETYPE_LIBRARY=${DEPS_LIB_DIR}/libfreetype.a
        -DHB_BUILD_TESTS=OFF
        -DHB_BUILD_SUBSET=OFF
    BUILD_BYPRODUCTS ${DEPS_LIB_DIR}/libharfbuzz.a
)

# --- minimp4 (lightweight MP4 muxer, header-only) ---
ExternalProject_Add(ext_minimp4
    GIT_REPOSITORY https://github.com/nicklockwood/minimp4.git
    GIT_TAG        main
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ""
    INSTALL_COMMAND ${CMAKE_COMMAND} -E copy
        <SOURCE_DIR>/minimp4.h ${DEPS_INCLUDE_DIR}/minimp4.h
)

# ===========================================================================
# Main project
# ===========================================================================

# Include directories for all deps
include_directories(${DEPS_INCLUDE_DIR} ${DEPS_INCLUDE_DIR}/freetype2)
link_directories(${DEPS_LIB_DIR})

# Source files
set(SOURCES
    src/main.cpp
    src/core/pipeline.cpp
    src/input/dcp_reader.cpp
    src/input/imf_reader.cpp
    src/input/mxf_demuxer.cpp
    src/decode/j2k_decoder.cpp
    src/decode/pcm_decoder.cpp
    src/color/xyz_to_rgb.cpp
    src/subtitles/smpte_tt_parser.cpp
    src/subtitles/cinecanvas_parser.cpp
    src/subtitles/ttml_parser.cpp
    src/subtitles/subtitle_burner.cpp
    src/subtitles/subtitle_muxer.cpp
    src/encode/h264_encoder.cpp
    src/encode/h265_encoder.cpp
    src/encode/prores_encoder.cpp
    src/encode/aac_encoder.cpp
    src/output/mp4_muxer.cpp
)

add_executable(dcpconv ${SOURCES})

# Dependencies ordering
add_dependencies(dcpconv
    ext_openjpeg ext_asdcplib ext_x264 ext_x265
    ext_fdkaac ext_freetype ext_harfbuzz ext_minimp4
)

# Link order matters for static libs
target_link_libraries(dcpconv
    # asdcplib
    ${DEPS_LIB_DIR}/libas02.a
    ${DEPS_LIB_DIR}/libasdcp.a
    ${DEPS_LIB_DIR}/libkumu.a
    # encoders
    ${DEPS_LIB_DIR}/libx264.a
    ${DEPS_LIB_DIR}/libx265.a
    ${DEPS_LIB_DIR}/libfdk-aac.a
    # decoders
    ${DEPS_LIB_DIR}/libopenjp2.a
    # text rendering
    ${DEPS_LIB_DIR}/libharfbuzz.a
    ${DEPS_LIB_DIR}/libfreetype.a
    # XML
    ${DEPS_LIB_DIR}/libxerces-c.a
    # crypto
    ${DEPS_LIB_DIR}/libssl.a
    ${DEPS_LIB_DIR}/libcrypto.a
    # macOS frameworks
    "-framework CoreFoundation"
    "-framework CoreText"
    "-framework Security"
    "-framework VideoToolbox"
    "-framework CoreMedia"
    "-framework CoreVideo"
    # system
    pthread
    dl
    z
    iconv
    m
)

# Install
install(TARGETS dcpconv RUNTIME DESTINATION bin)

message(STATUS "")
message(STATUS "=== dcpconv build configuration ===")
message(STATUS "  Version:     ${PROJECT_VERSION}")
message(STATUS "  Build type:  ${CMAKE_BUILD_TYPE}")
message(STATUS "  Deps prefix: ${DEPS_INSTALL_DIR}")
message(STATUS "===================================")
