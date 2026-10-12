// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>

#include "common/logging/log.h"
#include "core/libraries/ngs2/ngs2_codec.h"
#include "core/libraries/ngs2/ngs2_hevag_table.h"

extern "C" {
#include <libatrac9.h>
}

namespace Libraries::Ngs2 {

namespace {

constexpr size_t VagFrameSize = 16;
constexpr size_t VagFrameSamples = 28;

/// Bytes per sample of a PCM type, 0 for anything else.
u32 PcmSampleSize(u32 type) {
    switch (type) {
    case WaveformPcmI8:
    case WaveformPcmU8:
        return 1;
    case WaveformPcmI16L:
    case WaveformPcmI16B:
        return 2;
    case WaveformPcmI24L:
    case WaveformPcmI24B:
        return 3;
    case WaveformPcmI32L:
    case WaveformPcmI32B:
    case WaveformPcmF32L:
    case WaveformPcmF32B:
        return 4;
    case WaveformPcmF64L:
    case WaveformPcmF64B:
        return 8;
    default:
        return 0;
    }
}

float ReadPcm(u32 type, const u8* p) {
    switch (type) {
    case WaveformPcmI8:
        return static_cast<s8>(p[0]) / 128.0f;
    case WaveformPcmU8:
        return (static_cast<int>(p[0]) - 128) / 128.0f;
    case WaveformPcmI16L:
        return static_cast<s16>(p[0] | (p[1] << 8)) / 32768.0f;
    case WaveformPcmI16B:
        return static_cast<s16>(p[1] | (p[0] << 8)) / 32768.0f;
    case WaveformPcmI24L:
        return static_cast<s32>((p[0] << 8) | (p[1] << 16) | (static_cast<u32>(p[2]) << 24)) /
               2147483648.0f;
    case WaveformPcmI24B:
        return static_cast<s32>((p[2] << 8) | (p[1] << 16) | (static_cast<u32>(p[0]) << 24)) /
               2147483648.0f;
    case WaveformPcmI32L:
        return static_cast<s32>(p[0] | (p[1] << 8) | (p[2] << 16) |
                                (static_cast<u32>(p[3]) << 24)) /
               2147483648.0f;
    case WaveformPcmI32B:
        return static_cast<s32>(p[3] | (p[2] << 8) | (p[1] << 16) |
                                (static_cast<u32>(p[0]) << 24)) /
               2147483648.0f;
    case WaveformPcmF32L: {
        float value;
        std::memcpy(&value, p, sizeof(value));
        return value;
    }
    case WaveformPcmF32B: {
        const u8 swapped[4] = {p[3], p[2], p[1], p[0]};
        float value;
        std::memcpy(&value, swapped, sizeof(value));
        return value;
    }
    case WaveformPcmF64L: {
        double value;
        std::memcpy(&value, p, sizeof(value));
        return static_cast<float>(value);
    }
    case WaveformPcmF64B: {
        u8 swapped[8];
        for (int i = 0; i < 8; ++i) {
            swapped[i] = p[7 - i];
        }
        double value;
        std::memcpy(&value, swapped, sizeof(value));
        return static_cast<float>(value);
    }
    default:
        return 0.0f;
    }
}

} // namespace

namespace {

u16 Read16(const u8* p) {
    return static_cast<u16>(p[0] | (p[1] << 8));
}

u32 Read32(const u8* p) {
    return static_cast<u32>(p[0] | (p[1] << 8) | (p[2] << 16)) | (static_cast<u32>(p[3]) << 24);
}

u32 Read32BE(const u8* p) {
    return (static_cast<u32>(p[0]) << 24) | static_cast<u32>((p[1] << 16) | (p[2] << 8) | p[3]);
}

// KSDATAFORMAT_SUBTYPE_ATRAC9 {47E142D2-36BA-4D8D-88FC-61654F8C836C}, as stored.
constexpr u8 Atrac9Guid[16] = {0xD2, 0x42, 0xE1, 0x47, 0xBA, 0x36, 0x8D, 0x4D,
                               0x88, 0xFC, 0x61, 0x65, 0x4F, 0x8C, 0x83, 0x6C};

void SetSingleBlock(OrbisNgs2WaveformInfo* info, u32 skip) {
    info->numBlocks = 1;
    auto& block = info->aBlock[0];
    block.dataOffset = info->dataOffset;
    block.dataSize = info->dataSize;
    block.numRepeats = 0;
    block.numSkipSamples = skip;
    block.numSamples = info->numSamples;
    block.reserved = 0;
    block.userData = 0;
}

bool ParseRiff(const u8* data, size_t size, OrbisNgs2WaveformInfo* info) {
    if (size < 12 || std::memcmp(data, "RIFF", 4) != 0 || std::memcmp(data + 8, "WAVE", 4) != 0) {
        return false;
    }
    const u8* fmt = nullptr;
    u32 fmt_size = 0;
    const u8* fact = nullptr;
    u32 fact_size = 0;
    const u8* smpl = nullptr;
    u32 smpl_size = 0;
    size_t data_at = 0;
    u32 data_size = 0;
    size_t at = 12;
    while (at + 8 <= size) {
        const u8* chunk = data + at;
        const u32 chunk_size = Read32(chunk + 4);
        const u8* body = chunk + 8;
        const u32 available = static_cast<u32>(std::min<size_t>(chunk_size, size - (at + 8)));
        if (std::memcmp(chunk, "fmt ", 4) == 0) {
            fmt = body;
            fmt_size = available;
        } else if (std::memcmp(chunk, "fact", 4) == 0) {
            fact = body;
            fact_size = available;
        } else if (std::memcmp(chunk, "smpl", 4) == 0) {
            smpl = body;
            smpl_size = available;
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            data_at = at + 8;
            // A header read ahead of streaming comes without all of its data: the chunk's own
            // size counts, not what is in memory.
            data_size = chunk_size;
            break;
        }
        at += 8 + static_cast<size_t>(chunk_size) + (chunk_size & 1);
    }
    if (fmt == nullptr || fmt_size < 16 || data_at == 0) {
        return false;
    }

    u16 tag = Read16(fmt);
    const u32 channels = Read16(fmt + 2);
    const u32 rate = Read32(fmt + 4);
    const u32 block_align = Read16(fmt + 12);
    const u32 bits = Read16(fmt + 14);
    bool atrac9 = false;
    u32 config = 0;
    if (tag == 0xFFFE && fmt_size >= 40) {
        if (std::memcmp(fmt + 24, Atrac9Guid, 16) == 0 && fmt_size >= 48) {
            atrac9 = true;
            config = Read32BE(fmt + 44);
        } else {
            tag = Read16(fmt + 24); // the PCM or float sub-format
        }
    }

    info->format.numChannels = channels;
    info->format.sampleRate = rate;
    info->dataOffset = static_cast<u32>(data_at);
    info->dataSize = data_size;
    u32 delay = 0;

    if (atrac9) {
        u8 bytes[4] = {static_cast<u8>(config >> 24), static_cast<u8>(config >> 16),
                       static_cast<u8>(config >> 8), static_cast<u8>(config)};
        void* handle = Atrac9GetHandle();
        Atrac9CodecInfo codec{};
        const bool ok = handle != nullptr && Atrac9InitDecoder(handle, bytes) == 0 &&
                        Atrac9GetCodecInfo(handle, &codec) == 0 && codec.framesInSuperframe > 0;
        if (handle != nullptr) {
            Atrac9ReleaseHandle(handle);
        }
        if (!ok) {
            return false;
        }
        info->format.waveformType = WaveformAtrac9;
        info->format.configData = config;
        info->audioFrameSize = static_cast<u32>(codec.superframeSize);
        info->numAudioFrameSamples =
            static_cast<u32>(codec.frameSamples * codec.framesInSuperframe);
        info->audioUnitSize = static_cast<u32>(codec.superframeSize / codec.framesInSuperframe);
        info->numAudioUnitSamples = static_cast<u32>(codec.frameSamples);
        info->numAudioUnitPerFrame = static_cast<u32>(codec.framesInSuperframe);
        // fact: the total samples, then for ATRAC9 the samples the encoder put in front in
        // the third value.
        if (fact != nullptr && fact_size >= 12) {
            delay = Read32(fact + 8);
        } else if (fact != nullptr && fact_size >= 8) {
            delay = Read32(fact + 4);
        }
        info->numSamples = fact != nullptr && fact_size >= 4
                               ? Read32(fact)
                               : data_size / std::max<u32>(info->audioFrameSize, 1) *
                                     info->numAudioFrameSamples;
    } else {
        u32 type = WaveformNone;
        if (tag == 1) {
            type = bits == 8    ? WaveformPcmU8
                   : bits == 16 ? WaveformPcmI16L
                   : bits == 24 ? WaveformPcmI24L
                   : bits == 32 ? WaveformPcmI32L
                                : WaveformNone;
        } else if (tag == 3) {
            type = bits == 32 ? WaveformPcmF32L : bits == 64 ? WaveformPcmF64L : WaveformNone;
        }
        if (type == WaveformNone || channels == 0 || block_align == 0) {
            return false;
        }
        info->format.waveformType = type;
        info->audioUnitSize = block_align;
        info->numAudioUnitSamples = 1;
        info->numAudioUnitPerFrame = 1;
        info->audioFrameSize = block_align;
        info->numAudioFrameSamples = 1;
        info->numSamples = data_size / block_align;
    }
    info->numDelaySamples = delay;

    // smpl: the loop count at 28, the first loop's start and end at 36 + 8 and 36 + 12.
    if (smpl != nullptr && smpl_size >= 36 + 24 && Read32(smpl + 28) > 0) {
        info->loopBeginPosition = Read32(smpl + 36 + 8);
        info->loopEndPosition = Read32(smpl + 36 + 12);
    }
    SetSingleBlock(info, delay);
    return true;
}

bool ParseVag(const u8* data, size_t size, OrbisNgs2WaveformInfo* info) {
    if (size < 0x30 || std::memcmp(data, "VAGp", 4) != 0) {
        return false;
    }
    const u32 data_size = Read32BE(data + 0x0C);
    const u32 channels = std::max<u32>(data[0x1E], 1);
    info->format.waveformType = WaveformVag;
    info->format.numChannels = channels;
    info->format.sampleRate = Read32BE(data + 0x10);
    info->dataOffset = 0x30;
    info->dataSize = data_size;
    info->audioUnitSize = static_cast<u32>(VagFrameSize * channels);
    info->numAudioUnitSamples = static_cast<u32>(VagFrameSamples);
    info->numAudioUnitPerFrame = 1;
    info->audioFrameSize = info->audioUnitSize;
    info->numAudioFrameSamples = info->numAudioUnitSamples;
    info->numSamples = data_size / info->audioUnitSize * static_cast<u32>(VagFrameSamples);
    SetSingleBlock(info, 0);
    return true;
}

} // namespace

bool ParseWaveformHeader(const u8* data, size_t size, OrbisNgs2WaveformInfo* info) {
    if (data == nullptr || info == nullptr) {
        return false;
    }
    std::memset(info, 0, sizeof(*info));
    return ParseRiff(data, size, info) || ParseVag(data, size, info);
}

WaveformDecoder::~WaveformDecoder() {
    Release();
}

void WaveformDecoder::Release() {
    if (atrac9 != nullptr) {
        Atrac9ReleaseHandle(atrac9);
        atrac9 = nullptr;
    }
}

bool WaveformDecoder::Setup(const OrbisNgs2WaveformFormat& format) {
    Release();
    type = WaveformNone;
    channels = 0;
    if (format.numChannels == 0 || format.numChannels > ORBIS_NGS2_MAX_VOICE_CHANNELS) {
        return false;
    }

    if (format.waveformType == WaveformAtrac9) {
        // The configuration travels as the number its four bytes spell.
        atrac9_config[0] = static_cast<u8>(format.configData >> 24);
        atrac9_config[1] = static_cast<u8>(format.configData >> 16);
        atrac9_config[2] = static_cast<u8>(format.configData >> 8);
        atrac9_config[3] = static_cast<u8>(format.configData);
        atrac9 = Atrac9GetHandle();
        Atrac9CodecInfo info{};
        if (atrac9 == nullptr || Atrac9InitDecoder(atrac9, atrac9_config) != 0 ||
            Atrac9GetCodecInfo(atrac9, &info) != 0 || info.framesInSuperframe <= 0 ||
            info.channels <= 0 || info.channels > ORBIS_NGS2_MAX_VOICE_CHANNELS) {
            LOG_ERROR(Lib_Ngs2, "unusable ATRAC9 configuration {:#010x}", format.configData);
            Release();
            return false;
        }
        superframe_size = info.superframeSize;
        frames_in_superframe = info.framesInSuperframe;
        frame_samples = info.frameSamples;
        channels = static_cast<u32>(info.channels);
    } else if (format.waveformType == WaveformVag || PcmSampleSize(format.waveformType) != 0) {
        channels = format.numChannels;
    } else {
        LOG_ERROR(Lib_Ngs2, "unknown waveform type {:#x}", format.waveformType);
        return false;
    }
    type = format.waveformType;
    Restart();
    return true;
}

void WaveformDecoder::Restart() {
    std::memset(vag_history, 0, sizeof(vag_history));
    frame_index = 0;
    superframe_remaining = superframe_size;
    if (type == WaveformAtrac9 && atrac9 != nullptr) {
        // The decoder overlaps each frame with the one before it; starting clean keeps the
        // tail of unrelated audio out of the first frame.
        Atrac9ReleaseHandle(atrac9);
        atrac9 = Atrac9GetHandle();
        if (atrac9 != nullptr) {
            Atrac9InitDecoder(atrac9, atrac9_config);
        }
    }
}

size_t WaveformDecoder::DecodeFrame(const u8* data, size_t size, std::vector<float>& out) {
    if (type == WaveformAtrac9) {
        if (atrac9 == nullptr || size < static_cast<size_t>(superframe_remaining)) {
            return 0;
        }
        const size_t start = out.size();
        out.resize(start + static_cast<size_t>(frame_samples) * channels);
        int used = 0;
        if (Atrac9DecodeF32(atrac9, data, out.data() + start, &used, 0) != 0 || used <= 0 ||
            used > superframe_remaining) {
            // A frame that does not decode is played as silence, the stream stays in step.
            std::fill(out.begin() + static_cast<std::ptrdiff_t>(start), out.end(), 0.0f);
            used = superframe_remaining;
            frame_index = frames_in_superframe - 1;
        }
        size_t consumed = static_cast<size_t>(used);
        superframe_remaining -= used;
        if (++frame_index >= frames_in_superframe) {
            // Whatever is left of the superframe is padding.
            consumed += static_cast<size_t>(superframe_remaining);
            superframe_remaining = superframe_size;
            frame_index = 0;
        }
        return consumed;
    }

    if (type == WaveformVag) {
        // One 16-byte frame per channel in turn.
        if (size < VagFrameSize * channels) {
            return 0;
        }
        const size_t start = out.size();
        out.resize(start + VagFrameSamples * channels);
        for (u32 channel = 0; channel < channels; ++channel) {
            const u8* frame = data + VagFrameSize * channel;
            u32 predictor = ((frame[0] >> 4) & 0xf) | (frame[1] & 0xf0);
            const u32 shift = frame[0] & 0xf;
            const u32 flag = frame[1] & 0xf;
            if (predictor > 127) {
                predictor = 0;
            }
            const float* coefficients = HevagCoefficients[predictor];
            float* history = vag_history[channel];
            for (size_t i = 0; i < VagFrameSamples; ++i) {
                float sample = 0.0f;
                // Flag 7 marks a frame that is there to be skipped.
                if (flag < 7) {
                    const u8 byte = frame[2 + i / 2];
                    const int nibble = (i & 1) != 0 ? byte >> 4 : byte & 0xf;
                    const int code = ((nibble ^ 8) - 8) * 4096;
                    sample = static_cast<float>(shift < 32 ? code >> shift : 0) +
                             history[0] * coefficients[0] + history[1] * coefficients[1] +
                             history[2] * coefficients[2] + history[3] * coefficients[3];
                }
                history[3] = history[2];
                history[2] = history[1];
                history[1] = history[0];
                history[0] = sample;
                out[start + i * channels + channel] =
                    std::clamp(sample, -32768.0f, 32767.0f) / 32768.0f;
            }
        }
        return VagFrameSize * channels;
    }

    const u32 sample_size = PcmSampleSize(type);
    if (sample_size == 0) {
        return 0;
    }
    const size_t frame_size = size_t{sample_size} * channels;
    // PCM has no frames of its own; a few hundred samples at a time keeps the queue short.
    const size_t frames = std::min<size_t>(size / frame_size, 256);
    if (frames == 0) {
        return 0;
    }
    const size_t start = out.size();
    out.resize(start + frames * channels);
    for (size_t i = 0; i < frames * channels; ++i) {
        out[start + i] = ReadPcm(type, data + i * sample_size);
    }
    return frames * frame_size;
}

} // namespace Libraries::Ngs2
