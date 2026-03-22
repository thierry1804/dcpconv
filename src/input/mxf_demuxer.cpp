/**
 * mxf_demuxer.cpp - MXF essence extraction via asdcplib
 */

#include "mxf_demuxer.h"

#include <AS_DCP.h>
#include <AS_02.h>
#include <KM_fileio.h>
#include <MXF.h>
#include <Metadata.h>

#include <iostream>
#include <fstream>

namespace dcpconv {

struct MXFDemuxer::Impl {
    Kumu::FileReaderFactory factory;

    // DCP (AS-DCP) readers
    ASDCP::JP2K::MXFReader j2k_reader{factory};
    ASDCP::PCM::MXFReader pcm_reader{factory};

    // IMF (AS-02) readers
    AS_02::JP2K::MXFReader j2k_reader_02{factory};
    AS_02::PCM::MXFReader pcm_reader_02{factory};

    bool is_as02 = false;  // true for IMF
    bool is_open = false;

    ASDCP::JP2K::FrameBuffer j2k_buf;
    ASDCP::PCM::FrameBuffer pcm_buf;
};

MXFDemuxer::MXFDemuxer() : impl_(std::make_unique<Impl>()) {}
MXFDemuxer::~MXFDemuxer() = default;

void MXFDemuxer::open(const std::string& filepath, Asset::Type type) {
    type_ = type;
    current_frame_ = 0;

    if (type == Asset::Type::VIDEO) {
        // Try AS-02 (IMF) first, fall back to AS-DCP (DCP)
        if (ASDCP_SUCCESS(impl_->j2k_reader_02.OpenRead(filepath.c_str()))) {
            impl_->is_as02 = true;
            // AS-02 has no FillPictureDescriptor — just allocate max buffer
            impl_->j2k_buf.Capacity(16 * 1024 * 1024); // 16MB max per frame
        } else if (ASDCP_SUCCESS(impl_->j2k_reader.OpenRead(filepath.c_str()))) {
            impl_->is_as02 = false;
            ASDCP::JP2K::PictureDescriptor desc;
            impl_->j2k_reader.FillPictureDescriptor(desc);
            impl_->j2k_buf.Capacity(16 * 1024 * 1024);
        } else {
            throw std::runtime_error("Cannot open MXF video: " + filepath);
        }
    } else if (type == Asset::Type::AUDIO) {
        ASDCP::Rational edit_rate(24, 1); // Will be overridden
        if (ASDCP_SUCCESS(impl_->pcm_reader_02.OpenRead(filepath.c_str(), edit_rate))) {
            impl_->is_as02 = true;
            // AS-02 has no FillAudioDescriptor — read from MXF header
            ASDCP::MXF::OP1aHeader& header = impl_->pcm_reader_02.OP1aHeader();
            std::list<ASDCP::MXF::InterchangeObject*> obj_list;
            header.GetMDObjectsByType(
                ASDCP::DefaultSMPTEDict().Type(ASDCP::MDD_WaveAudioDescriptor).ul,
                obj_list);
            uint32_t frame_bytes = 48000 * 3 * 6; // default: 48kHz, 24-bit, 6ch per frame
            for (auto* obj : obj_list) {
                auto* wd = dynamic_cast<ASDCP::MXF::WaveAudioDescriptor*>(obj);
                if (wd) {
                    frame_bytes = AS_02::MXF::CalcFrameBufferSize(*wd, edit_rate);
                    break;
                }
            }
            impl_->pcm_buf.Capacity(frame_bytes);
        } else if (ASDCP_SUCCESS(impl_->pcm_reader.OpenRead(filepath.c_str()))) {
            impl_->is_as02 = false;
            ASDCP::PCM::AudioDescriptor desc;
            impl_->pcm_reader.FillAudioDescriptor(desc);
            uint32_t frame_bytes = ASDCP::PCM::CalcFrameBufferSize(desc);
            impl_->pcm_buf.Capacity(frame_bytes);
        } else {
            throw std::runtime_error("Cannot open MXF audio: " + filepath);
        }
    }

    impl_->is_open = true;
}

bool MXFDemuxer::read_frame(std::vector<uint8_t>& out) {
    if (!impl_->is_open || type_ != Asset::Type::VIDEO) return false;

    ASDCP::Result_t result = impl_->is_as02
        ? impl_->j2k_reader_02.ReadFrame(current_frame_, impl_->j2k_buf)
        : impl_->j2k_reader.ReadFrame(current_frame_, impl_->j2k_buf);

    if (!ASDCP_SUCCESS(result)) return false;

    out.assign(impl_->j2k_buf.RoData(),
               impl_->j2k_buf.RoData() + impl_->j2k_buf.Size());
    current_frame_++;
    return true;
}

bool MXFDemuxer::read_audio(std::vector<uint8_t>& out, int num_samples) {
    if (!impl_->is_open || type_ != Asset::Type::AUDIO) return false;

    ASDCP::Result_t result = impl_->is_as02
        ? impl_->pcm_reader_02.ReadFrame(current_frame_, impl_->pcm_buf)
        : impl_->pcm_reader.ReadFrame(current_frame_, impl_->pcm_buf);

    if (!ASDCP_SUCCESS(result)) return false;

    out.assign(impl_->pcm_buf.RoData(),
               impl_->pcm_buf.RoData() + impl_->pcm_buf.Size());
    current_frame_++;
    return true;
}

std::string MXFDemuxer::extract_subtitle_xml(const std::string& filepath) {
    // Try SMPTE timed text
    Kumu::FileReaderFactory ttFactory;
    ASDCP::TimedText::MXFReader tt_reader(ttFactory);
    if (ASDCP_SUCCESS(tt_reader.OpenRead(filepath.c_str()))) {
        ASDCP::TimedText::FrameBuffer buf;
        buf.Capacity(4 * 1024 * 1024); // 4MB

        std::string xml;
        ASDCP::TimedText::TimedTextDescriptor desc;
        tt_reader.FillTimedTextDescriptor(desc);

        ASDCP::TimedText::FrameBuffer xml_buf;
        xml_buf.Capacity(4 * 1024 * 1024);

        if (ASDCP_SUCCESS(tt_reader.ReadTimedTextResource(xml_buf))) {
            xml.assign(reinterpret_cast<const char*>(xml_buf.RoData()),
                       xml_buf.Size());
            return xml;
        }
    }

    // Try AS-02 timed text (IMF)
    AS_02::TimedText::MXFReader tt_reader_02(ttFactory);
    if (ASDCP_SUCCESS(tt_reader_02.OpenRead(filepath.c_str()))) {
        ASDCP::TimedText::FrameBuffer xml_buf;
        xml_buf.Capacity(4 * 1024 * 1024);

        if (ASDCP_SUCCESS(tt_reader_02.ReadTimedTextResource(xml_buf))) {
            std::string xml;
            xml.assign(reinterpret_cast<const char*>(xml_buf.RoData()),
                       xml_buf.Size());
            return xml;
        }
    }

    return "";
}

void MXFDemuxer::seek(int64_t frame) {
    current_frame_ = frame;
}

} // namespace dcpconv
