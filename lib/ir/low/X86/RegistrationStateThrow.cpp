//===- RegistrationStateThrow.cpp - Direct PE32 throw arguments ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind an exact CRT call to initialized parent or callback scalar storage.
//===----------------------------------------------------------------------===//
#include "RegistrationStateSolver.h"

#include "neverd/lift/X86Regs.h"

namespace neverd::registration_state {
std::optional<RegistrationRuntimeThrow>
RegistrationStateSolver::runtimeThrowArguments(
    const Domain &State, const RegistrationCalleeFrameContract &Callee,
    std::vector<RegistrationObjectExtent> &Reads) {
  if (!KnownCxx || State.Unknown)
    return std::nullopt;
  const auto &Frame = State.Frame;
  const auto &SP = Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride];
  FrameValue Object, Table;
  if (SP.CallbackAddress) {
    if (!Frame.callbackMemoryIsPrivate(*SP.CallbackAddress, 8, true))
      return std::nullopt;
    Object = Frame.loadCallback(SP.CallbackAddress->Offset, 4);
    Table = Frame.loadCallback(SP.CallbackAddress->Offset + 4, 4);
  } else if (SP.Offset) {
    if (int64_t(*SP.Offset) + 8 > int64_t(*Chain.RegistrationOffset) - 4 ||
        !charge(8))
      return std::nullopt;
    for (int64_t Byte = *SP.Offset; Byte < int64_t(*SP.Offset) + 8; ++Byte)
      if (!State.InitializedFrameBytes.count(int32_t(Byte)))
        return std::nullopt;
    Object = Frame.load(*SP.Offset, 4);
    Table = Frame.load(*SP.Offset + 4, 4);
  } else
    return std::nullopt;
  if (Object.Constant == 0 && Table.Constant == 0 && !Object.MayBeFrame &&
      !Table.MayBeFrame)
    return RegistrationRuntimeThrow{};
  if (!Table.Constant || !*Table.Constant || Table.MayBeFrame ||
      Object.ReturnPC || Object.FrameOnlyFromCall || Object.EntryOffset ||
      !charge(Callee.RuntimeThrowInfos.size() + Frame.cellCount()))
    return std::nullopt;
  const auto Info = llvm::find_if(Callee.RuntimeThrowInfos, [&](const auto &I) {
    return I.Address == *Table.Constant;
  });
  if (Info == Callee.RuntimeThrowInfos.end() || !Info->ObjectSize ||
      Info->ObjectSize > UINT16_MAX)
    return std::nullopt;
  RegistrationRuntimeThrow Result{Info->Address, Info->ObjectSize};
  if (Object.CallbackAddress) {
    if (!Frame.callbackMemoryIsPrivate(*Object.CallbackAddress,
                                       Info->ObjectSize, true))
      return std::nullopt;
    const auto Begin = Object.CallbackAddress->Offset;
    const int64_t End = int64_t(Begin) + Info->ObjectSize;
    for (const auto &[Offset, Value] : Frame.CallbackCells)
      if (Value.MayBeFrame && int64_t(Offset) < End &&
          Begin < int64_t(Offset) + 4)
        return std::nullopt;
    Result.CallbackVA = Object.CallbackAddress->Entry;
    Result.ObjectOffset = Begin;
  } else if (Object.Offset) {
    const RegistrationObjectExtent Extent{0, int32_t(Info->ObjectSize)};
    if (!projectFrameObject(State, *Object.Offset, parentStackOffset(State),
                            {Extent}, Reads, true))
      return std::nullopt;
    Result.ObjectOffset = *Object.Offset;
  } else
    return std::nullopt;
  return Result;
}
} // namespace neverd::registration_state
