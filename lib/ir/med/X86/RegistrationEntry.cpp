//===- RegistrationEntry.cpp - PE32 physical parameter projection --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep register parameters and incoming stack homes in physical ABI order.
//===----------------------------------------------------------------------===//
#include "neverd/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/ir/med/X86RegistrationEntry.h"

namespace neverd {
namespace {
std::optional<X86RegistrationEntryABI> describeEntry(const MedFunc &Function) {
  if (!Function.RegistrationCxxEntryPopBytes || Function.IsVariadic ||
      !Function.RegistrationStates ||
      !Function.RegistrationStates->IncomingFrameAccessesComplete)
    return std::nullopt;
  const auto &Accesses = Function.RegistrationStates->IncomingFrameAccesses;
  if (Function.Params.size() > limits::kMaxRegistrationEHStateWork ||
      Accesses.size() >
          limits::kMaxRegistrationEHStateWork - Function.Params.size())
    return std::nullopt;
  X86RegistrationEntryABI ABI;
  ABI.PopBytes = *Function.RegistrationCxxEntryPopBytes;
  if (ABI.PopBytes % 4)
    return std::nullopt;
  ABI.StackWords = ABI.PopBytes / 4;
  for (const auto &Access : Accesses) {
    const int64_t End = int64_t(Access.Offset) + Access.Width;
    if (Access.Offset < 8 || !Access.Width || End > INT32_MAX)
      return std::nullopt;
    ABI.StackWords = std::max(ABI.StackWords, unsigned((End - 8 + 3) / 4));
  }
  if (ABI.StackWords > limits::kMaxRegistrationEHRecords ||
      (ABI.PopBytes && ABI.StackWords * 4 != ABI.PopBytes))
    return std::nullopt;
  const auto &Registers = getTargetRegInfo(Arch::X86).IntParamRegs;
  for (unsigned Index = 0; Index != Function.Params.size(); ++Index) {
    const auto &Param = Function.Params[Index];
    if ((Param.Kind != MedVar::Param && Param.Kind != MedVar::Reg) ||
        Param.Size != 4)
      return std::nullopt;
    if (Param.RegOff != kNoParamReg) {
      if (Index != ABI.RegisterCount || ABI.RegisterCount >= 2 ||
          Param.RegOff != Registers[ABI.RegisterCount])
        return std::nullopt;
      ++ABI.RegisterCount;
    } else if (Param.Kind != MedVar::Param || Param.Id != int(Index) ||
               Index - ABI.RegisterCount >= ABI.StackWords) {
      return std::nullopt;
    }
  }
  // The supported Microsoft register conventions pop every stack argument.
  if (ABI.RegisterCount && ABI.StackWords && !ABI.PopBytes)
    return std::nullopt;
  return ABI;
}
} // namespace

std::optional<X86RegistrationEntryABI>
projectX86RegistrationEntry(const MedFunc &Function) {
  const auto ABI = describeEntry(Function);
  if (!ABI || Function.Params.size() != ABI->RegisterCount + ABI->StackWords)
    return std::nullopt;
  return ABI;
}

void completeX86RegistrationEntryParameters(MedFunc &Function) {
  const auto ABI = describeEntry(Function);
  if (!ABI)
    return;
  while (Function.Params.size() < ABI->RegisterCount + ABI->StackWords) {
    MedVar Param;
    Param.Kind = MedVar::Param;
    Param.Id = Function.Params.size();
    Param.Size = 4;
    Param.RegOff = kNoParamReg;
    Param.TheArch = Arch::X86;
    Function.Params.push_back(Param);
  }
}
} // namespace neverd
