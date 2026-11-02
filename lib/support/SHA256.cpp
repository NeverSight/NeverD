//===- SHA256.cpp - Shared one-shot SHA-256 -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Runtime-selected SHA-256 compression with shared padding and cancellation.
///
//===----------------------------------------------------------------------===//

#include "neverd/support/SHA256.h"

#include "SHA256Internal.h"

#include "neverd/support/Limits.h"
#include "neverd/support/LoadProgress.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/SHA256.h"
#include "llvm/TargetParser/Host.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <iterator>

#ifndef NEVERD_SHA256_PORTABLE
#if (defined(__aarch64__) || defined(_M_ARM64)) && !defined(__AARCH64EB__)
#define NEVERD_SHA_EXTENSIONS 1
#define NEVERD_SHA_ARM64 1
#if defined(_MSC_VER) && !defined(__clang__)
#include <arm64_neon.h>
#else
#include <arm_neon.h>
#endif
#if defined(__APPLE__)
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <asm/hwcap.h>
#include <sys/auxv.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__) ||           \
    defined(_M_IX86)
#define NEVERD_SHA_EXTENSIONS 1
#include <immintrin.h>
#if (defined(__x86_64__) || defined(_M_X64)) && !defined(_M_ARM64EC)
#define NEVERD_SHA_X86_64 1
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif
#endif
#endif

namespace neverd {

#ifdef NEVERD_SHA_EXTENSIONS
namespace {

#define NEVERD_SHA256_SIZE(Name, Value) constexpr size_t Name = Value;
#define NEVERD_SHA256_BYTE(Name, Value) constexpr uint8_t Name = Value;
#ifdef NEVERD_SHA_ARM64
#if defined(__APPLE__)
#define NEVERD_SHA256_DARWIN_FEATURE(Name)                                     \
  constexpr const char *DarwinFeature = Name;
#elif defined(__linux__)
#define NEVERD_SHA256_LINUX_FEATURES(Features)                                 \
  constexpr unsigned long LinuxFeatures = Features;
#elif defined(_WIN32)
#define NEVERD_SHA256_WINDOWS_FEATURE(Feature)                                 \
  constexpr DWORD WindowsFeature = Feature;
#else
#define NEVERD_SHA256_ARM_FEATURES(Features, Target)                           \
  constexpr llvm::StringLiteral TargetFeatures(Features);
#endif
#else
#define NEVERD_SHA256_LANES(Name, Value) constexpr int Name = Value;
#endif
#include "SHA256.def"

constexpr uint32_t InitialState[] = {
#define NEVERD_SHA256_INITIAL_STATE(Value) Value,
#include "SHA256.def"
};

constexpr uint32_t RoundConstants[] = {
#define NEVERD_SHA256_ROUND_CONSTANT(Value) Value,
#include "SHA256.def"
};

constexpr size_t WordBytes = sizeof(uint32_t);
constexpr size_t LengthBytes = sizeof(uint64_t);
constexpr size_t WordsPerRegister = RegisterBytes / WordBytes;
constexpr size_t StateWords = std::size(InitialState);

/// The rounds take the message schedule, and the round constants, one
/// register -- a quad of words -- at a time; a block's own words are the
/// first quads of its schedule.
/// Full unrolling lets Clang keep the message quads in vector registers
/// by making each schedule index a constant on the 64-bit targets.
constexpr size_t Quads = std::size(RoundConstants) / WordsPerRegister;
constexpr size_t BlockQuads = BlockBytes / RegisterBytes;

#ifndef NEVERD_SHA_ARM64
constexpr size_t HalfRegisterBytes = RegisterBytes / 2;

/// The pshufb control that reverses the bytes of each word of a register:
/// the message words are big-endian.
constexpr std::array<uint8_t, RegisterBytes> wordByteSwap() {
  std::array<uint8_t, RegisterBytes> Control = {};
  for (size_t I = 0; I != RegisterBytes; ++I)
    Control[I] =
        static_cast<uint8_t>(I - I % WordBytes + WordBytes - 1 - I % WordBytes);
  return Control;
}
constexpr std::array<uint8_t, RegisterBytes> WordByteSwap = wordByteSwap();
#endif

/// Whether the processor has every feature compress() is compiled for.
bool hasTargetFeatures() {
#if defined(NEVERD_SHA_ARM64) && defined(__APPLE__)
  // LLVM's host feature map does not report Darwin ARM64 capabilities.
  int Supported = 0;
  size_t Size = sizeof(Supported);
  return sysctlbyname(DarwinFeature, &Supported, &Size, nullptr, 0) == 0 &&
         Size == sizeof(Supported) && Supported == 1;
#elif defined(NEVERD_SHA_ARM64) && defined(__linux__)
  // The kernel's process capabilities also work without a mounted /proc.
  return (getauxval(AT_HWCAP) & LinuxFeatures) == LinuxFeatures;
#elif defined(NEVERD_SHA_ARM64) && defined(_WIN32)
  return IsProcessorFeaturePresent(WindowsFeature) != 0;
#elif defined(NEVERD_SHA_X86_64)
  // CPUID is part of the x86-64 baseline. Query only the compression owner's
  // required leaves; constructing LLVM's complete feature map is unnecessary.
  return detail::x86HasSHA256Features([](uint32_t Leaf, uint32_t Subleaf) {
#ifdef _MSC_VER
    int Registers[4];
    __cpuidex(Registers, static_cast<int>(Leaf), static_cast<int>(Subleaf));
    return detail::X86CPUIDResult{static_cast<uint32_t>(Registers[0]),
                                  static_cast<uint32_t>(Registers[1]),
                                  static_cast<uint32_t>(Registers[2]),
                                  static_cast<uint32_t>(Registers[3])};
#else
    detail::X86CPUIDResult Registers;
    __cpuid_count(Leaf, Subleaf, Registers[0], Registers[1], Registers[2],
                  Registers[3]);
    return Registers;
#endif
  });
#else
  const llvm::StringMap<bool> Host = llvm::sys::getHostCPUFeatures();
#ifdef NEVERD_SHA_ARM64
  return Host.lookup(TargetFeatures);
#else
  // Retain LLVM's CPUID availability handling for 32-bit x86.
  return llvm::all_of(detail::SHA256X86Features, [&](const auto &Feature) {
    return Host.lookup(Feature.Name);
  });
#endif
#endif
}

#ifdef NEVERD_SHA_ARM64
/// ACLE's SHA256H/H2 instructions consume four rounds and preserve the state
/// in ABCD/EFGH order. SU0/SU1 replace the oldest schedule quad once consumed.
/// Only this function may contain SHA instructions; dispatch checks the OS's
/// feature report before calling it. Padding never requires an input overread.
#if !defined(_MSC_VER) || defined(__clang__)
#define NEVERD_SHA256_ARM_FEATURES(Features, Target)                           \
  __attribute__((target(Target)))
#endif
#include "SHA256.def"
LLVM_ATTRIBUTE_NOINLINE
void compress(uint32_t *State, const uint8_t *Data, size_t Blocks) {
  uint32x4_t ABCD = vld1q_u32(State);
  uint32x4_t EFGH = vld1q_u32(State + WordsPerRegister);
  for (; Blocks != 0; --Blocks, Data += BlockBytes) {
    const uint32x4_t SavedABCD = ABCD, SavedEFGH = EFGH;
    uint32x4_t Schedule[BlockQuads];
    for (size_t Quad = 0; Quad != BlockQuads; ++Quad)
      Schedule[Quad] = vreinterpretq_u32_u8(
          vrev32q_u8(vld1q_u8(Data + Quad * RegisterBytes)));
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
    for (size_t Quad = 0; Quad != Quads; ++Quad) {
      uint32x4_t &Words = Schedule[Quad % BlockQuads];
      const uint32x4_t Message =
          vaddq_u32(Words, vld1q_u32(RoundConstants + Quad * WordsPerRegister));
      const uint32x4_t PreviousABCD = ABCD;
      ABCD = vsha256hq_u32(ABCD, EFGH, Message);
      EFGH = vsha256h2q_u32(EFGH, PreviousABCD, Message);
      if (Quad + BlockQuads < Quads) {
        Words = vsha256su0q_u32(Words, Schedule[(Quad + 1) % BlockQuads]);
        Words = vsha256su1q_u32(Words, Schedule[(Quad + 2) % BlockQuads],
                                Schedule[(Quad + 3) % BlockQuads]);
      }
    }
    ABCD = vaddq_u32(ABCD, SavedABCD);
    EFGH = vaddq_u32(EFGH, SavedEFGH);
  }
  vst1q_u32(State, ABCD);
  vst1q_u32(State + WordsPerRegister, EFGH);
}
#else
/// Run \p Blocks blocks of \p Data through the compression function.
///
/// The instructions keep the state as ABEF and CDGH and take the message
/// schedule, with the round constants added, a quad at a time.  The schedule
/// is kept in as many registers as a block has quads: each quad past the
/// block's own is derived from the ones before it, into the register of the
/// one it no longer needs.
///
/// The function is compiled for the target features SHA256.def names, which
/// the build need not enable; sha256() calls it only on a processor that has
/// them.
#if !defined(_MSC_VER) || defined(__clang__)
__attribute__((target(
#define NEVERD_SHA256_X86_FIRST_FEATURE(Name, Leaf, Subleaf, Register, Bit) Name
#define NEVERD_SHA256_X86_FEATURE(Name, Leaf, Subleaf, Register, Bit) "," Name
#include "SHA256.def"
    )))
#endif
LLVM_ATTRIBUTE_NOINLINE void compress(uint32_t *State, const uint8_t *Data,
                                      size_t Blocks) {
  const __m128i ByteSwap =
      _mm_loadu_si128(reinterpret_cast<const __m128i *>(WordByteSwap.data()));
  __m128i Swapped = _mm_shuffle_epi32(
      _mm_loadu_si128(reinterpret_cast<const __m128i *>(State)),
      SwapWordPairs); // CDAB
  __m128i CDGH =
      _mm_shuffle_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i *>(
                            State + WordsPerRegister)),
                        ReverseWords);                              // EFGH
  __m128i ABEF = _mm_alignr_epi8(Swapped, CDGH, HalfRegisterBytes); // ABEF
  CDGH = _mm_blend_epi16(CDGH, Swapped, UpperHalf);                 // CDGH

  for (; Blocks != 0; --Blocks, Data += BlockBytes) {
    const __m128i SavedABEF = ABEF, SavedCDGH = CDGH;
    __m128i Schedule[BlockQuads];
    for (size_t Quad = 0; Quad != BlockQuads; ++Quad)
      Schedule[Quad] =
          _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i *>(
                               Data + Quad * RegisterBytes)),
                           ByteSwap);
#if defined(__clang__) && (defined(__x86_64__) || defined(_M_X64))
#pragma clang loop unroll(full)
#endif
    for (size_t Quad = 0; Quad != Quads; ++Quad) {
      const __m128i Current = Schedule[Quad % BlockQuads];
      __m128i Message = _mm_add_epi32(
          Current, _mm_loadu_si128(reinterpret_cast<const __m128i *>(
                       RoundConstants + Quad * WordsPerRegister)));
      CDGH = _mm_sha256rnds2_epu32(CDGH, ABEF, Message);
      // Finish the next quad: add W[t-7], from the quad before this one and
      // this one, and sigma1 of W[t-2], from this one.
      if (const size_t Next = Quad + 1; Next >= BlockQuads && Next < Quads) {
        __m128i &Words = Schedule[Next % BlockQuads];
        const __m128i Previous = Schedule[(Quad + BlockQuads - 1) % BlockQuads];
        Words =
            _mm_add_epi32(Words, _mm_alignr_epi8(Current, Previous, WordBytes));
        Words = _mm_sha256msg2_epu32(Words, Current);
      }
      Message = _mm_shuffle_epi32(Message, UpperWordsLow);
      ABEF = _mm_sha256rnds2_epu32(ABEF, CDGH, Message);
      // Start the quad whose W[t-16] is the quad before this one, in whose
      // register it goes: add sigma0 of W[t-15], from that quad and this one.
      if (const size_t Later = Quad + BlockQuads - 1;
          Later >= BlockQuads && Later < Quads) {
        __m128i &Words = Schedule[Later % BlockQuads];
        Words = _mm_sha256msg1_epu32(Words, Current);
      }
    }
    ABEF = _mm_add_epi32(ABEF, SavedABEF);
    CDGH = _mm_add_epi32(CDGH, SavedCDGH);
  }

  Swapped = _mm_shuffle_epi32(ABEF, ReverseWords);          // FEBA
  CDGH = _mm_shuffle_epi32(CDGH, SwapWordPairs);            // DCHG
  ABEF = _mm_blend_epi16(Swapped, CDGH, UpperHalf);         // DCBA
  CDGH = _mm_alignr_epi8(CDGH, Swapped, HalfRegisterBytes); // HGFE
  _mm_storeu_si128(reinterpret_cast<__m128i *>(State), ABEF);
  _mm_storeu_si128(reinterpret_cast<__m128i *>(State + WordsPerRegister), CDGH);
}
#endif

/// How many blocks the padding fills when \p Rest bytes of the message are
/// left after its whole blocks: the byte that ends the message and its
/// length follow them.
constexpr size_t tailBlocks(size_t Rest) {
  return llvm::divideCeil(Rest + sizeof(MessageEnd) + LengthBytes, BlockBytes);
}

std::array<uint8_t, 32> sha256WithExtensions(llvm::ArrayRef<uint8_t> Data,
                                             const LoadProgress &Progress) {
  uint32_t State[StateWords];
  llvm::copy(InitialState, State);
  const size_t Blocks = Data.size() / BlockBytes;
  if (!Progress) {
    compress(State, Data.data(), Blocks);
  } else {
    static_assert(limits::support::kLoadByteCheckpointBytes % BlockBytes == 0);
    constexpr size_t ChunkBlocks =
        limits::support::kLoadByteCheckpointBytes / BlockBytes;
    for (size_t Done = 0; Done < Blocks;) {
      const size_t Count = std::min(ChunkBlocks, Blocks - Done);
      compress(State, Data.data() + Done * BlockBytes, Count);
      Done += Count;
      Progress.report("hash", Done * BlockBytes, Data.size());
    }
  }

  uint8_t Tail[tailBlocks(BlockBytes - 1) * BlockBytes] = {};
  const size_t Rest = Data.size() % BlockBytes;
  if (Rest != 0)
    std::memcpy(Tail, Data.data() + Blocks * BlockBytes, Rest);
  Tail[Rest] = MessageEnd;
  const size_t TailBytes = tailBlocks(Rest) * BlockBytes;
  llvm::support::endian::write64be(Tail + TailBytes - LengthBytes,
                                   static_cast<uint64_t>(Data.size()) *
                                       CHAR_BIT);
  compress(State, Tail, TailBytes / BlockBytes);

  std::array<uint8_t, 32> Digest;
  static_assert(sizeof(Digest) == sizeof(State),
                "the digest is the final state, big-endian");
  for (size_t I = 0; I != StateWords; ++I)
    llvm::support::endian::write32be(Digest.data() + I * WordBytes, State[I]);
  Progress.report("hash", Data.size(), Data.size());
  return Digest;
}

} // namespace
#endif

bool detail::sha256UsesHardware() {
#ifdef NEVERD_SHA_EXTENSIONS
  static const bool HasTargetFeatures = hasTargetFeatures();
  return HasTargetFeatures;
#else
  return false;
#endif
}

std::array<uint8_t, 32> sha256(llvm::ArrayRef<uint8_t> Data) {
  return sha256(Data, {});
}

std::array<uint8_t, 32> sha256(llvm::ArrayRef<uint8_t> Data,
                               const LoadProgress &Progress) {
  Progress.report("hash", 0, Data.size());
#ifdef NEVERD_SHA_EXTENSIONS
  if (detail::sha256UsesHardware())
    return sha256WithExtensions(Data, Progress);
#endif
  if (!Progress)
    return llvm::SHA256::hash(Data);
  llvm::SHA256 Hash;
  for (size_t Done = 0; Done < Data.size();) {
    const size_t Count =
        std::min(limits::support::kLoadByteCheckpointBytes, Data.size() - Done);
    Hash.update(Data.slice(Done, Count));
    Done += Count;
    Progress.report("hash", Done, Data.size());
  }
  const auto Digest = Hash.final();
  Progress.report("hash", Data.size(), Data.size());
  return Digest;
}

} // namespace neverd
