//===- FuncDetectorDetail.h - Per-arch function scanners --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Internal declarations shared between FuncDetector.cpp and the
/// architecture-specific scanners (FuncDetectorX86.cpp, FuncDetectorARM.cpp,
/// FuncDetectorAArch64.cpp).  Not a public header.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_FUNCDETECTORDETAIL_H
#define NEVERD_IR_LOW_FUNCDETECTORDETAIL_H

#include "neverd/decode/Decoder.h"
#include "neverd/loader/BinaryImage.h"

#include <atomic>
#include <functional>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace neverd {
namespace func_detect_detail {

/// Internal unwind marker; the public controlled detector translates it to
/// nullopt only after every scan worker has joined.
struct DiscoveryCancelled {};

struct Cancellation {
  const std::function<bool()> &Probe;
  mutable std::atomic<bool> Requested{false};

  void check() const {
    if (Requested.load(std::memory_order_relaxed) || (Probe && Probe())) {
      Requested.store(true, std::memory_order_relaxed);
      throw DiscoveryCancelled{};
    }
  }
};

inline void checkCancellation(const Cancellation *Control) {
  if (Control)
    Control->check();
}

/// One step of the x86 call-target sweep: the instruction (or the byte that
/// does not decode) at \p Addr, where the sweep goes next, and the direct
/// call target and linked PE code address it found there.
struct CallScanStep {
  va_t Addr = 0;
  va_t Next = 0;
  va_t Target = InvalidVA;
  va_t CodeTarget = InvalidVA;
};

/// A stretch of an x86 image that one executable section of one executable
/// segment owns alone -- or one section-less executable segment -- so that
/// BinaryImage::hasExecutableCodeOwnerRange accepts every byte range inside
/// it.  Lo == Hi when there is none.
struct CodeInterval {
  va_t Lo = 0;
  va_t Hi = 0;
};

/// The widest such stretch around \p Addr.
CodeInterval codeIntervalAround(const BinaryImage &Img, va_t Addr);

/// The step a sweep of [..., \p End) of \p Seg takes at \p Cur, or nothing
/// where the segment's bytes end.  A sweep is these steps from its start, so
/// two sweeps of one range that reach the same address agree from there on.
/// \p Known caches the code stretch the sweep is in.
std::optional<CallScanStep> stepCallsX86(const BinaryImage &Img, Decoder &Dec,
                                         const Segment *Seg, va_t Cur, va_t End,
                                         CodeInterval &Known);

/// Functions x86-64 code takes the address of with RIP-relative `lea`, such
/// as main handed to __libc_start_main: each target that starts with a
/// prologue, for the bounded decoder to verify.  For images with no other
/// function metadata, binary files.
void scanCodePointersX64(const BinaryImage &Img, Decoder &Dec,
                         std::set<va_t> &Out,
                         const Cancellation *Control = nullptr);

/// The code an i386 instruction names through a relocated four-byte
/// immediate, such as `push offset handler` or `mov [esi+10h], offset
/// callback`: (instruction, target) pairs, in sweep order.  A relocated
/// displacement names memory the instruction reads or writes, not code to
/// run, and is left out.
void scanCodeAddressImmediatesX86(const BinaryImage &Img, Decoder &Dec,
                                  std::vector<std::pair<va_t, va_t>> &Out,
                                  const Cancellation *Control = nullptr);

/// Whether the x86 instruction at \p Insn lets the code address its
/// immediate names escape: `push imm32` passes it as an argument and
/// `mov dword ptr [mem], imm32` stores it.  A register load of it, such as a
/// catch block's `mov eax, offset continuation`, does not.
bool isX86EscapingCodeImmediate(const BinaryImage &Img, va_t Insn);

void scanSegmentCallsX86(const BinaryImage &Img, Decoder &Dec,
                         const Segment *Seg, va_t Start, va_t End,
                         std::set<va_t> &Out,
                         std::vector<std::pair<va_t, va_t>> &CodeAddresses,
                         const Cancellation *Control = nullptr);
void scanSegmentCallsARM(const BinaryImage &Img, Decoder &Dec,
                         const Segment *Seg, va_t Start, va_t End,
                         std::set<va_t> &Out,
                         const Cancellation *Control = nullptr);
void scanSegmentCallsAArch64(const BinaryImage &Img, const Segment *Seg,
                             va_t Start, va_t End, std::set<va_t> &Out,
                             const Cancellation *Control = nullptr);

inline void scanSegmentCalls(const BinaryImage &Img, Decoder &Dec,
                             const Segment *Seg, va_t Start, va_t End,
                             std::set<va_t> &Out,
                             std::vector<std::pair<va_t, va_t>> &CodeAddresses,
                             const Cancellation *Control = nullptr) {
  switch (Img.Arch) {
  case Arch::AArch64:
    scanSegmentCallsAArch64(Img, Seg, Start, End, Out, Control);
    return;
  case Arch::ARM:
    scanSegmentCallsARM(Img, Dec, Seg, Start, End, Out, Control);
    return;
  case Arch::X86:
  case Arch::X64:
    scanSegmentCallsX86(Img, Dec, Seg, Start, End, Out, CodeAddresses, Control);
    return;
  default:
    scanSegmentCallsX86(Img, Dec, Seg, Start, End, Out, CodeAddresses, Control);
    return;
  }
}

} // namespace func_detect_detail
} // namespace neverd

#endif // NEVERD_IR_LOW_FUNCDETECTORDETAIL_H
