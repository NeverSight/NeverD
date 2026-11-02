//===- IndirectCallSlot.h - The word an indirect call goes through -*- C++ -*-//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Finds the constant address of the word whose value an indirect call jumps
/// to, so evidence about that word (a function-pointer variable and the
/// routine it starts at) can reach the call.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_INDIRECTCALLSLOT_H
#define NEVERD_IR_MED_INDIRECTCALLSLOT_H

#include "neverd/Limits.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/ir/med/X86/I386PicAddress.h"

#include <map>
#include <optional>
#include <tuple>

namespace neverd {

/// The operation of a function that defines each value, indexed on first
/// use.  Adding or moving operations invalidates it.
class MedDefinitionIndex {
public:
  explicit MedDefinitionIndex(const MedFunc &Func) : Func(Func) {}

  void invalidate() {
    Definitions.clear();
    Built = false;
  }

  /// The operation that defines \p V, or null.
  const MedOp *find(const MedVar &V) {
    if (!Built) {
      for (const MedBlock &Block : Func.Blocks)
        for (const MedOp &Op : Block.Ops)
          if (Op.Output.Size > 0)
            Definitions.emplace(key(Op.Output), &Op);
      Built = true;
    }
    const auto It = Definitions.find(key(V));
    return It == Definitions.end() ? nullptr : It->second;
  }

private:
  static std::tuple<int, int, int> key(const MedVar &V) {
    return {static_cast<int>(V.Kind), V.Id, V.SSAVer};
  }

  const MedFunc &Func;
  std::map<std::tuple<int, int, int>, const MedOp *> Definitions;
  bool Built = false;
};

/// The constant address of the word whose value the indirect call \p Call
/// jumps to: the call's own constant slot, or a load whose value reaches the
/// call through copies, from an address that folds to a constant directly,
/// through copies, or as an i386 get-PC sum.  \p Lookup gives the operation
/// that defines a value, or null.
template <typename LookupDefinition>
std::optional<va_t> indirectCallSlot(const MedFunc &Func, const MedOp &Call,
                                     LookupDefinition Lookup) {
  if (Call.Opcode != NdOp::INDIR_CALL || Call.NumInputs == 0)
    return std::nullopt;
  if (Call.Inputs[0].isConst())
    return Call.Inputs[0].ConstVal;
  MedVar Target = Call.Inputs[0];
  const MedOp *Load = nullptr;
  for (int Step = 0; Step < limits::kCallTargetSlotDepth && !Load; ++Step) {
    const MedOp *Def = Lookup(Target);
    if (!Def || Def->NumInputs < 1)
      return std::nullopt;
    if (Def->Opcode == NdOp::LOAD)
      Load = Def;
    else if (Def->Opcode == NdOp::COPY && !Def->Inputs[0].isConst())
      Target = Def->Inputs[0];
    else
      return std::nullopt;
  }
  if (!Load || Load->MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return std::nullopt;
  MedVar Address = Load->Inputs[0];
  for (int Step = 0; Step < limits::kCallTargetSlotDepth; ++Step) {
    if (Address.isConst())
      return Address.ConstVal;
    const MedOp *Def = Lookup(Address);
    if (!Def || Def->Opcode != NdOp::COPY || Def->NumInputs < 1)
      break;
    Address = Def->Inputs[0];
  }
  if (const std::optional<uint64_t> Folded =
          foldI386PicAddress(Func, Address, Lookup))
    return static_cast<va_t>(*Folded);
  return std::nullopt;
}

} // namespace neverd

#endif // NEVERD_IR_MED_INDIRECTCALLSLOT_H
