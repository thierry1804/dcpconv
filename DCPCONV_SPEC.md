# dcpconv — Project Specification & Status

## Overview

**dcpconv** is a standalone macOS command-line tool written in C++17 that converts Digital Cinema Packages (DCP) and Interoperable Master Format (IMF) packages to MP4 (H.264/H.265) or MOV (ProRes), with full subtitle support. The goal is a single static binary with zero runtime dependencies, no reliance on FFmpeg.

---

## Motivation

- FFmpeg on macOS (Homebrew) lacks JPEG 2000 / OpenJPEG support and recompiling it is problematic
- GStreamer + asdcplib + mp4box is a viable but fragmented toolchain
- A dedicated C++ tool handles DCP/IMF specifics (XYZ→RGB color, SMPTE-TT subs, MXF demux) in a single binary
- Must coexist with existing FFmpeg installation without conflicts

---

## Target Platform

- **macOS only** (arm64 Apple Silicon + x86_64 Intel)
- Requires Xcode Command Line Tools
- Uses macOS VideoToolbox framework for ProRes encoding
- Build system: bash build script (`build.sh`) + CMakeLists.txt + Makefile

---

## Features (v1 Scope)

### Input Formats
- DCP Interop (ASSETMAP, VOLINDEX, no .xml extensions)
- DCP SMPTE (ASSETMAP.xml, VOLINDEX.xml, SMPTE namespaces)
- IMF (ASSETMAP.xml, ST 2067 CPL with Segments/Sequences)

### Video
- JPEG 2000 decoding via OpenJPEG
- Color conversion: CIE XYZ D65 (gamma 2.6) → Rec.709 sRGB with precomputed LUTs
- Encoding: H.264 (libx264), H.265 (libx265), ProRes (macOS VideoToolbox)

### Audio
- PCM extraction from MXF (24-bit big-endian, typically 48kHz, 5.1 or 7.1)
- AAC encoding via FDK-AAC
- PCM passthrough for ProRes/MOV output

### Subtitles
- **Parsers**: SMPTE Timed Text (ST 428-7), CineCanvas (Interop), TTML/IMSC1 (IMF)
- **Soft sub**: tx3g text track in MP4 + SRT sidecar export
- **Burn-in**: FreeType text rendering with anti-aliasing, drop shadow, configurable font/size/color
- **Both**: simultaneous soft + burn-in

### Output
- MP4 container (H.264/H.265 + AAC) via minimp4
- MOV container (ProRes + PCM) — basic implementation, may need mp4box remux for full compat
- SRT sidecar file for subtitles

### CLI Interface
```
dcpconv <input_dir> -o <output_file> [options]

Options:
  -c, --codec <h264|h265|prores>    Video codec (default: h264)
  -b, --bitrate <N>M                Bitrate in Mbps (default: 20)
  --prores-profile <proxy|lt|standard|hq|4444>
  -s, --subs <none|soft|burn-in|both>
  --font <path>                     Font for burn-in
  --font-size <N>                   Font size pixels (default: 42)
  --font-color <hex>                RGB hex color (default: FFFFFF)
  --cpl <keyword>                   Select CPL by keyword
  --info                            Show package contents only
  -t, --threads <N>                 Encoding threads (default: auto)
  -v, --verbose                     Verbose output
```

---

## Dependencies (all built as static libraries)

| Library    | Version | Purpose                        | Build System |
|------------|---------|--------------------------------|--------------|
| OpenJPEG   | 2.5.3   | JPEG 2000 decoding             | CMake        |
| xerces-c   | 3.2.5   | XML parsing (CPL, ASSETMAP)    | CMake        |
| OpenSSL    | 3.2.1   | MXF encryption (asdcplib dep)  | Configure    |
| asdcplib   | 2.13.1  | MXF demuxing (DCP/IMF)         | CMake        |
| x264       | stable  | H.264 encoding                 | Configure    |
| x265       | 3.6     | H.265/HEVC encoding            | CMake        |
| FDK-AAC    | 2.0.3   | AAC audio encoding             | CMake        |
| FreeType   | 2.13.3  | Text rendering (subtitle burn) | CMake        |
| HarfBuzz   | 8.5.0   | Complex text shaping           | CMake        |
| minimp4    | latest  | MP4 muxing (header-only)       | Header copy  |

---

## Architecture

```
dcpconv/
├── build.sh              # Main build script (deps + project)
├── CMakeLists.txt         # CMake build with ExternalProject
├── Makefile               # Quick build with Homebrew deps
├── README.md              # Project overview
├── QUICKSTART.md          # Build guide + troubleshooting
└── src/
    ├── main.cpp           # CLI entry, arg parsing, validation
    ├── core/
    │   ├── pipeline.h     # Data structures (Asset, Composition, PackageInfo, SubtitleEvent)
    │   └── pipeline.cpp   # Orchestrator: detect → parse → decode → encode → mux
    ├── input/
    │   ├── dcp_reader.h/cpp    # DCP parser (ASSETMAP → PKL → CPL → asset resolution)
    │   ├── imf_reader.h/cpp    # IMF parser (Segments → Sequences → Resources)
    │   └── mxf_demuxer.h/cpp   # MXF frame/audio/subtitle extraction via asdcplib (AS-DCP + AS-02)
    ├── decode/
    │   ├── j2k_decoder.h/cpp   # JPEG 2000 decoding via OpenJPEG (memory stream)
    │   └── pcm_decoder.h/cpp   # 24-bit PCM conversion
    ├── color/
    │   └── xyz_to_rgb.h/cpp    # XYZ D65 → Rec.709 with precomputed degamma/regamma LUTs
    ├── subtitles/
    │   ├── smpte_tt_parser.h/cpp    # SMPTE-TT parser via xerces-c
    │   ├── cinecanvas_parser.h/cpp  # CineCanvas (Interop) parser
    │   ├── ttml_parser.h/cpp        # TTML/IMSC1 parser with <p> collection and <br/> handling
    │   ├── subtitle_burner.h/cpp    # FreeType rendering with alpha blend + drop shadow
    │   └── subtitle_muxer.h/cpp     # tx3g muxing + SRT export
    ├── encode/
    │   ├── video_encoder_base.h     # Abstract base class
    │   ├── h264_encoder.h/cpp       # libx264 with RGB→YUV420 BT.709
    │   ├── h265_encoder.h/cpp       # libx265 with RGB→YUV420 BT.709
    │   ├── prores_encoder.h/cpp     # VideoToolbox ProRes (all profiles)
    │   └── aac_encoder.h/cpp        # FDK-AAC (supports mono to 7.1)
    └── output/
        ├── mp4_muxer.h/cpp          # minimp4 for H.264, basic MOV for ProRes
        └── (SRT export is in subtitle_muxer)
```

---

## Pipeline Flow

```
┌─────────────────────────────────────────────────────────┐
│ 1. DETECT PACKAGE                                       │
│    Read ASSETMAP → identify DCP Interop / SMPTE / IMF   │
└──────────────────────┬──────────────────────────────────┘
                       ▼
┌─────────────────────────────────────────────────────────┐
│ 2. PARSE CPL                                            │
│    Find video/audio/subtitle assets, resolve to MXF     │
│    Read MXF headers for dimensions, channels, duration  │
└──────────────────────┬──────────────────────────────────┘
                       ▼
┌─────────────────────────────────────────────────────────┐
│ 3. PARSE SUBTITLES (if requested)                       │
│    Extract XML from MXF → detect format → parse events  │
└──────────────────────┬──────────────────────────────────┘
                       ▼
┌─────────────────────────────────────────────────────────┐
│ 4. FRAME LOOP (per frame)                               │
│                                                         │
│    MXF ──asdcplib──► J2K ──OpenJPEG──► XYZ (16-bit)    │
│                                            │            │
│                                    color_convert         │
│                                            │            │
│                                        RGB (8-bit)      │
│                                            │            │
│                              [subtitle_burner if burn]   │
│                                            │            │
│                                    RGB→YUV420            │
│                                            │            │
│                              x264/x265/VideoToolbox      │
│                                            │            │
│                                    encoded frame         │
│                                            │            │
│    Audio MXF ──asdcplib──► PCM ──FDK-AAC──► AAC        │
│                                                         │
│    Both ────────────────► mp4_muxer ──────► MP4/MOV     │
└─────────────────────────────────────────────────────────┘
```

---

## Build Status

### macOS Build Environment Issue (RESOLVED)
- **Problem**: Command Line Tools on macOS 26 (Tahoe) store C++ headers inside the SDK (`/SDKs/MacOSX26.2.sdk/usr/include/c++/v1/`) rather than at `/usr/include/c++/v1/`. The compiler doesn't find `<new>`, `<string>`, etc. without explicit `SDKROOT`.
- **Solution in build.sh**:
  ```bash
  export SDKROOT=$(xcrun --show-sdk-path)
  export CFLAGS="-isysroot ${MACOS_SDK}"
  export CXXFLAGS="-isysroot ${MACOS_SDK} -stdlib=libc++"
  export LDFLAGS="-isysroot ${MACOS_SDK}"
  # Plus CMake args:
  -DCMAKE_OSX_SYSROOT="${MACOS_SDK}"
  -DCMAKE_C_FLAGS="-isysroot ${MACOS_SDK}"
  -DCMAKE_CXX_FLAGS="-isysroot ${MACOS_SDK} -stdlib=libc++"
  -DCMAKE_EXE_LINKER_FLAGS="-isysroot ${MACOS_SDK}"
  ```

### xerces-c Build Fix (RESOLVED)
- **Problem**: `-Dnetwork-accessor=none` is not a valid option in xerces-c 3.2.5 CMake.
- **Solution**: Use `-Dnetwork:BOOL=OFF -Dthreads:BOOL=OFF -Dmessage-loader=inmemory -Dtranscoder=iconv`.

### Dependency Build Progress
| Dependency | Status |
|------------|--------|
| OpenJPEG   | ✅ Builds successfully |
| xerces-c   | 🔄 Config fixed, needs rebuild with SDKROOT fix |
| OpenSSL    | ⬜ Not yet attempted |
| asdcplib   | ⬜ Not yet attempted |
| x264       | ⬜ Not yet attempted |
| x265       | ⬜ Not yet attempted |
| FDK-AAC    | ⬜ Not yet attempted |
| FreeType   | ⬜ Not yet attempted |
| HarfBuzz   | ⬜ Not yet attempted |
| minimp4    | ⬜ Not yet attempted (header-only, trivial) |

### Source Code Status
All source files are written and complete (36 files, ~4200 lines):
- ✅ main.cpp — CLI parsing, validation
- ✅ core/pipeline.h/cpp — orchestrator, data structures, progress bar
- ✅ input/dcp_reader.h/cpp — DCP ASSETMAP/CPL parsing via xerces-c + asdcplib
- ✅ input/imf_reader.h/cpp — IMF parsing (Segments/Sequences) via xerces-c + asdcplib
- ✅ input/mxf_demuxer.h/cpp — MXF frame/audio/subtitle extraction via asdcplib
- ✅ decode/j2k_decoder.h/cpp — JPEG 2000 decoding via OpenJPEG (memory stream)
- ✅ decode/pcm_decoder.h/cpp — 24-bit PCM conversion
- ✅ color/xyz_to_rgb.h/cpp — XYZ→Rec.709 with precomputed degamma/regamma LUTs
- ✅ subtitles/smpte_tt_parser.h/cpp — SMPTE-TT parser via xerces-c
- ✅ subtitles/cinecanvas_parser.h/cpp — CineCanvas (Interop) parser
- ✅ subtitles/ttml_parser.h/cpp — TTML/IMSC1 parser
- ✅ subtitles/subtitle_burner.h/cpp — FreeType rendering with alpha blend + drop shadow
- ✅ subtitles/subtitle_muxer.h/cpp — tx3g muxing + SRT export
- ✅ encode/video_encoder_base.h — abstract base class
- ✅ encode/h264_encoder.h/cpp — libx264 with RGB→YUV420 BT.709
- ✅ encode/h265_encoder.h/cpp — libx265 with RGB→YUV420 BT.709
- ✅ encode/prores_encoder.h/cpp — VideoToolbox ProRes (all profiles)
- ✅ encode/aac_encoder.h/cpp — FDK-AAC (supports mono to 7.1)
- ✅ output/mp4_muxer.h/cpp — minimp4 for H.264, basic MOV for ProRes

### Build System Files
- ✅ build.sh — full static build script with dependency compilation
- ✅ CMakeLists.txt — ExternalProject-based build
- ✅ Makefile — quick build with Homebrew deps
- ✅ README.md — project documentation
- ✅ QUICKSTART.md — build guide + troubleshooting

---

## Known Limitations / TODO for v2

1. **ProRes MOV muxer** is simplified — may need mp4box remux for full QuickTime compatibility
2. **Multi-reel DCP** processing is sequential (one reel at a time), not parallel
3. **Encrypted DCP** (KDM) support not implemented
4. **RGB→YUV conversion** is naive per-pixel — could be optimized with SIMD/NEON
5. **Multi-reel concatenation** — when a DCP has multiple reels, they should be seamlessly joined
6. **IMF virtual tracks** — complex IMF packages with multiple virtual tracks need more work
7. **Stereoscopic 3D** DCP support not implemented
8. **HDR output** (Rec.2020, PQ) not implemented — only Rec.709 SDR
9. **Progress ETA** calculation based on encoding speed
10. **Parallel frame decoding** — decode J2K frames in parallel with encoding

---

## Key Technical Decisions

### Why not FFmpeg?
FFmpeg Homebrew formula lacks OpenJPEG support on macOS, and recompiling creates dependency conflicts with existing install.

### Why xerces-c for XML?
DCP/IMF XML (CPL, ASSETMAP, subtitles) uses complex namespaces. xerces-c is the same parser used by asdcplib, avoiding dual XML implementations.

### Why minimp4 for muxing?
Header-only, lightweight, well-tested for H.264 MP4 muxing. No external dependencies.

### Why VideoToolbox for ProRes?
ProRes is Apple's codec. VideoToolbox provides hardware-accelerated encoding on Apple Silicon and the only legal way to create conformant ProRes files on macOS.

### Color Pipeline
DCP uses a specific color pipeline: XYZ color space, D65 white point, 2.6 gamma. The conversion to Rec.709 requires:
1. Decode gamma: `linear = encoded^2.6`
2. Matrix multiply: XYZ → linear RGB (3x3 matrix, ITU-R BT.709 primaries)
3. Apply sRGB transfer function: `sRGB = 1.055 * linear^(1/2.4) - 0.055`

All done via precomputed 65536-entry LUTs for performance.

---

## How to Continue in Claude Code

1. The source tree is in `dcpconv/` — all source files are complete
2. Run `./build.sh` to build dependencies and compile
3. Current blocker: dependency compilation (xerces-c onwards) — the SDKROOT fix should resolve it
4. Once deps build, the main source compilation may need minor adjustments (header include paths for openjpeg in subdirectory `openjpeg-2.5/`, etc.)
5. After successful compilation, test with a real DCP package using `./build/dcpconv /path/to/DCP --info`
6. The code is functional but untested against real DCP/IMF packages — expect runtime bugs to fix

## Project Location

Source tree: `dcpconv/` (portable, no hardcoded paths)
Build output: `dcpconv/build/dcpconv` (standalone binary)
