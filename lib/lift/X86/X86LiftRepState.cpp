//===- X86LiftRepState.cpp - Complete REP register and flag state --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep completion in the native instruction. In particular, zero-count
/// addr32 LODS must not inherit MOVS's high-register write behavior.
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/X86/X86RepState.h"

#include "llvm/Support/ErrorHandling.h"

#include <array>

namespace neverd {

void liftRepState(X86Lifter &L, X86Lifter::LiftState &S, const cs_x86 &X86,
                  Intrinsic Id, unsigned Element, bool NotEqual) {
  const auto C = x86RepContract(Id);
  if (!C)
    llvm::report_fatal_error("invalid x86 REP family");
  const uint16_t Word = L.targetArch() == Arch::X64 ? 8 : 4;
  unsigned Segment = 0;
  if (C->Source)
    for (const auto Prefix : X86.prefix)
      if (Prefix == 0x26 || Prefix == 0x2e || Prefix == 0x36 ||
          Prefix == 0x3e || Prefix == 0x64 || Prefix == 0x65)
        Segment = Prefix;
  const auto Descriptor = x86RepDescriptor(Id, L.targetArch(), Element,
                                           S.AddressSize, Segment, NotEqual);
  if (!Descriptor)
    llvm::report_fatal_error("invalid x86 REP encoding controls");

  const std::array<uint64_t, 6> FlagRegisters = {
      x86reg::CF, x86reg::PF, x86reg::AF, x86reg::ZF, x86reg::SF, x86reg::OF};
  NdVar Status = NdVar::scalar(0, X86RepStatusBytes);
  auto AppendFlag = [&](uint64_t Register, unsigned Bit) {
    const auto Boolean = S.makeTemp(1);
    S.emit(NdOp::INT_NOTEQUAL, Boolean,
           {NdVar::reg(Register, 1), NdVar::scalar(0, 1)});
    const auto Wide = S.makeTemp(X86RepStatusBytes);
    S.emit(NdOp::INT_ZEXT, Wide, {Boolean});
    const auto Shifted = S.makeTemp(X86RepStatusBytes);
    S.emit(NdOp::INT_LEFT, Shifted,
           {Wide, NdVar::scalar(Bit, X86RepStatusBytes)});
    const auto Next = S.makeTemp(X86RepStatusBytes);
    S.emit(NdOp::INT_OR, Next, {Status, Shifted});
    Status = Next;
  };
  AppendFlag(x86reg::DF, X86RepDirectionShift);
  if (C->Compare)
    for (unsigned I = 0; I < FlagRegisters.size(); ++I)
      AppendFlag(FlagRegisters[I], X86RepFlagBits[I]);

  std::array<uint64_t, 3> Registers;
  if (Id == Intrinsic::X86RepMovsState || Id == Intrinsic::X86RepCmpsState)
    Registers = {x86reg::RSI, x86reg::RDI, x86reg::RCX};
  else if (Id == Intrinsic::X86RepLodsState)
    Registers = {x86reg::RSI, x86reg::RCX, x86reg::RAX};
  else
    Registers = {C->Source ? x86reg::RSI : x86reg::RDI, x86reg::RCX,
                 C->Port ? x86reg::RDX : x86reg::RAX};
  std::array<NdVar, 3> Results;
  Results[0] = S.makeTemp(Word);
  S.emitIntrinsic(Id, Results[0],
                  {NdVar::reg(Registers[0], Word),
                   NdVar::reg(Registers[1], Word),
                   NdVar::reg(Registers[2], C->Port ? 2 : Word), Status,
                   NdVar::scalar(*Descriptor, X86RepDescriptorBytes)});
  // Bind every auxiliary SSA definition before committing any register write.
  for (unsigned I = 1; I < C->WordResults; ++I) {
    Results[I] = S.makeTemp(Word);
    S.emit(NdOp::COPY, Results[I], {S.makeTemp(Word)});
  }
  NdVar Flags;
  if (C->Compare) {
    Flags = S.makeTemp(X86RepFlagBytes);
    S.emit(NdOp::COPY, Flags, {S.makeTemp(X86RepFlagBytes)});
  }
  for (unsigned I = 0; I < C->WordResults; ++I)
    S.emit(NdOp::COPY, NdVar::reg(Registers[I], Word), {Results[I]});
  if (C->Compare)
    for (unsigned I = 0; I < FlagRegisters.size(); ++I) {
      const auto Shifted = S.makeTemp(X86RepFlagBytes);
      S.emit(NdOp::INT_RIGHT, Shifted,
             {Flags, NdVar::scalar(X86RepFlagBits[I], X86RepFlagBytes)});
      const auto Low = S.makeTemp(1);
      S.emit(NdOp::SUBBYTES, Low, {Shifted, NdVar::scalar(0, 4)});
      S.emit(NdOp::INT_AND, NdVar::reg(FlagRegisters[I], 1),
             {Low, NdVar::scalar(1, 1)});
    }
}

} // namespace neverd
