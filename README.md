# DCPConv — DCP/IMF to MP4/ProRes Converter

A standalone command-line tool for macOS (Apple Silicon & Intel) that converts Digital Cinema Packages (DCP) and Interoperable Master Format (IMF) packages to MP4 (H.264/H.265) or MOV (ProRes), with full subtitle support.

## Features

- **Input formats**: DCP (Interop & SMPTE), IMF (ST 2067)
- **Video decoding**: JPEG 2000 via OpenJPEG
- **Color conversion**: XYZ D65 → Rec.709 with 2.6 gamma decode / sRGB encode
- **Video encoding**: H.264 (libx264), H.265 (libx265), ProRes (VideoToolbox on macOS)
- **Audio**: PCM extraction from MXF, encoding to AAC (FDK-AAC) or PCM passthrough
- **Subtitles**: SMPTE Timed Text, CineCanvas (Interop), IMSC1/TTML (IMF)
  - Soft subtitles (embedded text track in MP4)
  - Burn-in (rendered onto video via FreeType + HarfBuzz)
- **Output**: MP4 (H.264/H.265 + AAC) or MOV (ProRes + PCM)
- **Standalone**: Single static binary, no runtime dependencies

## Dependencies (all built statically by build.sh)

| Library    | Version  | Purpose                              |
|------------|----------|--------------------------------------|
| OpenJPEG   | 2.5.x    | JPEG 2000 decoding                   |
| xerces-c   | 3.2.x    | XML parsing (CPL, ASSETMAP, subtitles) |
| OpenSSL    | 3.x      | MXF encryption support (libcrypto)   |
| asdcplib   | latest   | MXF demuxing (AS-DCP for DCP, AS-02 for IMF) |
| x264       | stable   | H.264 video encoding                 |
| x265       | 3.6      | H.265/HEVC video encoding            |
| FDK-AAC    | 2.0.3    | AAC audio encoding                   |
| FreeType   | 2.13.x   | Font rasterization for subtitle burn-in |
| HarfBuzz   | 10.x     | Complex text shaping for subtitles   |
| minimp4    | latest   | Lightweight MP4/MOV container muxing |

## Prerequisites

- macOS 13+ (Ventura or later recommended)
- Xcode Command Line Tools (`xcode-select --install`)
- CMake 3.16+ (`brew install cmake`)
- autotools (`brew install automake autoconf libtool`) — required for FDK-AAC
- Git

## Build

The project uses a self-contained `build.sh` script that downloads, compiles, and statically links all dependencies. No system libraries are required beyond the macOS SDK.

```bash
# Extract the source archive
unzip dcpconv-src.zip -d dcpconv
cd dcpconv

# Build everything (dependencies + dcpconv binary)
chmod +x build.sh
./build.sh

# Result: standalone binary
./build/dcpconv --help
```

The build script will:
1. Detect the macOS SDK and architecture (arm64/x86_64)
2. Download and compile all 10 dependencies as static libraries
3. Compile 18 source files and link them into a single binary
4. Output the binary at `./build/dcpconv`

Build time is approximately 10-15 minutes on first run (mostly x265). Subsequent runs skip already-built dependencies.

## Usage

```bash
# Basic DCP to MP4 (H.264, 15 Mbps default)
dcpconv /path/to/DCP/ -o output.mp4

# DCP to H.265 with custom bitrate
dcpconv /path/to/DCP/ -o output.mp4 --codec h265 --bitrate 30M

# IMF to ProRes HQ
dcpconv /path/to/IMF/ -o output.mov --codec prores --profile hq

# With subtitle burn-in
dcpconv /path/to/DCP/ -o output.mp4 --subs burn-in --font "Helvetica" --font-size 42

# With soft subtitles (embedded text track)
dcpconv /path/to/DCP/ -o output.mp4 --subs soft

# Both soft + burn-in subtitles
dcpconv /path/to/DCP/ -o output.mp4 --subs both

# Select specific CPL (if package contains multiple compositions)
dcpconv /path/to/DCP/ -o output.mp4 --cpl "feature"

# List package contents without converting
dcpconv /path/to/DCP/ --info

# Multi-threaded encoding
dcpconv /path/to/DCP/ -o output.mp4 --threads 8
```

## Architecture

```
dcpconv/
├── build.sh                     # Self-contained build script (deps + binary)
├── src/
│   ├── main.cpp                 # CLI entry point, argument parsing
│   ├── core/
│   │   └── pipeline.cpp/h       # Conversion pipeline orchestrator
│   ├── input/
│   │   ├── dcp_reader.cpp/h     # DCP package parser (ASSETMAP, CPL)
│   │   ├── imf_reader.cpp/h     # IMF package parser (Segments, Sequences)
│   │   └── mxf_demuxer.cpp/h    # MXF essence extraction (AS-DCP & AS-02)
│   ├── decode/
│   │   ├── j2k_decoder.cpp/h    # JPEG 2000 → raw pixels (OpenJPEG)
│   │   └── pcm_decoder.cpp/h    # PCM audio frame extraction
│   ├── color/
│   │   └── xyz_to_rgb.cpp/h     # CIE XYZ D65 → Rec.709 RGB conversion
│   ├── subtitles/
│   │   ├── smpte_tt_parser.cpp/h    # SMPTE Timed Text (SMPTE DCP)
│   │   ├── cinecanvas_parser.cpp/h  # CineCanvas (Interop DCP)
│   │   ├── ttml_parser.cpp/h        # TTML/IMSC1 (IMF)
│   │   ├── subtitle_burner.cpp/h    # Text rendering via FreeType/HarfBuzz
│   │   └── subtitle_muxer.cpp/h     # Soft subtitle track creation
│   ├── encode/
│   │   ├── h264_encoder.cpp/h   # H.264 encoding (libx264)
│   │   ├── h265_encoder.cpp/h   # H.265 encoding (libx265)
│   │   ├── prores_encoder.cpp/h # ProRes encoding (VideoToolbox)
│   │   └── aac_encoder.cpp/h    # AAC encoding (FDK-AAC)
│   └── output/
│       └── mp4_muxer.cpp/h      # MP4/MOV container muxing (minimp4)
└── CMakeLists.txt
```

## Pipeline Flow

```
DCP/IMF Package
    │
    ├── ASSETMAP.xml ──> dcp_reader / imf_reader ──> Composition
    │                                                     │
    ├── Video MXF ──> mxf_demuxer ──> J2K frames          │
    │                                    │                │
    │                              j2k_decoder            │
    │                                    │                │
    │                              xyz_to_rgb             │
    │                                    │                │
    │                         h264/h265/prores_encoder    │
    │                                    │                │
    ├── Audio MXF ──> mxf_demuxer ──> PCM ──> aac_encoder │
    │                                              │      │
    ├── Subtitle XML ──> smpte_tt/cinecanvas/ttml  │      │
    │                         │                    │      │
    │                   subtitle_burner            │      │
    │                   subtitle_muxer             │      │
    │                         │                    │      │
    └─────────────────────────┴────────────────────┘      │
                              │                           │
                         mp4_muxer ──> output.mp4/.mov    │
                              │                           │
                           pipeline <─────────────────────┘
```

## Technical Notes

- **asdcplib API**: Uses `Kumu::FileReaderFactory` for MXF reader construction. AS-DCP readers (DCP) support `FillPictureDescriptor`/`FillAudioDescriptor`; AS-02 readers (IMF) read metadata via `OP1aHeader()` MXF descriptors.
- **x265**: Built with `-DCMAKE_CXX_STANDARD=14` to avoid C++17 `register` keyword issues.
- **CMake compatibility**: Uses `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` for projects with old `cmake_minimum_required`.
- **ProRes**: Uses macOS VideoToolbox hardware encoder when available; the `Impl` struct is public to allow the VT output callback access.

## License

This project is provided as-is for professional DCP/IMF workflow use.
