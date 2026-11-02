//===- ResourceLimits.h - Benchmark defaults -------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defaults for repeatable component measurements.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_NEVERD_BENCH_RESOURCELIMITS_H
#define NEVERD_TOOLS_NEVERD_BENCH_RESOURCELIMITS_H

namespace neverd::bench {

inline constexpr unsigned kSignatureRepetitions = 7;
inline constexpr unsigned kSignatureIndexRepetitions = 1;
inline constexpr unsigned kSignatureThreads = 1;
inline constexpr char kSignatureRoot[] = "signatures";
inline constexpr unsigned kMedRepetitions = 7;
inline constexpr unsigned kMedThreads = 1;
inline constexpr unsigned kMedMaxFunctions = 0;

// sigprof/sigprof.cpp
/// Samples per CPU-second when NEVERD_SIGPROF_HZ is unset. Prime, so the
/// timer does not beat against periodic work.
inline constexpr unsigned kSigprofDefaultHz = 997;
/// Frames kept per sample, including the interrupted instruction.
inline constexpr unsigned kSigprofMaxFrames = 80;
/// Frames the handler may add above the interrupted one.
inline constexpr unsigned kSigprofHandlerFrames = 8;
/// Sample buffer in 64-bit words. It is reserved, not committed, at load.
inline constexpr unsigned long long kSigprofBufferWords = 1ull << 27;

} // namespace neverd::bench

#endif // NEVERD_TOOLS_NEVERD_BENCH_RESOURCELIMITS_H
