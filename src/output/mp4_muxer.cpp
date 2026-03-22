/**
 * mp4_muxer.cpp - MP4/MOV container muxer
 *
 * Uses minimp4 for H.264 muxing.
 * For H.265 and ProRes, uses a basic manual MP4 box writer.
 *
 * The MP4 format is conceptually simple:
 * ftyp → mdat (media data) → moov (metadata + sample tables)
 */

#include "mp4_muxer.h"
#define MINIMP4_IMPLEMENTATION
#include <minimp4.h>
#include <iostream>
#include <fstream>
#include <cstring>

namespace dcpconv {

struct MP4Muxer::Impl {
    Config config;
    FILE* file = nullptr;

    // minimp4 muxer (for H.264)
    MP4E_mux_t* mux = nullptr;
    mp4_h26x_writer_t h264_writer;
    bool use_minimp4 = false;

    // Track indices for minimp4
    int video_track = -1;
    int audio_track = -1;

    // Manual mdat accumulation (for H.265/ProRes)
    std::vector<uint8_t> mdat_buffer;
    struct SampleEntry {
        int64_t offset;
        int64_t size;
        int64_t pts;
        bool is_key;
    };
    std::vector<SampleEntry> video_samples;
    std::vector<SampleEntry> audio_samples;

    // Subtitle samples
    struct SubSample {
        std::vector<uint8_t> data;
        int64_t start_ms;
        int64_t end_ms;
    };
    std::vector<SubSample> subtitle_samples;
};

MP4Muxer::MP4Muxer() : impl_(std::make_unique<Impl>()) {}

MP4Muxer::~MP4Muxer() {
    if (impl_->mux) {
        MP4E_close(impl_->mux);
    }
    if (impl_->file) {
        fclose(impl_->file);
    }
}

void MP4Muxer::init(const Config& cfg) {
    impl_->config = cfg;

    impl_->file = fopen(cfg.output_path.c_str(), "wb");
    if (!impl_->file) {
        throw std::runtime_error("Cannot open output file: " + cfg.output_path);
    }

    if (cfg.is_prores) {
        // ProRes uses MOV container — we'll build manually
        impl_->use_minimp4 = false;
    } else {
        // H.264 and H.265: use minimp4 for H.264
        impl_->use_minimp4 = true;
        impl_->mux = MP4E_open(0 /* sequential */, 0 /* fragmented */,
                                impl_->file,
                                [](int64_t offset, const void* data, size_t size, void* token) -> int {
                                    FILE* f = (FILE*)token;
                                    if (fseek(f, (long)offset, SEEK_SET)) return 1;
                                    return fwrite(data, 1, size, f) != size;
                                });

        if (!impl_->mux) {
            throw std::runtime_error("Failed to create MP4 muxer");
        }

        // Initialize H.264 writer
        if (mp4_h26x_write_init(&impl_->h264_writer, impl_->mux,
                                 cfg.width, cfg.height, 0 /* h264 */)) {
            throw std::runtime_error("Failed to init H.264 writer");
        }
    }
}

void MP4Muxer::write_video(const uint8_t* data, size_t size, int64_t frame) {
    if (size == 0) return;

    if (impl_->use_minimp4) {
        // minimp4 handles NAL parsing for H.264
        int fps_num = impl_->config.fps_num;
        int fps_den = impl_->config.fps_den;
        int duration = 90000 * fps_den / fps_num; // in 90kHz timescale

        mp4_h26x_write_nal(&impl_->h264_writer, data, size, duration);
    } else {
        // Manual mdat accumulation for ProRes/H.265
        Impl::SampleEntry entry;
        entry.offset = impl_->mdat_buffer.size();
        entry.size = size;
        entry.pts = frame;
        entry.is_key = true; // ProRes is all intra

        impl_->mdat_buffer.insert(impl_->mdat_buffer.end(),
                                   data, data + size);
        impl_->video_samples.push_back(entry);
    }
}

void MP4Muxer::write_audio(const uint8_t* data, size_t size, int64_t frame) {
    if (size == 0) return;

    if (impl_->use_minimp4 && impl_->mux) {
        // Add audio track if needed
        if (impl_->audio_track < 0) {
            MP4E_track_t track_cfg;
            memset(&track_cfg, 0, sizeof(track_cfg));
            track_cfg.track_media_kind = e_audio;
            track_cfg.time_scale = impl_->config.audio_sample_rate;
            track_cfg.default_duration = 1024; // AAC frame size
            track_cfg.u.a.channelcount = impl_->config.audio_channels;

            impl_->audio_track = MP4E_add_track(impl_->mux, &track_cfg);
        }

        int duration = 1024; // AAC standard frame size
        MP4E_put_sample(impl_->mux, impl_->audio_track,
                        data, size, duration, MP4E_SAMPLE_DEFAULT);
    } else {
        Impl::SampleEntry entry;
        entry.offset = impl_->mdat_buffer.size();
        entry.size = size;
        entry.pts = frame;
        entry.is_key = true;

        impl_->mdat_buffer.insert(impl_->mdat_buffer.end(),
                                   data, data + size);
        impl_->audio_samples.push_back(entry);
    }
}

void MP4Muxer::write_subtitle(const uint8_t* data, size_t size,
                                int64_t start_ms, int64_t end_ms) {
    Impl::SubSample sample;
    sample.data.assign(data, data + size);
    sample.start_ms = start_ms;
    sample.end_ms = end_ms;
    impl_->subtitle_samples.push_back(sample);
}

void MP4Muxer::finalize() {
    if (impl_->use_minimp4) {
        // Flush H.264 writer
        mp4_h26x_write_close(&impl_->h264_writer);

        // Close muxer (writes moov)
        MP4E_close(impl_->mux);
        impl_->mux = nullptr;
    } else {
        // Write ProRes/H.265 MOV manually
        // This is a simplified MOV writer for ProRes
        // A full implementation would write proper ftyp + mdat + moov

        if (impl_->file) {
            // Write ftyp
            const char ftyp[] = {
                0,0,0,0x14, 'f','t','y','p',
                'q','t',' ',' ',
                0,0,0,0,
                'q','t',' ',' '
            };
            fwrite(ftyp, 1, sizeof(ftyp), impl_->file);

            // Write mdat
            uint64_t mdat_size = impl_->mdat_buffer.size() + 8;
            uint8_t mdat_header[8];
            mdat_header[0] = (mdat_size >> 24) & 0xFF;
            mdat_header[1] = (mdat_size >> 16) & 0xFF;
            mdat_header[2] = (mdat_size >> 8) & 0xFF;
            mdat_header[3] = mdat_size & 0xFF;
            mdat_header[4] = 'm'; mdat_header[5] = 'd';
            mdat_header[6] = 'a'; mdat_header[7] = 't';
            fwrite(mdat_header, 1, 8, impl_->file);
            fwrite(impl_->mdat_buffer.data(), 1,
                   impl_->mdat_buffer.size(), impl_->file);

            // TODO: Write moov atom with sample table
            // This is the most complex part - for a v1, consider
            // writing raw data and using mp4box to remux
            std::cout << "Note: ProRes MOV may need remuxing with mp4box "
                      << "for full compatibility.\n";
        }
    }

    // Export SRT as sidecar if we have subtitles
    if (!impl_->subtitle_samples.empty()) {
        std::string srt_path = impl_->config.output_path;
        size_t dot = srt_path.rfind('.');
        if (dot != std::string::npos)
            srt_path = srt_path.substr(0, dot) + ".srt";
        else
            srt_path += ".srt";

        std::ofstream srt(srt_path);
        for (size_t i = 0; i < impl_->subtitle_samples.size(); ++i) {
            const auto& s = impl_->subtitle_samples[i];
            srt << (i + 1) << "\n";

            auto fmt = [](int64_t ms) -> std::string {
                char buf[32];
                int h = ms / 3600000;
                int m = (ms % 3600000) / 60000;
                int sec = (ms % 60000) / 1000;
                int f = ms % 1000;
                snprintf(buf, sizeof(buf), "%02d:%02d:%02d,%03d", h, m, sec, f);
                return std::string(buf);
            };

            srt << fmt(s.start_ms) << " --> " << fmt(s.end_ms) << "\n";

            // Extract text from tx3g format (skip 2-byte length prefix)
            if (s.data.size() > 2) {
                std::string text(s.data.begin() + 2, s.data.end());
                srt << text << "\n";
            }
            srt << "\n";
        }
        std::cout << "Subtitles exported: " << srt_path << "\n";
    }

    if (impl_->file) {
        fclose(impl_->file);
        impl_->file = nullptr;
    }
}

} // namespace dcpconv
