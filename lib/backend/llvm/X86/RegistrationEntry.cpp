//===- RegistrationEntry.cpp - PE32 C++ parent entry ABI -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "neverd/Limits.h"
#include "neverd/backend/llvm/X86RegistrationEntry.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedIR.h"

#include "llvm/IR/Function.h"

#include <set>

namespace neverd {
std::optional<llvm::CallingConv::ID>
getX86RegistrationCxxEntryABI(const MedFunc &Source,
                              const llvm::Function &Function) {
  if (Source.Params.size() != Function.arg_size() || Source.IsVariadic ||
      Source.CalleePopBytes || !Source.RegistrationCallerCleanupABIComplete ||
      !Source.RegistrationStates ||
      !Source.RegistrationStates->IncomingFrameAccessesComplete)
    return std::nullopt;
  for (const auto &Arg : Function.args())
    if (Function.getAttributes().hasParamAttrs(Arg.getArgNo()))
      return std::nullopt;
  if (Source.Params.empty())
    return llvm::CallingConv::C;
  // A single physical ECX stays in ECX. Thiscall with stack arguments would
  // pop caller-owned words on return; that needs a different ABI projection.
  const auto &First = Source.Params.front();
  if (Source.Params.size() == 1 &&
      (First.Kind == MedVar::Param || First.Kind == MedVar::Reg) &&
      First.RegOff == getTargetRegInfo(Arch::X86).IntParamRegs[0] &&
      First.Size == 4 && Function.getArg(0)->getType()->isIntegerTy(32))
    return llvm::CallingConv::X86_ThisCall;
  const auto &Accesses = Source.RegistrationStates->IncomingFrameAccesses;
  if (Source.Params.size() > limits::kMaxRegistrationEHStateWork ||
      Accesses.size() >
          limits::kMaxRegistrationEHStateWork - Source.Params.size())
    return std::nullopt;
  std::set<int32_t> ReadWords;
  for (const auto &Access : Accesses)
    if (!Access.Write && Access.Offset >= 8 && Access.Width == 4)
      ReadWords.insert(Access.Offset);
  for (unsigned Index = 0; Index != Source.Params.size(); ++Index) {
    const auto &Param = Source.Params[Index];
    if (Param.Kind != MedVar::Param || Param.RegOff != kNoParamReg ||
        Param.Id != int(Index) || Param.Size != 4 ||
        !Function.getArg(Index)->getType()->isIntegerTy(32) ||
        Index > (INT32_MAX - 8) / 4 || !ReadWords.count(8 + 4 * Index))
      return std::nullopt;
  }
  return llvm::CallingConv::C;
}
} // namespace neverd
