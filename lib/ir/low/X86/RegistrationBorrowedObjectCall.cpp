//===- RegistrationBorrowedObjectCall.cpp - PE32 object calls -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compose checked ECX object effects and private scalar argument cleanup.
//===----------------------------------------------------------------------===//

#include "RegistrationPrivateFrame.h"

#include "neverd/Limits.h"
#include "neverd/lift/X86/X86Regs.h"

namespace neverd::registration_abi {

std::optional<PrivateInitializerResult>
applyBorrowedObjectCall(PrivateFrameState &State,
                        const RegistrationLeafCalleeABI &Callee,
                        llvm::ArrayRef<RegistrationObjectExtent> Initialized,
                        size_t &Work, ImageFrameEffects &Effects) {
  using namespace registration_state;
  const auto SP =
      State.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride].Offset;
  const unsigned This = x86reg::RCX / x86reg::GeneralRegStride;
  const auto Object = State.Borrow.Registers[This];
  const auto &ABI = Callee;
  if (!SP || !Object.Offset || *Object.Offset < 0 || Object.FrameOnlyFromCall ||
      Object.ReturnPC || State.Frame.Registers[This].MayBeFrame ||
      State.Frame.Registers[This].preferredAddress() ||
      State.CopySource.Registers[This].MayBeFrame ||
      ABI.StackPopBytes > UINT16_MAX || ABI.StackPopBytes % 4 ||
      int64_t(*SP) + ABI.StackPopBytes > 4 || !ABI.CallerPCWrites.empty() ||
      !chargeCalleeWork(Work, ABI.StackPopBytes))
    return std::nullopt;
  const int32_t AfterSP = int32_t(int64_t(*SP) + ABI.StackPopBytes);
  // Each domain retains partial pointer spills. No outgoing scalar may borrow
  // this invocation's private frame, its destination or a copy-source object.
  for (int64_t Byte = *SP; Byte < AfterSP; ++Byte)
    if (!State.Initialized.count(int32_t(Byte)))
      return std::nullopt;
  for (const auto *Domain :
       {&State.Frame, &State.BorrowSpills, &State.CopySourceSpills}) {
    if (!chargeCalleeWork(Work, Domain->Cells.size()))
      return std::nullopt;
    for (const auto &[Cell, Value] : Domain->Cells)
      if (Value.MayBeFrame && int64_t(Cell) < AfterSP &&
          int64_t(*SP) < int64_t(Cell) + 4)
        return std::nullopt;
  }
  for (const auto Ranges : {llvm::ArrayRef(ABI.ECXReads),
                            llvm::ArrayRef(ABI.ECXWrites), Initialized})
    for (const auto &Range : Ranges) {
      const int64_t End = int64_t(*Object.Offset) + Range.End;
      if (!chargeCalleeWork(Work, 1) || Range.Begin < 0 ||
          Range.End <= Range.Begin || End > INT32_MAX ||
          End > limits::kMaxRegistrationEHStateWork)
        return std::nullopt;
    }
  for (const auto &Range : ABI.ECXReads)
    Effects.ECXReads.emplace(*Object.Offset + Range.Begin,
                             *Object.Offset + Range.End);
  for (const auto &Range : ABI.ECXWrites) {
    if (!chargeCalleeWork(Work, State.Borrow.Cells.size()))
      return std::nullopt;
    Effects.ECXWrites.emplace(*Object.Offset + Range.Begin,
                              *Object.Offset + Range.End);
    State.Borrow.forgetCellValues(*Object.Offset + Range.Begin,
                                  uint32_t(Range.End - Range.Begin));
  }
  for (const auto &Range : Initialized) {
    if (!chargeCalleeWork(Work, size_t(Range.End - Range.Begin)))
      return std::nullopt;
    for (int32_t Byte = Range.Begin; Byte != Range.End; ++Byte)
      State.ObjectInitialized.insert(*Object.Offset + Byte);
  }
  if (!chargeCalleeWork(Work,
                        State.Initialized.size() + State.Frame.Cells.size() +
                            State.BorrowSpills.Cells.size() +
                            State.CopySourceSpills.Cells.size() +
                            ABI.ImageReads.size() + ABI.ImageWrites.size()))
    return std::nullopt;
  State.Initialized.erase(State.Initialized.begin(),
                          State.Initialized.lower_bound(AfterSP));
  for (auto *Domain :
       {&State.Frame, &State.BorrowSpills, &State.CopySourceSpills})
    Domain->Cells.erase(Domain->Cells.begin(),
                        Domain->Cells.lower_bound(AfterSP));
  for (const auto &Range : ABI.ImageReads)
    Effects.Reads.emplace(Range.Begin, Range.End);
  for (const auto &Range : ABI.ImageWrites)
    Effects.Writes.emplace(Range.Begin, Range.End);
  return PrivateInitializerResult{AfterSP, Object};
}

} // namespace neverd::registration_abi
