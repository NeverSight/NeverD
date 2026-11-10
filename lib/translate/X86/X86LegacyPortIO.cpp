//===- X86LegacyPortIO.cpp - Protected port element execution -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Own i386 port decoding, TSS selection, segments and index/count publication.
//===----------------------------------------------------------------------===//

#include "X86LegacyPortIO.h"

#include "llvm/Support/Errc.h"

#include <cstdint>
#include <optional>
#include <utility>

namespace neverd::translate::detail {

std::optional<X86LegacyPortInstruction>
decodeLegacyPortIO(llvm::ArrayRef<uint8_t> Bytes, bool Code32) {
  X86LegacyEncoding E(Bytes, Code32);
  const uint8_t Opcode = E.byte();
  const bool String = (Opcode & 0xfc) == 0x6c;
  const bool Immediate = (Opcode & 0xfc) == 0xe4;
  if ((!String && !Immediate && (Opcode & 0xfc) != 0xec) ||
      E.Group1Prefix == 0xf2 || (!String && E.Group1Prefix == 0xf3))
    return std::nullopt;
  X86LegacyPortInstruction F;
  F.Input = !(Opcode & 2);
  F.String = String;
  F.Repeat = E.Group1Prefix == 0xf3;
  F.Locked = E.Group1Prefix == 0xf0;
  F.Bytes = Opcode & 1 ? E.OperandBytes : 1;
  F.AddressBytes = E.AddressBytes;
  F.Segment = F.Input ? X86SegmentRegister::ES
                      : E.Segment.value_or(X86SegmentRegister::DS);
  if (Immediate)
    F.ImmediatePort = E.byte();
  return E.done() ? std::optional(F) : std::nullopt;
}

llvm::Expected<X86PortIOResult>
executeLegacyPortIO(X86LegacyExecutionState &S, X86PagedMemory &M,
                    const X86LegacyPortInstruction &F, uint32_t Fallthrough,
                    const X86LegacyExecutionOptions &O) {
  X86PortIOResult R;
  const auto Fault = [&](uint8_t Vector) {
    R.Access.Status = X86PageWalkStatus::Exception;
    R.Access.Exception = X86MemoryException{Vector, 0, {}};
  };
  if (F.Locked) {
    Fault(6);
    return R;
  }
  const uint16_t Port =
      F.ImmediatePort ? uint16_t(*F.ImmediatePort) : uint16_t(S.Core.GPR[2]);
  if ((S.EFlags & 0x20000) || S.Core.CPL > ((S.EFlags >> 12) & 3)) {
    const unsigned Type = S.TR.Attributes & 0x9f;
    if (S.TR.Unusable || (Type != 0x83 && Type != 0x8b))
      return llvm::createStringError(
          llvm::errc::invalid_argument,
          "legacy port permissions require a usable TSS cache");
    if (Type == 0x83) {
      Fault(13); // A 16-bit TSS has no I/O bitmap.
      return R;
    }
    auto Checked =
        checkX86PortBitmap(M, S.TR.Base, S.TR.Limit, Port, F.Bytes, true);
    if (!Checked)
      return Checked.takeError();
    R.Access = std::move(*Checked);
    if (R.Access.Status != X86PageWalkStatus::Completed)
      return R;
  }
  const uint32_t AddressMask = F.AddressBytes == 2 ? UINT16_MAX : UINT32_MAX;
  const uint32_t Count = S.Core.GPR[1] & AddressMask;
  if (F.Repeat && !Count) {
    S.EIP = Fallthrough;
    R.Completed = true;
    return R;
  }
  std::optional<X86PortMemoryOperand> Operand;
  if (F.String) {
    const uint32_t Offset = S.Core.GPR[F.Input ? 7 : 6] & AddressMask;
    auto Checked = checkX86LegacySegmentAccess(
        S.Core.Segments[unsigned(F.Segment)],
        {F.Segment,
         F.Input ? X86SegmentAccessKind::Write : X86SegmentAccessKind::Read,
         Offset, F.Bytes, O.Segments.FullLimitChoice,
         bool(S.EFlags & 0x20000)});
    if (!Checked)
      return Checked.takeError();
    Operand = X86PortMemoryOperand{
        {Checked->Linear,
         F.Input ? MemoryAccessKind::Write : MemoryAccessKind::Read, S.Core.CPL,
         false, F.Segment == X86SegmentRegister::SS, true}};
    if (Checked->FaultVector) {
      Operand->Fault = X86MemoryException{*Checked->FaultVector, 0, {}};
    } else if (S.Core.CPL == 3 && (M.controls().CR0 & 0x40000) &&
               (S.EFlags & 0x40000) && (Checked->Linear & (F.Bytes - 1))) {
      if (O.Segments.AlignmentPriority == X86AlignmentPriority::BeforePaging)
        Operand->Fault = X86MemoryException{17, 0, {}};
      else
        Operand->AlignmentFaultAfterPaging = true;
    }
  }
  auto Transferred = transferX86PortIO(
      M, *O.PortIO, {Port, uint8_t(F.Bytes), F.Input, S.Core.GPR[0]}, Operand);
  if (!Transferred)
    return Transferred.takeError();
  Transferred->Access.SelfModifications.insert(
      Transferred->Access.SelfModifications.begin(),
      R.Access.SelfModifications.begin(), R.Access.SelfModifications.end());
  R = std::move(*Transferred);
  if (!R.Completed)
    return R;
  if (F.Input && !F.String) {
    const uint32_t Mask = F.Bytes == 4 ? UINT32_MAX : (1u << (F.Bytes * 8)) - 1;
    S.Core.GPR[0] = (S.Core.GPR[0] & ~Mask) | R.InputValue;
  }
  const auto Advance = [&](unsigned Index, uint32_t Delta) {
    auto &V = S.Core.GPR[Index];
    V = (V & ~AddressMask) | ((V + Delta) & AddressMask);
  };
  if (F.String)
    Advance(F.Input ? 7 : 6, S.EFlags & 0x400 ? -F.Bytes : F.Bytes);
  if (F.Repeat) {
    Advance(1, UINT32_MAX);
    R.RepeatContinue = Count != 1;
  }
  if (!R.RepeatContinue)
    S.EIP = Fallthrough;
  R.Completed = !R.RepeatContinue;
  return R;
}

} // namespace neverd::translate::detail
