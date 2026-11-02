//===- RegistrationSEHRegisters.cpp - Finally register state ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Materializes checked finally register exchanges before SSA construction.

#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/X86/X86RegistrationSEHCall.h"
#include "neverd/ir/med/X86/X86RegistrationSEHRegisters.h"

#include "llvm/ADT/STLExtras.h"

#include <set>

namespace neverd::x86_registration {

bool hasValidSEHRegisterShape(const MedOp &Op) {
  if (!isSEHRegisterIntrinsic(Op) || Op.NumInputs < 5 ||
      Op.NumInputs > Op.Inputs.size() || Op.Dead || Op.OriginSeq != -1 ||
      Op.SourceCallHint || Op.ExactArguments || !Op.IntrinsicOutputs.empty() ||
      Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  for (unsigned I = 1; I != 5; ++I)
    if (!Op.Inputs[I].isConst())
      return false;
  const uint64_t Role = Op.Inputs[1].ConstVal;
  const bool Read = Role == uint64_t(SEHRegisterRole::CallbackInput) ||
                    Role == uint64_t(SEHRegisterRole::CallOutput);
  if (Role > uint64_t(SEHRegisterRole::CallOutput) || Op.Inputs[1].Size != 1 ||
      Op.Inputs[2].Size != 4 || Op.Inputs[3].Size != 4 ||
      Op.Inputs[4].Size != 2 || !Op.Inputs[2].ConstVal ||
      Op.Inputs[2].ConstVal > UINT32_MAX || !Op.Inputs[3].ConstVal ||
      Op.Inputs[3].ConstVal != Op.Addr || Op.Addr > UINT32_MAX ||
      !llvm::is_contained(SEHDataRegisters, Op.Inputs[4].ConstVal))
    return false;
  if (Read)
    return Op.Inputs[0].ConstVal == uint64_t(Intrinsic::X86SEHRegisterRead) &&
           Op.NumInputs == 5 && Op.Output.Kind == MedVar::Reg &&
           Op.Output.Id >= 0 && Op.Output.TheArch == Arch::X86 &&
           Op.Output.Size == 4 && Op.Output.RegOff == Op.Inputs[4].ConstVal;
  return Op.Inputs[0].ConstVal == uint64_t(Intrinsic::X86SEHRegisterWrite) &&
         Op.NumInputs == 6 && !Op.Output.Size && Op.Inputs[5].Size == 4;
}

} // namespace neverd::x86_registration

namespace neverd {

void LowToMedConverter::modelSEHFinallyRegisters(MedFunc &Func) {
  using namespace x86_registration;
  if (TargetArch != Arch::X86 || TargetFormat != BinaryFormat::COFF)
    return;
  const auto Returns = getRegistrationSEHReturns(Func);
  const auto Nonlocal = getRegistrationSEHNonlocalCalls(Func);
  if (!Returns || !Nonlocal)
    return;
  auto Calls = Returns->ReturningCalls;
  Calls.insert(Nonlocal->begin(), Nonlocal->end());
  std::set<va_t> Entries;
  for (const auto &[Identity, Target] : Calls)
    Entries.insert(Target);
  if (Entries.empty())
    return;

  const auto Append = [&](std::vector<MedOp> &Ops, SEHRegisterRole Role,
                          va_t Entry, va_t Site) {
    const bool Read = Role == SEHRegisterRole::CallbackInput ||
                      Role == SEHRegisterRole::CallOutput;
    for (uint64_t Offset : SEHDataRegisters) {
      MedOp Op;
      Op.Opcode = NdOp::INTRINSIC;
      Op.Addr = Site;
      Op.addInput(
          MedVar::makeConst(uint64_t(Read ? Intrinsic::X86SEHRegisterRead
                                          : Intrinsic::X86SEHRegisterWrite),
                            2));
      Op.addInput(MedVar::makeConst(uint64_t(Role), 1));
      Op.addInput(MedVar::makeConst(Entry, 4));
      Op.addInput(MedVar::makeConst(Site, 4));
      Op.addInput(MedVar::makeConst(Offset, 2));
      const MedVar Value = ndVarToMedVar(NdVar::reg(Offset, 4));
      if (Read)
        Op.Output = Value;
      else
        Op.addInput(Value);
      Ops.push_back(std::move(Op));
    }
  };
  for (auto &Block : Func.Blocks) {
    std::vector<MedOp> Ops;
    if (Entries.count(Block.StartAddr))
      Append(Ops, SEHRegisterRole::CallbackInput, Block.StartAddr,
             Block.StartAddr);
    for (auto &Op : Block.Ops) {
      const auto Call = Calls.find({Block.Id, Op.Addr, Op.OriginSeq});
      const auto Return = Returns->ReturnBlocks.find(Block.Id);
      if (Op.Opcode == NdOp::RETURN && Return != Returns->ReturnBlocks.end())
        Append(Ops, SEHRegisterRole::CallbackOutput, Return->second, Op.Addr);
      if (Op.Opcode == NdOp::CALL && Call != Calls.end())
        Append(Ops, SEHRegisterRole::CallInput, Call->second, Op.Addr);
      const va_t Site = Op.Addr;
      const bool Returning =
          Op.Opcode == NdOp::CALL &&
          Returns->ReturningCalls.count({Block.Id, Site, Op.OriginSeq});
      Ops.push_back(std::move(Op));
      if (Returning)
        Append(Ops, SEHRegisterRole::CallOutput, Call->second, Site);
    }
    Block.Ops = std::move(Ops);
  }
}

} // namespace neverd
