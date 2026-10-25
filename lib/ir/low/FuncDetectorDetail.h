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
#include "neverd/decode/X86InstructionScan.h"
#include "neverd/loader/BinaryImage.h"

#include <optional>
#include <set>

namespace neverd {
namespace func_detect_detail {

/// One step of the x86 call-target sweep: the instruction (or the byte that
/// does not decode) at \p Addr, where the sweep goes next, and the direct
/// call target it found there.
struct CallScanStep {
  va_t Addr = 0;
  va_t Next = 0;
  va_t Target = InvalidVA;
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
/// \p Scan may cache the same segment bytes and mode over a wider range;
/// this step still rejects instructions crossing \p End.
std::optional<CallScanStep> stepCallsX86(const BinaryImage &Img, Decoder &Dec,
                                         const Segment *Seg, va_t Cur, va_t End,
                                         CodeInterval &Known,
                                         X86InstructionScan *Scan = nullptr);

/// Functions x86-64 code takes the address of with RIP-relative `lea`, such
/// as main handed to __libc_start_main: each target that starts with a
/// prologue, for the bounded decoder to verify.  For images with no other
/// function metadata, binary files.
void scanCodePointersX64(const BinaryImage &Img, Decoder &Dec,
                         std::set<va_t> &Out);

/// The code an i386 instruction names through a relocated four-byte
/// immediate, such as `push offset handler` or `mov [esi+10h], offset
/// callback`: (instruction, target) pairs, in sweep order.  A relocated
/// displacement names memory the instruction reads or writes, not code to
/// run, and is left out.
void scanCodeAddressImmediatesX86(const BinaryImage &Img, Decoder &Dec,
                                  std::vector<std::pair<va_t, va_t>> &Out);

/// Whether the x86 instruction at \p Insn lets the code address its
/// immediate names escape: `push imm32` passes it as an argument and
/// `mov dword ptr [mem], imm32` stores it.  A register load of it, such as a
/// catch block's `mov eax, offset continuation`, does not.
bool isX86EscapingCodeImmediate(const BinaryImage &Img, va_t Insn);

void scanSegmentCallsX86(const BinaryImage &Img, Decoder &Dec,
                         const Segment *Seg, va_t Start, va_t End,
                         std::set<va_t> &Out);
void scanSegmentCallsARM(const BinaryImage &Img, Decoder &Dec,
                         const Segment *Seg, va_t Start, va_t End,
                         std::set<va_t> &Out);
void scanSegmentCallsAArch64(const BinaryImage &Img, const Segment *Seg,
                             va_t Start, va_t End, std::set<va_t> &Out);

inline void scanSegmentCalls(const BinaryImage &Img, Decoder &Dec,
                             const Segment *Seg, va_t Start, va_t End,
                             std::set<va_t> &Out) {
  switch (Img.Arch) {
  case Arch::AArch64:
    scanSegmentCallsAArch64(Img, Seg, Start, End, Out);
    return;
  case Arch::ARM:
    scanSegmentCallsARM(Img, Dec, Seg, Start, End, Out);
    return;
  case Arch::X86:
  case Arch::X64:
    scanSegmentCallsX86(Img, Dec, Seg, Start, End, Out);
    return;
  default:
    scanSegmentCallsX86(Img, Dec, Seg, Start, End, Out);
    return;
  }
}

} // namespace func_detect_detail
} // namespace neverd

#endif // NEVERD_IR_LOW_FUNCDETECTORDETAIL_H
