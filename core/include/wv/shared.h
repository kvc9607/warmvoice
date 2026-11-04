// Shared-memory block between the WarmVoice app (user session) and the APO (inside
// audiodg.exe). One block per microphone endpoint, named "Global\WarmVoice_<endpoint guid>".
//
//   app -> APO : Resolved parameters        (seqlock, APO only reads)
//   APO -> app : Analysis + heartbeat       (seqlock, app only reads)
//   APO -> app : raw input ring (10 s)      (for the "录音对比" A/B recording)
//
// The APO never waits for the app: if the app is closed, it keeps the last parameters.
// SPDX-License-Identifier: MIT
#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>
#include "wv/params.h"

namespace wv {

constexpr uint32_t kSharedMagic = 0x57564D31;  // "WVM1"
constexpr uint32_t kRecordCapacity = 48000 * 10;

struct SharedBlock {
    uint32_t magic;
    uint32_t size;                      // sizeof(SharedBlock), guards against version mismatch
    // app -> APO
    std::atomic<uint32_t> paramSeq;     // odd while being written
    Resolved params;
    std::atomic<uint32_t> resetProfile; // app increments to ask for a fresh voice profile
    // APO -> app
    std::atomic<uint32_t> analysisSeq;
    Analysis analysis;
    std::atomic<uint64_t> heartbeat;    // hops processed; the app shows "已装到..." while it moves
    // raw input ring at the device rate (mono)
    std::atomic<uint32_t> recordRate;
    std::atomic<uint64_t> recordWritten; // total samples ever written
    float record[kRecordCapacity];
};

// Seqlock helpers. Writer: begin -> copy -> end. Reader: retry until a stable even sequence.
template <typename T>
inline void seqWrite(std::atomic<uint32_t>& seq, T& dst, const T& src) {
    const uint32_t s = seq.load(std::memory_order_relaxed);
    seq.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    std::memcpy((void*)&dst, (const void*)&src, sizeof(T));
    std::atomic_thread_fence(std::memory_order_release);
    seq.store(s + 2, std::memory_order_release);
}

// Returns false if the writer was busy (caller just tries again next time, never spins).
template <typename T>
inline bool seqRead(const std::atomic<uint32_t>& seq, const T& src, T& out, uint32_t* seenSeq = nullptr) {
    const uint32_t a = seq.load(std::memory_order_acquire);
    if (a & 1u) return false;
    std::memcpy((void*)&out, (const void*)&src, sizeof(T));
    std::atomic_thread_fence(std::memory_order_acquire);
    const uint32_t b = seq.load(std::memory_order_relaxed);
    if (a != b) return false;
    if (seenSeq) *seenSeq = a;
    return true;
}

}  // namespace wv
