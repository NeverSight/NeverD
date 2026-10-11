//===- RegistrationRuntimeThrowInfo.cpp - Direct PE32 throw types ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Decode bounded immutable candidates without assigning a type to a call.
//===----------------------------------------------------------------------===//
#include "RegistrationABIPrivate.h"

#include "neverd/ir/low/LowIR.h"
#include "neverd/loader/BinaryImage.h"

namespace neverd::registration_abi {
std::optional<std::vector<RegistrationRuntimeThrowInfo>>
collectRegistrationRuntimeThrowInfos(const LowFunc &Function,
                                     const BinaryImage &Image, size_t &Work) {
  std::set<va_t> Candidates;
  for (const auto &Block : Function.Blocks)
    for (const auto &Op : Block.Ops) {
      if (Op.NumInputs > std::size(Op.Inputs) ||
          !chargeCalleeWork(Work, 1 + Op.NumInputs))
        return std::nullopt;
      for (unsigned I = 0; I != Op.NumInputs; ++I) {
        const auto &Value = Op.Inputs[I];
        if (Value.isConst() && Value.Size == 4 && Value.Offset <= UINT32_MAX)
          Candidates.insert(Value.Offset);
      }
    }
  std::vector<RegistrationRuntimeThrowInfo> Result;
  for (va_t Address : Candidates) {
    const auto *Owner = Image.getSegmentFor(Address);
    if (!Owner || !Owner->isReadable() || Owner->isWritable() ||
        Owner->isExecutable() || !Image.readVA(Address, 16))
      continue;
    // The trivial-copy decoder reads three fixed records and at most 4096 name
    // bytes. Charge its full bound even when the candidate fails early.
    if (!chargeCalleeWork(Work, 16 + 8 + 28 + 4096))
      return std::nullopt;
    if (const auto Info =
            coff_loader::getCheckedX86SimpleCxxThrowInfo(Image, Address))
      Result.push_back(
          {Info->Address, Info->TypeDescriptorVA, Info->ObjectSize});
  }
  return Result;
}
} // namespace neverd::registration_abi
