//===- Decoder.h - Capstone-based instruction decoder --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares the Decoder class that wraps Capstone for instruction decoding
/// and lifting to LowIR, and the UnliftedInstruction exception for strict
/// mode failures.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DECODE_DECODER_H
#define NEVERD_DECODE_DECODER_H

#include "neverd/ir/low/LowIR.h"
#include "neverd/loader/BinaryImage.h"

#include <capstone/capstone.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace neverd {

class X86Lifter;
class ARMLifter;
class AArch64Lifter;

/// Thrown by Decoder::liftToLow when strict mode is enabled and an
/// instruction has no lifter.
class UnliftedInstruction : public std::runtime_error {
public:
  UnliftedInstruction(va_t Addr, const char *Mnem, const char *Ops)
      : std::runtime_error(formatMsg(Addr, Mnem, Ops)), TheAddr(Addr),
        TheMnemonic(Mnem ? Mnem : ""), TheOpStr(Ops ? Ops : "") {}

  va_t getAddr() const { return TheAddr; }
  const std::string &getMnemonic() const { return TheMnemonic; }
  const std::string &getOpStr() const { return TheOpStr; }

private:
  static std::string formatMsg(va_t A, const char *M, const char *O) {
    std::string S = "unlifted instruction at 0x";
    char Buf[32];
    snprintf(Buf, sizeof(Buf), "%llX", static_cast<unsigned long long>(A));
    S += Buf;
    S += ": ";
    S += (M ? M : "?");
    if (O && *O) {
      S += " ";
      S += O;
    }
    return S;
  }
  va_t TheAddr;
  std::string TheMnemonic;
  std::string TheOpStr;
};

struct DecodedInsn {
  va_t Addr;
  uint16_t Size;
  uint32_t Id;
  cs_insn *Raw;
};

class Decoder {
public:
  Decoder();
  ~Decoder();

  bool init(Arch A, InstructionMode Mode = InstructionMode::Default);

  /// Decode a single instruction at \p Addr; returns size or 0 on failure.
  int decodeOne(const uint8_t *Bytes, size_t Len, va_t Addr, DecodedInsn &Out);

  /// Decode a single instruction for the lift / CFG path, trying a Capstone-
  /// free native fast path for the common AArch64 classes and falling back to
  /// decodeOne (full Capstone) otherwise.  Produces the same operand detail
  /// the lifter reads and lifts identically to Capstone (see
  /// AArch64NativeDecode.h), but leaves the rendered mnemonic/op_str text
  /// empty, so it must not be used by callers that print disassembly.
  /// Identical to decodeOne on non-AArch64 targets.
  int decodeOneForLift(const uint8_t *Bytes, size_t Len, va_t Addr,
                       DecodedInsn &Out);

  /// Lightweight decode for classification-only passes (function-entry
  /// verification, size stepping): fills Addr/Size/Id/Raw but skips the
  /// capstone id fixups that decodeOne performs for the lift path.  Combined
  /// with setDetail(false) it also skips the expensive per-instruction
  /// operand-detail fill.  Only Size and Id are meaningful when detail is
  /// disabled; targets with operand-aware terminators must leave it enabled.
  /// Returns size or 0.
  int decodeOneLight(const uint8_t *Bytes, size_t Len, va_t Addr,
                     DecodedInsn &Out);

  /// Enable/disable capstone operand-detail generation for subsequent decodes.
  /// Detail is on by default (the lift path needs operands); turning it off for
  /// classification-only scans (which read just Size/Id) skips the per-
  /// instruction cs_detail fill and is materially faster.  No-op if the state
  /// is unchanged.
  void setDetail(bool On);
  bool detailEnabled() const { return Detail; }

  /// True when \p Insn is a trap execution can continue past, so the bytes
  /// after it may still belong to the same function.  Only x86 `int3` is.
  bool isResumableTrap(const DecodedInsn &Insn) const;

  /// Lift a single decoded instruction to LowIR ops.
  void liftToLow(const DecodedInsn &Insn, std::vector<LowOp> &Ops);

  /// Whether \p Insn ends a function's straight-line decode.  Dispatches to
  /// the active architecture lifter's terminator classification.
  bool isFunctionTerminator(const DecodedInsn &Insn) const;

  /// Direct (immediate) call target of \p Insn, or InvalidVA if \p Insn is
  /// not a direct call.  Dispatches to the active architecture lifter.
  va_t directCallTarget(const DecodedInsn &Insn) const;

  /// Encoded return-pop immediate, including an explicitly encoded zero.
  /// Empty for ordinary returns and non-x86 instructions.
  std::optional<uint64_t> returnImmediate(const DecodedInsn &Insn) const;

  /// Destination execution-mode contract of a control transfer.  The source
  /// mode must be the effective decode mode (never ARM's Default alias).
  LowInstructionTargetMode controlTargetMode(const DecodedInsn &Insn,
                                             InstructionMode SourceMode) const;

  /// Target VA of a relocation-free PC-relative address-of (x86 `lea rip`), or
  /// InvalidVA.  Used to record a same-section function pointer the assembler
  /// resolved, which carries no relocation for the loader to catch.
  va_t pcRelCodeRefTarget(const DecodedInsn &Insn) const;

  void setStrict(bool S);
  bool isStrict() const { return Strict; }

  csh getHandle() const { return Handle; }

  /// x87 stack-top (TOP) state for the active x86 lifter, read by the CFG
  /// builder around each lift to re-base ST(i) references into CFG order.
  /// No-ops / 0 for non-x86 targets.
  int getX86FpuTop() const;
  void resetX86FpuState();
  bool x86FpuDidReset() const;

  /// Largest x86 `ret imm` callee-cleanup pop seen while lifting the current
  /// function (the i386 SysV sret hidden-pointer pop); 0 for non-x86 / ordinary
  /// `ret`.  Reset by resetX86FpuState().  Read by the CFG builder after a
  /// function's instructions are lifted to record LowFunc::CalleePopBytes.
  int getX86RetPopBytes() const;

private:
  /// Decode an x86 fence carrying otherwise redundant operand-size prefixes.
  /// Capstone rejects these encodings even though LLVM and real x86-64
  /// binaries accept them.  Returns true after populating InsnBuf.
  bool decodePrefixedX86Fence(const uint8_t *Bytes, size_t Len, va_t Addr);

  /// Correct capstone decode-id quirks on \p I, dispatching to the active
  /// architecture lifter's fixup.
  void fixupDecodedInsn(cs_insn *I) const;

  csh Handle = 0;
  cs_insn *InsnBuf = nullptr;
  Arch TargetArch = Arch::Unknown;
  bool Strict = true;
  bool Detail = true;

  std::unique_ptr<X86Lifter> X86;
  std::unique_ptr<ARMLifter> ARM;
  std::unique_ptr<AArch64Lifter> AArch64;
};

} // namespace neverd

#endif // NEVERD_DECODE_DECODER_H
