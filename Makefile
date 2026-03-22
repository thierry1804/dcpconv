#
# dcpconv Makefile - Quick build using Homebrew dependencies
#
# For a fully static build, use build.sh instead.
# This Makefile links against Homebrew-installed libraries.
#
# Prerequisites:
#   brew install openjpeg xerces-c openssl@3 freetype harfbuzz
#   # x264, x265, fdk-aac, asdcplib must be built from source
#

CXX := c++
CXXFLAGS := -std=c++17 -O2 -DNDEBUG -Wall -Wextra
LDFLAGS :=

# Homebrew prefix (auto-detect Apple Silicon vs Intel)
BREW_PREFIX := $(shell brew --prefix 2>/dev/null || echo /usr/local)

# Include paths
INCLUDES := \
	-I$(BREW_PREFIX)/include \
	-I$(BREW_PREFIX)/include/freetype2 \
	-I$(BREW_PREFIX)/opt/openssl@3/include

# Library paths
LIBDIRS := \
	-L$(BREW_PREFIX)/lib \
	-L$(BREW_PREFIX)/opt/openssl@3/lib

# Static libraries (prefer static)
LIBS := \
	-lasdcp -lkumu -las02 \
	-lx264 -lx265 -lfdk-aac \
	-lopenjp2 \
	-lharfbuzz -lfreetype \
	-lxerces-c \
	-lssl -lcrypto

# macOS frameworks
FRAMEWORKS := \
	-framework CoreFoundation \
	-framework CoreText \
	-framework Security \
	-framework VideoToolbox \
	-framework CoreMedia \
	-framework CoreVideo

# System libraries
SYSLIBS := -lpthread -ldl -lz -liconv -lm

# Sources
SRCDIR := src
SOURCES := \
	$(SRCDIR)/main.cpp \
	$(SRCDIR)/core/pipeline.cpp \
	$(SRCDIR)/input/dcp_reader.cpp \
	$(SRCDIR)/input/imf_reader.cpp \
	$(SRCDIR)/input/mxf_demuxer.cpp \
	$(SRCDIR)/decode/j2k_decoder.cpp \
	$(SRCDIR)/decode/pcm_decoder.cpp \
	$(SRCDIR)/color/xyz_to_rgb.cpp \
	$(SRCDIR)/subtitles/smpte_tt_parser.cpp \
	$(SRCDIR)/subtitles/cinecanvas_parser.cpp \
	$(SRCDIR)/subtitles/ttml_parser.cpp \
	$(SRCDIR)/subtitles/subtitle_burner.cpp \
	$(SRCDIR)/subtitles/subtitle_muxer.cpp \
	$(SRCDIR)/encode/h264_encoder.cpp \
	$(SRCDIR)/encode/h265_encoder.cpp \
	$(SRCDIR)/encode/prores_encoder.cpp \
	$(SRCDIR)/encode/aac_encoder.cpp \
	$(SRCDIR)/output/mp4_muxer.cpp

# Build directory
BUILDDIR := build
OBJECTS := $(patsubst $(SRCDIR)/%.cpp,$(BUILDDIR)/%.o,$(SOURCES))
TARGET := $(BUILDDIR)/dcpconv

# ============================================================================

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(OBJECTS)
	@echo "Linking $@..."
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ $(LIBDIRS) $(LIBS) $(FRAMEWORKS) $(SYSLIBS) -o $@
	@echo "Done: $@ ($$(du -h $@ | cut -f1))"

$(BUILDDIR)/%.o: $(SRCDIR)/%.cpp
	@mkdir -p $(dir $@)
	@echo "Compiling $<..."
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -rf $(BUILDDIR)

install: $(TARGET)
	sudo cp $(TARGET) /usr/local/bin/dcpconv
	@echo "Installed to /usr/local/bin/dcpconv"

# Show dep check
check-deps:
	@echo "Checking dependencies..."
	@for lib in openjpeg xerces-c openssl@3 freetype harfbuzz; do \
		if brew list $$lib &>/dev/null; then \
			echo "  ✓ $$lib"; \
		else \
			echo "  ✗ $$lib (missing)"; \
		fi; \
	done
	@echo ""
	@echo "Manual deps (check if installed):"
	@for lib in libasdcp.a libx264.a libx265.a libfdk-aac.a; do \
		if [ -f "$(BREW_PREFIX)/lib/$$lib" ] || [ -f "/usr/local/lib/$$lib" ]; then \
			echo "  ✓ $$lib"; \
		else \
			echo "  ✗ $$lib (build from source)"; \
		fi; \
	done
