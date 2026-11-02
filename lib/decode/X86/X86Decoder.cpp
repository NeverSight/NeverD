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
#include "neverd/decode/X86/X86TransactionEncoding.h"
#include "neverd/ir/X86/X87Context.h"
#include "neverd/lift/X86/X86Lifter.h"
#include "neverd/support/BinaryEncoding.h"

#include "capstone/capstone.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
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

void Decoder::fixupX86DecodeMetadata(cs_insn *I) const {
  fixupX86MpxDetail(I);
  fixupX86DisplacementDetail(I);
  // The pinned Capstone sign-extends XBEGIN using the address size in i386,
  // not the encoded immediate size. Normalize only that known decoder
  // representation before CFG consumers or lifting see the fallback.
  if (TargetArch != Arch::X86 || !Detail || !Text || !I || !I->detail ||
      I->id != X86_INS_XBEGIN)
    return;
  const auto E =
      x86TransactionBeginEncoding({I->bytes, I->size}, I->address, false);
  auto &X = I->detail->x86;
  if (!E || !x86TransactionBeginDetailMatches(X, *E, false))
    return;
  const auto *Immediate = I->bytes + E->OpcodeOffset + 2;
  uint64_t Raw = E->ImmediateBytes == 2 ? readLE<uint16_t>(Immediate)
                                        : readLE<uint32_t>(Immediate);
  // Reproduce the decoder's sign-bit extension, not a truncation: with
  // addr16 and a clear bit 15, the upper half of a rel32 stays present.
  if (X.addr_size == 2 && (Raw & 0x8000))
    Raw |= ~UINT64_C(0xffff);
  const auto Legacy = (I->address + I->size + Raw) & UINT32_MAX;
  if (uint64_t(X.operands[0].imm) != E->Target &&
      uint64_t(X.operands[0].imm) == Legacy) {
    X.operands[0].imm = E->Target;
    std::snprintf(I->op_str, sizeof(I->op_str), "0x%llx",
                  static_cast<unsigned long long>(E->Target));
  }
}

void Decoder::fixupX86DisplacementDetail(cs_insn *I) const {
  // The bundled decoder can initialize long-mode displacementSize from the
  // operand-size override, although ModRM still consumes a complete disp32.
  // Correct that metadata once, before relocation ownership and form audits
  // consume it. Do not reinterpret genuine i386 disp16, moffs, compressed
  // disp8 or an instruction decoded without populated operand detail.
  if (TargetArch != Arch::X64 || !Detail || !Text || !I || !I->detail ||
      !I->size || I->size > 15)
    return;
  auto &X = I->detail->x86;
  auto &E = X.encoding;
  if (E.disp_size != 2 || X.prefix[2] != 0x66 ||
      (X.addr_size != 4 && X.addr_size != 8) || !E.modrm_offset ||
      E.modrm_offset >= I->size || X.op_count > std::size(X.operands))
    return;
  const uint8_t ModRM = I->bytes[E.modrm_offset];
  const unsigned Mod = ModRM >> 6, RM = ModRM & 7;
  if (X.modrm != ModRM || Mod == 3 || Mod == 1)
    return;
  size_t Cursor = E.modrm_offset + 1;
  bool HasDisp32 = Mod == 2 || RM == 5;
  if (RM == 4) {
    if (Cursor >= I->size || X.sib != I->bytes[Cursor])
      return;
    HasDisp32 = Mod == 2 || (I->bytes[Cursor] & 7) == 5;
    ++Cursor;
  }
  if (!HasDisp32 || E.disp_offset != Cursor || Cursor + 4 > I->size)
    return;
  const size_t End = Cursor + 4;
  if (E.imm_size ? E.imm_offset != End || End + E.imm_size != I->size
                 : E.imm_offset != 0 || End != I->size)
    return;
  uint32_t Bits = 0;
  for (unsigned N = 0; N != 4; ++N)
    Bits |= uint32_t{I->bytes[Cursor + N]} << (N * 8);
  const int64_t Displacement = static_cast<int32_t>(Bits);
  if (X.disp != Displacement)
    return;
  bool HasMemory = false;
  for (unsigned N = 0; N != X.op_count; ++N)
    if (X.operands[N].type == X86_OP_MEM) {
      if (X.operands[N].mem.disp != Displacement)
        return;
      HasMemory = true;
    }
  if (HasMemory)
    E.disp_size = 4;
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
