// Minimal WAV reader/writer (PCM 16/24/32-bit and float32) for the offline tools.
// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

struct Wav {
    int rate = 48000, channels = 1;
    std::vector<float> data;  // interleaved
    size_t frames() const { return channels ? data.size() / channels : 0; }
};

inline bool readWav(const std::string& path, Wav& w, std::string* err = nullptr) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { if (err) *err = "cannot open " + path; return false; }
    auto fail = [&](const char* m) { if (err) *err = m; std::fclose(f); return false; };
    char id[4]; uint32_t sz;
    if (std::fread(id, 1, 4, f) != 4 || std::memcmp(id, "RIFF", 4)) return fail("not RIFF");
    if (std::fread(&sz, 4, 1, f) != 1 || std::fread(id, 1, 4, f) != 4 || std::memcmp(id, "WAVE", 4)) return fail("not WAVE");
    int fmt = 0, bits = 0;
    while (std::fread(id, 1, 4, f) == 4 && std::fread(&sz, 4, 1, f) == 1) {
        if (!std::memcmp(id, "fmt ", 4)) {
            std::vector<uint8_t> b(sz);
            if (std::fread(b.data(), 1, sz, f) != sz || sz < 16) return fail("bad fmt chunk");
            fmt = b[0] | (b[1] << 8);
            w.channels = b[2] | (b[3] << 8);
            w.rate = (int)(b[4] | (b[5] << 8) | (b[6] << 16) | ((uint32_t)b[7] << 24));
            bits = b[14] | (b[15] << 8);
            if (fmt == 0xFFFE && sz >= 26) fmt = b[24] | (b[25] << 8);
        } else if (!std::memcmp(id, "data", 4)) {
            std::vector<uint8_t> b(sz);
            sz = (uint32_t)std::fread(b.data(), 1, sz, f);
            const int bps = bits / 8;
            const size_t n = sz / bps;
            w.data.resize(n);
            for (size_t i = 0; i < n; ++i) {
                const uint8_t* p = &b[i * bps];
                if (fmt == 3 && bits == 32) { float v; std::memcpy(&v, p, 4); w.data[i] = v; }
                else if (bits == 16) w.data[i] = (int16_t)(p[0] | (p[1] << 8)) / 32768.0f;
                else if (bits == 24) w.data[i] = (int32_t)(((uint32_t)p[0] << 8) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 24)) / 2147483648.0f;
                else if (bits == 32) { int32_t v; std::memcpy(&v, p, 4); w.data[i] = v / 2147483648.0f; }
                else return fail("unsupported sample format");
            }
            std::fclose(f);
            return true;
        } else {
            std::fseek(f, sz + (sz & 1), SEEK_CUR);
        }
    }
    return fail("no data chunk");
}

inline bool writeWav(const std::string& path, const Wav& w) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const uint32_t n = (uint32_t)w.data.size(), dataBytes = n * 2;
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); u32(36 + dataBytes); std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); u32(16); u16(1); u16((uint16_t)w.channels); u32((uint32_t)w.rate);
    u32((uint32_t)w.rate * w.channels * 2); u16((uint16_t)(w.channels * 2)); u16(16);
    std::fwrite("data", 1, 4, f); u32(dataBytes);
    for (uint32_t i = 0; i < n; ++i) {
        float v = w.data[i] * 32767.0f;
        v = v > 32767.0f ? 32767.0f : (v < -32768.0f ? -32768.0f : v);
        const int16_t s = (int16_t)(v < 0 ? v - 0.5f : v + 0.5f);
        std::fwrite(&s, 2, 1, f);
    }
    std::fclose(f);
    return true;
}
