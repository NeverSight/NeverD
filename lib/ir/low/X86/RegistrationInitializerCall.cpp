//===- RegistrationInitializerCall.cpp - PE32 private construction --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compose a checked initializer into a preserved private throw helper.
//===----------------------------------------------------------------------===//

#include "RegistrationPrivateFrame.h"

#include "neverd/lift/X86/X86Regs.h"

namespace neverd::registration_abi {

std::optional<PrivateInitializerResult>
applyPrivateInitializerCall(PrivateFrameState &State,
                            const RegistrationInitializerCalleeABI &Callee,
                            size_t &Work, ImageFrameEffects &Effects,
                            RegistrationThrowCalleeABI &Throw) {
  using namespace registration_state;
  const auto SP =
      State.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride].Offset;
  const auto Object =
      State.Frame.Registers[x86reg::RCX / x86reg::GeneralRegStride];
  const auto &ABI = Callee.Object;
  if (!SP || !Object.Offset || Object.FrameOnlyFromCall || Object.ReturnPC ||
      ABI.StackPopBytes > UINT16_MAX || ABI.StackPopBytes % 4 ||
      int64_t(*SP) + ABI.StackPopBytes > 4 ||
      !chargeCalleeWork(Work, ABI.StackPopBytes + State.Frame.Cells.size()))
    return std::nullopt;
  const int32_t AfterSP = int32_t(int64_t(*SP) + ABI.StackPopBytes);
  // These are complete initialized scalar words, not pointer arguments. A
  // partial pointer spill retains taint even if another store changed a byte.
  for (int64_t Byte = *SP; Byte < AfterSP; ++Byte)
    if (!State.Initialized.count(int32_t(Byte)))
      return std::nullopt;
  for (const auto &[Cell, Value] : State.Frame.Cells)
    if (Value.MayBeFrame && int64_t(Cell) < AfterSP &&
        int64_t(*SP) < int64_t(Cell) + 4)
      return std::nullopt;
  for (const auto *Ranges :
       {&ABI.ECXReads, &ABI.ECXWrites, &Callee.Initialized})
    for (const auto &Range : *Ranges) {
      const int64_t Begin = int64_t(*Object.Offset) + Range.Begin;
      const int64_t End = int64_t(*Object.Offset) + Range.End;
      if (!chargeCalleeWork(Work, 1) || Range.Begin < 0 ||
          Range.End <= Range.Begin || Begin < AfterSP || End > 4)
        return std::nullopt;
    }
  for (const auto &Write : ABI.ECXWrites) {
    if (!chargeCalleeWork(Work, State.Frame.Cells.size()))
      return std::nullopt;
    const int64_t Begin = int64_t(*Object.Offset) + Write.Begin;
    const int64_t End = int64_t(*Object.Offset) + Write.End;
    State.Frame.forgetCellValues(int32_t(Begin), uint32_t(End - Begin));
  }
  for (const auto &Range : Callee.Initialized) {
    const int32_t Begin = int32_t(int64_t(*Object.Offset) + Range.Begin);
    const int32_t End = int32_t(int64_t(*Object.Offset) + Range.End);
    if (!chargeCalleeWork(Work, size_t(End - Begin) + State.Frame.Cells.size()))
      return std::nullopt;
    for (int64_t Byte = Begin; Byte < End; ++Byte)
      State.Initialized.insert(int32_t(Byte));
    for (auto It = State.Frame.Cells.begin(); It != State.Frame.Cells.end();)
      if (Begin <= It->first && int64_t(It->first) + 4 <= End)
        It = State.Frame.Cells.erase(It);
      else
        ++It;
  }
  if (!chargeCalleeWork(Work,
                        State.Initialized.size() + State.Frame.Cells.size() +
                            ABI.ImageReads.size() + ABI.ImageWrites.size() +
                            ABI.CodeRanges.size()))
    return std::nullopt;
  State.Initialized.erase(State.Initialized.begin(),
                          State.Initialized.lower_bound(AfterSP));
  State.Frame.Cells.erase(State.Frame.Cells.begin(),
                          State.Frame.Cells.lower_bound(AfterSP));
  for (const auto &Range : ABI.ImageReads)
    Effects.Reads.emplace(Range.Begin, Range.End);
  for (const auto &Range : ABI.ImageWrites)
    Effects.Writes.emplace(Range.Begin, Range.End);
  Throw.CodeRanges.insert(Throw.CodeRanges.end(), ABI.CodeRanges.begin(),
                          ABI.CodeRanges.end());
  return PrivateInitializerResult{AfterSP, Object};
}

} // namespace neverd::registration_abi
