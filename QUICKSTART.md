# dcpconv — Quick Start & Troubleshooting

## TL;DR

```bash
cd dcpconv
chmod +x build.sh
./build.sh
```

This downloads, compiles, and links everything into a single binary:
`build/dcpconv`

Takes ~15-30 minutes on first build (dependencies are cached for rebuilds).

## Prerequisites

Only two things needed before running `build.sh`:

1. **Xcode Command Line Tools**
   ```bash
   xcode-select --install
   ```

2. **CMake** (if not already installed)
   ```bash
   brew install cmake
   ```

Optional but recommended:
```bash
brew install nasm    # SIMD optimizations for x264/x265
brew install git     # if not already present
```

## Build Options

### Option 1: Full Static Build (recommended)

```bash
./build.sh
```

Produces a standalone binary with zero runtime dependencies.
Safe for any Mac, no conflict with existing FFmpeg or other tools.

### Option 2: Quick Build with Homebrew deps

If you already have some dependencies installed via Homebrew:

```bash
make check-deps    # see what's installed
make -j8           # build
```

Note: This links dynamically — the binary depends on Homebrew libs.

### Option 3: CMake Build

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(sysctl -n hw.ncpu)
```

Uses CMake ExternalProject to build all deps. Most robust but slowest
first build.

## Verify Installation

```bash
./build/dcpconv --help

# Check it has no unwanted dependencies
otool -L ./build/dcpconv
# Should only show /usr/lib/* and /System/Library/*
```

## Common Build Issues

### "No ASSETMAP found"
Your input directory is not a valid DCP/IMF package. A valid DCP has:
```
my_dcp/
├── ASSETMAP.xml (or ASSETMAP)
├── VOLINDEX.xml (or VOLINDEX)
├── CPL_*.xml
├── PKL_*.xml
├── video.mxf
├── audio.mxf
└── subtitle.mxf (optional)
```

### OpenSSL build fails on Apple Silicon
Make sure the architecture detection is correct:
```bash
uname -m
# Should show arm64 for Apple Silicon, x86_64 for Intel
```

If building for Rosetta, force the architecture:
```bash
arch -x86_64 ./build.sh
```

### x264 configure fails
Usually missing nasm:
```bash
brew install nasm
```

### asdcplib cmake can't find OpenSSL/xerces
The build script should handle this, but if building manually:
```bash
cmake .. -DOPENSSL_ROOT_DIR=/path/to/deps \
         -DXERCES_ROOT=/path/to/deps \
         -DCMAKE_PREFIX_PATH=/path/to/deps
```

### "No system font found for subtitle rendering"
The burn-in renderer looks for fonts at:
- /System/Library/Fonts/Helvetica.ttc
- /System/Library/Fonts/HelveticaNeue.ttc
- /Library/Fonts/Arial.ttf

Specify a custom font:
```bash
dcpconv /path/to/DCP -o out.mp4 -s burn-in --font /path/to/font.ttf
```

### VideoToolbox error for ProRes
ProRes encoding requires macOS 10.13+ and a compatible GPU for
hardware acceleration. On older Macs or CI:
```bash
# Use H.264 instead
dcpconv /path/to/DCP -o out.mp4 -c h264
```

## Updating Dependencies

To rebuild a specific dependency:
```bash
cd build/deps/src/openjpeg
git pull
cd build && make -j8 && make install
```

Then rebuild dcpconv:
```bash
cd /path/to/dcpconv/build
make -j8
```

## Performance Tips

- **Threads**: By default, x264/x265 use all cores. Override with `-t N`
- **Bitrate**: Default 20 Mbps. For archival: `-b 50M`. For preview: `-b 5M`
- **Speed**: Use `-c h264` with default preset for fastest encoding.
  ProRes is faster (intra-only) but files are much larger.
- **4K DCP**: Expect ~2-5 fps encoding speed on a modern Mac.
  A 2h 4K film takes ~2-4 hours to convert.

## Architecture Notes

The tool is designed as a pipeline:

```
MXF → asdcplib → J2K frames → OpenJPEG → XYZ pixels
                                              ↓
                                    XYZ → RGB (BT.709)
                                              ↓
                              [subtitle burn-in via FreeType]
                                              ↓
                              RGB → YUV → x264/x265/ProRes
                                              ↓
                              encoded frames → minimp4 → .mp4/.mov
```

Audio follows a parallel path:
```
MXF → asdcplib → 24-bit PCM → FDK-AAC → AAC frames → mp4 mux
```

Subtitles:
```
MXF → asdcplib → XML → parser → SubtitleEvent list
                                       ↓
                          burn-in: FreeType renders onto RGB frames
                          soft: tx3g samples muxed into MP4
                          both: both paths simultaneously
```
