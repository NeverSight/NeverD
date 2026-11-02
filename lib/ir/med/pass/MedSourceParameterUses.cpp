//===- MedSourceParameterUses.cpp - Source parameter use proofs -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Prove observed entry bytes and whole-value source pointer roles.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/MedSourceParameterUses.h"

#include "neverd/ir/Limits.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/ir/med/MedNoReturn.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <tuple>
#include <utility>

namespace neverd {
namespace {
using ValueKey = std::tuple<MedVar::VarKind, int, int>;

ValueKey key(const MedVar &Value) {
  return {Value.Kind, Value.Id, Value.SSAVer};
}

// Forward-only parameters can have no SSA id. Their recorded physical slots
// distinguish them; this adds no value for an unobserved incoming register.
using PointerValueKey = std::pair<ValueKey, uint64_t>;
PointerValueKey pointerKey(const MedVar &Value) {
  return {key(Value),
          (Value.Kind == MedVar::Param || Value.Kind == MedVar::Reg) &&
                  Value.Id < 0
              ? Value.RegOff
              : 0};
}

enum Use : unsigned { Pointer = 1, Scalar = 2 };

struct ValueUses {
  unsigned Roles = 0;
  // Null after pointer evidence means that its declarations disagree or do not
  // carry a supported object pointee. The pointer role itself remains valid.
  TypeRef PointerType;
  // Backward identity edges; narrow copies propagate only conflicting uses.
  std::vector<std::pair<PointerValueKey, bool>> Inputs;
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
  // A graph without an observed return cannot prove an incoming byte dead.
  // Effects-only demand can still establish positive input evidence when the
  // shared termination proof independently certifies the no-return graph.
  // A flag alone never turns an unfinished fragment into a complete body.
  bool HasReturn = false;
  for (const auto &Block : Function.Blocks)
    for (const auto &Op : Block.Ops)
      HasReturn |= Op.Opcode == NdOp::RETURN;
  if (!HasReturn &&
      !(Demand == SourceEntryDemand::EffectsOnly && Function.DoesNotReturn &&
        hasProvenNoReturnExit(Function, Hint.Architecture)))
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
    if (Value.Size > 64 ||
        Nodes.size() >
            ::neverd::limits::ir::
                kMedSourceParameterUsesObservedMedSourceEntryBytesObserveNodesSizeLimit ||
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
        Location.Kind != SourceABICarrierKind::FloatingRegister &&
        Location.Kind != SourceABICarrierKind::BooleanRegister)
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
      // Source stack preparation already proved these stores unobservable.
      // Rooting their addresses would revive an otherwise unused entry SP.
      if (Op.Opcode == NdOp::STORE && Op.PrologueSave)
        continue;
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
  size_t Remaining = ::neverd::limits::ir::
      kMedSourceParameterUsesObservedMedSourceEntryBytesRemaining;
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

namespace {
// Source ABI equality compares carrier structure, not C's nominal record or
// typedef identity. Keep those names as well when retaining a declaration.
bool samePointerType(const TypeRef &Left, const TypeRef &Right) {
  if (Left == Right)
    return true;
  if (!equalSourceTypes(Left, Right))
    return false;
  auto L = Left, R = Right;
  while (L->Kind == NdTypeKind::Ptr && R->Kind == NdTypeKind::Ptr) {
    if (L->SourceName != R->SourceName || L->TypedefName != R->TypedefName)
      return false;
    L = L->Pointee;
    R = R->Pointee;
  }
  return L->SourceName == R->SourceName && L->TypedefName == R->TypedefName;
}

TypeRef objectPointerType(const TypeRef &Type) {
  if (!Type || !equalSourceTypes(Type, Type))
    return nullptr;
  auto Pointee = Type;
  while (Pointee->Kind == NdTypeKind::Ptr)
    Pointee = Pointee->Pointee;
  // Callback, enum and anonymous-record declarations need their own source
  // identity proof. Keep their previously established generic pointer role.
  if (Pointee->Kind == NdTypeKind::Func ||
      (Pointee->Kind == NdTypeKind::Struct &&
       (Pointee->IsEnum || Pointee->SourceName.empty())))
    return nullptr;
  return Type;
}

// Forwarding proves the nominal pointee, not the wrapper's original typedef
// or any object extent. Keep that distinction in both source pipelines.
TypeRef pointerCandidate(const TypeRef &Type) {
  TypeRef Result;
  auto *Next = &Result;
  for (auto Node = Type; Node; Node = Node->Pointee) {
    auto Copy = std::make_shared<NdType>();
    Copy->Kind = Node->Kind;
    Copy->Size = Node->Kind == NdTypeKind::Struct ? 0 : Node->Size;
    Copy->IsSigned = Node->IsSigned;
    Copy->IsEnum = Node->IsEnum;
    Copy->SourceName = Node->SourceName;
    *Next = Copy;
    Next = &Copy->Pointee;
    if (Node->Kind != NdTypeKind::Ptr)
      break;
  }
  return Result;
}
} // namespace

static std::vector<TypeRef> inferPointerParameters(
    const MedFunc &Function, unsigned PointerBytes,
    const std::map<va_t, std::vector<TypeRef>> *CalleeParameters,
    const std::map<va_t, std::vector<TypeRef>> *CalleeRegisterParameters =
        nullptr) {
  std::vector<TypeRef> Result(Function.Params.size());
  std::map<PointerValueKey, ValueUses> Values;
  std::set<PointerValueKey> Definitions;
  std::map<PointerValueKey, const MedVar *> Parameters;
  std::map<uint64_t, const MedVar *> EntryRegisters;
  for (const auto &Parameter : Function.Params) {
    if (Parameter.Id < 0 && Parameter.RegOff == kNoParamReg)
      continue;
    if (Parameter.Kind != MedVar::Param ||
        !Parameters.emplace(pointerKey(Parameter), &Parameter).second ||
        (Parameter.RegOff != kNoParamReg &&
         !EntryRegisters.emplace(Parameter.RegOff, &Parameter).second))
      return Result;
  }

  // SSA version zero can be the first implicit definition after a call.
  // Its physical register alone is not evidence of an incoming parameter.
  std::set<PointerValueKey> Clobbered;
  for (const auto &Clobber : Function.CallClobbers)
    Clobbered.insert(pointerKey(Clobber.Value));
  std::map<std::pair<int, int>, const MedCallInfo *> Calls;
  for (const auto &Call : Function.CallInfos)
    Calls.emplace(std::pair(Call.BlockId, Call.OpIdx), &Call);

  bool Ambiguous = false;
  std::map<PointerValueKey, std::pair<PointerValueKey, bool>> EntryUses;
  auto Observe = [&](const MedVar &Value) {
    if (Value.Kind == MedVar::Reg && Value.SSAVer == 0 &&
        !Clobbered.count(pointerKey(Value))) {
      const auto Found = EntryRegisters.find(Value.RegOff);
      if (Found != EntryRegisters.end()) {
        // Generic ABI inference records Params separately from SSA live-ins.
        // Only the entry version at that exact physical location represents
        // the parameter; later versions of the register are unrelated.
        const auto Parameter = pointerKey(*Found->second);
        const bool Whole =
            Value.Size == PointerBytes && Found->second->Size == PointerBytes;
        const auto [Use, Added] =
            EntryUses.emplace(pointerKey(Value), std::pair(Parameter, Whole));
        if (!Added) {
          Ambiguous |= Use->second.first != Parameter;
          Use->second.second &= Whole;
        }
      }
    }
    if (Value.Kind != MedVar::Param)
      return;
    const auto Found = Parameters.find(pointerKey(Value));
    if (Found == Parameters.end() && Value.Id >= 0 &&
        static_cast<size_t>(Value.Id) < Function.Params.size()) {
      const auto &Parameter = Function.Params[Value.Id];
      if (Parameter.Id < 0 && Parameter.RegOff != kNoParamReg &&
          Parameter.RegOff == Value.RegOff &&
          Parameter.SSAVer == Value.SSAVer) {
        Values[pointerKey(Value)].Inputs.emplace_back(
            pointerKey(Parameter),
            Value.Size == PointerBytes && Parameter.Size == PointerBytes);
        return;
      }
    }
    if (Found == Parameters.end() || Found->second->RegOff != Value.RegOff)
      Ambiguous = true;
  };
  struct UseFact {
    PointerValueKey Value;
    unsigned Role;
    TypeRef Type;
  };
  std::vector<UseFact> Pending;
  auto Seed = [&](const MedVar &Value, unsigned Role, TypeRef Type = nullptr) {
    Observe(Value);
    if (!Value.isConst())
      Pending.push_back(
          {pointerKey(Value),
           Role == Pointer && Value.Size != PointerBytes ? Scalar : Role,
           Role == Pointer ? objectPointerType(Type) : nullptr});
  };
  auto Connect = [&](const MedVar &Output, const MedVar &Input) {
    Observe(Input);
    if (!Input.isConst())
      Values[pointerKey(Output)].Inputs.emplace_back(
          pointerKey(Input),
          Output.Size == PointerBytes && Input.Size == PointerBytes);
  };
  auto Define = [&](const MedVar &Output) {
    if (Output.Size && !Output.isConst() &&
        (Output.Kind == MedVar::Param ||
         !Definitions.insert(pointerKey(Output)).second))
      Ambiguous = true;
  };

  for (const auto &Block : Function.Blocks) {
    for (const auto &Phi : Block.Phis) {
      Define(Phi.Output);
      for (const auto &[Predecessor, Input] : Phi.Args)
        Connect(Phi.Output, Input);
    }
    for (size_t OpIndex = 0; OpIndex < Block.Ops.size(); ++OpIndex) {
      const auto &Op = Block.Ops[OpIndex];
      if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
          Op.Output == Op.Inputs[0] && Op.Output.Size == Op.Inputs[0].Size)
        continue;
      Define(Op.Output);
      if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1) {
        Connect(Op.Output, Op.Inputs[0]);
        // A narrowed view is a scalar use even if its result is dead.
        if (Op.Output.Size != PointerBytes || Op.Inputs[0].Size != PointerBytes)
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
      if (!BoundCall &&
          (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL)) {
        const auto Found = Calls.find({Block.Id, static_cast<int>(OpIndex)});
        if (Found != Calls.end()) {
          const auto &Call = *Found->second;
          const std::vector<TypeRef> *Types = nullptr;
          const auto *Declarations = Call.ArgumentsInCalleeRegisterOrder
                                         ? CalleeRegisterParameters
                                         : CalleeParameters;
          if (Declarations && !Op.SourceCallHint)
            if (auto Declared = Declarations->find(Call.TargetAddr);
                Declared != Declarations->end() &&
                Declared->second.size() == Call.Args.size())
              Types = &Declared->second;
          // Explicit operands and a recovered list must describe the same
          // call before a declaration can assign roles to those operands.
          bool SameArguments = Op.NumInputs == 1;
          if (Op.NumInputs == Call.Args.size() + 1) {
            SameArguments = true;
            for (size_t I = 0; I < Call.Args.size(); ++I)
              SameArguments &= Op.Inputs[I + 1] == Call.Args[I] &&
                               Op.Inputs[I + 1].Size == Call.Args[I].Size;
          }
          for (size_t I = 0; I < Call.Args.size(); ++I) {
            const auto Type = Types ? (*Types)[I] : nullptr;
            Seed(Call.Args[I],
                 SameArguments && Type && Type->Kind == NdTypeKind::Ptr &&
                         Type->Size == PointerBytes
                     ? Pointer
                     : Scalar,
                 Type);
          }
          if (SameArguments) {
            if (Op.NumInputs)
              Seed(Op.Inputs[0], Scalar);
            continue;
          }
        } else if (CalleeParameters && !Op.SourceCallHint && Op.NumInputs &&
                   Op.Inputs[0].isConst()) {
          // Fixed scalar declarations also bind explicit source-order operands
          // before SSA. Independent register-bank routes may have no CallInfo.
          if (const auto Declared =
                  CalleeParameters->find(Op.Inputs[0].ConstVal);
              Declared != CalleeParameters->end() &&
              Op.NumInputs == Declared->second.size() + 1) {
            Seed(Op.Inputs[0], Scalar);
            for (size_t I = 0; I < Declared->second.size(); ++I) {
              const auto &Type = Declared->second[I];
              Seed(Op.Inputs[I + 1],
                   Type && Type->Kind == NdTypeKind::Ptr &&
                           Type->Size == PointerBytes
                       ? Pointer
                       : Scalar,
                   Type);
            }
            continue;
          }
        }
      }
      if (!BoundCall &&
          (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) &&
          !Calls.count({Block.Id, static_cast<int>(OpIndex)}) &&
          Op.NumInputs == 1 && !Op.ExactArguments &&
          !(Op.IntegerCallSignature &&
            Op.IntegerCallSignature->Parameters.empty()) &&
          !(Op.ScalarCallSignature && Op.ScalarCallSignature->Scalars.empty() &&
            Op.ScalarCallSignature->ParameterBytes.empty()))
        // A target-only call can observe hidden inputs. Ignoring it would
        // turn an incomplete use graph into a complete pointer-use proof.
        Ambiguous = true;
      for (unsigned Index = 0; Index < Op.NumInputs; ++Index) {
        unsigned Role = Scalar;
        if (BoundCall && Index) {
          const auto &Type = Parameters[Index - 1].Type;
          if (Type->Kind == NdTypeKind::Ptr && Type->Size == PointerBytes)
            Role = Pointer;
        }
        // Other operations may consume integer bits, compare addresses, or
        // change their identity. They never seed pointer evidence here.
        Seed(Op.Inputs[Index], Role,
             BoundCall && Index ? Parameters[Index - 1].Type : nullptr);
      }
    }
  }
  if (Ambiguous)
    return Result;
  // A local definition may own version zero, including a call result in the
  // same register as a forward-only parameter. It is not an entry observation;
  // independent explicit Param uses still refer to their original slots.
  for (const auto &[Value, Parameter] : EntryUses)
    if (!Definitions.count(Value))
      Values[Value].Inputs.push_back(Parameter);

  // Each role visits each value once. A pointee changes at most twice:
  // unobserved -> one declared type -> ambiguous. Loops and shared PHI chains
  // converge without recursion, path enumeration or block-order dependence.
  for (size_t Cursor = 0; Cursor < Pending.size(); ++Cursor) {
    const auto [Value, Roles, Type] = Pending[Cursor];
    auto &Node = Values[Value];
    unsigned Changes = Roles & ~Node.Roles;
    if (Roles & Pointer) {
      if (!(Node.Roles & Pointer)) {
        Node.PointerType = Type;
      } else if (Node.PointerType &&
                 (!Type || !samePointerType(Node.PointerType, Type))) {
        Node.PointerType.reset();
        Changes |= Pointer;
      }
    }
    Node.Roles |= Roles;
    if (!Changes)
      continue;
    for (const auto &[Input, PreservesPointer] : Node.Inputs)
      Pending.push_back({Input, PreservesPointer ? Changes : Scalar,
                         PreservesPointer ? Node.PointerType : nullptr});
  }
  TypeRef GenericPointer;
  for (size_t Index = 0; Index < Function.Params.size(); ++Index) {
    const auto &Parameter = Function.Params[Index];
    const auto Found = Values.find(pointerKey(Parameter));
    if ((Parameter.Id < 0 && Parameter.RegOff == kNoParamReg) ||
        Parameter.Size != PointerBytes || Found == Values.end() ||
        Found->second.Roles != Pointer)
      continue;
    if (Found->second.PointerType) {
      Result[Index] = pointerCandidate(Found->second.PointerType);
    } else {
      if (!GenericPointer) {
        GenericPointer = NdType::makePtr(NdType::makeVoid());
        GenericPointer->Size = PointerBytes;
      }
      Result[Index] = GenericPointer;
    }
  }
  return Result;
}

std::vector<TypeRef> inferMedSourcePointerParameterTypes(
    const MedFunc &Function, Arch Architecture,
    const std::map<va_t, std::vector<TypeRef>> &CalleeParameters,
    const std::map<va_t, std::vector<TypeRef>> &CalleeRegisterParameters) {
  return inferPointerParameters(Function,
                                getTargetRegInfo(Architecture).PointerSize,
                                &CalleeParameters, &CalleeRegisterParameters);
}

std::vector<bool> inferMedSourcePointerParameters(const MedFunc &Function) {
  const auto Types = inferPointerParameters(Function, 8, nullptr);
  std::vector<bool> Result;
  Result.reserve(Types.size());
  for (const auto &Type : Types)
    Result.push_back(bool(Type));
  return Result;
}

std::vector<bool> inferMedSourcePointerParameters(
    const MedFunc &Function, Arch Architecture,
    const std::map<va_t, std::vector<TypeRef>> &CalleeParameters) {
  const auto Types = inferMedSourcePointerParameterTypes(Function, Architecture,
                                                         CalleeParameters);
  std::vector<bool> Result;
  Result.reserve(Types.size());
  for (const auto &Type : Types)
    Result.push_back(bool(Type));
  return Result;
}

} // namespace neverd
