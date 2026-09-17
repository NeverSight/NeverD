#include "neverd/ir/med/MedSourceParameterUses.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/med/MedIR.h"

#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <tuple>

namespace neverd {
namespace {
using ValueKey = std::tuple<MedVar::VarKind, int, int>;

ValueKey key(const MedVar &Value) {
  return {Value.Kind, Value.Id, Value.SSAVer};
}

enum Use : unsigned { Pointer = 1, Scalar = 2 };

struct ValueUses {
  unsigned Roles = 0;
  // Backward identity edges; narrow copies propagate only conflicting uses.
  std::vector<std::pair<ValueKey, bool>> Inputs;
};
} // namespace

std::optional<std::map<uint64_t, uint64_t>>
observedMedSourceEntryBytes(const MedFunc &Function,
                            const SourceFunctionTypeHint &Hint,
                            SourceEntryDemand Demand) {
  std::string Error;
  if (Function.Blocks.empty() || !Hint.HasExplicitABI ||
      !validateSourceABI(Hint, Error))
    return std::nullopt;
  // A graph without any observed return may be an unfinished lifting
  // fragment. It cannot prove that an incoming byte is unobservable in the
  // complete source function. Retain the original validation in that case.
  bool HasReturn = false;
  for (const auto &Block : Function.Blocks)
    for (const auto &Op : Block.Ops)
      HasReturn |= Op.Opcode == NdOp::RETURN;
  if (!HasReturn)
    return std::nullopt;
  struct Node {
    MedVar Value;
    const MedOp *Definition = nullptr;
    const PhiNode *Phi = nullptr;
    const MedCallClobber *Clobber = nullptr;
    uint64_t Needed = 0;
  };
  std::map<ValueKey, Node> Nodes;
  bool Valid = true;
  auto Mask = [](unsigned Bytes) -> uint64_t {
    return Bytes >= 64 ? ~uint64_t(0) : (uint64_t(1) << Bytes) - 1;
  };
  auto Observe = [&](const MedVar &Value) -> Node * {
    if (Value.isConst() || !Value.Size)
      return nullptr;
    if (Value.Size > 64 || Nodes.size() > 65536 ||
        (Value.Kind == MedVar::Reg &&
         Value.RegOff > ~uint64_t(0) - Value.Size)) {
      Valid = false;
      return nullptr;
    }
    auto [It, Added] = Nodes.try_emplace(key(Value));
    if (Added)
      It->second.Value = Value;
    else if (Value.Kind == MedVar::Reg &&
             It->second.Value.RegOff != Value.RegOff)
      Valid = false;
    It->second.Value.Size = std::max(It->second.Value.Size, Value.Size);
    return &It->second;
  };
  for (const auto &Block : Function.Blocks) {
    for (const auto &Phi : Block.Phis) {
      if (auto *N = Observe(Phi.Output)) {
        Valid &= !N->Definition && !N->Phi;
        N->Phi = &Phi;
      }
      for (const auto &[Pred, Value] : Phi.Args)
        Observe(Value);
    }
    for (const auto &Op : Block.Ops) {
      const bool Seed = Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
                        Op.Output == Op.Inputs[0] &&
                        Op.Output.Size == Op.Inputs[0].Size;
      if (auto *N = Observe(Op.Output); N && !Seed) {
        Valid &= !N->Definition && !N->Phi;
        N->Definition = &Op;
      }
      for (unsigned I = 0; I < Op.NumInputs; ++I)
        Observe(Op.Inputs[I]);
    }
  }
  // Calls can create version zero as their first implicit definition. Such a
  // value is not an entry input. Preserve only an explicitly recorded prefix;
  // demand for any other byte leaves this proof unavailable.
  for (const auto &Clobber : Function.CallClobbers) {
    if (auto *N = Observe(Clobber.Value)) {
      Valid &= !N->Definition && !N->Phi && !N->Clobber &&
               Clobber.PreservedPrefixSize <= Clobber.Value.Size &&
               Clobber.PreservedPrefixSize <= Clobber.PreservedInput.Size &&
               (!Clobber.PreservedPrefixSize ||
                key(Clobber.Value) != key(Clobber.PreservedInput));
      N->Clobber = &Clobber;
    }
    if (Clobber.PreservedPrefixSize)
      Observe(Clobber.PreservedInput);
  }
  std::deque<ValueKey> Pending;
  auto Need = [&](const MedVar &Value, uint64_t Bytes) {
    if (auto *N = Observe(Value)) {
      const auto Added = Bytes & Mask(Value.Size) & ~N->Needed;
      if (Added) {
        N->Needed |= Added;
        Pending.push_back(key(Value));
      }
    }
  };
  // Root every observed version of a declared result register. Keeping earlier
  // writes is conservative across arbitrary CFGs and never assumes a nearest
  // predecessor owns the return. Only the declaration's bytes are observable.
  auto RootReturn = [&](const SourceABIValueLocation &Location) {
    if (Location.Kind != SourceABICarrierKind::IntegerRegister &&
        Location.Kind != SourceABICarrierKind::FloatingRegister)
      return;
    for (const auto &[Key, N] : Nodes) {
      const auto &V = N.Value;
      if (V.Kind != MedVar::Reg)
        continue;
      uint64_t Required = 0;
      for (unsigned I = 0; I < V.Size; ++I)
        if (V.RegOff + I >= Location.RegisterOffset &&
            V.RegOff + I - Location.RegisterOffset < Location.ValueBytes)
          Required |= uint64_t(1) << I;
      Need(V, Required);
    }
  };
  if (Demand == SourceEntryDemand::EffectsAndReturns) {
    RootReturn(Hint.ReturnLocation);
    for (const auto &Location : Hint.ReturnComponents)
      RootReturn(Location);
  }
  for (const auto &Block : Function.Blocks)
    for (const auto &Op : Block.Ops) {
      if (Op.Opcode == NdOp::RETURN) {
        if (Demand == SourceEntryDemand::EffectsOnly)
          continue;
        for (unsigned I = 0; I < Op.NumInputs; ++I)
          if (Op.Inputs[I].Kind != MedVar::Reg)
            Need(Op.Inputs[I], Mask(Op.Inputs[I].Size));
        continue;
      }
      const bool Effect =
          Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE ||
          Op.Opcode == NdOp::ATOMIC_XCHG || Op.Opcode == NdOp::ATOMIC_ADD ||
          Op.Opcode == NdOp::ATOMIC_CMPXCHG || Op.Opcode == NdOp::CALL ||
          Op.Opcode == NdOp::INDIR_CALL || Op.Opcode == NdOp::INTRINSIC ||
          Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR ||
          Op.Opcode == NdOp::INDIR_BR ||
          Op.MemoryOrdering != NdMemoryOrdering::None ||
          Op.MemoryAddressSpace != NdMemoryAddressSpace::Default;
      if (Effect)
        for (unsigned I = 0; I < Op.NumInputs; ++I)
          Need(Op.Inputs[I], Mask(Op.Inputs[I].Size));
    }
  for (const auto &Call : Function.CallInfos)
    for (const auto &Argument : Call.Args)
      Need(Argument, Mask(Argument.Size));
  for (const auto &Clobber : Function.CallClobbers)
    if (Clobber.PreservedPrefixSize)
      Need(Clobber.PreservedInput, Mask(Clobber.PreservedPrefixSize));
  size_t Remaining = 262144;
  std::map<uint64_t, uint64_t> Result;
  // A generic parameter with no graph occurrence has no dead-byte proof.
  // Keep that original evidence, rather than treating an absent graph as a
  // certificate that the incoming carrier is irrelevant.
  for (const auto &Parameter : Function.Params)
    if (Parameter.Id >= 0 && Parameter.RegOff != kNoParamReg &&
        std::none_of(Nodes.begin(), Nodes.end(), [&](const auto &Entry) {
          const auto &V = Entry.second.Value;
          return (V.Kind == MedVar::Reg || V.Kind == MedVar::Param) &&
                 V.RegOff == Parameter.RegOff;
        }))
      Result[Parameter.RegOff] |= Mask(Parameter.Size);
  while (Valid && !Pending.empty() && Remaining--) {
    const auto Key = Pending.front();
    Pending.pop_front();
    const auto &N = Nodes.at(Key);
    const uint64_t Bytes = N.Needed;
    if (N.Clobber) {
      const auto &Clobber = *N.Clobber;
      const auto Preserved = Mask(Clobber.PreservedPrefixSize);
      Valid &= !(Bytes & ~Preserved);
      if (Clobber.PreservedPrefixSize)
        Need(Clobber.PreservedInput, Bytes & Preserved);
      continue;
    }
    if (N.Phi) {
      for (const auto &[Pred, Input] : N.Phi->Args) {
        Valid &= Input.Size == N.Phi->Output.Size;
        Need(Input, Bytes);
      }
      continue;
    }
    if (!N.Definition) {
      if ((N.Value.Kind == MedVar::Reg && N.Value.SSAVer == 0) ||
          (N.Value.Kind == MedVar::Param && N.Value.RegOff != kNoParamReg))
        Result[N.Value.RegOff] |= Bytes;
      else if (N.Value.Kind != MedVar::Param && N.Value.Kind != MedVar::Stack)
        Valid = false;
      continue;
    }
    const auto &Op = *N.Definition;
    if (Op.Opcode == NdOp::CONCAT && Op.NumInputs == 2 &&
        Op.Inputs[0].Size + Op.Inputs[1].Size == Op.Output.Size) {
      Need(Op.Inputs[1], Bytes);
      Need(Op.Inputs[0],
           Op.Inputs[1].Size < 64 ? Bytes >> Op.Inputs[1].Size : 0);
    } else if (Op.Opcode == NdOp::SUBBYTES) {
      if (Op.NumInputs != 2 || !Op.Inputs[1].isConst() ||
          Op.Inputs[1].ConstVal >= 64 ||
          Op.Inputs[1].ConstVal + Op.Output.Size > Op.Inputs[0].Size) {
        Valid = false;
        continue;
      }
      Need(Op.Inputs[0], Bytes << Op.Inputs[1].ConstVal);
    } else if ((Op.Opcode == NdOp::COPY || Op.Opcode == NdOp::INT_ZEXT ||
                Op.Opcode == NdOp::INT_SEXT) &&
               Op.NumInputs == 1) {
      Need(Op.Inputs[0], Bytes);
      if (Bytes & ~Mask(Op.Inputs[0].Size)) {
        if (Op.Opcode == NdOp::INT_SEXT && Op.Inputs[0].Size)
          Need(Op.Inputs[0], uint64_t(1) << (Op.Inputs[0].Size - 1));
        else if (Op.Opcode != NdOp::INT_ZEXT)
          Valid = false;
      }
    } else {
      for (unsigned I = 0; I < Op.NumInputs; ++I)
        Need(Op.Inputs[I], Mask(Op.Inputs[I].Size));
    }
  }
  return Valid && Pending.empty() ? std::optional(std::move(Result))
                                  : std::nullopt;
}

std::optional<std::set<uint64_t>>
observedMedSourceEntryRegisters(const MedFunc &Function,
                                const SourceFunctionTypeHint &Hint,
                                SourceEntryDemand Demand) {
  const auto Bytes = observedMedSourceEntryBytes(Function, Hint, Demand);
  if (!Bytes)
    return std::nullopt;
  std::set<uint64_t> Registers;
  for (const auto &[Register, Mask] : *Bytes)
    Registers.insert(Register);
  return Registers;
}

std::vector<bool> inferMedSourcePointerParameters(const MedFunc &Function) {
  std::vector<bool> Result(Function.Params.size(), false);
  std::map<ValueKey, ValueUses> Values;
  std::set<ValueKey> Definitions;
  std::map<ValueKey, const MedVar *> Parameters;
  std::map<uint64_t, const MedVar *> EntryRegisters;
  for (const auto &Parameter : Function.Params) {
    if (Parameter.Id < 0)
      continue;
    if (Parameter.Kind != MedVar::Param ||
        !Parameters.emplace(key(Parameter), &Parameter).second ||
        (Parameter.RegOff != kNoParamReg &&
         !EntryRegisters.emplace(Parameter.RegOff, &Parameter).second))
      return Result;
  }

  bool Ambiguous = false;
  auto Observe = [&](const MedVar &Value) {
    if (Value.Kind == MedVar::Reg && Value.SSAVer == 0) {
      const auto Found = EntryRegisters.find(Value.RegOff);
      if (Found != EntryRegisters.end()) {
        // Generic ABI inference records Params separately from SSA live-ins.
        // Only the entry version at that exact physical location represents
        // the parameter; later versions of the register are unrelated.
        Values[key(Value)].Inputs.emplace_back(
            key(*Found->second), Value.Size == 8 && Found->second->Size == 8);
      }
    }
    if (Value.Kind != MedVar::Param)
      return;
    const auto Found = Parameters.find(key(Value));
    if (Found == Parameters.end() || Found->second->RegOff != Value.RegOff)
      Ambiguous = true;
  };
  std::vector<std::pair<ValueKey, unsigned>> Pending;
  auto Seed = [&](const MedVar &Value, unsigned Role) {
    Observe(Value);
    if (!Value.isConst())
      Pending.emplace_back(key(Value),
                           Role == Pointer && Value.Size != 8 ? Scalar : Role);
  };
  auto Connect = [&](const MedVar &Output, const MedVar &Input) {
    Observe(Input);
    if (!Input.isConst())
      Values[key(Output)].Inputs.emplace_back(key(Input), Output.Size == 8 &&
                                                              Input.Size == 8);
  };
  auto Define = [&](const MedVar &Output) {
    if (Output.Size && !Output.isConst() &&
        (Output.Kind == MedVar::Param ||
         (Output.Kind == MedVar::Reg && Output.SSAVer == 0 &&
          EntryRegisters.count(Output.RegOff)) ||
         !Definitions.insert(key(Output)).second))
      Ambiguous = true;
  };

  for (const auto &Block : Function.Blocks) {
    for (const auto &Phi : Block.Phis) {
      Define(Phi.Output);
      for (const auto &[Predecessor, Input] : Phi.Args)
        Connect(Phi.Output, Input);
    }
    for (const auto &Op : Block.Ops) {
      if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
          Op.Output == Op.Inputs[0] && Op.Output.Size == Op.Inputs[0].Size)
        continue;
      Define(Op.Output);
      if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1) {
        Connect(Op.Output, Op.Inputs[0]);
        // A narrowed view is a scalar use even if its result is dead.
        if (Op.Output.Size != 8 || Op.Inputs[0].Size != 8)
          Seed(Op.Inputs[0], Scalar);
        continue;
      }
      std::string Error;
      const auto Parameters =
          Op.SourceCallHint ? sourceABIParameters(Op.SourceCallHint->Signature)
                            : std::vector<SourceABIParameter>{};
      const bool BoundCall =
          (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) &&
          Op.SourceCallHint &&
          validateSourceABI(Op.SourceCallHint->Signature, Error) &&
          Op.NumInputs == Parameters.size() + 1;
      for (unsigned Index = 0; Index < Op.NumInputs; ++Index) {
        unsigned Role = Scalar;
        if (BoundCall && Index) {
          const auto &Type = Parameters[Index - 1].Type;
          if (Type->Kind == NdTypeKind::Ptr && Type->Size == 8)
            Role = Pointer;
        }
        // Other operations may consume integer bits, compare addresses, or
        // change their identity. They never seed pointer evidence here.
        Seed(Op.Inputs[Index], Role);
      }
    }
  }
  if (Ambiguous)
    return Result;

  // Each role visits each SSA value once. Loops and shared PHI chains converge
  // without recursion, path enumeration, or dependence on block order.
  for (size_t Cursor = 0; Cursor < Pending.size(); ++Cursor) {
    const auto [Value, Roles] = Pending[Cursor];
    auto &Node = Values[Value];
    const unsigned NewRoles = Roles & ~Node.Roles;
    if (!NewRoles)
      continue;
    Node.Roles |= NewRoles;
    for (const auto &[Input, PreservesPointer] : Node.Inputs)
      Pending.emplace_back(Input, PreservesPointer ? NewRoles : Scalar);
  }
  for (size_t Index = 0; Index < Function.Params.size(); ++Index) {
    const auto &Parameter = Function.Params[Index];
    const auto Found = Values.find(key(Parameter));
    Result[Index] = Parameter.Id >= 0 && Parameter.Size == 8 &&
                    Found != Values.end() && Found->second.Roles == Pointer;
  }
  return Result;
}
} // namespace neverd
