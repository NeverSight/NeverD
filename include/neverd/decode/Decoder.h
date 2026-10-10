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
#include "neverd/ir/low/LowPreservedState.h"
#include "neverd/ir/low/LowUndefinedEffects.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/ArrayRef.h"

#include <capstone/capstone.h>
#include <memory>
#include <optional>
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

/// One loader-authenticated address occurrence inside an encoded instruction.
/// FieldVA identifies the exact relocation field; TargetVA is the resolved
/// address represented by that field, and Provenance records whether the
/// loader classified it as data or code.  The architecture lifter must still
/// match FieldVA to the decoded operand encoding before consuming it: numeric
/// equality elsewhere in the same instruction is not occurrence provenance.
struct RelocatedAddressOperand {
  va_t FieldVA = InvalidVA;
  uint64_t EncodedValue = 0;
  va_t TargetVA = InvalidVA;
  uint8_t Width = 0;
  ConstantAddressProvenance Provenance = ConstantAddressProvenance::Unknown;
  va_t TargetOwnerVA = InvalidVA;
  bool PCRelativeFromInstructionEnd = false;
};

/// One loader-authenticated scalar or negative relocation field inside an
/// encoded instruction.  These records never confer address provenance; the
/// x86 lifter binds the exact immediate/displacement field to a LowOp input so
/// a later CFG proof can consume its identity without matching by numeric
/// value. An unrelocated-immediate record instead certifies disjointness from
/// the loader's complete object relocation write footprint.
struct RelocatedScalarOperand {
  enum class Kind : uint8_t {
    I386ELFGOTPC,
    I386ELFAmbiguousGOTOFF,
    I386ELFUnrelocatedImmediate,
    AArch64ELFUnrelocatedWideMove,
  };

  va_t FieldVA = InvalidVA;
  uint64_t EncodedValue = 0;
  uint8_t Width = 0;
  Kind Semantics = Kind::I386ELFGOTPC;
};

class Decoder {
public:
  Decoder();
  ~Decoder();

  bool init(Arch A, InstructionMode Mode = InstructionMode::Default);

  /// Start from image evidence, then select the mode at each decoded address.
  bool init(const BinaryImage &Img);
  /// Apply the lifting settings \p Img implies beyond its architecture and
  /// mode, such as the system call convention of its platform.
  void configureFor(const BinaryImage &Img);
  bool selectMode(const BinaryImage &Img, va_t Addr,
                  std::optional<InstructionMode> IncomingMode = {});
  InstructionMode currentMode() const { return CurrentMode; }

  /// Release active decoder state. Decoding returns failure until init
  /// succeeds.
  void reset();

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
  /// capstone id fixups that decodeOne performs for the lift path. Encoded
  /// displacement detail is normalized identically when available. Combined
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

  /// Enable/disable capstone's printing of the mnemonic and operand text for
  /// subsequent decodes.  Text is on by default.  With it off, a decode fills
  /// only Addr/Size/Id and the instruction bytes: no text and no operand
  /// detail, and an x86 compare with a predicate keeps capstone's base id.
  /// About half an x86 decode is printing, so a sweep that reads only sizes
  /// and call ids turns it off.  No-op if the state is unchanged.
  void setText(bool On);
  bool textEnabled() const { return Text; }

  /// True when \p Insn is a trap execution can continue past, so the bytes
  /// after it may still belong to the same function.  Only x86 `int3` is.
  bool isResumableTrap(const DecodedInsn &Insn) const;

  /// Lift a single decoded instruction to LowIR ops. When supplied, \p Relocs
  /// describes exact loader-authenticated address operands in the encoded
  /// instruction; unsupported architectures/operand encodings ignore them.
  /// Optional undefined effects describe the exact appended instruction span,
  /// without changing ordinary LowIR. They are reset before every attempt and
  /// published only on success; Missing is never evidence of defined outputs.
  /// Insn must come from the trusted decoder: the undefined-output audit checks
  /// supported operand shapes, not raw-byte/detail authentication or complete
  /// equivalence of the lifted implementation. Non-complete records have no
  /// usable Effects; their diagnostic and operation binding remain available.
  /// Optional PreservedState follows the same reset-before-attempt discipline
  /// but carries an independent, closed x64 architectural bank audit. Neither
  /// sidecar authenticates decoded metadata supplied by an untrusted caller.
  void liftToLow(const DecodedInsn &Insn, std::vector<LowOp> &Ops,
                 llvm::ArrayRef<RelocatedAddressOperand> Relocs = {},
                 llvm::ArrayRef<RelocatedScalarOperand> ScalarRelocs = {},
                 LowInstructionUndefinedEffects *UndefinedEffects = nullptr,
                 LowInstructionPreservedState *PreservedState = nullptr);

  /// Explicit target-value projection for ordinary unsegmented x64 r/m64
  /// near CALL. Unlike the import-slot representation, this retains the
  /// target LOAD even for a constant slot. Returns false without changing Ops
  /// for other architectures or unsupported encodings; stack effects are
  /// still the responsibility of the machine-state recovery contract.
  bool liftX64MemoryCallToLow(
      const DecodedInsn &Insn, std::vector<LowOp> &Ops,
      LowInstructionUndefinedEffects *UndefinedEffects = nullptr,
      LowInstructionPreservedState *PreservedState = nullptr);

  /// Exact scalar relocation operand consumed by the most recently lifted x86
  /// instruction, if any.  The occurrence is reset for every instruction.
  std::optional<RelocatedInstructionScalarOperandOccurrence>
  getX86ScalarOperandOccurrence() const;

  /// Whether \p Insn ends a function's straight-line decode.  Dispatches to
  /// the active architecture lifter's terminator classification.
  bool isFunctionTerminator(const DecodedInsn &Insn) const;

  /// Whether \p Insn returns to a caller, as the active architecture
  /// lifter classifies it, or nullopt when no lifter classifies returns.
  std::optional<bool> returnsToCaller(const DecodedInsn &Insn) const;

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
  /// Reset all x86 instruction-sequence state before an independent function
  /// or translation block.  The historical name is retained for compatibility.
  void resetX86FpuState();
  bool x86FpuDidReset() const;

  /// Largest x86 `ret imm` callee-cleanup pop seen while lifting the current
  /// function (the i386 SysV sret hidden-pointer pop); 0 for non-x86 / ordinary
  /// `ret`.  Reset by resetX86FpuState().  Read by the CFG builder after a
  /// function's instructions are lifted to record LowFunc::CalleePopBytes.
  int getX86RetPopBytes() const;

  /// Exact LowIR producer for a completed i386 `call $+5; pop reg` pair in the
  /// most recently lifted instruction.
  std::optional<I386GetPcOccurrence> getX86GetPcOccurrence() const;

private:
  /// Decode an x86 fence carrying otherwise redundant operand-size prefixes.
  /// Capstone rejects these encodings even though LLVM and real x86-64
  /// binaries accept them.  Returns true after populating InsnBuf.
  bool decodePrefixedX86Fence(const uint8_t *Bytes, size_t Len, va_t Addr);

  /// Decode the unprefixed register forms of 0F 1A /r and 0F 1B /r.  These
  /// encodings are no-ops, but Capstone rejects them instead of consuming the
  /// ModR/M byte.  Mandatory-prefix MPX encodings are deliberately excluded.
  bool decodeUnprefixedX86MpxRegisterNop(const uint8_t *Bytes, size_t Len,
                                         va_t Addr);

  /// Apply instruction-id normalization that does not require operand detail.
  /// Both full and lightweight decode paths use this semantic profile.
  void fixupDecodedInsnId(cs_insn *I) const;

  /// Correct a known long-mode disp32 width misreport only when the complete
  /// encoded field agrees with the decoded displacement and memory operands.
  /// Shared by full and lightweight decodes with operand detail enabled.
  void fixupX86DisplacementDetail(cs_insn *I) const;

  /// Correct capstone decode-id quirks on \p I, dispatching to the active
  /// architecture lifter's fixup.
  void fixupDecodedInsn(cs_insn *I) const;

  csh Handle = 0;
  cs_insn *InsnBuf = nullptr;
  Arch TargetArch = Arch::Unknown;
  InstructionMode CurrentMode = InstructionMode::Default;
  bool Strict = true;
  bool Detail = true;
  bool Text = true;

  std::unique_ptr<X86Lifter> X86;
  std::unique_ptr<ARMLifter> ARM;
  std::unique_ptr<AArch64Lifter> AArch64;
};

} // namespace neverd

#endif // NEVERD_DECODE_DECODER_H
