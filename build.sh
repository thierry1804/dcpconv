#!/bin/bash
#
# dcpconv build script for macOS
# Builds all dependencies statically and produces a standalone binary
#
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
DEPS_DIR="${BUILD_DIR}/deps"
JOBS=$(sysctl -n hw.ncpu 2>/dev/null || echo 4)

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

log()   { echo -e "${GREEN}[BUILD]${NC} $1"; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $1"; }
error() { echo -e "${RED}[ERROR]${NC} $1"; exit 1; }
step()  { echo -e "\n${CYAN}━━━ $1 ━━━${NC}"; }

# ============================================================================
# Pre-flight checks
# ============================================================================

step "Pre-flight checks"

# Check for Xcode command line tools
if ! xcode-select -p &>/dev/null; then
    error "Xcode Command Line Tools not found. Install with: xcode-select --install"
fi
log "Xcode CLI tools: OK"

# Check for CMake
if ! command -v cmake &>/dev/null; then
    if command -v brew &>/dev/null; then
        log "Installing cmake via Homebrew..."
        brew install cmake
    else
        error "cmake not found. Install with: brew install cmake"
    fi
fi
log "cmake: $(cmake --version | head -1)"

# Check for nasm (needed for x264/x265)
if ! command -v nasm &>/dev/null; then
    if command -v brew &>/dev/null; then
        log "Installing nasm via Homebrew..."
        brew install nasm
    else
        warn "nasm not found - x264/x265 will build without SIMD optimizations"
    fi
fi

# Check architecture
ARCH=$(uname -m)
log "Architecture: ${ARCH}"

if [ "$ARCH" = "arm64" ]; then
    CMAKE_ARCH_FLAGS="-DCMAKE_OSX_ARCHITECTURES=arm64"
    OPENSSL_TARGET="darwin64-arm64-cc"
else
    CMAKE_ARCH_FLAGS="-DCMAKE_OSX_ARCHITECTURES=x86_64"
    OPENSSL_TARGET="darwin64-x86_64-cc"
fi

# ============================================================================
# Create build directories
# ============================================================================

mkdir -p "${BUILD_DIR}"
mkdir -p "${DEPS_DIR}/src"
mkdir -p "${DEPS_DIR}/include"
mkdir -p "${DEPS_DIR}/lib"

# ============================================================================
# Build dependencies
# ============================================================================

# Detect macOS SDK path
MACOS_SDK=$(xcrun --show-sdk-path 2>/dev/null)
if [ -z "$MACOS_SDK" ]; then
    error "Cannot find macOS SDK. Run: xcode-select --install"
fi
log "macOS SDK: ${MACOS_SDK}"

# Verify C++ headers exist in SDK
if [ ! -f "${MACOS_SDK}/usr/include/c++/v1/new" ]; then
    error "C++ headers missing from SDK at ${MACOS_SDK}. Reinstall Command Line Tools."
fi
log "C++ headers: OK"

# Force sysroot everywhere - critical for macOS with CLT-only setup
export SDKROOT="${MACOS_SDK}"
export CFLAGS="-isysroot ${MACOS_SDK}"
export CXXFLAGS="-isysroot ${MACOS_SDK} -stdlib=libc++"
export LDFLAGS="-isysroot ${MACOS_SDK}"
export MACOSX_DEPLOYMENT_TARGET=""

COMMON_CMAKE_ARGS=(
    -DCMAKE_BUILD_TYPE=Release
    -DBUILD_SHARED_LIBS=OFF
    -DCMAKE_INSTALL_PREFIX="${DEPS_DIR}"
    -DCMAKE_OSX_SYSROOT="${MACOS_SDK}"
    -DCMAKE_CXX_STANDARD=17
    -DCMAKE_C_FLAGS="-isysroot ${MACOS_SDK}"
    -DCMAKE_CXX_FLAGS="-isysroot ${MACOS_SDK} -stdlib=libc++"
    -DCMAKE_EXE_LINKER_FLAGS="-isysroot ${MACOS_SDK}"
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5
    ${CMAKE_ARCH_FLAGS}
)

# --- OpenJPEG ---
build_openjpeg() {
    step "Building OpenJPEG"
    cd "${DEPS_DIR}/src"
    if [ ! -d openjpeg ]; then
        git clone --depth 1 --branch v2.5.3 https://github.com/uclouvain/openjpeg.git
    fi
    cd openjpeg
    mkdir -p build && cd build
    cmake .. "${COMMON_CMAKE_ARGS[@]}" \
        -DBUILD_CODEC=OFF -DBUILD_TESTING=OFF
    make -j${JOBS}
    make install
    log "OpenJPEG: OK"
}

# --- xerces-c ---
build_xerces() {
    step "Building xerces-c"
    cd "${DEPS_DIR}/src"
    if [ ! -d xerces-c ]; then
        git clone --depth 1 --branch v3.2.5 https://github.com/apache/xerces-c.git
    fi
    cd xerces-c
    # Clean previous build
    rm -rf build
    mkdir -p build && cd build
    cmake .. "${COMMON_CMAKE_ARGS[@]}" \
        -Dnetwork:BOOL=OFF \
        -Dtranscoder=iconv \
        -Dthreads:BOOL=OFF \
        -Dmessage-loader=inmemory
    make -j${JOBS}
    make install
    log "xerces-c: OK"
}

# --- OpenSSL ---
build_openssl() {
    step "Building OpenSSL"
    cd "${DEPS_DIR}/src"
    if [ ! -d openssl ]; then
        git clone --depth 1 --branch openssl-3.2.1 https://github.com/openssl/openssl.git
    fi
    cd openssl
    # Clean previous build if any
    make clean 2>/dev/null || true
    ./Configure ${OPENSSL_TARGET} \
        --prefix="${DEPS_DIR}" \
        --openssldir="${DEPS_DIR}/ssl" \
        no-shared no-tests no-docs
    make -j${JOBS}
    make install_sw
    log "OpenSSL: OK"
}

# --- asdcplib ---
build_asdcplib() {
    step "Building asdcplib"
    cd "${DEPS_DIR}/src"
    if [ ! -d asdcplib ]; then
        git clone https://github.com/cinecert/asdcplib.git
    fi
    cd asdcplib
    mkdir -p build && cd build
    cmake .. "${COMMON_CMAKE_ARGS[@]}" \
        -DOPENSSL_ROOT_DIR="${DEPS_DIR}" \
        -DXERCES_ROOT="${DEPS_DIR}" \
        -DCMAKE_PREFIX_PATH="${DEPS_DIR}"
    make -j${JOBS}
    make install
    log "asdcplib: OK"
}

# --- x264 ---
build_x264() {
    step "Building x264"
    cd "${DEPS_DIR}/src"
    if [ ! -d x264 ]; then
        git clone --depth 1 --branch stable https://code.videolan.org/videolan/x264.git
    fi
    cd x264
    # Clean
    make clean 2>/dev/null || true

    # x264 configure is sensitive to env CFLAGS — override with sysroot + arch
    # Also force CC so configure finds the compiler despite global env
    if [ "$ARCH" = "arm64" ]; then
        CC=/usr/bin/cc \
        CFLAGS="-isysroot ${MACOS_SDK} -arch arm64" \
        LDFLAGS="-isysroot ${MACOS_SDK} -arch arm64" \
        ./configure \
            --prefix="${DEPS_DIR}" \
            --enable-static \
            --disable-cli \
            --disable-opencl \
            --bit-depth=all \
            --host=aarch64-apple-darwin
    else
        CC=/usr/bin/cc \
        CFLAGS="-isysroot ${MACOS_SDK}" \
        LDFLAGS="-isysroot ${MACOS_SDK}" \
        ./configure \
            --prefix="${DEPS_DIR}" \
            --enable-static \
            --disable-cli \
            --disable-opencl \
            --bit-depth=all
    fi
    make -j${JOBS}
    make install
    log "x264: OK"
}

# --- x265 ---
build_x265() {
    step "Building x265"
    cd "${DEPS_DIR}/src"
    if [ ! -d x265 ]; then
        git clone --depth 1 --branch Release_3.6 \
            https://bitbucket.org/multicoreware/x265_git.git x265
    fi
    cd x265/source
    # Patch: replace OLD policies (rejected by CMake 4.x) with NEW
    # Restore original if previously patched, then re-patch
    [ -f CMakeLists.txt.bak ] && cp CMakeLists.txt.bak CMakeLists.txt
    sed -i.bak 's/OLD)/NEW)/g' CMakeLists.txt
    rm -rf build
    mkdir -p build && cd build
    cmake .. "${COMMON_CMAKE_ARGS[@]}" \
        -DCMAKE_CXX_STANDARD=14 \
        -DCMAKE_CXX_FLAGS="-isysroot ${MACOS_SDK} -stdlib=libc++ -Wno-register" \
        -DENABLE_SHARED=OFF \
        -DENABLE_CLI=OFF \
        -DENABLE_ASSEMBLY=OFF
    make -j${JOBS}
    make install
    log "x265: OK"
}

# --- FDK-AAC ---
build_fdkaac() {
    step "Building FDK-AAC"
    cd "${DEPS_DIR}/src"
    if [ ! -d fdk-aac ]; then
        git clone --depth 1 --branch v2.0.3 https://github.com/mstorsjo/fdk-aac.git
    fi
    cd fdk-aac
    # FDK-AAC v2.0.3 uses autotools, not CMake
    if [ ! -f configure ]; then
        autoreconf -fiv
    fi
    ./configure \
        --prefix="${DEPS_DIR}" \
        --enable-static \
        --disable-shared \
        CC=/usr/bin/cc \
        CXX=/usr/bin/c++ \
        CFLAGS="-isysroot ${MACOS_SDK}" \
        CXXFLAGS="-isysroot ${MACOS_SDK} -stdlib=libc++" \
        LDFLAGS="-isysroot ${MACOS_SDK}"
    make -j${JOBS}
    make install
    log "FDK-AAC: OK"
}

# --- FreeType ---
build_freetype() {
    step "Building FreeType"
    cd "${DEPS_DIR}/src"
    if [ ! -d freetype ]; then
        git clone --depth 1 --branch VER-2-13-3 \
            https://gitlab.freedesktop.org/freetype/freetype.git
    fi
    cd freetype
    rm -rf build
    mkdir -p build && cd build
    cmake .. "${COMMON_CMAKE_ARGS[@]}" \
        -DFT_DISABLE_HARFBUZZ=ON \
        -DFT_DISABLE_BZIP2=ON \
        -DFT_DISABLE_BROTLI=ON \
        -DFT_DISABLE_PNG=ON
    make -j${JOBS}
    make install
    log "FreeType: OK"
}

# --- HarfBuzz ---
build_harfbuzz() {
    step "Building HarfBuzz"
    cd "${DEPS_DIR}/src"
    if [ ! -d harfbuzz ]; then
        git clone --depth 1 --branch 8.5.0 https://github.com/harfbuzz/harfbuzz.git
    fi
    cd harfbuzz
    rm -rf build
    mkdir -p build && cd build
    cmake .. "${COMMON_CMAKE_ARGS[@]}" \
        -DHB_HAVE_FREETYPE=ON \
        -DFREETYPE_INCLUDE_DIR_freetype2="${DEPS_DIR}/include/freetype2" \
        -DFREETYPE_INCLUDE_DIR_ft2build="${DEPS_DIR}/include" \
        -DFREETYPE_LIBRARY="${DEPS_DIR}/lib/libfreetype.a" \
        -DHB_BUILD_TESTS=OFF \
        -DHB_BUILD_SUBSET=OFF
    make -j${JOBS}
    make install
    log "HarfBuzz: OK"
}

# --- minimp4 (header-only) ---
install_minimp4() {
    step "Installing minimp4"
    cd "${DEPS_DIR}/src"
    if [ ! -d minimp4 ]; then
        git clone --depth 1 https://github.com/lieff/minimp4.git
    fi
    cp minimp4/minimp4.h "${DEPS_DIR}/include/"
    log "minimp4: OK"
}

# ============================================================================
# Build all dependencies
# ============================================================================

step "Building all dependencies (this may take 15-30 minutes)"

build_openjpeg
build_xerces
build_openssl
build_asdcplib
build_x264
build_x265
build_fdkaac
build_freetype
build_harfbuzz
install_minimp4

# ============================================================================
# Verify dependencies
# ============================================================================

step "Verifying dependencies"

REQUIRED_LIBS=(
    libopenjp2.a
    libxerces-c.a
    libssl.a
    libcrypto.a
    libasdcp.a
    libas02.a
    libkumu.a
    libx264.a
    libx265.a
    libfdk-aac.a
    libfreetype.a
    libharfbuzz.a
)

ALL_OK=true
for lib in "${REQUIRED_LIBS[@]}"; do
    if [ -f "${DEPS_DIR}/lib/${lib}" ]; then
        log "  ✓ ${lib}"
    else
        warn "  ✗ ${lib} NOT FOUND"
        ALL_OK=false
    fi
done

if [ -f "${DEPS_DIR}/include/minimp4.h" ]; then
    log "  ✓ minimp4.h"
else
    warn "  ✗ minimp4.h NOT FOUND"
    ALL_OK=false
fi

if [ "$ALL_OK" = false ]; then
    error "Some dependencies are missing. Check the build output above."
fi

# ============================================================================
# Build dcpconv
# ============================================================================

step "Building dcpconv"

cd "${BUILD_DIR}"

# Compile all source files
SOURCES=(
    "${SCRIPT_DIR}/src/main.cpp"
    "${SCRIPT_DIR}/src/core/pipeline.cpp"
    "${SCRIPT_DIR}/src/input/dcp_reader.cpp"
    "${SCRIPT_DIR}/src/input/imf_reader.cpp"
    "${SCRIPT_DIR}/src/input/mxf_demuxer.cpp"
    "${SCRIPT_DIR}/src/decode/j2k_decoder.cpp"
    "${SCRIPT_DIR}/src/decode/pcm_decoder.cpp"
    "${SCRIPT_DIR}/src/color/xyz_to_rgb.cpp"
    "${SCRIPT_DIR}/src/subtitles/smpte_tt_parser.cpp"
    "${SCRIPT_DIR}/src/subtitles/cinecanvas_parser.cpp"
    "${SCRIPT_DIR}/src/subtitles/ttml_parser.cpp"
    "${SCRIPT_DIR}/src/subtitles/subtitle_burner.cpp"
    "${SCRIPT_DIR}/src/subtitles/subtitle_muxer.cpp"
    "${SCRIPT_DIR}/src/encode/h264_encoder.cpp"
    "${SCRIPT_DIR}/src/encode/h265_encoder.cpp"
    "${SCRIPT_DIR}/src/encode/prores_encoder.cpp"
    "${SCRIPT_DIR}/src/encode/aac_encoder.cpp"
    "${SCRIPT_DIR}/src/output/mp4_muxer.cpp"
)

INCLUDES=(
    -I"${DEPS_DIR}/include"
    -I"${DEPS_DIR}/include/openjpeg-2.5"
    -I"${DEPS_DIR}/include/freetype2"
)

LIBS=(
    "${DEPS_DIR}/lib/libas02.a"
    "${DEPS_DIR}/lib/libasdcp.a"
    "${DEPS_DIR}/lib/libkumu.a"
    "${DEPS_DIR}/lib/libx264.a"
    "${DEPS_DIR}/lib/libx265.a"
    "${DEPS_DIR}/lib/libfdk-aac.a"
    "${DEPS_DIR}/lib/libopenjp2.a"
    "${DEPS_DIR}/lib/libharfbuzz.a"
    "${DEPS_DIR}/lib/libfreetype.a"
    "${DEPS_DIR}/lib/libxerces-c.a"
    "${DEPS_DIR}/lib/libssl.a"
    "${DEPS_DIR}/lib/libcrypto.a"
)

FRAMEWORKS=(
    -framework CoreFoundation
    -framework CoreText
    -framework Security
    -framework VideoToolbox
    -framework CoreMedia
    -framework CoreVideo
)

SYSTEM_LIBS=(
    -lpthread
    -ldl
    -lz
    -liconv
    -lm
    -lc++
)

log "Compiling ${#SOURCES[@]} source files..."

OBJECTS=()
for src in "${SOURCES[@]}"; do
    obj="${BUILD_DIR}/$(basename "${src}" .cpp).o"
    log "  Compiling $(basename "${src}")..."
    c++ -std=c++17 -O2 -DNDEBUG \
        "${INCLUDES[@]}" \
        -c "${src}" -o "${obj}"
    OBJECTS+=("${obj}")
done

log "Linking dcpconv..."
c++ -std=c++17 -O2 \
    "${OBJECTS[@]}" \
    "${LIBS[@]}" \
    "${FRAMEWORKS[@]}" \
    "${SYSTEM_LIBS[@]}" \
    -o "${BUILD_DIR}/dcpconv"

# ============================================================================
# Post-build
# ============================================================================

step "Post-build"

# Strip debug symbols
strip "${BUILD_DIR}/dcpconv"

# Show binary info
BINARY_SIZE=$(du -h "${BUILD_DIR}/dcpconv" | cut -f1)
log "Binary: ${BUILD_DIR}/dcpconv (${BINARY_SIZE})"
log "Architecture: $(file "${BUILD_DIR}/dcpconv" | cut -d: -f2)"

# Verify no dynamic deps on our static libs
log "\nDynamic dependencies:"
otool -L "${BUILD_DIR}/dcpconv" | grep -v "/usr/lib\|/System\|dcpconv:" || true

# Optional: copy to /usr/local/bin
echo ""
echo -e "${GREEN}Build successful!${NC}"
echo ""
echo "Usage:"
echo "  ${BUILD_DIR}/dcpconv /path/to/DCP/ -o output.mp4"
echo ""
echo "To install system-wide:"
echo "  sudo cp ${BUILD_DIR}/dcpconv /usr/local/bin/"
echo ""
