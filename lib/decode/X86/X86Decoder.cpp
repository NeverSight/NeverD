//===- X86Decoder.cpp - x86 decoder support ------------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// x86 handle reuse, decode normalization and lifter state adapters.
///
//===----------------------------------------------------------------------===//

#include "../DecoderDetail.h"

#include "neverd/decode/Decoder.h"
#include "neverd/decode/Limits.h"
#include "neverd/ir/X86/X87Context.h"
#include "neverd/lift/X86/X86Lifter.h"

#include "X86ScanAdapter.h"
#include "capstone/capstone.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace neverd {
namespace decoder_detail {
namespace {
/// The x86 capstone handles decoders have closed, by mode.  Opening one
/// builds capstone's instruction tables for that handle anew, which a
/// decompile did for every decoder it opened: its run's, and each callee
/// graph's.  Beyond the tables, which depend on the mode alone, a handle
/// keeps nothing from one instruction to the next but the options a decoder
/// sets (detail and text), which take() returns to a new handle's.
class SpareX86Handles {
public:
  /// A closed handle of \p Mode as cs_open() with detail on leaves one, or
  /// 0 when none is spare.
  csh take(cs_mode Mode) {
    csh Handle = 0;
    {
      std::lock_guard<std::mutex> Lock(Mutex);
      for (size_t I = Handles.size(); I-- > 0;)
        if (Handles[I].first == Mode) {
          Handle = Handles[I].second;
          Handles.erase(Handles.begin() + static_cast<std::ptrdiff_t>(I));
          break;
        }
    }
    if (Handle) {
      cs_option(Handle, CS_OPT_DETAIL, CS_OPT_OFF);
      cs_option(Handle, CS_OPT_DETAIL, CS_OPT_ON);
      cs_option(Handle, CS_OPT_TEXT, CS_OPT_ON);
    }
    return Handle;
  }
  /// Keep \p Handle, of \p Mode, for a later decoder; close it when enough
  /// are spare.
  void give(cs_mode Mode, csh Handle) {
    {
      std::lock_guard<std::mutex> Lock(Mutex);
      if (Handles.size() < MaxSpare) {
        Handles.emplace_back(Mode, Handle);
        return;
      }
    }
    cs_close(&Handle);
  }

private:
  /// More than the decoders a decompile has open at once.
  static constexpr size_t MaxSpare = limits::decode::kDecoderMaxSpare;
  std::mutex Mutex;
  std::vector<std::pair<cs_mode, csh>> Handles;
};

/// Never destroyed: a decoder may close its handle after static destructors
/// have run.
SpareX86Handles &spareX86Handles() {
  static SpareX86Handles *Spare = new SpareX86Handles;
  return *Spare;
}

} // namespace

cs_mode x86HandleMode(Arch A) {
  return A == Arch::X64 ? CS_MODE_64 : CS_MODE_32;
}

csh takeX86Handle(cs_mode Mode) { return spareX86Handles().take(Mode); }

void releaseX86Handle(csh Handle, Arch A) {
  spareX86Handles().give(x86HandleMode(A), Handle);
}

bool isX86AlignmentNop(const DecodedInsn &Insn) {
  return Insn.Id == X86_INS_NOP;
}

} // namespace decoder_detail

size_t
Decoder::classifyX86Prefix(llvm::ArrayRef<uint8_t> Bytes, va_t Addr,
                           llvm::MutableArrayRef<uint32_t> Output) const {
  return X86_scanBatch(Handle, Bytes.data(), Bytes.size(), Addr, Output.data(),
                       Output.size())
      .count;
}

va_t Decoder::directX86CallTarget(llvm::ArrayRef<uint8_t> Bytes,
                                 va_t Addr) const {
  va_t Target;
  const uint16_t Length =
      X86_scanCallTarget(Handle, Bytes.data(), Bytes.size(), Addr, &Target);
  return Length && Length == Bytes.size() ? Target : InvalidVA;
}

void Decoder::configureX86For(const BinaryImage &Img) {
  if (!X86)
    return;
  // A PE image runs on Windows, whose system service convention an x64
  // SYSCALL follows.
  X86->setSyscallConvention(Img.abiFormat() == BinaryFormat::COFF
                                ? X86Lifter::SyscallConvention::WindowsNT
                                : X86Lifter::SyscallConvention::Linux);
  // The calling convention also decides whether long double values cross
  // calls, and so who owns the x87 register stack.
  X86->setX87ContextOwnership(
      x87ContextOwnsStack(Img.Arch, Img.abiFormat(), Img.X87ContextOverride));
}

void Decoder::setX87ContextOwnership(bool Owned) {
  if (X86)
    X86->setX87ContextOwnership(Owned);
}

bool Decoder::decodePrefixedX86Fence(const uint8_t *Bytes, size_t Len,
                                     va_t Addr) {
  if (!X86 || !Bytes || !InsnBuf)
    return false;

  size_t PrefixCount = 0;
  while (PrefixCount < Len && Bytes[PrefixCount] == 0x66)
    ++PrefixCount;
  if (PrefixCount == 0 || PrefixCount >= Len || PrefixCount >= 15)
    return false;

  const uint8_t *Code = Bytes + PrefixCount;
  size_t Remaining = Len - PrefixCount;
  uint64_t InnerAddr = Addr + PrefixCount;
  if (!cs_disasm_iter(Handle, &Code, &Remaining, &InnerAddr, InsnBuf))
    return false;
  if (InsnBuf->id != X86_INS_LFENCE && InsnBuf->id != X86_INS_MFENCE &&
      InsnBuf->id != X86_INS_SFENCE)
    return false;

  const size_t InnerSize = InsnBuf->size;
  const size_t TotalSize = PrefixCount + InnerSize;
  if (TotalSize > 15)
    return false;

  std::memmove(InsnBuf->bytes + PrefixCount, InsnBuf->bytes, InnerSize);
  std::memset(InsnBuf->bytes, 0x66, PrefixCount);
  InsnBuf->address = Addr;
  InsnBuf->size = static_cast<uint16_t>(TotalSize);
  return true;
}

bool Decoder::liftX64MemoryCallToLow(
    const DecodedInsn &Insn, std::vector<LowOp> &Ops,
    LowInstructionUndefinedEffects *UndefinedEffects,
    LowInstructionPreservedState *PreservedState) {
  if (PreservedState)
    *PreservedState = {};
  if (UndefinedEffects) {
    *UndefinedEffects = {};
    UndefinedEffects->Diagnostic =
        "instruction has no audited x64 memory-call projection";
  }
  return X86 && X86->liftX64MemoryCall(Insn.Raw, Ops, UndefinedEffects,
                                       PreservedState);
}

int Decoder::getX86FpuTop() const { return X86 ? X86->getFpuTop() : 0; }

void Decoder::resetX86FpuState() {
  if (X86)
    X86->resetFpuState();
}

bool Decoder::x86FpuDidReset() const { return X86 && X86->fpuDidReset(); }

int Decoder::getX86RetPopBytes() const {
  return X86 ? X86->getRetPopBytes() : 0;
}

std::optional<I386GetPcOccurrence> Decoder::getX86GetPcOccurrence() const {
  return X86 ? X86->getLastGetPcOccurrence() : std::nullopt;
}

std::optional<RelocatedInstructionScalarOperandOccurrence>
Decoder::getX86ScalarOperandOccurrence() const {
  return X86 ? X86->getLastScalarOperandOccurrence() : std::nullopt;
}

std::optional<uint64_t>
Decoder::returnImmediate(const DecodedInsn &Insn) const {
  if (!Insn.Raw || !X86)
    return std::nullopt;
  return X86Lifter::returnImmediate(Insn.Raw);
}

va_t Decoder::pcRelCodeRefTarget(const DecodedInsn &Insn) const {
  if (!Insn.Raw)
    return InvalidVA;
  // Only x86/x64 has the relocation-free same-section `lea rip` address-of
  // form; AArch64/ARM materialize code addresses through relocations the loader
  // already records (adrp+add, GOTOFF, literal pool).
  if (X86)
    return X86Lifter::pcRelCodeRefTarget(Insn.Raw);
  return InvalidVA;
}

} // namespace neverd
