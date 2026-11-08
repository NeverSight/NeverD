//===- SBFCallXContextFlow.cpp - Interprocedural CALLX must-facts ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Solves whole-function CFGs with input-keyed argument, return and scratch
/// summaries. Only the final summary dependency graph supplies CALLX evidence.
///
//===----------------------------------------------------------------------===//

#include "SBFCallXMemoryEffects.h"

#include "neverd/sbf/analysis/SBFAnalysisLimits.h"
#include "neverd/sbf/analysis/SBFCallXContexts.h"
#include "neverd/sbf/runtime/SBFSemantics.h"
#include "neverd/sbf/runtime/SBFSyscalls.h"
#include "neverd/sbf/runtime/SBFVMConfig.h"

#include "llvm/ADT/FoldingSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <array>
#include <bit>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd::sbf {
namespace {

using namespace callx_detail;

using MissingRegisters = std::array<CallXMissingFact, kRegisterCount>;

constexpr uint16_t registerBit(unsigned Register) {
  return uint16_t{1} << Register;
}

constexpr uint16_t AllRegisters = (uint16_t{1} << kRegisterCount) - 1;

struct CallInputs {
  uint16_t Registers = AllRegisters;
  bool Memory = true;
};

struct InstructionDependencies {
  uint16_t Reads = 0;
  uint16_t Writes = 0;
  bool Memory = false;
};

InstructionDependencies instructionDependencies(const MedInstruction &I,
                                                CallInputs Callee = {}) {
  InstructionDependencies Result;
  if (I.Semantics.WritesDestination)
    Result.Writes = registerBit(I.Dst);
  if (I.Call != CallKind::None) {
    Result.Writes |= (uint16_t{1} << callClobberedRegisterCount(I.Call)) - 1;
    if (I.Call == CallKind::Syscall) {
      // The host sees only its five argument carriers. Its memory is opaque.
      Result.Reads = ((uint16_t{1} << kArgumentRegisterCount) - 1)
                     << kFirstArgumentRegister;
      Result.Memory = true;
    } else {
      Result.Reads = Callee.Registers;
      Result.Memory = Callee.Memory;
    }
    if (I.Op == Operation::CallX)
      Result.Reads |= registerBit(I.CallRegister);
    return Result;
  }
  if (I.Op == Operation::Exit)
    Result.Reads |= registerBit(kReturnRegister);
  Result.Memory = I.Op == Operation::Load;
  switch (I.Form) {
  case OperandForm::Dst:
  case OperandForm::Endian:
  case OperandForm::BranchImm:
  case OperandForm::StoreImm:
    Result.Reads |= registerBit(I.Dst);
    break;
  case OperandForm::DstImm:
    if (I.Op != Operation::Mov)
      Result.Reads |= registerBit(I.Dst);
    break;
  case OperandForm::DstSrc:
    if (I.Op != Operation::Mov)
      Result.Reads |= registerBit(I.Dst);
    Result.Reads |= registerBit(I.Src);
    break;
  case OperandForm::Load:
    Result.Reads |= registerBit(I.Src);
    break;
  case OperandForm::StoreReg:
  case OperandForm::BranchReg:
    Result.Reads |= registerBit(I.Dst) | registerBit(I.Src);
    break;
  default:
    break;
  }
  return Result;
}

void pushAbstractFrame(RegisterState &State, const SBFProgram &Program) {
  if (versionHasFeature(Program.Low.TheVersion,
                        VersionFeature::ManualStackFrames))
    return;
  RegisterValue &Frame = State[kFramePointerRegister];
  const uint64_t Stride =
      automaticFrameStride(Program.Low.TheVersion, Program.Config);
  if (Frame.ValueKind == RegisterValue::Kind::Constant) {
    Frame.Value += Stride;
  } else if (Frame.ValueKind == RegisterValue::Kind::StackAddress) {
    int64_t Offset;
    if (llvm::AddOverflow(Frame.Offset, static_cast<int64_t>(Stride), Offset))
      Frame = {};
    else
      Frame.Offset = Offset;
  } else {
    Frame = {};
  }
}

struct TaggedMemoryCase {
  uint64_t Tag = 0;
  StoredRegisterMemory Values;
  MemoryPostconditions Postconditions;
  // May-definition history, independent of whether an object's fields remain
  // known. Summary names are rebound only when they escape into the caller.
  std::vector<uint64_t> DefinedCaptures;

  bool operator==(const TaggedMemoryCase &) const = default;
};

// A complete, bounded partition of one returned tag. The summary address is
// entry-relative; a caller substitutes it before retaining any conditional
// cells. Register copies refer to this tag only until their next definition.
struct TaggedMemory {
  RegisterValue Address;
  unsigned Width = 0;
  uint16_t Registers = 0;
  std::vector<TaggedMemoryCase> Cases;
  // Only names newly introduced by this call can be removed by selecting a
  // case. A definition that predates the call remains part of caller history.
  std::vector<uint64_t> ConditionalCaptures;

  uint64_t retainedByteEstimate() const {
    uint64_t Bytes = Cases.capacity() * sizeof(TaggedMemoryCase) +
                     ConditionalCaptures.capacity() * sizeof(uint64_t);
    for (const auto &Case : Cases)
      Bytes += Case.Values.retainedByteEstimate() +
               Case.Postconditions.retainedByteEstimate() +
               Case.DefinedCaptures.capacity() * sizeof(uint64_t);
    return Bytes;
  }

  bool operator==(const TaggedMemory &) const = default;
};

struct NullableCapture {
  uint64_t Capture;
  uint16_t Registers;
  bool ConditionalDefinition = true;
  // Full-width stores of this exact value on every non-null path. Numeric
  // ranges and independent expression alternatives do not establish identity.
  StoredRegisterMemory Values;
  MemoryPostconditions Postconditions;

  uint64_t retainedByteEstimate() const {
    return Values.retainedByteEstimate() +
           Postconditions.retainedByteEstimate();
  }

  bool operator==(const NullableCapture &) const = default;
};

struct ContextState {
  std::array<ProvenValueSet, kRegisterCount> Registers{};
  ScratchState Scratch;
  std::array<ProvenValueSet, kRegisterCount> Origins{};
  ScratchState OriginScratch;
  MemoryPostconditions Postconditions;
  std::array<std::optional<size_t>, kRegisterCount> InputExpressions{};
  // Private expression IDs, never VM values. The canonical typed-cell owner
  // supplies full-width store, overlap and coordinate invalidation semantics.
  StoredRegisterMemory InputSpills;
  MemoryEffects InputWrites;
  std::optional<TaggedMemory> Tagged;
  // Captures introduced by this invocation on any path reaching this state.
  // A missing field of an already introduced capture is unknown. A capture
  // absent from the other path has no value there, so its own fields can stay
  // conditional on the surviving pointer. This never proves non-aliasing.
  std::vector<uint64_t> DefinedCaptures;
  // One scalar return, exactly {null, captured value}, with conditional stores
  // of that identity. Copies retain the relation until their next definition.
  // A null branch removes a name only if its definition was also conditional.
  std::optional<NullableCapture> Nullable;

  bool definesCapture(uint64_t Capture) const {
    return std::binary_search(DefinedCaptures.begin(), DefinedCaptures.end(),
                              Capture);
  }

  void defineCapture(uint64_t Capture) {
    const auto Position = std::lower_bound(DefinedCaptures.begin(),
                                           DefinedCaptures.end(), Capture);
    if (Position == DefinedCaptures.end() || *Position != Capture)
      DefinedCaptures.insert(Position, Capture);
  }

  ContextState() = default;
  ContextState(const MachineState &State) : Scratch(State.Scratch) {
    Scratch.Memory.clear();
    for (unsigned R = 0; R < kRegisterCount; ++R)
      Registers[R] = State.Registers[R];
  }

  MachineState scalar() const {
    MachineState State;
    for (unsigned R = 0; R < kRegisterCount; ++R)
      State.Registers[R] = Registers[R].singleton();
    State.Scratch = Scratch;
    return State;
  }

  void join(const ContextState &Other, const SBFProgram &Program,
            const std::shared_ptr<const StoredMemoryCoordinates> &Coordinates,
            MemoryPostconditions::ExpressionJoin MergeExpression = nullptr) {
    const auto IsNull = [](const ContextState &State) {
      const auto Value = State.Registers[0].singleton();
      return Value.ValueKind == RegisterValue::Kind::Constant &&
             Value.Value == 0;
    };
    const auto ReturnFacts =
        [&](const ContextState &State) -> std::optional<NullableCapture> {
      if (State.Nullable && (State.Nullable->Registers & registerBit(0)))
        return State.Nullable;
      const auto Value = State.Registers[0].singleton();
      const auto Bounds =
          unsignedValueRange(Value, Coordinates->Addresses.get());
      if (Value.ValueKind != RegisterValue::Kind::SymbolicAddress ||
          Value.Offset || !Bounds || !Bounds->Lower)
        return std::nullopt;
      NullableCapture Facts{Value.Value, registerBit(0), true, {}, {}};
      Facts.Values.setCoordinates(Coordinates);
      Facts.Values.includeCells(State.Scratch.Values, [&](const auto &Address) {
        return State.Scratch.Values.readValues(Address).singleton() == Value;
      });
      State.Postconditions.visitValues(
          [&](const auto &Address, const auto &Stored, auto) {
            if (Stored.singleton() == Value)
              Facts.Postconditions.store(Address, sizeof(uint64_t), Stored);
          });
      return Facts;
    };
    if (Nullable && Other.Nullable &&
        Nullable->Capture == Other.Nullable->Capture) {
      Nullable->Registers &= Other.Nullable->Registers;
      Nullable->ConditionalDefinition &= Other.Nullable->ConditionalDefinition;
      Nullable->Values.joinValues(Other.Nullable->Values);
      Nullable->Postconditions.meet(Other.Nullable->Postconditions);
      if (!Nullable->Registers)
        Nullable.reset();
    } else if (IsNull(*this) || IsNull(Other)) {
      const auto &Zero = IsNull(*this) ? *this : Other;
      auto Facts = ReturnFacts(IsNull(*this) ? Other : *this);
      if (Facts) {
        Facts->Registers = registerBit(0);
        Facts->ConditionalDefinition &= !Zero.definesCapture(Facts->Capture);
      }
      Nullable = std::move(Facts);
    } else if (Nullable || Other.Nullable) {
      auto Left = ReturnFacts(*this);
      auto Right = ReturnFacts(Other);
      if (Left && Right && Left->Capture == Right->Capture) {
        Left->Registers = registerBit(0);
        Left->ConditionalDefinition &= Right->ConditionalDefinition;
        Left->Values.joinValues(Right->Values);
        Left->Postconditions.meet(Right->Postconditions);
        Nullable = std::move(Left);
      } else {
        Nullable.reset();
      }
    }
    StoredRegisterMemory ConditionalObjects;
    if (!DefinedCaptures.empty())
      ConditionalObjects.includeCells(Scratch.Values, [&](const auto &Address) {
        return Address.ValueKind == RegisterValue::Kind::SymbolicAddress &&
               definesCapture(Address.Value) &&
               !Other.definesCapture(Address.Value);
      });
    if (!Other.DefinedCaptures.empty())
      ConditionalObjects.includeCells(
          Other.Scratch.Values, [&](const auto &Address) {
            return Address.ValueKind == RegisterValue::Kind::SymbolicAddress &&
                   Other.definesCapture(Address.Value) &&
                   !definesCapture(Address.Value);
          });
    for (unsigned R = 0; R < kRegisterCount; ++R)
      if (InputExpressions[R] != Other.InputExpressions[R])
        InputExpressions[R] =
            MergeExpression
                ? MergeExpression(Origins[R], InputExpressions[R],
                                  Other.Origins[R], Other.InputExpressions[R])
                : std::nullopt;
    for (unsigned R = 0; R < kRegisterCount; ++R)
      Registers[R].join(Other.Registers[R]);
    for (unsigned R = 0; R < kRegisterCount; ++R)
      Origins[R].join(Other.Origins[R]);
    Scratch.Memory.meet(Other.Scratch.Memory);
    Scratch.Values.joinValues(Other.Scratch.Values);
    Scratch.Values.includeCells(ConditionalObjects,
                                [](const auto &) { return true; });
    for (const auto Capture : Other.DefinedCaptures)
      defineCapture(Capture);
    OriginScratch.meet(Other.OriginScratch);
    Postconditions.meet(Other.Postconditions, MergeExpression);
    InputSpills.meet(Other.InputSpills);
    InputWrites.include(Other.InputWrites, symbolicInputs(), Program);
    if (!Tagged || !Other.Tagged || Tagged->Address != Other.Tagged->Address ||
        Tagged->Width != Other.Tagged->Width ||
        Tagged->Cases.size() != Other.Tagged->Cases.size() ||
        !std::equal(
            Tagged->Cases.begin(), Tagged->Cases.end(),
            Other.Tagged->Cases.begin(),
            [](const auto &A, const auto &B) { return A.Tag == B.Tag; })) {
      Tagged.reset();
    } else {
      Tagged->Registers &= Other.Tagged->Registers;
      const bool SameDefinitions =
          Tagged->ConditionalCaptures == Other.Tagged->ConditionalCaptures;
      if (!SameDefinitions)
        Tagged->ConditionalCaptures.clear();
      for (size_t I = 0; I < Tagged->Cases.size(); ++I) {
        Tagged->Cases[I].Values.joinValues(Other.Tagged->Cases[I].Values);
        Tagged->Cases[I].Postconditions.meet(
            Other.Tagged->Cases[I].Postconditions, MergeExpression);
        auto &Captures = Tagged->Cases[I].DefinedCaptures;
        if (SameDefinitions) {
          for (uint64_t Capture : Other.Tagged->Cases[I].DefinedCaptures) {
            const auto Position =
                std::lower_bound(Captures.begin(), Captures.end(), Capture);
            if (Position == Captures.end() || *Position != Capture)
              Captures.insert(Position, Capture);
          }
        } else {
          Captures.clear();
        }
      }
    }
  }
};

llvm::ArrayRef<RegisterValue> possibleValues(const ProvenValueSet &Values);

ProvenValueSet translateFrameValues(const ProvenValueSet &Values,
                                    std::optional<int64_t> Delta) {
  if (Delta && *Delta == 0)
    return Values;
  std::optional<ProvenValueSet> Result;
  for (auto Value : Values.values()) {
    if (Value.ValueKind == RegisterValue::Kind::StackAddress) {
      int64_t Offset;
      if (!Delta || llvm::AddOverflow(Value.Offset, *Delta, Offset))
        return {};
      Value.Offset = Offset;
    }
    if (!Result)
      Result = Value;
    else
      Result->join(Value);
  }
  return Result.value_or(ProvenValueSet{});
}

bool translateFrameInput(ContextState &State, int64_t Delta) {
  for (const auto &Register : State.Registers)
    for (const auto &Value : Register.values()) {
      int64_t Offset;
      if (Value.ValueKind == RegisterValue::Kind::StackAddress &&
          llvm::AddOverflow(Value.Offset, Delta, Offset))
        return false;
    }
  if (!State.Scratch.Values.translateFrameCoordinates(Delta))
    return false;
  for (auto &Register : State.Registers)
    Register = translateFrameValues(Register, Delta);
  return true;
}

void rebaseReturnedFrame(StoredRegisterMemory &Memory,
                         std::optional<int64_t> Delta) {
  if (!Delta || !Memory.translateFrameCoordinates(*Delta))
    Memory.discardFrameCoordinates();
}

ProvenValueSet
mapSymbolicValues(const ProvenValueSet &Input,
                  llvm::function_ref<RegisterValue(RegisterValue)> Map) {
  std::optional<ProvenValueSet> Result;
  for (auto Value : Input.values()) {
    if (Value.ValueKind == RegisterValue::Kind::SymbolicAddress)
      Value = Map(Value);
    if (!Result)
      Result = Value;
    else
      Result->join(Value);
  }
  return Result.value_or(ProvenValueSet{});
}

void widenCapturedValues(ContextState &State,
                         const SymbolicAddressSpace &Addresses) {
  State.Tagged.reset();
  State.Nullable.reset();
  const auto Widen = [&](RegisterValue Value) {
    const auto Range = unsignedValueRange(Value, &Addresses);
    return Range ? unsignedRangeValue(*Range) : RegisterValue{};
  };
  for (unsigned R = 0; R < kRegisterCount; ++R) {
    State.Registers[R] = mapSymbolicValues(State.Registers[R], Widen);
    State.Origins[R] = mapSymbolicValues(State.Origins[R], Widen);
  }
  State.Scratch.Values.transformSymbols(Widen);
  State.OriginScratch.Values.transformSymbols(Widen);
  State.InputSpills.transformSymbols(Widen);
  State.Postconditions.transformSymbols(Widen);
}

// Formal entry-relative origins and immutable captures name one value.
// Equal scalar ranges or independent finite alternatives do not prove equality.
bool hasValueIdentity(const RegisterValue &Origin) {
  return Origin.ValueKind == RegisterValue::Kind::StackAddress ||
         Origin.ValueKind == RegisterValue::Kind::SymbolicAddress;
}

/// Narrow only scalar, unsigned comparisons. Unknown scalars acquire bounds
/// here only when the comparison constrains a potential heap address. This
/// domain does not introduce ordinary integer-range fixed points.
bool refineAddressBranch(const MedInstruction *Last, size_t Successor,
                         ContextState &State, const SBFProgram &Program,
                         const SymbolicAddressSpace &Addresses,
                         uint16_t ReturnCarriers) {
  if (!Last || Last->Width != kDoubleWordBitWidth ||
      Last->Semantics.Terminator != TerminatorKind::ConditionalJump)
    return true;
  const bool Taken = Successor == *Last->BranchTarget;
  RegisterValue Right =
      Last->Semantics.Source == OperandSourceKind::SourceRegister
          ? State.Registers[Last->Src].singleton()
          : RegisterValue{
                RegisterValue::Kind::Constant,
                normalizeImmediate(Last->Immediate, Last->Semantics.Immediate),
                0};
  unsigned Register = Last->Dst;
  Operation Predicate = Last->Op;
  if (Right.ValueKind != RegisterValue::Kind::Constant) {
    Right = State.Registers[Last->Dst].singleton();
    if (Right.ValueKind != RegisterValue::Kind::Constant ||
        Last->Semantics.Source != OperandSourceKind::SourceRegister)
      return true;
    Register = Last->Src;
    switch (Predicate) {
    case Operation::UGt:
      Predicate = Operation::ULt;
      break;
    case Operation::UGe:
      Predicate = Operation::ULe;
      break;
    case Operation::ULt:
      Predicate = Operation::UGt;
      break;
    case Operation::ULe:
      Predicate = Operation::UGe;
      break;
    default:
      break;
    }
  }
  const uint64_t Constant = Right.Value;
  if (State.Registers[Register].unknown() &&
      (!(ReturnCarriers & registerBit(Register)) || Constant < kHeapStart ||
       Constant >= kHeapStart + kMemoryRegionSize))
    return true;
  std::optional<ProvenValueSet> Refined;
  bool Reachable = false;
  for (const auto &Value : possibleValues(State.Registers[Register])) {
    auto Bounds = unsignedValueRange(Value, &Addresses);
    if (!Bounds && Value.ValueKind != RegisterValue::Kind::Unknown)
      return true;
    const auto Before = Bounds.value_or(UnsignedValueRange{});
    auto Range = Before;
    bool Possible = true;
    switch (Predicate) {
    case Operation::Eq:
    case Operation::Ne: {
      const bool Equal = Taken == (Predicate == Operation::Eq);
      if (Equal) {
        Possible = Constant >= Range.Lower && Constant <= Range.Upper;
        Range = {Constant, Constant};
      } else if (Range.Lower == Constant) {
        Possible = Range.Upper != Constant;
        if (Possible)
          ++Range.Lower;
      } else if (Range.Upper == Constant) {
        --Range.Upper;
      }
      break;
    }
    case Operation::UGt:
    case Operation::ULe:
      if (Taken == (Predicate == Operation::UGt)) {
        Possible = Constant != UINT64_MAX && Range.Upper > Constant;
        if (Possible)
          Range.Lower = std::max(Range.Lower, Constant + 1);
      } else {
        Possible = Range.Lower <= Constant;
        Range.Upper = std::min(Range.Upper, Constant);
      }
      break;
    case Operation::UGe:
    case Operation::ULt:
      if (Taken == (Predicate == Operation::UGe)) {
        Possible = Range.Upper >= Constant;
        Range.Lower = std::max(Range.Lower, Constant);
      } else {
        Possible = Constant != 0 && Range.Lower < Constant;
        if (Possible)
          Range.Upper = std::min(Range.Upper, Constant - 1);
      }
      break;
    default:
      return true;
    }
    if (!Possible)
      continue;
    Reachable = true;
    // An unchanged captured value retains its equality identity. A narrowed
    // identity becomes a range rather than mutating globally shared bounds.
    RegisterValue Narrowed =
        Range == Before ? Value : unsignedRangeValue(Range);
    if (Value.ValueKind == RegisterValue::Kind::Unknown &&
        Range.Lower < kHeapStart)
      Narrowed = {};
    if (!Refined)
      Refined = Narrowed;
    else
      Refined->join(Narrowed);
  }
  if (Reachable) {
    const uint16_t ReturnAliases =
        State.Nullable && (State.Nullable->Registers & registerBit(Register))
            ? State.Nullable->Registers
            : 0;
    if (ReturnAliases) {
      const auto Value = Refined->singleton();
      if (Value.ValueKind == RegisterValue::Kind::Constant &&
          Value.Value == 0) {
        if (State.Nullable->ConditionalDefinition)
          std::erase(State.DefinedCaptures, State.Nullable->Capture);
        State.Nullable.reset();
      } else if (llvm::all_of(Refined->values(), [&](const auto &Alternative) {
                   const auto Bounds =
                       unsignedValueRange(Alternative, &Addresses);
                   return Bounds && Bounds->Lower != 0 &&
                          Bounds->Lower <= Bounds->Upper;
                 })) {
        State.Scratch.Values.includeCells(State.Nullable->Values,
                                          [](const auto &) { return true; });
        std::array<ProvenValueSet, kRegisterCount> Identity;
        const auto Inputs = symbolicInputs();
        for (unsigned R = 0; R < kRegisterCount; ++R)
          Identity[R] = Inputs[R];
        State.Postconditions.include(State.Nullable->Postconditions, Identity,
                                     Program);
        State.Nullable.reset();
      }
    }
    const auto Origin = State.Origins[Register].singleton();
    const bool ScalarOrigin =
        llvm::all_of(Refined->values(), [](const RegisterValue &Value) {
          return Value.ValueKind == RegisterValue::Kind::Constant ||
                 Value.ValueKind == RegisterValue::Kind::UnsignedRange ||
                 Value.ValueKind == RegisterValue::Kind::SymbolicAddress;
        });
    for (unsigned R = 0; R < kRegisterCount; ++R)
      if (R == Register || (ReturnAliases & registerBit(R)) ||
          (hasValueIdentity(Origin) &&
           State.Origins[R].singleton() == Origin)) {
        State.Registers[R] = *Refined;
        if (ScalarOrigin)
          State.Origins[R] = *Refined;
      }
  }
  return Reachable;
}

std::optional<size_t> knownBranchSuccessor(const MedInstruction *Last,
                                           const ContextState &State) {
  if (!Last || Last->Semantics.Terminator != TerminatorKind::ConditionalJump)
    return std::nullopt;
  const ProvenValueSet Immediate(RegisterValue{
      RegisterValue::Kind::Constant,
      normalizeImmediate(Last->Immediate, Last->Semantics.Immediate), 0});
  const auto &Right =
      Last->Semantics.Source == OperandSourceKind::SourceRegister
          ? State.Registers[Last->Src]
          : Immediate;
  std::optional<bool> Taken;
  for (const auto &Left : State.Registers[Last->Dst].values()) {
    if (Left.ValueKind != RegisterValue::Kind::Constant)
      return std::nullopt;
    for (const auto &RHS : Right.values()) {
      if (RHS.ValueKind != RegisterValue::Kind::Constant)
        return std::nullopt;
      const bool Outcome =
          evaluateBranchCondition(Last->Op, Last->Width, Left.Value, RHS.Value);
      if (Taken && *Taken != Outcome)
        return std::nullopt;
      Taken = Outcome;
    }
  }
  if (!Taken)
    return std::nullopt;
  return *Taken ? *Last->BranchTarget : Last->Slot + Last->SlotWidth;
}

void pushAbstractFrame(std::array<ProvenValueSet, kRegisterCount> &State,
                       const SBFProgram &Program) {
  const auto Values = State[kFramePointerRegister].values();
  std::optional<ProvenValueSet> Frames;
  for (const auto &Value : Values) {
    RegisterState Registers{};
    Registers[kFramePointerRegister] = Value;
    pushAbstractFrame(Registers, Program);
    if (!Frames)
      Frames = Registers[kFramePointerRegister];
    else
      Frames->join(Registers[kFramePointerRegister]);
  }
  State[kFramePointerRegister] = Frames.value_or(ProvenValueSet{});
}

void pushAbstractFrame(ContextState &State, const SBFProgram &Program) {
  pushAbstractFrame(State.Registers, Program);
}

uint64_t stateBytes(const ContextState &State) {
  uint64_t Bytes = sizeof(State) + State.Scratch.retainedByteEstimate() +
                   State.OriginScratch.retainedByteEstimate() +
                   State.Postconditions.retainedByteEstimate() +
                   State.InputSpills.retainedByteEstimate() +
                   State.InputWrites.Writes.capacity() * sizeof(MemoryWrite) +
                   State.DefinedCaptures.capacity() * sizeof(uint64_t);
  if (State.Tagged)
    Bytes += State.Tagged->retainedByteEstimate();
  if (State.Nullable)
    Bytes += State.Nullable->retainedByteEstimate();
  for (const auto &Value : State.Registers)
    Bytes += Value.allocatedBytes();
  for (const auto &Origin : State.Origins)
    Bytes += Origin.allocatedBytes();
  return Bytes;
}

std::optional<uint64_t> maximumCopyLength(const ProvenValueSet &Lengths) {
  if (Lengths.unknown())
    return std::nullopt;
  uint64_t Maximum = 0;
  for (const auto &Length : Lengths.values()) {
    if (Length.ValueKind != RegisterValue::Kind::Constant ||
        Length.Value > kMaxModeledScratchBytes)
      return std::nullopt;
    Maximum = std::max(Maximum, Length.Value);
  }
  return Maximum;
}

void profileInput(llvm::FoldingSetNodeID &ID, size_t Block, bool Generic,
                  const ContextState &Input) {
  ID.AddInteger(Block);
  ID.AddBoolean(Generic);
  for (const auto &Value : Input.Registers)
    Value.profile(ID);
  Input.Scratch.Memory.profile(ID);
  Input.Scratch.Values.profile(ID);
}

struct ContextCall : CallXSummaryEdge {
  bool ReadProjectedInput = false;
  std::vector<CallXContextEvidence> InputExpressionTargets;
};

struct FunctionContext : llvm::FoldingSetNode {
  bool HasMemoryTransfer = false;
  bool HasInlineStores = false;
  size_t ID;
  size_t EntryBlock;
  ContextState Input;
  std::optional<ContextState> Return;
  std::optional<TaggedMemory> TaggedReturn;
  std::vector<CallXContextEvidence> Sites;
  std::vector<ContextCall> Calls;
  bool IsRoot = false;
  bool Published = false;
  bool Generic;
  bool Complete = false;
  bool PreservesMemory = true;
  bool WritesOnlyOwnAndChildFrames = true;
  // The symbolic lane can express an argument using its relation to the entry
  // frame even when the body never reads r10. Preserve that anchor on reuse.
  uint16_t ReuseInputRegisters = registerBit(kFramePointerRegister);
  // Keep exploring the original register inputs while variant space remains.
  // At capacity, reads confined to nonreturning, unproven subtrees cannot
  // guard the returning value or the target evidence already retained.
  uint16_t ObservableInputRegisters = registerBit(kFramePointerRegister);
  // The generic node outlives replaceable forwarding variants, so eviction
  // cannot repeatedly rediscover the same exploratory image binding.
  uint16_t ExploredImageRegisters = 0;
  uint16_t ExploredCapturedRegisters = 0;
  uint16_t ExploredFrameRegisters = 0;
  bool ReuseReadsMemory = false;
  bool HasTargetEvidence = false;
  MemoryEffects Effects;
  MemoryEffects ReadEffects;
  size_t RootGeneration = 0;
  size_t IncomingReferences = 0;
  uint64_t RetainedBytes = 0;

  FunctionContext(size_t ID, size_t EntryBlock, const ContextState &Input,
                  bool Generic)
      : ID(ID), EntryBlock(EntryBlock), Input(Input), Generic(Generic) {}

  void Profile(llvm::FoldingSetNodeID &ID) const {
    profileInput(ID, EntryBlock, Generic, Input);
  }

  bool isUnreferencedSolverNode() const {
    return !Generic && Complete && !IsRoot && !Published &&
           IncomingReferences == 0;
  }
};

struct BlockState {
  ContextState Entry;
  bool Queued = false;
  uint16_t DefinitelyWritten = 0;
};

struct RecursionRetry {
  FunctionContext *Summary;
  size_t CalleeEntry;
};

class ContextFlow {
public:
  explicit ContextFlow(const SBFProgram &Program)
      : Program(Program), Index(Program.Med),
        Offsets(Program.Med.Blocks.size() + 1) {
    for (const MedBlock &Block : Program.Med.Blocks)
      ByEntry.emplace(Block.StartSlot, Block.ID);
    for (const CFGEdge &Edge : Program.Low.Edges)
      if (Edge.To && getEdgeKindInfo(Edge.Kind).IsIntraprocedural)
        ++Offsets[Edge.From + 1];
    std::partial_sum(Offsets.begin(), Offsets.end(), Offsets.begin());
    Successors.resize(Offsets.back());
    auto Next = Offsets;
    for (const CFGEdge &Edge : Program.Low.Edges)
      if (Edge.To && getEdgeKindInfo(Edge.Kind).IsIntraprocedural)
        Successors[Next[Edge.From]++] = *Edge.To;
    std::vector<bool> Seen(Program.Med.Blocks.size());
    std::vector<std::pair<size_t, size_t>> DFS;
    std::vector<size_t> Postorder;
    for (size_t Root = 0; Root < Seen.size(); ++Root) {
      if (Seen[Root])
        continue;
      Seen[Root] = true;
      DFS.emplace_back(Root, Offsets[Root]);
      while (!DFS.empty()) {
        auto &[Block, Edge] = DFS.back();
        if (Edge == Offsets[Block + 1]) {
          Postorder.push_back(Block);
          DFS.pop_back();
          continue;
        }
        const size_t Child = Successors[Edge++];
        if (!Seen[Child]) {
          Seen[Child] = true;
          DFS.emplace_back(Child, Offsets[Child]);
        }
      }
    }
    BlockOrder.resize(Seen.size());
    for (size_t I = 0; I < Postorder.size(); ++I)
      BlockOrder[Postorder[I]] = Postorder.size() - I - 1;
  }

  llvm::Expected<CallXContextReport> run();

private:
  llvm::Expected<FunctionContext *>
  summarize(size_t Entry, const ContextState &Input, bool Generic = false,
            CallXInputGeneralization *Generalization = nullptr,
            bool *ReadProjectedInput = nullptr);
  llvm::Expected<bool> memoryInputsMatch(const FunctionContext &Candidate,
                                         const ContextState &Input,
                                         bool &CoveringInputs);
  llvm::Error computeInputDependencies();
  llvm::Error computeReturnReachability();
  uint16_t usedCalleeInputs(const MedInstruction &Call, size_t Entry,
                            uint16_t Inputs) const {
    // EXIT alone observes only its scalar result. If the caller discards
    // that result, incoming r0 cannot affect the continuation or memory.
    if (!LiveCallReturns[Call.Slot] &&
        Index.find(Program.Med.Blocks[Entry].StartSlot)->Op == Operation::Exit)
      return Inputs & ~registerBit(kReturnRegister);
    return Inputs;
  }
  llvm::Error refineLoadedEquality(const MedBlock &Block, size_t Successor,
                                   ContextState &State);
  llvm::Expected<std::array<std::optional<RegisterValue>, kRegisterCount>>
  unchangedLoadedCells(const MedBlock &Block, uint16_t Registers,
                       const ContextState &State);
  ContextState genericInput() const;
  llvm::Expected<RegisterValue> captureAddress(size_t Owner, size_t Slot,
                                               uint64_t Source,
                                               UnsignedValueRange Bounds);
  llvm::Error collectEntryPrefixes(CallXContextReport &Report);
  void reclaimContext(FunctionContext &Context,
                      std::vector<size_t> *Unreferenced = nullptr,
                      bool ReleasePublishedChildren = false);
  void publishRoot(FunctionContext &Context);
  void reclaimOrphanedContexts(size_t Entry);
  std::optional<RecursionRetry>
  unblockedRecursiveCall(FunctionContext &Context) const;
  bool hasCompletedObjectResult(const FunctionContext &Context) const;
  llvm::Expected<std::optional<CallXCompletedResultBinding>>
  completedObjectBinding(const FunctionContext &Original,
                         const FunctionContext &Replacement);
  llvm::Expected<bool> generalizeRedundantContext(
      size_t Entry, const ContextState &Input,
      std::optional<RecursionRetry> Retry = std::nullopt);
  llvm::Expected<bool> replay(FunctionContext &Context, size_t BlockID,
                              ContextState &State, uint16_t &Defined,
                              bool Collect);
  llvm::Expected<bool> call(FunctionContext &Context,
                            const MedInstruction &Instruction,
                            ContextState &State, bool Collect);
  struct MemoryTransferInstantiation {
    ContextState Output;
    MemoryEffects Effects;
    MemoryEffects Reads;
    std::optional<size_t> CopySlot;
    uint64_t CopySize = 0;
    size_t Instructions = 0;
    size_t Stores = 0;
  };
  llvm::Expected<std::optional<MemoryTransferInstantiation>>
  instantiateMemoryTransferPath(const FunctionContext &Owner, size_t Entry,
                                const ContextState &Input);
  llvm::Expected<bool> callTarget(FunctionContext &Context,
                                  const MedInstruction &Instruction,
                                  ContextState &State, bool Collect);
  bool hasRegisteredLegacyRuntime(const MedInstruction &Instruction) const {
    return Instruction.Dispatch ==
               CallDispatchPolicy::LegacyRuntimeThenFunction &&
           Program.isSyscallRegistered(
               static_cast<uint32_t>(Instruction.Immediate));
  }
  std::optional<size_t> rawTargetSlot(va_t Address) const {
    // Raw CALLX subtracts in uint64_t and floors to a slot. An unaligned
    // address or an LDDW continuation is not rejected by address resolution.
    const uint64_t Slot =
        (Address - Program.Low.TextAddress) / kInstructionSize;
    if (Slot < Program.Low.Instructions.size())
      return static_cast<size_t>(Slot);
    return std::nullopt;
  }
  void transfer(const MedInstruction &Instruction, ContextState &State,
                MissingRegisters *Missing);
  void transferValues(const MedInstruction &Instruction, ContextState &State);
  void transferTaggedMemory(const MedInstruction &Instruction,
                            ContextState &State);
  llvm::Expected<bool> refineTaggedBranch(const MedBlock &Block,
                                          size_t Successor,
                                          ContextState &State);
  void refineOrigins(const FunctionContext &Context, ContextState &State);
  llvm::Expected<size_t> inputExpression(CallXInputExpression Expression);
  llvm::Expected<std::optional<size_t>>
  inputUnionExpression(llvm::ArrayRef<size_t> Alternatives);
  llvm::Error joinStates(ContextState &State, const ContextState &Other);
  bool needsSummaryInputGuards(const FunctionContext &Summary,
                               const ContextCall &Edge) const {
    // A structurally nonreturning subtree cannot affect caller returns. Its
    // inputs still guard any target evidence retained there or on this edge.
    // Reusing an unknown target cannot invent a proof for different inputs.
    return MayReturnFromBlock[Summary.EntryBlock] ||
           Summary.HasTargetEvidence || !Edge.InputExpressionTargets.empty();
  }
  llvm::Expected<std::optional<size_t>> inputMemoryGuard(MemoryEffects Effects);
  void addMemoryReadEffects(MemoryEffects &Effects,
                            const MedInstruction &Instruction,
                            const ContextState &State) const;
  void addMemoryAccessEffects(MemoryEffects &Effects,
                              const MedInstruction &Instruction,
                              const ContextState &State,
                              unsigned AddressRegister) const;
  llvm::Expected<std::optional<size_t>>
  inputOriginExpression(const ProvenValueSet &Origins);
  llvm::Expected<std::optional<size_t>> composeReturnExpression(
      size_t Expression, const ContextState &State,
      const std::array<ProvenValueSet, kRegisterCount> &OriginEntry);
  llvm::Error transferInputExpression(const MedInstruction &Instruction,
                                      ContextState &State);
  bool inputExpressionBound(size_t Root, const ContextState &Input) const;
  uint64_t inputEvaluationBytes(size_t Root) const;
  llvm::Expected<ProvenValueSet>
  evaluateInputExpression(size_t Root, const ContextState &Input,
                          const ScratchState &EntryMemory,
                          std::vector<CallXInputExpressionValue> &Proof);
  llvm::Expected<size_t>
  applyStoredExpressions(const MemoryPostconditions &Postconditions,
                         const ContextState &Input,
                         const ScratchState &EntryMemory, ScratchState &Output);
  llvm::Error instantiateTargetInputs(FunctionContext &Caller,
                                      const FunctionContext &Callee,
                                      const ContextState &Input,
                                      ContextCall &Edge);
  ProvenValueSet loadTypedValue(const MedInstruction &Instruction,
                                const ProvenValueSet &Bases,
                                const StoredRegisterMemory &Memory) const;
  ProvenValueSet loadOrigin(const MedInstruction &Instruction,
                            const ContextState &State) const;
  void classifyTarget(CallXContextEvidence &Evidence,
                      const ProvenValueSet &Values,
                      const CallXMissingFact &Missing) const;
  CallXContextEvidence evidence(const FunctionContext &Context,
                                const MedInstruction &Instruction,
                                const ContextState &State,
                                const MissingRegisters &Missing,
                                bool BeyondEntry) const;
  llvm::Error retain(uint64_t Bytes) {
    if (Bytes > kCallXContextRetainedByteBudget - RetainedBytes) {
      SpecializationBudgetExhausted = true;
      return llvm::make_error<llvm::StringError>(
          kCallXContextMemoryBudgetDiagnostic, llvm::inconvertibleErrorCode());
    }
    RetainedBytes += Bytes;
    PeakRetainedBytes = std::max(PeakRetainedBytes, RetainedBytes);
    return llvm::Error::success();
  }
  llvm::Error visit() {
    if (Visits == kCallXContextInstructionVisitBudget) {
      SpecializationBudgetExhausted = true;
      return llvm::make_error<llvm::StringError>(
          kCallXContextWorkBudgetDiagnostic, llvm::inconvertibleErrorCode());
    }
    ++Visits;
    return llvm::Error::success();
  }

  const SBFProgram &Program;
  std::shared_ptr<SymbolicAddressSpace> CapturedAddresses =
      std::make_shared<SymbolicAddressSpace>();
  std::shared_ptr<const StoredMemoryCoordinates> Coordinates =
      std::make_shared<StoredMemoryCoordinates>(StoredMemoryCoordinates{
          frameTranslationLimit(Program.Low.TheVersion, Program.Config),
          CapturedAddresses});
  std::vector<size_t> CaptureOwners;
  std::map<std::tuple<size_t, size_t, uint64_t, uint64_t, uint64_t>, size_t>
      CaptureIndex;
  MedInstructionIndex Index;
  std::map<size_t, size_t> ByEntry;
  std::vector<size_t> Offsets;
  std::vector<size_t> Successors;
  std::vector<size_t> BlockOrder;
  llvm::FoldingSet<FunctionContext> Cache;
  std::vector<std::unique_ptr<FunctionContext>> Contexts;
  std::set<size_t> ActiveEntries;
  std::map<size_t, FunctionContext *> GenericContexts;
  // Live variants in creation order. Stable context IDs outlive reclaimed
  // nodes; scanning that history for each replacement becomes quadratic.
  std::map<size_t, llvm::SmallVector<FunctionContext *,
                                     kCallXContextSpecializationsPerEntry>>
      Specializations;
  std::vector<uint16_t> InputRegisters;
  std::vector<uint16_t> ReturnCarriers;
  std::vector<bool> StoredReturnValues;
  std::vector<bool> LiveCallReturns;
  std::vector<bool> ReadsMemory;
  std::vector<bool> ContainsCalls;
  std::vector<bool> ReachesCallX;
  size_t ProjectedRegisters = 0;
  size_t ProjectedScratchSnapshots = 0;
  size_t DependencyVisits = 0;
  size_t CacheEvictions = 0;
  size_t GeneralizedPublishedSummaries = 0;
  size_t UnblockedRecursionRetries = 0;
  std::set<std::pair<size_t, size_t>> RetriedUnblockedCalls;
  size_t EquivalentGenericReuses = 0;
  size_t EquivalentSpecializedReuses = 0;
  size_t CoveringSpecializedReuses = 0;
  size_t SkippedNonCallXRoots = 0;
  size_t SkippedConstantBranchSuccessors = 0;
  size_t CanonicalizedFrameInputs = 0;
  size_t RootGeneration = 0;
  using ExpressionKey =
      std::tuple<CallXInputExpressionKind, size_t, uint64_t, int64_t, Opcode,
                 std::vector<size_t>, std::optional<size_t>>;
  std::map<ExpressionKey, size_t> ExpressionIndex;
  std::vector<CallXInputExpression> InputExpressions;
  std::vector<uint16_t> InputExpressionRegisters;
  std::vector<uint8_t> InputExpressionPaths;
  using MemoryGuardKey =
      std::tuple<bool, std::vector<std::tuple<RegisterValue::Kind, uint64_t,
                                              int64_t, uint64_t>>>;
  std::map<MemoryGuardKey, size_t> MemoryGuardIndex;
  std::vector<MemoryEffects> InputMemoryGuards;
  size_t InstantiatedTargetInputExpressions = 0;
  size_t InstantiatedStoredValueInputExpressions = 0;
  size_t InstantiatedReturnInputExpressions = 0;
  std::vector<uint8_t> MayReturnFromBlock;
  size_t ReturnReachabilityVisits = 0;
  bool SpecializationBudgetExhausted = false;
  size_t Visits = 0;
  size_t CacheHits = 0;
  uint64_t RetainedBytes = 0;
  uint64_t PeakRetainedBytes = 0;
};

llvm::Error ContextFlow::computeReturnReachability() {
  // Least fixed point of matched guest returns. A call requires its callee
  // AND continuation to return; a branch needs any successor. Host and unknown
  // indirect calls may return, including catalogued terminal syscalls. A
  // mandatory recursion cycle cannot bootstrap itself from an EXIT after it.
  const size_t Count = Program.Med.Blocks.size();
  const size_t Before = Visits;
  uint64_t TemporaryBytes =
      Count * (4 * sizeof(size_t) + sizeof(uint8_t)) + 2 * sizeof(size_t);
  if (auto Error = retain(Count * sizeof(uint8_t) + TemporaryBytes))
    return Error;
  const auto Release =
      llvm::scope_exit([&] { RetainedBytes -= TemporaryBytes; });
  MayReturnFromBlock.assign(Count, false);
  std::vector<uint8_t> TailReturns(Count);
  std::vector<size_t> RemainingCalls(Count), ReverseOffsets(Count + 1);
  const auto DirectCallee =
      [&](const MedInstruction &Instruction) -> std::optional<size_t> {
    if (Instruction.Call != CallKind::Internal || !Instruction.CallTarget ||
        hasRegisteredLegacyRuntime(Instruction))
      return std::nullopt;
    const auto It = ByEntry.find(*Instruction.CallTarget);
    if (It == ByEntry.end() || !Index.find(*Instruction.CallTarget))
      return std::nullopt;
    return It->second;
  };
  for (const auto &Block : Program.Med.Blocks) {
    if (auto Error = visit())
      return Error;
    const auto *Last = Index.find(Block.EndSlot - 1);
    TailReturns[Block.ID] = Last && Last->Op == Operation::Exit;
    for (size_t Slot = Block.StartSlot; Slot < Block.EndSlot; ++Slot) {
      const auto *Instruction = Index.find(Slot);
      if (!Instruction)
        continue;
      if (auto Error = visit())
        return Error;
      if (const auto Entry = DirectCallee(*Instruction)) {
        ++RemainingCalls[Block.ID];
        ++ReverseOffsets[*Entry + 1];
      }
    }
    for (size_t E = Offsets[Block.ID]; E < Offsets[Block.ID + 1]; ++E) {
      if (auto Error = visit())
        return Error;
      ++ReverseOffsets[Successors[E] + 1];
    }
  }
  std::partial_sum(ReverseOffsets.begin(), ReverseOffsets.end(),
                   ReverseOffsets.begin());
  struct Dependency {
    size_t Block;
    bool Callee;
  };
  const uint64_t EdgeBytes = ReverseOffsets.back() * sizeof(Dependency);
  if (auto Error = retain(EdgeBytes))
    return Error;
  TemporaryBytes += EdgeBytes;
  std::vector<Dependency> Reverse(ReverseOffsets.back());
  auto Next = ReverseOffsets;
  for (const auto &Block : Program.Med.Blocks) {
    for (size_t Slot = Block.StartSlot; Slot < Block.EndSlot; ++Slot) {
      const auto *Instruction = Index.find(Slot);
      if (!Instruction)
        continue;
      if (auto Error = visit())
        return Error;
      if (const auto Entry = DirectCallee(*Instruction))
        Reverse[Next[*Entry]++] = {Block.ID, true};
    }
    for (size_t E = Offsets[Block.ID]; E < Offsets[Block.ID + 1]; ++E) {
      if (auto Error = visit())
        return Error;
      Reverse[Next[Successors[E]]++] = {Block.ID, false};
    }
  }
  std::vector<size_t> Pending;
  Pending.reserve(Count);
  const auto Admit = [&](size_t Block) {
    if (!MayReturnFromBlock[Block] && TailReturns[Block] &&
        RemainingCalls[Block] == 0) {
      MayReturnFromBlock[Block] = true;
      Pending.push_back(Block);
    }
  };
  for (size_t Block = 0; Block < Count; ++Block) {
    if (auto Error = visit())
      return Error;
    Admit(Block);
  }
  for (size_t I = 0; I < Pending.size(); ++I) {
    const size_t Block = Pending[I];
    for (size_t E = ReverseOffsets[Block]; E < ReverseOffsets[Block + 1]; ++E) {
      if (auto Error = visit())
        return Error;
      const auto &Use = Reverse[E];
      if (Use.Callee)
        --RemainingCalls[Use.Block];
      else
        TailReturns[Use.Block] = true;
      Admit(Use.Block);
    }
  }
  ReturnReachabilityVisits = Visits - Before;
  return llvm::Error::success();
}

void ContextFlow::reclaimContext(FunctionContext &Context,
                                 std::vector<size_t> *Unreferenced,
                                 bool ReleasePublishedChildren) {
  Cache.RemoveNode(&Context);
  for (const auto &Edge : Context.Calls)
    if (Edge.CalleeSummaryID) {
      auto &Child = *Contexts[*Edge.CalleeSummaryID];
      --Child.IncomingReferences;
      if (ReleasePublishedChildren && Child.IncomingReferences == 0 &&
          !Child.Generic && !Child.IsRoot && Child.Complete &&
          Child.RootGeneration < RootGeneration &&
          !ActiveEntries.contains(
              Program.Med.Blocks[Child.EntryBlock].StartSlot))
        Child.Published = false;
      if (Unreferenced && Child.isUnreferencedSolverNode())
        Unreferenced->push_back(Child.ID);
    }
  RetainedBytes -= Context.RetainedBytes;
  auto &Variants = Specializations.at(Context.EntryBlock);
  Variants.erase(std::find(Variants.begin(), Variants.end(), &Context));
  Contexts[Context.ID].reset();
  ++CacheEvictions;
}

void ContextFlow::publishRoot(FunctionContext &Context) {
  Context.IsRoot = true;
  if (Context.Published)
    return;
  std::vector<size_t> Pending;
  Pending.reserve(Contexts.size());
  Context.Published = true;
  Pending.push_back(Context.ID);
  for (size_t I = 0; I < Pending.size(); ++I)
    for (const auto &Edge : Contexts[Pending[I]]->Calls)
      if (Edge.CalleeSummaryID) {
        auto &Child = *Contexts[*Edge.CalleeSummaryID];
        if (!Child.Published) {
          Child.Published = true;
          Pending.push_back(Child.ID);
        }
      }
}

void ContextFlow::reclaimOrphanedContexts(size_t Entry) {
  // Avoid discarding reusable inputs when the four variants are all pinned.
  const auto CouldReleaseEntry = [&](const auto &Context) {
    return Context && Context->EntryBlock == Entry && !Context->Generic &&
           !Context->Published && Context->Complete &&
           Context->RootGeneration < RootGeneration &&
           !ActiveEntries.contains(Program.Med.Blocks[Entry].StartSlot);
  };
  const auto &Variants = Specializations.at(Entry);
  if (std::none_of(Variants.begin(), Variants.end(), CouldReleaseEntry))
    return;

  std::vector<bool> Live(Contexts.size());
  std::vector<size_t> Pending;
  Pending.reserve(Contexts.size());
  for (const auto &Context : Contexts) {
    if (!Context)
      continue;
    Live[Context->ID] = Context->Published;
    if (!Context->Published &&
        (Context->Generic || !Context->Complete ||
         Context->RootGeneration == RootGeneration ||
         ActiveEntries.contains(
             Program.Med.Blocks[Context->EntryBlock].StartSlot))) {
      Live[Context->ID] = true;
      Pending.push_back(Context->ID);
    }
  }
  for (size_t I = 0; I < Pending.size(); ++I)
    for (const auto &Edge : Contexts[Pending[I]]->Calls)
      if (Edge.CalleeSummaryID && !Live[*Edge.CalleeSummaryID]) {
        Live[*Edge.CalleeSummaryID] = true;
        Pending.push_back(*Edge.CalleeSummaryID);
      }
  if (std::none_of(Variants.begin(), Variants.end(), [&](const auto &Context) {
        return CouldReleaseEntry(Context) && !Live[Context->ID];
      }))
    return;

  // Release old orphan parents and their newly unreferenced children only
  // when that can supply a variant. Current-root and published graphs stay
  // live.
  Pending.clear();
  for (const auto &Context : Contexts)
    if (Context && !Live[Context->ID] && Context->isUnreferencedSolverNode())
      Pending.push_back(Context->ID);
  for (size_t I = 0; I < Pending.size(); ++I)
    if (!Live[Pending[I]])
      reclaimContext(*Contexts[Pending[I]], &Pending);
}

std::optional<RecursionRetry>
ContextFlow::unblockedRecursiveCall(FunctionContext &Context) const {
  const size_t OwnEntry = Program.Med.Blocks[Context.EntryBlock].StartSlot;
  for (const auto &Call : Context.Calls)
    if (Call.Barrier == CallXSummaryBarrier::RecursiveCall &&
        Call.CalleeEntrySlot && *Call.CalleeEntrySlot != OwnEntry &&
        !ActiveEntries.contains(*Call.CalleeEntrySlot) &&
        !RetriedUnblockedCalls.contains(
            {Context.EntryBlock, *Call.CalleeEntrySlot}))
      return RecursionRetry{&Context, *Call.CalleeEntrySlot};
  return std::nullopt;
}

bool ContextFlow::hasCompletedObjectResult(
    const FunctionContext &Context) const {
  if (!Context.Complete || !Context.Return || Context.TaggedReturn ||
      !Context.Effects.Unknown || !Context.Return->Scratch.Memory.empty() ||
      Context.Return->Postconditions.copyCount() ||
      Context.Return->Postconditions.expressionCount() ||
      Context.Return->InputExpressions[kReturnRegister])
    return false;
  const auto Object = Context.Return->Registers[kReturnRegister].singleton();
  return Object.ValueKind == RegisterValue::Kind::SymbolicAddress &&
         CaptureOwners[Object.Value] == Context.ID &&
         Context.Return->Origins[kReturnRegister] ==
             Context.Return->Registers[kReturnRegister];
}

llvm::Expected<std::optional<CallXCompletedResultBinding>>
ContextFlow::completedObjectBinding(const FunctionContext &Original,
                                    const FunctionContext &Replacement) {
  const auto NoBinding = std::optional<CallXCompletedResultBinding>{};
  if (!hasCompletedObjectResult(Replacement))
    return NoBinding;
  // The old calls have both completed. Compare their actual results under a
  // single changed frame argument, not a guessed dependency of a new input.
  std::optional<int64_t> Delta;
  for (unsigned R = 0; R < kRegisterCount; ++R) {
    if (Original.Input.Registers[R] == Replacement.Input.Registers[R])
      continue;
    const auto A = Original.Input.Registers[R].singleton();
    const auto B = Replacement.Input.Registers[R].singleton();
    int64_t Offset;
    if (Delta || R == kFramePointerRegister ||
        A.ValueKind != RegisterValue::Kind::StackAddress ||
        B.ValueKind != RegisterValue::Kind::StackAddress ||
        A.Value != B.Value || llvm::SubOverflow(A.Offset, B.Offset, Offset))
      return NoBinding;
    Delta = Offset;
  }
  if (!Delta)
    return NoBinding;
  const auto A = Original.Return->Registers[kReturnRegister].singleton();
  const auto B = Replacement.Return->Registers[kReturnRegister].singleton();
  if (A.Offset != B.Offset)
    return NoBinding;
  // Equal bounds alone do not identify a returned value. Require the same
  // capturing instruction and source as well. Neither this rename
  // nor a distinct capture ID establishes freshness or non-aliasing.
  const auto Source = [&](const FunctionContext &Owner, uint64_t Capture) {
    auto It = CaptureIndex.lower_bound(
        std::tuple(Owner.ID, size_t{0}, uint64_t{0}, uint64_t{0}, uint64_t{0}));
    for (; It != CaptureIndex.end() && std::get<0>(It->first) == Owner.ID; ++It)
      if (It->second == Capture)
        return It;
    return CaptureIndex.end();
  };
  const auto AS = Source(Original, A.Value);
  const auto BS = Source(Replacement, B.Value);
  if (AS == CaptureIndex.end() || BS == CaptureIndex.end() ||
      std::tie(std::get<1>(AS->first), std::get<2>(AS->first),
               std::get<3>(AS->first), std::get<4>(AS->first)) !=
          std::tie(std::get<1>(BS->first), std::get<2>(BS->first),
                   std::get<3>(BS->first), std::get<4>(BS->first)))
    return NoBinding;
  const auto Rename = [&](RegisterValue Value) {
    if (Value.ValueKind == RegisterValue::Kind::SymbolicAddress &&
        Value.Value == B.Value)
      Value.Value = A.Value;
    return Value;
  };
  const auto &AP = Original.Return->Postconditions;
  const auto &BP = Replacement.Return->Postconditions;
  // Include the temporary maps used while applying/renaming typed cells.
  // Applying a postcondition also builds raw byte runs; those are redundant
  // with the checked typed cells below, but their workspace is still charged.
  constexpr uint64_t RawCellBytes =
      sizeof(std::pair<const va_t, std::vector<uint8_t>>) + 4 * sizeof(void *) +
      sizeof(uint64_t);
  const uint64_t Bytes =
      3 * sizeof(ScratchState) +
      3 * Replacement.Return->Scratch.Values.retainedByteEstimate() +
      4 * (AP.retainedByteEstimate() + BP.retainedByteEstimate() +
           (AP.size() + BP.size()) * RawCellBytes);
  if (auto Error = retain(Bytes))
    return std::move(Error);
  const auto Release = llvm::scope_exit([&] { RetainedBytes -= Bytes; });
  auto Returned = Replacement.Return->Scratch.Values;
  if (!Returned.translateFrameCoordinates(*Delta))
    return NoBinding;
  Returned.transformSymbols(Rename);
  if (Returned != Original.Return->Scratch.Values)
    return NoBinding;
  ScratchState ActualA, ActualB;
  ActualA.Values.setCoordinates(Coordinates);
  ActualB.Values.setCoordinates(Coordinates);
  if (AP.apply(ActualA, Original.Input.Registers, Program) != AP.size() ||
      BP.apply(ActualB, Replacement.Input.Registers, Program) != BP.size() ||
      ActualA.Values.size() != AP.size() ||
      ActualB.Values.size() != BP.size() ||
      !ActualB.Values.translateFrameCoordinates(*Delta))
    return NoBinding;
  ActualB.Values.transformSymbols(Rename);
  if (ActualA.Values != ActualB.Values)
    return NoBinding;
  return CallXCompletedResultBinding{*Delta, B.Value, A.Value};
}

llvm::Expected<bool>
ContextFlow::generalizeRedundantContext(size_t Entry, const ContextState &Input,
                                        std::optional<RecursionRetry> Retry) {
  if (ActiveEntries.contains(Program.Med.Blocks[Entry].StartSlot))
    return false;
  // Favor a changed image argument when the retained variants repeat one.
  // For forwarding wrappers, also explore an image binding absent from every
  // variant, once per entry register. Wrappers have no sites of their own;
  // preserve the existing demand order of functions that own CALLX sites.
  // The generic node remembers explored registers across evictions. These
  // select work only; image addresses supply no target or alias proof. A first
  // captured binding can likewise demand one exploration per entry register,
  // after all retained variants used literal, frame or unknown values.
  const auto &Variants = Specializations.at(Entry);
  auto &Generic = *GenericContexts.at(Entry);
  bool NewImageArgument = false;
  bool UnrepresentedImageBinding = false;
  uint16_t NewImageRegisters = 0;
  uint16_t NewCapturedRegisters = 0;
  uint16_t NewFrameRegisters = 0;
  uint16_t NewScalarRegisters = 0;
  for (unsigned R = 0; R < kRegisterCount; ++R) {
    const auto Value = Input.Registers[R].singleton();
    // A new scalar selector can use space occupied by equal completed object
    // results. This does not enable opaque-result eviction or input reuse.
    if (!Retry && Generic.Sites.empty() && R != kFramePointerRegister &&
        Value.ValueKind == RegisterValue::Kind::Constant &&
        !Program.ExecutableImage.findRegion(Value.Value, 1, true) &&
        llvm::all_of(Variants, [&](const auto *Variant) {
          const auto Old = Variant->Input.Registers[R].singleton();
          return Old.ValueKind == RegisterValue::Kind::Constant && Old != Value;
        }))
      NewScalarRegisters |= registerBit(R);
    if (R != kFramePointerRegister &&
        Value.ValueKind == RegisterValue::Kind::SymbolicAddress &&
        !(Generic.ExploredCapturedRegisters & registerBit(R)) &&
        llvm::all_of(Variants, [&](const auto *Variant) {
          return llvm::none_of(
              Variant->Input.Registers[R].values(), [](const auto &Old) {
                return Old.ValueKind == RegisterValue::Kind::SymbolicAddress;
              });
        }))
      NewCapturedRegisters |= registerBit(R);
    // A forwarding wrapper can pin all variants on snapshots of one frame
    // descriptor. Explore a distinct descriptor once per entry register only
    // when the retained known bindings agree; frame membership is not an
    // alias proof and does not change the replacement obligations below.
    if (Generic.Sites.empty() && R != kFramePointerRegister &&
        Value.ValueKind == RegisterValue::Kind::StackAddress &&
        !(Generic.ExploredFrameRegisters & registerBit(R))) {
      const auto Known = llvm::find_if(Variants, [&](const auto *Variant) {
        const auto Old = Variant->Input.Registers[R].singleton();
        return Old.ValueKind == RegisterValue::Kind::StackAddress &&
               Old.Value == Value.Value && Old != Value;
      });
      if (Known != Variants.end() &&
          llvm::all_of(Variants, [&](const auto *Variant) {
            return Variant->Input.Registers[R].unknown() ||
                   Variant->Input.Registers[R] == (*Known)->Input.Registers[R];
          }))
        NewFrameRegisters |= registerBit(R);
    }
    if (R == kFramePointerRegister ||
        Value.ValueKind != RegisterValue::Kind::Constant ||
        !Program.ExecutableImage.findRegion(Value.Value, 1, true))
      continue;
    const bool Absent =
        std::all_of(Variants.begin(), Variants.end(), [&](const auto *Variant) {
          if (Variant->Input.Registers[R].unknown())
            return true;
          const auto Old = Variant->Input.Registers[R].singleton();
          return Old.ValueKind == RegisterValue::Kind::Constant &&
                 Old != Value &&
                 Program.ExecutableImage.findRegion(Old.Value, 1, true);
        });
    if (!Absent)
      continue;
    UnrepresentedImageBinding |= Generic.Sites.empty();
    const auto Known = std::find_if(
        Variants.begin(), Variants.end(), [&](const auto *Variant) {
          return !Variant->Input.Registers[R].unknown();
        });
    NewImageArgument |=
        Known != Variants.end() &&
        std::all_of(Variants.begin(), Variants.end(), [&](const auto *Variant) {
          return Variant->Input.Registers[R].unknown() ||
                 Variant->Input.Registers[R] == (*Known)->Input.Registers[R];
        });
    if (Generic.Sites.empty() &&
        !(Generic.ExploredImageRegisters & registerBit(R)))
      NewImageRegisters |= registerBit(R);
  }
  const bool OnlyPreciseSiblingResult =
      !Retry && !NewImageArgument && !NewImageRegisters &&
      !NewCapturedRegisters && !NewFrameRegisters;
  if (OnlyPreciseSiblingResult && !UnrepresentedImageBinding &&
      !NewScalarRegisters)
    return false;
  // Replacing an old incoming edge must leave the caller transfer unchanged.
  // Start with the opaque-memory case: no returned cells or affine return
  // origins can be lost, and the scalar result must agree exactly.
  const auto OpaqueReturn = [](const FunctionContext &Context) {
    if (!Context.Complete || !Context.Return || Context.TaggedReturn ||
        !Context.Effects.Unknown || !Context.Return->Scratch.empty() ||
        Context.Return->Postconditions.size() ||
        Context.Return->Postconditions.copyCount())
      return false;
    for (const auto &Origin : Context.Return->Origins[0].values())
      if (Origin.ValueKind != RegisterValue::Kind::Constant)
        return false;
    return std::none_of(Context.Return->Registers[0].values().begin(),
                        Context.Return->Registers[0].values().end(),
                        [](const RegisterValue &Value) {
                          return Value.ValueKind ==
                                 RegisterValue::Kind::StackAddress;
                        });
  };
  const bool GenericOpaque = OpaqueReturn(Generic);
  if (!GenericOpaque && !NewScalarRegisters)
    return false;
  if (Retry && Generic.Return->InputExpressions[kReturnRegister])
    return false;

  using TargetCoordinates = std::tuple<size_t, size_t, size_t, va_t>;
  using TargetKey = std::pair<TargetCoordinates, const CallXContextEvidence *>;
  // Equal target addresses do not preserve distinct input/load conditions.
  // Borrow the stable proof steps during this comparison instead of copying
  // their value sets into another retained representation.
  const auto TargetLess = [](const TargetKey &A, const TargetKey &B) {
    if (A.first != B.first)
      return A.first < B.first;
    const auto &Left = *A.second;
    const auto &Right = *B.second;
    if (Left.InputExpressionValues.empty() ||
        Right.InputExpressionValues.empty())
      return Left.InputExpressionValues.size() <
             Right.InputExpressionValues.size();
    if (Left.InputExpression != Right.InputExpression)
      return Left.InputExpression < Right.InputExpression;
    const auto ValuesLess = [](const ProvenValueSet &A,
                               const ProvenValueSet &B) {
      return std::lexicographical_compare(
          A.values().begin(), A.values().end(), B.values().begin(),
          B.values().end(), [](const auto &A, const auto &B) {
            return std::tie(A.ValueKind, A.Value, A.Offset) <
                   std::tie(B.ValueKind, B.Value, B.Offset);
          });
    };
    return std::lexicographical_compare(
        Left.InputExpressionValues.begin(), Left.InputExpressionValues.end(),
        Right.InputExpressionValues.begin(), Right.InputExpressionValues.end(),
        [&](const auto &A, const auto &B) {
          if (A.Expression != B.Expression)
            return A.Expression < B.Expression;
          if (A.Inputs != B.Inputs)
            return ValuesLess(A.Inputs, B.Inputs);
          return ValuesLess(A.Values, B.Values);
        });
  };
  std::set<TargetKey, decltype(TargetLess)> Required(TargetLess),
      Covered(TargetLess);
  constexpr uint64_t TargetBytes = sizeof(TargetKey) + 4 * sizeof(void *);
  uint64_t TemporaryBytes = 0;
  const auto Release =
      llvm::scope_exit([&] { RetainedBytes -= TemporaryBytes; });
  const auto ClearTargets = [&](auto &Targets) {
    const uint64_t Bytes = Targets.size() * TargetBytes;
    RetainedBytes -= Bytes;
    TemporaryBytes -= Bytes;
    Targets.clear();
  };
  // Stable-ID traversal storage is charged with each context ID, as for root
  // publication and orphan reclamation. Target keys are charged separately.
  std::vector<bool> Seen(Contexts.size());
  std::vector<size_t> Pending;
  Pending.reserve(Contexts.size());
  const auto CollectTargets = [&](const FunctionContext &Start, size_t Excluded,
                                  auto &Targets) -> llvm::Expected<bool> {
    std::fill(Seen.begin(), Seen.end(), false);
    Pending.clear();
    Pending.push_back(Start.ID);
    Seen[Start.ID] = true;
    const auto AddSites = [&](const FunctionContext &Context,
                              const ContextCall *Edge =
                                  nullptr) -> llvm::Expected<bool> {
      size_t Instantiated = 0;
      for (const auto &Template : Context.Sites) {
        const auto &Site =
            Edge && Instantiated < Edge->InputExpressionTargets.size() &&
                    Edge->InputExpressionTargets[Instantiated].CallXSlot ==
                        Template.CallXSlot
                ? Edge->InputExpressionTargets[Instantiated++]
                : Template;
        // A union of target alternatives is a different proof from separate
        // singleton contexts; do not discard that distinction here.
        if (Site.TargetValueAlternatives.values().size() > 1)
          return false;
        const auto &Caller = Edge ? *Contexts[Edge->CallerSummaryID] : Context;
        const size_t CallerEntry =
            Program.Med.Blocks[Caller.EntryBlock].StartSlot;
        const size_t CallerSlot = Edge ? Edge->CallerSlot : CallerEntry;
        if (Site.TargetAddress &&
            Targets
                .emplace(TargetCoordinates{CallerEntry, CallerSlot,
                                           Site.CallXSlot, *Site.TargetAddress},
                         &Site)
                .second) {
          if (auto Error = this->retain(TargetBytes))
            return std::move(Error);
          TemporaryBytes += TargetBytes;
        }
      }
      return true;
    };
    auto Added = AddSites(Start);
    if (!Added || !*Added)
      return Added;
    for (size_t I = 0; I < Pending.size(); ++I)
      for (const auto &Edge : Contexts[Pending[I]]->Calls) {
        if (!Edge.CalleeSummaryID)
          continue;
        if (*Edge.CalleeSummaryID == Excluded)
          return false;
        const auto &Child = *Contexts[*Edge.CalleeSummaryID];
        // Legacy dispatch suppresses the immediate child's target evidence;
        // its subsequent summary edges are still traversed by the report.
        if (!Edge.LegacyRuntime) {
          auto Added = AddSites(Child, &Edge);
          if (!Added || !*Added)
            return Added;
        }
        if (!Seen[Child.ID]) {
          Seen[Child.ID] = true;
          Pending.push_back(Child.ID);
        }
      }
    return true;
  };
  const auto IndependentlyPublished = [&](const FunctionContext &Sibling,
                                          size_t Excluded) {
    if (Sibling.IsRoot)
      return true;
    std::fill(Seen.begin(), Seen.end(), false);
    Pending.clear();
    for (const auto &Context : Contexts)
      if (Context && Context->IsRoot) {
        Seen[Context->ID] = true;
        Pending.push_back(Context->ID);
      }
    for (size_t I = 0; I < Pending.size(); ++I)
      for (const auto &Edge : Contexts[Pending[I]]->Calls)
        if (Edge.CalleeSummaryID && *Edge.CalleeSummaryID != Excluded) {
          if (*Edge.CalleeSummaryID == Sibling.ID && !Edge.LegacyRuntime)
            return true;
          if (!Seen[*Edge.CalleeSummaryID]) {
            Seen[*Edge.CalleeSummaryID] = true;
            Pending.push_back(*Edge.CalleeSummaryID);
          }
        }
    return false;
  };

  for (auto *Candidate : Specializations.at(Entry)) {
    if ((Retry && Candidate != Retry->Summary) ||
        (!Retry && (!Candidate->Published ||
                    Candidate->RootGeneration == RootGeneration)) ||
        Candidate->IsRoot)
      continue;
    const bool ObjectResult =
        NewScalarRegisters && hasCompletedObjectResult(*Candidate);
    const bool OpaqueResult = GenericOpaque && OpaqueReturn(*Candidate);
    if (!ObjectResult && (!OpaqueResult || (OnlyPreciseSiblingResult &&
                                            !UnrepresentedImageBinding)))
      continue;
    const bool GenericKeepsResult =
        OpaqueResult &&
        Candidate->Return->Registers[0] == Generic.Return->Registers[0] &&
        Candidate->Return->Origins[0] == Generic.Return->Origins[0];
    // Further image inputs can use redundant, more precise completed results.
    // Repeated opaque results keep the original once-per-register demand.
    if ((Retry && !GenericKeepsResult) ||
        (OnlyPreciseSiblingResult && GenericKeepsResult))
      continue;
    // A generic replacement must preserve the node's own caller proofs.
    // Otherwise only an already computed, independently published sibling
    // with equal results can own these completed incoming edges. Its input
    // key is unchanged: this does not authorize reuse for a new caller.
    const bool GenericKeepsSites = !std::any_of(
        Candidate->Sites.begin(), Candidate->Sites.end(),
        [&](const auto &Site) {
          return Site.TargetAddress &&
                 std::none_of(
                     Generic.Sites.begin(), Generic.Sites.end(),
                     [&](const auto &Replacement) {
                       return Replacement.CallXSlot == Site.CallXSlot &&
                              Replacement.TargetAddress == Site.TargetAddress;
                     });
        });
    const bool GenericEquivalent = GenericKeepsResult && GenericKeepsSites;
    if (!GenericEquivalent && Retry)
      continue;
    const auto SameSites = [&](const FunctionContext &Sibling) {
      if (Candidate->Sites.size() != Sibling.Sites.size())
        return false;
      return std::equal(
          Candidate->Sites.begin(), Candidate->Sites.end(),
          Sibling.Sites.begin(), [](const auto &A, const auto &B) {
            return A.CallXSlot == B.CallXSlot && A.Result == B.Result &&
                   A.TargetAddress == B.TargetAddress &&
                   A.TargetSlot == B.TargetSlot &&
                   A.TargetIsContinuation == B.TargetIsContinuation &&
                   A.TargetValueAlternatives == B.TargetValueAlternatives &&
                   A.InputExpression == B.InputExpression &&
                   A.InputExpressionValues.empty() &&
                   B.InputExpressionValues.empty() &&
                   std::tie(A.Missing.Kind, A.Missing.Slot, A.Missing.Address,
                            A.Missing.Coordinate, A.Missing.InputRegister) ==
                       std::tie(B.Missing.Kind, B.Missing.Slot,
                                B.Missing.Address, B.Missing.Coordinate,
                                B.Missing.InputRegister);
          });
    };
    ClearTargets(Required);
    auto Collected = CollectTargets(*Candidate, Candidate->ID, Required);
    if (!Collected)
      return Collected.takeError();
    if (!*Collected)
      continue;
    bool Redundant = GenericEquivalent && Required.empty();
    FunctionContext *Replacement = &Generic;
    std::optional<CallXCompletedResultBinding> ResultBinding;
    for (auto *Sibling : Specializations.at(Entry)) {
      if (Redundant)
        break;
      if (Sibling == Candidate || !Sibling->Complete || !Sibling->Published)
        continue;
      std::optional<CallXCompletedResultBinding> Binding;
      if (ObjectResult) {
        if (!SameSites(*Sibling))
          continue;
        auto Matched = completedObjectBinding(*Candidate, *Sibling);
        if (!Matched)
          return Matched.takeError();
        if (!*Matched)
          continue;
        Binding = **Matched;
      } else if (!GenericEquivalent &&
                 (!OpaqueReturn(*Sibling) ||
                  Sibling->Return->Registers[0] !=
                      Candidate->Return->Registers[0] ||
                  Sibling->Return->Origins[0] !=
                      Candidate->Return->Origins[0] ||
                  Sibling->Return->InputExpressions[0] !=
                      Candidate->Return->InputExpressions[0] ||
                  !SameSites(*Sibling)))
        continue;
      ClearTargets(Covered);
      auto Collected = CollectTargets(*Sibling, Candidate->ID, Covered);
      if (!Collected)
        return Collected.takeError();
      if (!*Collected ||
          !std::includes(Covered.begin(), Covered.end(), Required.begin(),
                         Required.end(), TargetLess) ||
          !IndependentlyPublished(*Sibling, Candidate->ID))
        continue;
      Redundant = true;
      ResultBinding = Binding;
      if (!GenericEquivalent)
        Replacement = Sibling;
    }
    if (!Redundant)
      continue;
    // Do not rewrite an edge in a graph whose fixed point is still being
    // computed. Completed old callers receive the same ABI facts above.
    bool ActiveCaller = false;
    for (const auto &Context : Contexts)
      if (Context)
        for (const auto &Edge : Context->Calls) {
          if (Edge.CalleeSummaryID != Candidate->ID)
            continue;
          ActiveCaller |= !Context->Complete;
          int64_t Offset;
          if (ResultBinding && Edge.CompletedResultBinding &&
              llvm::AddOverflow(ResultBinding->FrameOffset,
                                Edge.CompletedResultBinding->FrameOffset,
                                Offset))
            ActiveCaller = true;
        }
    if (ActiveCaller)
      continue;
    if (Retry) {
      constexpr uint64_t Bytes =
          sizeof(decltype(RetriedUnblockedCalls)::value_type) +
          4 * sizeof(void *);
      if (auto Error = retain(Bytes))
        return std::move(Error);
      RetriedUnblockedCalls.emplace(Entry, Retry->CalleeEntry);
    }
    const bool WasPublished = Candidate->Published;
    for (const auto &Context : Contexts)
      if (Context)
        for (auto &Edge : Context->Calls)
          if (Edge.CalleeSummaryID == Candidate->ID) {
            Edge.CalleeSummaryID = Replacement->ID;
            Edge.GeneralizedInputs = Replacement->Generic;
            Edge.InputGeneralization =
                Retry ? CallXInputGeneralization::ReplacedRecursiveContext
                : Replacement->Generic
                    ? CallXInputGeneralization::SpecializationLimit
                    : CallXInputGeneralization::EquivalentSpecializedResults;
            for (auto &Site : Edge.InputExpressionTargets)
              Site.SummaryID = Replacement->ID;
            if (ResultBinding) {
              auto Binding = *ResultBinding;
              if (Edge.CompletedResultBinding) {
                Binding.FrameOffset += Edge.CompletedResultBinding->FrameOffset;
                Binding.OriginalCapture =
                    Edge.CompletedResultBinding->OriginalCapture;
              }
              Edge.CompletedResultBinding = Binding;
            } else {
              Edge.PreservedOlderFrames = false;
              Edge.AppliedMemoryEffects = false;
              Edge.AppliedStoredValues = 0;
              Edge.InstantiatedReturnInputExpression = false;
            }
            ++Replacement->IncomingReferences;
            --Candidate->IncomingReferences;
          }
    Candidate->Published = false;
    Pending.clear();
    Pending.push_back(Candidate->ID);
    for (size_t I = 0; I < Pending.size(); ++I)
      reclaimContext(*Contexts[Pending[I]], &Pending, true);
    Generic.ExploredImageRegisters |= NewImageRegisters;
    Generic.ExploredCapturedRegisters |= NewCapturedRegisters;
    Generic.ExploredFrameRegisters |= NewFrameRegisters;
    if (WasPublished)
      ++GeneralizedPublishedSummaries;
    if (Retry)
      ++UnblockedRecursionRetries;
    return true;
  }
  return false;
}

ContextState ContextFlow::genericInput() const {
  ContextState Input;
  Input.Scratch.Values.setCoordinates(Coordinates);
  if (!versionHasFeature(Program.Low.TheVersion,
                         VersionFeature::ManualStackFrames))
    Input.Registers[kFramePointerRegister] = RegisterValue{
        RegisterValue::Kind::StackAddress,
        initialFramePointer(Program.Low.TheVersion, Program.Config), 0};
  return Input;
}

llvm::Expected<RegisterValue>
ContextFlow::captureAddress(size_t Owner, size_t Slot, uint64_t Source,
                            UnsignedValueRange Bounds) {
  const auto Key = std::tuple(Owner, Slot, Source, Bounds.Lower, Bounds.Upper);
  if (const auto It = CaptureIndex.find(Key); It != CaptureIndex.end())
    return RegisterValue{RegisterValue::Kind::SymbolicAddress, It->second, 0};
  const uint64_t Bytes = 2 * sizeof(UnsignedValueRange) + 2 * sizeof(size_t) +
                         sizeof(decltype(CaptureIndex)::value_type) +
                         6 * sizeof(void *);
  if (auto Error = retain(Bytes))
    return std::move(Error);
  const size_t ID = CapturedAddresses->size();
  CaptureIndex.emplace(Key, ID);
  CapturedAddresses->push_back(Bounds);
  CaptureOwners.push_back(Owner);
  return RegisterValue{RegisterValue::Kind::SymbolicAddress, ID, 0};
}

llvm::Error ContextFlow::computeInputDependencies() {
  const size_t Count = Program.Med.Blocks.size();
  InputRegisters.resize(Count);
  ReturnCarriers.resize(Count);
  LiveCallReturns.resize(Program.Low.Instructions.size(), true);
  StoredReturnValues.resize(Program.Low.Instructions.size());
  ReadsMemory.resize(Count);
  ContainsCalls.resize(Count);
  ReachesCallX.resize(Count);
  if (auto Error = retain(Count * (2 * sizeof(uint16_t) + 3 * sizeof(bool)) +
                          (LiveCallReturns.capacity() +
                           StoredReturnValues.capacity() + kBitsPerByte - 1) /
                              kBitsPerByte))
    return Error;
  // Direct call dependencies participate in the same monotone fixed point as
  // CFG successors. Unknown calls conservatively read every register/memory.
  std::vector<size_t> ReverseOffsets(Count + 1);
  for (size_t ID = 0; ID < Count; ++ID) {
    for (size_t E = Offsets[ID]; E < Offsets[ID + 1]; ++E)
      ++ReverseOffsets[Successors[E] + 1];
    const auto &Block = Program.Med.Blocks[ID];
    for (size_t Slot = Block.StartSlot; Slot < Block.EndSlot; ++Slot)
      if (const auto *I = Index.find(Slot);
          I && I->Call == CallKind::Internal && I->CallTarget &&
          ByEntry.contains(*I->CallTarget))
        ++ReverseOffsets[ByEntry.at(*I->CallTarget) + 1];
  }
  std::partial_sum(ReverseOffsets.begin(), ReverseOffsets.end(),
                   ReverseOffsets.begin());
  std::vector<size_t> Reverse(ReverseOffsets.back());
  auto Next = ReverseOffsets;
  for (size_t ID = 0; ID < Count; ++ID) {
    for (size_t E = Offsets[ID]; E < Offsets[ID + 1]; ++E)
      Reverse[Next[Successors[E]]++] = ID;
    const auto &Block = Program.Med.Blocks[ID];
    for (size_t Slot = Block.StartSlot; Slot < Block.EndSlot; ++Slot)
      if (const auto *I = Index.find(Slot);
          I && I->Call == CallKind::Internal && I->CallTarget &&
          ByEntry.contains(*I->CallTarget))
        Reverse[Next[ByEntry.at(*I->CallTarget)]++] = ID;
  }
  const uint64_t Bytes =
      (Reverse.size() + 2 * ReverseOffsets.size() + Count) * sizeof(size_t) +
      Count;
  if (auto Error = retain(Bytes))
    return Error;
  const auto Release = llvm::scope_exit([&] { RetainedBytes -= Bytes; });
  std::vector<bool> Queued(Count, true);
  std::deque<size_t> Worklist;
  for (size_t ID = Count; ID > 0; --ID)
    Worklist.push_back(ID - 1);
  while (!Worklist.empty()) {
    const size_t ID = Worklist.front();
    Worklist.pop_front();
    Queued[ID] = false;
    uint16_t Uses = 0;
    uint16_t ReturnUses = 0;
    bool Memory = false;
    bool Calls = false;
    bool CallX = false;
    for (size_t E = Offsets[ID]; E < Offsets[ID + 1]; ++E) {
      Uses |= InputRegisters[Successors[E]];
      ReturnUses |= ReturnCarriers[Successors[E]];
      Memory |= ReadsMemory[Successors[E]];
      Calls |= ContainsCalls[Successors[E]];
      CallX |= ReachesCallX[Successors[E]];
    }
    const auto &Block = Program.Med.Blocks[ID];
    for (size_t Slot = Block.EndSlot; Slot > Block.StartSlot;) {
      const auto *I = Index.find(--Slot);
      if (!I)
        continue;
      if (auto Error = visit())
        return Error;
      ++DependencyVisits;
      CallInputs Callee;
      if (I->Call != CallKind::None) {
        Calls = true;
        // Uses is the live-out mask at this call. The same fixed point includes
        // CFG successors and direct callees that consume r0 as an input.
        LiveCallReturns[I->Slot] = Uses & registerBit(kReturnRegister);
        if (I->Call == CallKind::Internal && I->CallTarget &&
            ByEntry.contains(*I->CallTarget) &&
            !hasRegisteredLegacyRuntime(*I)) {
          const size_t Entry = ByEntry.at(*I->CallTarget);
          if (!MayReturnFromBlock[Entry]) {
            // A matched nonreturning call makes the continuation unreachable.
            // Its own dependencies still participate in the fixed point.
            Uses = 0;
            ReturnUses = 0;
            Memory = false;
            CallX = false;
            LiveCallReturns[I->Slot] = false;
          }
          Callee = {usedCalleeInputs(*I, Entry, InputRegisters[Entry]),
                    ReadsMemory[Entry]};
          CallX |= ReachesCallX[Entry];
        } else {
          CallX |= I->Call != CallKind::Syscall;
        }
      }
      if (I->Op == Operation::Exit)
        ReturnUses |= registerBit(kReturnRegister);
      if (I->Op == Operation::Store && I->Form == OperandForm::StoreReg &&
          I->Width == kDoubleWordBitWidth)
        StoredReturnValues[I->Slot] = ReturnUses & registerBit(I->Src);
      if (I->Call != CallKind::None) {
        ReturnUses &=
            ~((uint16_t{1} << callClobberedRegisterCount(I->Call)) - 1);
      } else if (I->Semantics.WritesDestination) {
        const bool Live = ReturnUses & registerBit(I->Dst);
        ReturnUses &= ~registerBit(I->Dst);
        if (Live && I->Width == kDoubleWordBitWidth &&
            (I->Op == Operation::Mov || I->Op == Operation::Add ||
             I->Op == Operation::Sub)) {
          if (I->Op != Operation::Mov)
            ReturnUses |= registerBit(I->Dst);
          if (I->Form == OperandForm::DstSrc)
            ReturnUses |= registerBit(I->Src);
        }
      }
      const auto Dependencies = instructionDependencies(*I, Callee);
      Uses = (Uses & ~Dependencies.Writes) | Dependencies.Reads;
      Memory |= Dependencies.Memory;
    }
    if (Uses == InputRegisters[ID] && ReturnUses == ReturnCarriers[ID] &&
        Memory == ReadsMemory[ID] && Calls == ContainsCalls[ID] &&
        CallX == ReachesCallX[ID])
      continue;
    InputRegisters[ID] = Uses;
    ReturnCarriers[ID] = ReturnUses;
    ReadsMemory[ID] = Memory;
    ContainsCalls[ID] = Calls;
    ReachesCallX[ID] = CallX;
    for (size_t E = ReverseOffsets[ID]; E < ReverseOffsets[ID + 1]; ++E)
      if (!Queued[Reverse[E]]) {
        Queued[Reverse[E]] = true;
        Worklist.push_back(Reverse[E]);
      }
  }
  return llvm::Error::success();
}

llvm::ArrayRef<RegisterValue> possibleValues(const ProvenValueSet &Values) {
  static const RegisterValue Unknown;
  return Values.unknown() ? llvm::ArrayRef<RegisterValue>(Unknown)
                          : Values.values();
}

ProvenValueSet
registerResult(const MedInstruction &Instruction,
               const std::array<ProvenValueSet, kRegisterCount> &Input,
               const ProgramImage &Image,
               const StoredRegisterMemory *Memory = nullptr) {
  std::optional<ProvenValueSet> Output;
  for (const auto &Source : possibleValues(Input[Instruction.Src]))
    for (const auto &Destination : possibleValues(Input[Instruction.Dst])) {
      RegisterState Registers{};
      Registers[Instruction.Dst] = Destination;
      Registers[Instruction.Src] = Source;
      applyRegisterTransfer(Instruction, Registers, Image, Memory);
      const ProvenValueSet Value(Registers[Instruction.Dst]);
      if (!Output)
        Output = Value;
      else
        Output->join(Value);
    }
  return *Output;
}

void ContextFlow::addMemoryReadEffects(MemoryEffects &Effects,
                                       const MedInstruction &Instruction,
                                       const ContextState &State) const {
  const auto Base = State.Registers[Instruction.Src].singleton();
  const auto Address = effectiveAddress(Base, Instruction.Offset);
  if (Base.ValueKind != RegisterValue::Kind::Constant || !Address ||
      !Program.ExecutableImage.findRegion(
          *Address, Instruction.Width / kBitsPerByte, true))
    addMemoryAccessEffects(Effects, Instruction, State, Instruction.Src);
}

void ContextFlow::addMemoryAccessEffects(MemoryEffects &Effects,
                                         const MedInstruction &Instruction,
                                         const ContextState &State,
                                         unsigned AddressRegister) const {
  auto Origins = State.Origins[AddressRegister];
  // Literal addresses and captures can replace lost origins, but an actual
  // frame coordinate must never be reinterpreted as a formal entry register.
  if (Origins.unknown() &&
      llvm::all_of(State.Registers[AddressRegister].values(),
                   [](const RegisterValue &Value) {
                     return Value.ValueKind == RegisterValue::Kind::Constant ||
                            Value.ValueKind ==
                                RegisterValue::Kind::UnsignedRange ||
                            Value.ValueKind ==
                                RegisterValue::Kind::SymbolicAddress;
                   }))
    Origins = State.Registers[AddressRegister];
  for (const auto &Origin : possibleValues(Origins)) {
    RegisterState Base{};
    Base[0] = Origin;
    Effects.addWrite(substituteOrigin({RegisterValue::Kind::StackAddress, 1,
                                       Instruction.Offset},
                                      Base, Program),
                     Instruction.Width / kBitsPerByte, Program,
                     CapturedAddresses.get());
  }
}

llvm::Expected<std::optional<size_t>>
ContextFlow::inputMemoryGuard(MemoryEffects Effects) {
  if (!Effects.WritesFrames && Effects.Writes.empty())
    return std::optional<size_t>{};
  MemoryGuardKey Key{Effects.WritesFrames, {}};
  auto &Writes = std::get<1>(Key);
  for (const auto &Write : Effects.Writes)
    Writes.emplace_back(Write.Address.ValueKind, Write.Address.Value,
                        Write.Address.Offset, Write.Size);
  std::sort(Writes.begin(), Writes.end());
  if (const auto It = MemoryGuardIndex.find(Key); It != MemoryGuardIndex.end())
    return std::optional<size_t>{It->second};
  const uint64_t Bytes =
      2 * sizeof(MemoryEffects) + 2 * sizeof(CallXInputMemoryGuard) +
      sizeof(decltype(MemoryGuardIndex)::value_type) + 4 * sizeof(void *) +
      Effects.Writes.capacity() * sizeof(MemoryWrite) +
      Writes.capacity() *
          sizeof(std::tuple<RegisterValue::Kind, uint64_t, int64_t, uint64_t>) +
      2 * Effects.Writes.size() * sizeof(CallXMemoryWrite);
  if (auto Error = retain(Bytes))
    return std::move(Error);
  const size_t ID = InputMemoryGuards.size();
  MemoryGuardIndex.emplace(std::move(Key), ID);
  InputMemoryGuards.push_back(std::move(Effects));
  return std::optional<size_t>{ID};
}

llvm::Expected<size_t>
ContextFlow::inputExpression(CallXInputExpression Expression) {
  ExpressionKey Key;
  switch (Expression.Kind) {
  case CallXInputExpressionKind::InputRegister:
    Key = {Expression.Kind,
           Expression.Register,
           0,
           Expression.Offset,
           Opcode::Unknown,
           {},
           {}};
    break;
  case CallXInputExpressionKind::Constant:
    Key = {Expression.Kind,
           0,
           Expression.Constant.Value,
           0,
           Opcode::Unknown,
           {},
           {}};
    break;
  case CallXInputExpressionKind::Instruction: {
    const auto &Instruction = *Index.find(Expression.InstructionSlot);
    // These unary nodes bind the value independently of its physical carrier.
    // The fixed program version gives SourceOpcode one normalized transfer;
    // a PC or register rename does not change that transfer. Loads refer only
    // to the preserved entry-memory snapshot, never to a later memory epoch.
    const bool Load = Instruction.Op == Operation::Load;
    Key = {Expression.Kind,
           *Expression.Input,
           Load ? 0
                : normalizeImmediate(Instruction.Immediate,
                                     Instruction.Semantics.Immediate),
           Load ? Instruction.Offset : 0,
           Instruction.SourceOpcode,
           {},
           {}};
    Expression.Depth = InputExpressions[*Expression.Input].Depth + 1;
    break;
  }
  case CallXInputExpressionKind::InputAlternatives:
    Key = {Expression.Kind,         0, 0, 0, Opcode::Unknown,
           Expression.Alternatives, {}};
    Expression.Depth = 1;
    for (size_t Alternative : Expression.Alternatives)
      Expression.Depth += InputExpressions[Alternative].Depth;
    break;
  }
  std::get<6>(Key) = Expression.MemoryGuard;
  if (const auto It = ExpressionIndex.find(Key); It != ExpressionIndex.end())
    return It->second;
  // Include vector growth and the index node in the same retained estimate.
  constexpr uint64_t Bytes = 2 * sizeof(CallXInputExpression) +
                             sizeof(decltype(ExpressionIndex)::value_type) +
                             4 * sizeof(void *) + 2 * sizeof(size_t);
  // Both the index and the node own their alternative leaf references.
  if (auto Error = retain(Bytes + 2 * Expression.Alternatives.capacity() *
                                      sizeof(size_t)))
    return std::move(Error);
  const size_t ID = InputExpressions.size();
  ExpressionIndex.emplace(Key, ID);
  uint16_t Registers = 0;
  uint8_t Paths = 1;
  if (Expression.Kind == CallXInputExpressionKind::InputRegister) {
    Registers = registerBit(Expression.Register);
  } else if (Expression.Input) {
    Registers = InputExpressionRegisters[*Expression.Input];
    Paths = InputExpressionPaths[*Expression.Input];
  } else if (Expression.Kind == CallXInputExpressionKind::InputAlternatives) {
    Paths = 0;
    for (size_t Alternative : Expression.Alternatives) {
      Registers |= InputExpressionRegisters[Alternative];
      Paths += InputExpressionPaths[Alternative];
    }
  }
  InputExpressionRegisters.push_back(Registers);
  InputExpressionPaths.push_back(Paths);
  InputExpressions.push_back(std::move(Expression));
  return ID;
}

llvm::Expected<std::optional<size_t>>
ContextFlow::inputUnionExpression(llvm::ArrayRef<size_t> Alternatives) {
  std::vector<size_t> Members;
  for (size_t ID : Alternatives) {
    const auto &Node = InputExpressions[ID];
    if (Node.Kind == CallXInputExpressionKind::InputAlternatives)
      Members.insert(Members.end(), Node.Alternatives.begin(),
                     Node.Alternatives.end());
    else
      Members.push_back(ID);
  }
  std::sort(Members.begin(), Members.end());
  Members.erase(std::unique(Members.begin(), Members.end()), Members.end());
  if (Members.size() == 1)
    return std::optional<size_t>{Members.front()};
  size_t Paths = 0;
  for (size_t ID : Members) {
    Paths += InputExpressionPaths[ID];
    if (Paths > kCallXContextValueAlternatives)
      return std::optional<size_t>{};
  }
  CallXInputExpression Union;
  Union.Kind = CallXInputExpressionKind::InputAlternatives;
  Union.Alternatives = std::move(Members);
  auto ID = inputExpression(std::move(Union));
  if (!ID)
    return ID.takeError();
  return std::optional<size_t>{*ID};
}

llvm::Error ContextFlow::joinStates(ContextState &State,
                                    const ContextState &Other) {
  uint64_t Temporary =
      State.DefinedCaptures.empty() && Other.DefinedCaptures.empty()
          ? 0
          : sizeof(StoredRegisterMemory) +
                State.Scratch.Values.retainedByteEstimate() +
                Other.Scratch.Values.retainedByteEstimate() +
                2 *
                    (State.DefinedCaptures.size() +
                     Other.DefinedCaptures.size()) *
                    sizeof(uint64_t);
  for (const auto *Input : std::array<const ContextState *, 2>{&State, &Other})
    if (Input->Tagged)
      for (const auto &Case : Input->Tagged->Cases)
        Temporary += 2 * Case.DefinedCaptures.size() * sizeof(uint64_t);
  if (State.Nullable || Other.Nullable ||
      State.Registers[0].singleton() ==
          RegisterValue{RegisterValue::Kind::Constant, 0, 0} ||
      Other.Registers[0].singleton() ==
          RegisterValue{RegisterValue::Kind::Constant, 0, 0})
    for (const auto *Input :
         std::array<const ContextState *, 2>{&State, &Other}) {
      Temporary += Input->Scratch.Values.retainedByteEstimate() +
                   Input->Postconditions.retainedByteEstimate();
      if (Input->Nullable)
        Temporary += Input->Nullable->retainedByteEstimate();
    }
  if (auto Error = retain(Temporary))
    return Error;
  const auto Release = llvm::scope_exit([&] { RetainedBytes -= Temporary; });
  llvm::Error Failure = llvm::Error::success();
  State.join(Other, Program, Coordinates,
             [&](const ProvenValueSet &Left, std::optional<size_t> LeftID,
                 const ProvenValueSet &Right,
                 std::optional<size_t> RightID) -> std::optional<size_t> {
               if (Failure)
                 return std::nullopt;
               const auto Bind = [&](const ProvenValueSet &Origins,
                                     std::optional<size_t> ID)
                   -> llvm::Expected<std::optional<size_t>> {
                 return ID ? llvm::Expected<std::optional<size_t>>(ID)
                           : inputOriginExpression(Origins);
               };
               auto A = Bind(Left, LeftID);
               if (!A) {
                 Failure = A.takeError();
                 return std::nullopt;
               }
               if (!*A)
                 return std::nullopt;
               auto B = Bind(Right, RightID);
               if (!B) {
                 Failure = B.takeError();
                 return std::nullopt;
               }
               if (!*B)
                 return std::nullopt;
               auto Union = inputUnionExpression({**A, **B});
               if (!Union) {
                 Failure = Union.takeError();
                 return std::nullopt;
               }
               return *Union;
             });
  return Failure;
}

llvm::Expected<std::optional<size_t>>
ContextFlow::inputOriginExpression(const ProvenValueSet &Origins) {
  if (Origins.unknown() ||
      std::any_of(Origins.values().begin(), Origins.values().end(),
                  [](const auto &Origin) {
                    return Origin.ValueKind != RegisterValue::Kind::Constant &&
                           Origin.ValueKind !=
                               RegisterValue::Kind::StackAddress;
                  }))
    return std::optional<size_t>{};
  std::vector<size_t> Leaves;
  for (const auto &Origin : Origins.values()) {
    CallXInputExpression Leaf;
    if (Origin.ValueKind == RegisterValue::Kind::Constant) {
      Leaf.Kind = CallXInputExpressionKind::Constant;
      Leaf.Constant = Origin;
    } else {
      Leaf.Register = Origin.Value - 1;
      Leaf.Offset = Origin.Offset;
    }
    auto ID = inputExpression(Leaf);
    if (!ID)
      return ID.takeError();
    Leaves.push_back(*ID);
  }
  std::sort(Leaves.begin(), Leaves.end());
  Leaves.erase(std::unique(Leaves.begin(), Leaves.end()), Leaves.end());
  if (Leaves.size() == 1)
    return std::optional<size_t>{Leaves.front()};
  CallXInputExpression Expression;
  Expression.Kind = CallXInputExpressionKind::InputAlternatives;
  Expression.Alternatives = std::move(Leaves);
  auto ID = inputExpression(std::move(Expression));
  if (!ID)
    return ID.takeError();
  return std::optional<size_t>{*ID};
}

llvm::Expected<std::optional<size_t>> ContextFlow::composeReturnExpression(
    size_t Expression, const ContextState &State,
    const std::array<ProvenValueSet, kRegisterCount> &OriginEntry) {
  const size_t Depth = InputExpressions[Expression].Depth;
  const uint64_t Bytes =
      Depth * (sizeof(std::pair<size_t, bool>) + sizeof(size_t)) +
      3 * kCallXContextValueAlternatives * kCallXContextValueAlternatives *
          sizeof(size_t);
  if (auto Error = retain(Bytes))
    return std::move(Error);
  const auto Release = llvm::scope_exit([&] { RetainedBytes -= Bytes; });
  std::vector<std::pair<size_t, bool>> Pending;
  Pending.reserve(Depth);
  Pending.emplace_back(Expression, false);
  std::vector<size_t> Results;
  Results.reserve(InputExpressionPaths[Expression]);
  while (!Pending.empty()) {
    const auto [ID, Expanded] = Pending.back();
    Pending.pop_back();
    // Interning a substituted node can reallocate InputExpressions.
    auto Node = InputExpressions[ID];
    if (!Expanded && (Node.Input || !Node.Alternatives.empty())) {
      Pending.emplace_back(ID, true);
      if (Node.Input)
        Pending.emplace_back(*Node.Input, false);
      else
        for (auto It = Node.Alternatives.rbegin();
             It != Node.Alternatives.rend(); ++It)
          Pending.emplace_back(*It, false);
      continue;
    }
    if (auto Error = visit())
      return std::move(Error);
    if (Node.Kind == CallXInputExpressionKind::Constant) {
      Results.push_back(ID);
    } else if (Node.Kind == CallXInputExpressionKind::InputRegister) {
      if (Node.Offset == 0 && Node.Register != kFramePointerRegister &&
          State.InputExpressions[Node.Register]) {
        Results.push_back(*State.InputExpressions[Node.Register]);
        continue;
      }
      const auto Origins = substituteOrigins(
          ProvenValueSet(RegisterValue{RegisterValue::Kind::StackAddress,
                                       Node.Register + 1, Node.Offset}),
          OriginEntry, Program);
      auto Rebased = inputOriginExpression(Origins);
      if (!Rebased)
        return Rebased.takeError();
      if (!*Rebased)
        return std::optional<size_t>{};
      Results.push_back(**Rebased);
    } else if (Node.Kind == CallXInputExpressionKind::InputAlternatives) {
      const size_t First = Results.size() - Node.Alternatives.size();
      auto Union =
          inputUnionExpression(llvm::ArrayRef(Results).drop_front(First));
      if (!Union)
        return Union.takeError();
      if (!*Union)
        return std::optional<size_t>{};
      Results.resize(First);
      Results.push_back(**Union);
    } else {
      if (Index.find(Node.InstructionSlot)->Op == Operation::Load) {
        MemoryEffects Effects = State.InputWrites;
        if (Node.MemoryGuard) {
          RegisterState Entry{};
          for (unsigned R = 0; R < kRegisterCount; ++R)
            Entry[R] = OriginEntry[R].singleton();
          Effects.include(InputMemoryGuards[*Node.MemoryGuard], Entry, Program);
        }
        if (Effects.Unknown)
          return std::optional<size_t>{};
        auto Guard = inputMemoryGuard(std::move(Effects));
        if (!Guard)
          return Guard.takeError();
        Node.MemoryGuard = *Guard;
      }
      Node.Input = Results.back();
      auto Rebased = inputExpression(std::move(Node));
      if (!Rebased)
        return Rebased.takeError();
      Results.back() = *Rebased;
    }
  }
  return std::optional<size_t>{Results.back()};
}

llvm::Error
ContextFlow::transferInputExpression(const MedInstruction &Instruction,
                                     ContextState &State) {
  if (Instruction.Op == Operation::Store) {
    addMemoryAccessEffects(State.InputWrites, Instruction, State,
                           Instruction.Dst);
    MedInstruction Store = Instruction;
    Store.Form = OperandForm::StoreReg;
    Store.Src = Store.Dst == 0 ? 1 : 0;
    RegisterValue Value;
    if (Instruction.Form == OperandForm::StoreReg &&
        Instruction.Width == kDoubleWordBitWidth &&
        State.Registers[Instruction.Src].unknown() &&
        State.Origins[Instruction.Src].unknown() &&
        State.InputExpressions[Instruction.Src])
      Value = {RegisterValue::Kind::Constant,
               *State.InputExpressions[Instruction.Src], 0};
    if (State.InputSpills.empty() &&
        Value.ValueKind == RegisterValue::Kind::Unknown)
      return llvm::Error::success();
    std::optional<StoredRegisterMemory> Output;
    for (const auto &Base : possibleValues(State.Registers[Instruction.Dst])) {
      MachineState Spill;
      Spill.Registers[Store.Dst] = Base;
      Spill.Registers[Store.Src] = Value;
      Spill.Scratch.Values = State.InputSpills;
      applyTransfer(Store, Spill, Program.ExecutableImage,
                    ScratchLoadPolicy::ProvenValues);
      if (!Output)
        Output = std::move(Spill.Scratch.Values);
      else
        Output->meet(Spill.Scratch.Values);
    }
    State.InputSpills = std::move(*Output);
  }
  if (!Instruction.Semantics.WritesDestination)
    return llvm::Error::success();
  if (Instruction.Op == Operation::Load &&
      Instruction.Semantics.MemoryWidth == kDoubleWordBitWidth) {
    std::optional<size_t> Stored;
    const auto Addresses = substituteOrigins(
        RegisterValue{RegisterValue::Kind::StackAddress,
                      uint64_t{Instruction.Src} + 1, Instruction.Offset},
        State.Origins, Program);
    for (const auto &Address : possibleValues(Addresses)) {
      const auto Expression = State.Postconditions.readExpression(Address);
      if (!Expression || (Stored && *Stored != *Expression)) {
        Stored.reset();
        break;
      }
      Stored = Expression;
    }
    if (Stored) {
      State.InputExpressions[Instruction.Dst] = Stored;
      return llvm::Error::success();
    }
    for (const auto &Base : possibleValues(State.Registers[Instruction.Src])) {
      const auto Address = effectiveAddress(Base, Instruction.Offset);
      const auto Value =
          Address ? State.InputSpills.read(
                        *Address,
                        Base.ValueKind == RegisterValue::Kind::StackAddress
                            ? MemoryCoordinate::FrameRelative
                            : MemoryCoordinate::Absolute)
                  : RegisterValue{};
      if (Value.ValueKind != RegisterValue::Kind::Constant ||
          (Stored && *Stored != Value.Value)) {
        Stored.reset();
        break;
      }
      Stored = Value.Value;
    }
    if (Stored) {
      // The load happened before the spill. Later writes to its original
      // input cell cannot change the value already captured in this cell.
      State.InputExpressions[Instruction.Dst] = Stored;
      return llvm::Error::success();
    }
  }
  const bool Copy = Instruction.Op == Operation::Mov &&
                    Instruction.Form == OperandForm::DstSrc &&
                    Instruction.Width == kDoubleWordBitWidth;
  const bool Load = Instruction.Op == Operation::Load &&
                    Instruction.Semantics.MemoryWidth == kDoubleWordBitWidth &&
                    !State.InputWrites.Unknown;
  const bool Affine =
      (Instruction.Op == Operation::Add || Instruction.Op == Operation::Sub) &&
      Instruction.Form == OperandForm::DstImm &&
      Instruction.Width == kDoubleWordBitWidth;
  // Capture the source before killing Dst, including LD r2, [r2+40].
  const unsigned Source = Load || Copy ? Instruction.Src : Instruction.Dst;
  auto Input = State.InputExpressions[Source];
  if (Copy) {
    State.InputExpressions[Instruction.Dst] = Input;
    return llvm::Error::success();
  }
  State.InputExpressions[Instruction.Dst].reset();
  if (!Load && !Affine)
    return llvm::Error::success();
  // Reuse a complete canonical origin instead of allocating another guarded
  // expression for a field already known after local stores or helper returns.
  if (Load &&
      (State.InputWrites.WritesFrames || !State.InputWrites.Writes.empty()) &&
      !loadOrigin(Instruction, State).unknown())
    return llvm::Error::success();
  if (Load) {
    const auto Address =
        substituteOrigins(RegisterValue{RegisterValue::Kind::StackAddress,
                                        Source + 1, Instruction.Offset},
                          State.Origins, Program);
    // A guard that discards this very entry-frame cell cannot recover its
    // value from an actual caller. Local stored facts were handled above.
    if (llvm::any_of(Address.values(), [&](const auto &Value) {
          return State.InputWrites.invalidatesEntryFrame(Value, Program);
        }))
      return llvm::Error::success();
  }
  if (!Input && Load) {
    auto Origin = inputOriginExpression(State.Origins[Source]);
    if (!Origin)
      return Origin.takeError();
    Input = *Origin;
  }
  if (!Input) {
    const auto Value = State.Registers[Source].singleton();
    if (!Load || Value.ValueKind != RegisterValue::Kind::Constant)
      return llvm::Error::success();
    CallXInputExpression Constant;
    Constant.Kind = CallXInputExpressionKind::Constant;
    Constant.Constant = Value;
    auto ID = inputExpression(Constant);
    if (!ID)
      return ID.takeError();
    Input = *ID;
  }
  CallXInputExpression Step;
  Step.Kind = CallXInputExpressionKind::Instruction;
  Step.Input = Input;
  Step.InstructionSlot = Instruction.Slot;
  if (Load) {
    auto Guard = inputMemoryGuard(State.InputWrites);
    if (!Guard)
      return Guard.takeError();
    Step.MemoryGuard = *Guard;
  }
  auto ID = inputExpression(Step);
  if (!ID)
    return ID.takeError();
  State.InputExpressions[Instruction.Dst] = *ID;
  return llvm::Error::success();
}

ProvenValueSet
ContextFlow::loadTypedValue(const MedInstruction &Instruction,
                            const ProvenValueSet &Bases,
                            const StoredRegisterMemory &Memory) const {
  std::optional<ProvenValueSet> Output;
  for (const auto &Base : possibleValues(Bases)) {
    const auto Address = memoryAddress(Base, Instruction.Offset);
    const auto Stored =
        Address ? Memory.readValues(*Address, Instruction.Width / kBitsPerByte)
                : ProvenValueSet{};
    for (const auto &Value : possibleValues(Stored)) {
      StoredRegisterMemory Cell;
      Cell.setCoordinates(Coordinates);
      if (Address && !Stored.unknown())
        Cell.writeValues(*Address, Value, Instruction.Width / kBitsPerByte);
      RegisterState Registers{};
      Registers[Instruction.Src] = Base;
      applyRegisterTransfer(Instruction, Registers, Program.ExecutableImage,
                            &Cell);
      const ProvenValueSet Loaded(Registers[Instruction.Dst]);
      if (!Output)
        Output = Loaded;
      else
        Output->join(Loaded);
    }
  }
  return *Output;
}

ProvenValueSet ContextFlow::loadOrigin(const MedInstruction &Instruction,
                                       const ContextState &State) const {
  auto Origin = loadTypedValue(Instruction, State.Registers[Instruction.Src],
                               State.OriginScratch.Values);
  if (!Origin.unknown())
    return Origin;
  RegisterState Entry{};
  Entry[0] = State.Origins[Instruction.Src].singleton();
  return State.Postconditions.read(
      substituteOrigin(
          {RegisterValue::Kind::StackAddress, 1, Instruction.Offset}, Entry,
          Program),
      Instruction.Width / kBitsPerByte);
}

void ContextFlow::transferTaggedMemory(const MedInstruction &Instruction,
                                       ContextState &State) {
  if (!State.Tagged)
    return;
  if (Instruction.Call != CallKind::None) {
    // A fresh call can install its own result below. Retaining an older
    // partition across arbitrary calls or memory copies requires a separate
    // complete effect/identity proof.
    State.Tagged.reset();
    return;
  }
  auto &Tagged = *State.Tagged;
  if (Instruction.Op == Operation::Store) {
    StoredRegisterMemory Tag;
    Tag.setCoordinates(Coordinates);
    Tag.writeValues(Tagged.Address,
                    RegisterValue{RegisterValue::Kind::Constant, 0, 0},
                    Tagged.Width);
    for (const auto &Base : possibleValues(State.Registers[Instruction.Dst])) {
      const auto Address = memoryAddress(Base, Instruction.Offset);
      if (!Address) {
        State.Tagged.reset();
        return;
      }
      Tag.invalidate(*Address, Instruction.Width / kBitsPerByte);
      if (Tag.empty()) {
        State.Tagged.reset();
        return;
      }
      for (auto &Case : Tagged.Cases)
        Case.Values.invalidate(*Address, Instruction.Width / kBitsPerByte);
    }
    RegisterState Entry{};
    Entry[0] = State.Origins[Instruction.Dst].singleton();
    const auto Origin = substituteOrigin(
        {RegisterValue::Kind::StackAddress, 1, Instruction.Offset}, Entry,
        Program);
    for (auto &Case : Tagged.Cases)
      Case.Postconditions.store(Origin, Instruction.Width / kBitsPerByte, {});
  }
  if (!Instruction.Semantics.WritesDestination)
    return;
  const bool Copy = Instruction.Op == Operation::Mov &&
                    Instruction.Form == OperandForm::DstSrc &&
                    Instruction.Width == kDoubleWordBitWidth &&
                    (Tagged.Registers & registerBit(Instruction.Src));
  const bool Load = Instruction.Op == Operation::Load &&
                    Instruction.Width / kBitsPerByte == Tagged.Width &&
                    memoryAddress(State.Registers[Instruction.Src].singleton(),
                                  Instruction.Offset) == Tagged.Address;
  Tagged.Registers &= ~registerBit(Instruction.Dst);
  if (Copy || Load)
    Tagged.Registers |= registerBit(Instruction.Dst);
}

llvm::Expected<bool> ContextFlow::refineTaggedBranch(const MedBlock &Block,
                                                     size_t Successor,
                                                     ContextState &State) {
  const auto *Last = Index.find(Block.EndSlot - 1);
  if (!Last || !State.Tagged ||
      Last->Semantics.Terminator != TerminatorKind::ConditionalJump)
    return true;
  auto &Tagged = *State.Tagged;
  const uint16_t Read =
      registerBit(Last->Dst) |
      (Last->Semantics.Source == OperandSourceKind::SourceRegister
           ? registerBit(Last->Src)
           : 0);
  if (!(Read & Tagged.Registers))
    return true;
  ContextState Probe;
  Probe.Registers = State.Registers;
  llvm::erase_if(Tagged.Cases, [&](const auto &Case) {
    for (unsigned R = 0; R < kRegisterCount; ++R)
      if (Tagged.Registers & registerBit(R))
        Probe.Registers[R] =
            RegisterValue{RegisterValue::Kind::Constant, Case.Tag, 0};
    const auto Selected = knownBranchSuccessor(Last, Probe);
    return Selected && *Selected != Successor;
  });
  if (Tagged.Cases.empty())
    return false;
  std::erase_if(State.DefinedCaptures, [&](uint64_t Capture) {
    return std::binary_search(Tagged.ConditionalCaptures.begin(),
                              Tagged.ConditionalCaptures.end(), Capture) &&
           llvm::none_of(Tagged.Cases, [&](const auto &Case) {
             return std::binary_search(Case.DefinedCaptures.begin(),
                                       Case.DefinedCaptures.end(), Capture);
           });
  });
  const auto &First = Tagged.Cases.front();
  const uint64_t Temporary = sizeof(TaggedMemoryCase) +
                             First.Values.retainedByteEstimate() +
                             First.Postconditions.retainedByteEstimate();
  if (auto Error = retain(Temporary))
    return std::move(Error);
  const auto Release = llvm::scope_exit([&] { RetainedBytes -= Temporary; });
  TaggedMemoryCase Facts{First.Tag, First.Values, First.Postconditions, {}};
  ProvenValueSet Tags(
      RegisterValue{RegisterValue::Kind::Constant, Facts.Tag, 0});
  for (const auto &Case : llvm::drop_begin(Tagged.Cases)) {
    Facts.Values.joinValues(Case.Values);
    Facts.Postconditions.meet(Case.Postconditions);
    Tags.join(RegisterValue{RegisterValue::Kind::Constant, Case.Tag, 0});
  }
  State.Scratch.Values.includeCells(Facts.Values,
                                    [](const RegisterValue &) { return true; });
  std::array<ProvenValueSet, kRegisterCount> Identity;
  const auto Inputs = symbolicInputs();
  for (unsigned R = 0; R < kRegisterCount; ++R) {
    Identity[R] = Inputs[R];
    if (Tagged.Registers & registerBit(R)) {
      State.Registers[R] = Tags;
      State.Origins[R] = Tags;
    }
  }
  State.Postconditions.include(Facts.Postconditions, Identity, Program);
  uint16_t LoadedRegisters = 0;
  for (unsigned R = 0; R < kRegisterCount; ++R)
    if (State.Registers[R].singleton().ValueKind ==
        RegisterValue::Kind::Unknown)
      LoadedRegisters |= registerBit(R);
  if (LoadedRegisters) {
    auto Cells = unchangedLoadedCells(Block, LoadedRegisters, State);
    if (!Cells)
      return Cells.takeError();
    for (unsigned R = 0; R < kRegisterCount; ++R) {
      if (!(*Cells)[R])
        continue;
      const auto &Value = State.Scratch.Values.readValues(*(*Cells)[R]);
      if (!Value.unknown() && Value != State.Registers[R]) {
        State.Registers[R] = Value;
        // The old origin joined the unselected payloads as well. Rebuild its
        // literal/capture or frame-relative origin from this selected value.
        State.Origins[R] = {};
      }
    }
  }
  if (Tagged.Cases.size() == 1)
    State.Tagged.reset();
  return true;
}

void ContextFlow::transferValues(const MedInstruction &Instruction,
                                 ContextState &State) {
  const auto OriginAddress = [&](unsigned Register) {
    RegisterState Entry{};
    Entry[0] = State.Origins[Register].singleton();
    return substituteOrigin(
        {RegisterValue::Kind::StackAddress, 1, Instruction.Offset}, Entry,
        Program);
  };
  const bool ScalarLoad = Instruction.Op == Operation::Load;
  const auto LoadedOrigin =
      ScalarLoad ? State.Postconditions.read(OriginAddress(Instruction.Src),
                                             Instruction.Width / kBitsPerByte)
                 : ProvenValueSet{};
  MachineState Scalar = State.scalar();
  if (Instruction.Op == Operation::Store) {
    ProvenValueSet StoredOrigin =
        Instruction.Form == OperandForm::StoreReg
            ? State.Origins[Instruction.Src]
            : ProvenValueSet(RegisterValue{
                  RegisterValue::Kind::Constant,
                  normalizeImmediate(Instruction.Immediate,
                                     Instruction.Semantics.Immediate),
                  0});
    // Only literal actual values can replace lost formal origins. A frame
    // coordinate must remain relative to an entry input during composition.
    if (StoredOrigin.unknown() &&
        std::all_of(State.Registers[Instruction.Src].values().begin(),
                    State.Registers[Instruction.Src].values().end(),
                    [](const auto &Value) {
                      return Value.ValueKind == RegisterValue::Kind::Constant ||
                             Value.ValueKind ==
                                 RegisterValue::Kind::UnsignedRange ||
                             Value.ValueKind ==
                                 RegisterValue::Kind::SymbolicAddress;
                    }))
      StoredOrigin = State.Registers[Instruction.Src];
    if (StoredOrigin.unknown() && Instruction.Form == OperandForm::StoreReg &&
        Instruction.Width == kDoubleWordBitWidth &&
        State.InputExpressions[Instruction.Src])
      State.Postconditions.storeExpression(
          OriginAddress(Instruction.Dst),
          *State.InputExpressions[Instruction.Src]);
    else
      State.Postconditions.store(OriginAddress(Instruction.Dst),
                                 Instruction.Width / kBitsPerByte,
                                 StoredOrigin);
    MedInstruction Store = Instruction;
    if (Store.Form == OperandForm::StoreReg)
      Store.Src = Store.Dst == 0 ? 1 : 0;
    std::optional<ScratchState> OriginOutput;
    for (const auto &Base : possibleValues(State.Registers[Instruction.Dst]))
      for (const auto &Origin : possibleValues(StoredOrigin)) {
        MachineState Variant = Scalar;
        Variant.Scratch = State.OriginScratch;
        Variant.Registers[Store.Dst] = Base;
        if (Store.Form == OperandForm::StoreReg)
          Variant.Registers[Store.Src] = Origin;
        applyTransfer(Store, Variant, Program.ExecutableImage,
                      ScratchLoadPolicy::ProvenValues);
        if (!OriginOutput)
          OriginOutput = std::move(Variant.Scratch);
        else {
          OriginOutput->Memory.meet(Variant.Scratch.Memory);
          OriginOutput->Values.joinValues(Variant.Scratch.Values);
        }
      }
    State.OriginScratch = std::move(*OriginOutput);
    State.OriginScratch.Memory.clear();
  } else if (Instruction.Call != CallKind::None) {
    const bool Mutates = Instruction.Call != CallKind::Syscall ||
                         !Instruction.Syscall ||
                         !preservesCallerMemory(Instruction.Syscall->ID);
    const auto Length = State.Registers[3].singleton();
    const bool Copy =
        Instruction.Syscall && (Instruction.Syscall->ID == Syscall::Memcpy ||
                                Instruction.Syscall->ID == Syscall::Memmove);
    const bool Fill =
        Instruction.Syscall && Instruction.Syscall->ID == Syscall::Memset;
    const bool KnownTransfer =
        (Copy || Fill) && Length.ValueKind == RegisterValue::Kind::Constant &&
        Length.Value <= kMaxModeledScratchBytes;
    const auto DestinationOrigin = State.Origins[1].singleton();
    if (KnownTransfer && Copy)
      State.Postconditions.copyFrom(State.Origins[2].singleton(),
                                    DestinationOrigin, Length.Value, Program);
    else if (Mutates)
      State.Postconditions.clear();
    MachineState OriginMemory = Scalar;
    OriginMemory.Scratch = State.OriginScratch;
    applyTransfer(Instruction, OriginMemory, Program.ExecutableImage,
                  ScratchLoadPolicy::ProvenValues);
    State.OriginScratch = std::move(OriginMemory.Scratch);
    State.OriginScratch.Memory.clear();
    if (KnownTransfer) {
      const auto Destination = memoryAddress(Scalar.Registers[1]);
      State.OriginScratch.Values.visitScalarCells(
          [&](const RegisterValue &Address, unsigned Width,
              const ProvenValueSet &Value) {
            if (!Destination || Address.ValueKind != Destination->ValueKind ||
                (Address.ValueKind == RegisterValue::Kind::SymbolicAddress &&
                 Address.Value != Destination->Value))
              return;
            const uint64_t Delta =
                Address.ValueKind == RegisterValue::Kind::SymbolicAddress
                    ? static_cast<uint64_t>(Address.Offset) -
                          static_cast<uint64_t>(Destination->Offset)
                    : Address.Value - Destination->Value;
            if (Delta >= Length.Value || Width > Length.Value - Delta)
              return;
            RegisterState Entry{};
            Entry[0] = DestinationOrigin;
            State.Postconditions.store(
                substituteOrigin({RegisterValue::Kind::StackAddress, 1,
                                  static_cast<int64_t>(Delta)},
                                 Entry, Program),
                Width, Value);
          });
    }
  }
  if (Instruction.Call != CallKind::None) {
    for (unsigned R = 0; R < callClobberedRegisterCount(Instruction.Call);
         ++R) {
      State.Origins[R] = {};
      State.InputExpressions[R].reset();
    }
  } else if (Instruction.Semantics.WritesDestination) {
    if (Instruction.Op == Operation::Load) {
      State.Origins[Instruction.Dst] = loadOrigin(Instruction, State);
      if (State.Origins[Instruction.Dst].unknown())
        State.Origins[Instruction.Dst] = LoadedOrigin;
    } else {
      State.Origins[Instruction.Dst] =
          registerResult(Instruction, State.Origins, Program.ExecutableImage);
    }
  }
  if (Instruction.Op == Operation::Store) {
    // Both lanes use the canonical store owner over complete alternatives.
    // Alias invalidation and full-width pointer tags are not reimplemented.
    std::optional<ScratchState> Output;
    for (const auto &Base : possibleValues(State.Registers[Instruction.Dst]))
      for (const auto &Value :
           possibleValues(State.Registers[Instruction.Src])) {
        MachineState Variant = Scalar;
        Variant.Registers[Instruction.Dst] = Base;
        if (Instruction.Form == OperandForm::StoreReg)
          Variant.Registers[Instruction.Src] = Value;
        applyTransfer(Instruction, Variant, Program.ExecutableImage,
                      ScratchLoadPolicy::ProvenValues);
        if (!Output)
          Output = std::move(Variant.Scratch);
        else {
          Output->Memory.meet(Variant.Scratch.Memory);
          Output->Values.joinValues(Variant.Scratch.Values);
        }
      }
    State.Scratch = std::move(*Output);
    State.Scratch.Memory.clear();
    return;
  }
  if (Instruction.Call != CallKind::None) {
    applyTransfer(Instruction, Scalar, Program.ExecutableImage,
                  ScratchLoadPolicy::ProvenValues);
    State.Scratch = std::move(Scalar.Scratch);
    State.Scratch.Memory.clear();
    for (unsigned R = 0; R < callClobberedRegisterCount(Instruction.Call); ++R)
      State.Registers[R] = {};
    return;
  }
  if (!Instruction.Semantics.WritesDestination)
    return;
  if (ScalarLoad) {
    State.Registers[Instruction.Dst] = loadTypedValue(
        Instruction, State.Registers[Instruction.Src], State.Scratch.Values);
    if (State.Registers[Instruction.Dst].unknown() &&
        std::all_of(LoadedOrigin.values().begin(), LoadedOrigin.values().end(),
                    [](const auto &Value) {
                      return Value.ValueKind == RegisterValue::Kind::Constant ||
                             Value.ValueKind ==
                                 RegisterValue::Kind::UnsignedRange ||
                             Value.ValueKind ==
                                 RegisterValue::Kind::SymbolicAddress;
                    }))
      State.Registers[Instruction.Dst] = LoadedOrigin;
    // A successful narrow load is zero-extended even when its bytes are
    // unknown. Keep the type bound after the exact memory/origin lookups;
    // it supplies no address identity and cannot replace a stronger fact.
    if (State.Registers[Instruction.Dst].unknown() &&
        Instruction.Semantics.MemoryWidth < kDoubleWordBitWidth)
      State.Registers[Instruction.Dst] = unsignedRangeValue(
          {0, (uint64_t{1} << Instruction.Semantics.MemoryWidth) - 1});
  } else {
    State.Registers[Instruction.Dst] =
        registerResult(Instruction, State.Registers, Program.ExecutableImage,
                       &Scalar.Scratch.Values);
  }
}

void ContextFlow::refineOrigins(const FunctionContext &Context,
                                ContextState &State) {
  const auto Frame = Context.Input.Registers[kFramePointerRegister].singleton();
  for (unsigned R = 0; R < kRegisterCount; ++R) {
    const auto Value = State.Registers[R].singleton();
    if (Value.ValueKind == RegisterValue::Kind::StackAddress &&
        Frame.ValueKind == RegisterValue::Kind::StackAddress &&
        Value.Value == Frame.Value) {
      int64_t Offset;
      if (!llvm::SubOverflow(Value.Offset, Frame.Offset, Offset))
        State.Origins[R] = RegisterValue{RegisterValue::Kind::StackAddress,
                                         kRegisterCount, Offset};
    } else if (State.Origins[R].unknown() &&
               (Value.ValueKind == RegisterValue::Kind::Constant ||
                Value.ValueKind == RegisterValue::Kind::UnsignedRange ||
                Value.ValueKind == RegisterValue::Kind::SymbolicAddress)) {
      State.Origins[R] = Value;
    } else if (State.Registers[R].unknown()) {
      const auto Origin =
          substituteOrigins(State.Origins[R], Context.Input.Registers, Program);
      if (!Origin.unknown())
        State.Registers[R] = Origin;
    }
  }
}

llvm::Expected<std::array<std::optional<RegisterValue>, kRegisterCount>>
ContextFlow::unchangedLoadedCells(const MedBlock &Block, uint16_t Registers,
                                  const ContextState &State) {
  std::array<std::optional<RegisterValue>, kRegisterCount> Cells;
  std::array<unsigned, kRegisterCount> Carriers;
  for (unsigned R = 0; R < kRegisterCount; ++R)
    Carriers[R] = R;
  uint16_t Changed = 0;
  // All carriers share one bounded walk. A store/call ends the memory epoch;
  // a changed load base cannot use its current register value.
  for (size_t Slot = Block.EndSlot - 1; Registers && Slot > Block.StartSlot;) {
    const auto *Instruction = Index.find(--Slot);
    if (!Instruction)
      continue;
    if (auto Error = visit())
      return Error;
    if (Instruction->Call != CallKind::None ||
        Instruction->Op == Operation::Store)
      break;
    if (!Instruction->Semantics.WritesDestination)
      continue;
    for (unsigned R = 0; R < kRegisterCount; ++R) {
      if (!(Registers & registerBit(R)) || Instruction->Dst != Carriers[R])
        continue;
      if (Instruction->Op == Operation::Mov &&
          Instruction->Form == OperandForm::DstSrc &&
          Instruction->Width == kDoubleWordBitWidth) {
        Carriers[R] = Instruction->Src;
      } else {
        if (Instruction->Op == Operation::Load &&
            Instruction->Semantics.MemoryWidth == kDoubleWordBitWidth &&
            Instruction->Src != Instruction->Dst &&
            !(Changed & registerBit(Instruction->Src))) {
          Cells[R] =
              memoryAddress(State.Registers[Instruction->Src].singleton(),
                            Instruction->Offset);
        }
        Registers &= ~registerBit(R);
      }
    }
    Changed |= registerBit(Instruction->Dst);
  }
  return Cells;
}

llvm::Error ContextFlow::refineLoadedEquality(const MedBlock &Block,
                                              size_t Successor,
                                              ContextState &State) {
  const auto *Branch = Index.find(Block.EndSlot - 1);
  if (!Branch || Branch->Width != kDoubleWordBitWidth ||
      (Branch->Op != Operation::Eq && Branch->Op != Operation::Ne) ||
      (Successor == *Branch->BranchTarget) != (Branch->Op == Operation::Eq))
    return llvm::Error::success();
  unsigned Register = Branch->Dst;
  RegisterValue Equal =
      Branch->Semantics.Source == OperandSourceKind::SourceRegister
          ? State.Registers[Branch->Src].singleton()
          : RegisterValue{RegisterValue::Kind::Constant,
                          normalizeImmediate(Branch->Immediate,
                                             Branch->Semantics.Immediate),
                          0};
  if (Equal.ValueKind != RegisterValue::Kind::Constant) {
    if (Branch->Semantics.Source != OperandSourceKind::SourceRegister)
      return llvm::Error::success();
    Equal = State.Registers[Branch->Dst].singleton();
    Register = Branch->Src;
  }
  if (Equal.ValueKind != RegisterValue::Kind::Constant ||
      !State.Registers[Register].unknown())
    return llvm::Error::success();
  auto Cells = unchangedLoadedCells(Block, registerBit(Register), State);
  if (!Cells)
    return Cells.takeError();
  if (const auto &Address = (*Cells)[Register]) {
    // This edge proves the value of an unchanged cell, not a guest write.
    State.Scratch.Values.writeValues(*Address, Equal);
    State.Registers[Register] = Equal;
  }
  return llvm::Error::success();
}

void ContextFlow::transfer(const MedInstruction &Instruction,
                           ContextState &State, MissingRegisters *Missing) {
  transferTaggedMemory(Instruction, State);
  if (State.Nullable && Instruction.Op == Operation::Store) {
    for (const auto &Base : possibleValues(State.Registers[Instruction.Dst])) {
      const auto Address = memoryAddress(Base, Instruction.Offset);
      if (Address)
        State.Nullable->Values.invalidate(*Address,
                                          Instruction.Width / kBitsPerByte);
      else
        State.Nullable->Values.clear();
    }
    RegisterState Entry{};
    Entry[0] = State.Origins[Instruction.Dst].singleton();
    State.Nullable->Postconditions.store(
        substituteOrigin(
            {RegisterValue::Kind::StackAddress, 1, Instruction.Offset}, Entry,
            Program),
        Instruction.Width / kBitsPerByte, {});
  }
  if (State.Nullable && Instruction.Semantics.WritesDestination) {
    const bool Copy =
        Instruction.Op == Operation::Mov &&
        Instruction.Form == OperandForm::DstSrc &&
        Instruction.Width == kDoubleWordBitWidth &&
        (State.Nullable->Registers & registerBit(Instruction.Src));
    State.Nullable->Registers &= ~registerBit(Instruction.Dst);
    if (Copy)
      State.Nullable->Registers |= registerBit(Instruction.Dst);
    if (!State.Nullable->Registers)
      State.Nullable.reset();
  }
  const RegisterValue Base = State.Registers[Instruction.Src].singleton();
  const auto Address = Instruction.Op == Operation::Load
                           ? effectiveAddress(Base, Instruction.Offset)
                           : std::nullopt;
  const CallXMissingFact Source =
      Missing ? (*Missing)[Instruction.Src] : CallXMissingFact{};
  const CallXMissingFact Destination =
      Missing ? (*Missing)[Instruction.Dst] : CallXMissingFact{};
  transferValues(Instruction, State);
  if (!Missing || !Instruction.Semantics.WritesDestination)
    return;
  CallXMissingFact &Why = (*Missing)[Instruction.Dst];
  Why = {};
  if (!State.Registers[Instruction.Dst].unknown())
    return;
  if (Instruction.Op == Operation::Load) {
    if (!Address && Source.Kind != CallXMissingFactKind::None) {
      Why = Source;
    } else {
      Why.Slot = Instruction.Slot;
      Why.Address = Address;
      Why.Coordinate = Base.ValueKind == RegisterValue::Kind::StackAddress
                           ? MemoryCoordinate::FrameRelative
                           : MemoryCoordinate::Absolute;
      Why.Kind =
          Why.Coordinate == MemoryCoordinate::FrameRelative ||
                  (Address && isScratchAddress(*Address))
              ? (Instruction.Semantics.MemoryWidth == kDoubleWordBitWidth
                     ? CallXMissingFactKind::ScratchWord
                     : CallXMissingFactKind::ScratchLoadWidth)
          : Base.ValueKind == RegisterValue::Kind::InstructionDataAddress ||
                  (Address && *Address >= kInputStart &&
                   *Address - kInputStart < kMemoryRegionSize)
              ? CallXMissingFactKind::RuntimeMemory
              : CallXMissingFactKind::UnmappedLoad;
    }
  } else if (Instruction.Op == Operation::Mov &&
             Instruction.Form == OperandForm::DstSrc &&
             Instruction.Width == kDoubleWordBitWidth &&
             Source.Kind != CallXMissingFactKind::None) {
    Why = Source;
  } else if ((Instruction.Op == Operation::Add ||
              Instruction.Op == Operation::Sub) &&
             Instruction.Form == OperandForm::DstImm &&
             Destination.Kind != CallXMissingFactKind::None) {
    Why = Destination;
  } else {
    Why.Kind = CallXMissingFactKind::RegisterWrite;
    Why.Slot = Instruction.Slot;
  }
}

CallXContextEvidence ContextFlow::evidence(const FunctionContext &Context,
                                           const MedInstruction &Instruction,
                                           const ContextState &State,
                                           const MissingRegisters &Missing,
                                           bool BeyondEntry) const {
  CallXContextEvidence Evidence;
  Evidence.SummaryID = Context.ID;
  Evidence.GeneralizedInputs = Context.Generic;
  Evidence.InputGeneralization = Context.Generic
                                     ? CallXInputGeneralization::UnknownEntry
                                     : CallXInputGeneralization::None;
  Evidence.CalleeEntrySlot = Program.Med.Blocks[Context.EntryBlock].StartSlot;
  Evidence.CallXSlot = Instruction.Slot;
  Evidence.BeyondEntryBlock = BeyondEntry;
  Evidence.InputExpression = State.InputExpressions[Instruction.CallRegister];
  classifyTarget(Evidence, State.Registers[Instruction.CallRegister],
                 Missing[Instruction.CallRegister]);
  return Evidence;
}

void ContextFlow::classifyTarget(CallXContextEvidence &Evidence,
                                 const ProvenValueSet &Values,
                                 const CallXMissingFact &Missing) const {
  Evidence.Result = CallXContextResult::UnknownTarget;
  Evidence.TargetAddress.reset();
  Evidence.TargetSlot.reset();
  Evidence.TargetValueAlternatives = {};
  Evidence.Missing = {};
  const RegisterValue Target = Values.singleton();
  if (Target.ValueKind == RegisterValue::Kind::Constant) {
    Evidence.TargetAddress = Target.Value;
    if (const auto Slot = rawTargetSlot(Target.Value)) {
      Evidence.Result = CallXContextResult::ProvenTarget;
      Evidence.TargetSlot = Slot;
      Evidence.TargetIsContinuation =
          Program.Low.Instructions[*Slot].IsContinuation;
    } else {
      Evidence.Result = CallXContextResult::InvalidTarget;
    }
  } else {
    Evidence.Missing = Missing;
    if (Values.values().size() > 1) {
      Evidence.TargetValueAlternatives = Values;
      const bool KnownRawTargets = std::all_of(
          Evidence.TargetValueAlternatives.values().begin(),
          Evidence.TargetValueAlternatives.values().end(),
          [&](const auto &Value) {
            return Value.ValueKind == RegisterValue::Kind::Constant &&
                   rawTargetSlot(Value.Value).has_value();
          });
      if (KnownRawTargets) {
        Evidence.Result = CallXContextResult::ProvenTargetSet;
        Evidence.Missing = {};
      } else {
        Evidence.Missing.Kind = CallXMissingFactKind::MultipleTargetValues;
      }
    } else if (Evidence.Missing.Kind == CallXMissingFactKind::None)
      Evidence.Missing.Kind = CallXMissingFactKind::SymbolicTarget;
  }
}

bool ContextFlow::inputExpressionBound(size_t Root,
                                       const ContextState &Input) const {
  const uint16_t Registers = InputExpressionRegisters[Root];
  for (unsigned R = 0; R < kRegisterCount; ++R)
    if ((Registers & registerBit(R)) && Input.Registers[R].unknown())
      return false;
  return true;
}

uint64_t ContextFlow::inputEvaluationBytes(size_t Root) const {
  return InputExpressions[Root].Depth *
         (sizeof(std::pair<size_t, bool>) + sizeof(CallXInputExpressionValue) +
          sizeof(ProvenValueSet) +
          3 * kCallXContextValueAlternatives * sizeof(RegisterValue));
}

llvm::Expected<ProvenValueSet> ContextFlow::evaluateInputExpression(
    size_t Root, const ContextState &Input, const ScratchState &EntryMemory,
    std::vector<CallXInputExpressionValue> &Proof) {
  const size_t Depth = InputExpressions[Root].Depth;
  std::vector<std::pair<size_t, bool>> Pending;
  Pending.reserve(Depth);
  Pending.emplace_back(Root, false);
  std::vector<ProvenValueSet> Results;
  Results.reserve(InputExpressionPaths[Root]);
  Proof.reserve(Depth);
  while (!Pending.empty()) {
    const auto [ID, Expanded] = Pending.back();
    Pending.pop_back();
    const auto &Node = InputExpressions[ID];
    if (!Expanded && (Node.Input || !Node.Alternatives.empty())) {
      Pending.emplace_back(ID, true);
      if (Node.Input)
        Pending.emplace_back(*Node.Input, false);
      else
        for (auto It = Node.Alternatives.rbegin();
             It != Node.Alternatives.rend(); ++It)
          Pending.emplace_back(*It, false);
      continue;
    }
    if (auto Error = visit())
      return Error;
    CallXInputExpressionValue Witness;
    Witness.Expression = ID;
    ProvenValueSet Values;
    if (Node.Kind == CallXInputExpressionKind::InputRegister) {
      Witness.Inputs = Input.Registers[Node.Register];
      std::optional<ProvenValueSet> Output;
      for (const auto &Value : possibleValues(Witness.Inputs)) {
        RegisterState Actual{};
        Actual[Node.Register] = Value;
        const ProvenValueSet Result(substituteOrigin(
            {RegisterValue::Kind::StackAddress, Node.Register + 1, Node.Offset},
            Actual, Program));
        if (!Output)
          Output = Result;
        else
          Output->join(Result);
      }
      Values = std::move(*Output);
    } else if (Node.Kind == CallXInputExpressionKind::Constant) {
      Values = Node.Constant;
    } else if (Node.Kind == CallXInputExpressionKind::InputAlternatives) {
      const size_t First = Results.size() - Node.Alternatives.size();
      Values = Results[First];
      for (size_t I = First + 1; I < Results.size(); ++I)
        Values.join(Results[I]);
      Results.resize(First);
      Witness.Inputs = Values;
    } else {
      Values = std::move(Results.back());
      Results.pop_back();
      Witness.Inputs = Values;
      const auto &Instruction = *Index.find(Node.InstructionSlot);
      if (Instruction.Op == Operation::Load) {
        if (Node.MemoryGuard) {
          const auto &Effects = InputMemoryGuards[*Node.MemoryGuard];
          const uint64_t Bytes = EntryMemory.Values.retainedByteEstimate();
          if (auto Error = retain(Bytes))
            return Error;
          const auto ReleaseMemory =
              llvm::scope_exit([&] { RetainedBytes -= Bytes; });
          for (size_t I = 0;
               I < EntryMemory.Values.size() + Effects.Writes.size() + 1; ++I)
            if (auto Error = visit())
              return Error;
          ScratchState Memory;
          Memory.Values = EntryMemory.Values;
          Effects.invalidate(Memory, Input.Registers, Program);
          Values = loadTypedValue(Instruction, Values, Memory.Values);
        } else {
          Values = loadTypedValue(Instruction, Values, EntryMemory.Values);
        }
      } else {
        std::optional<ProvenValueSet> Output;
        for (const auto &Value : possibleValues(Values)) {
          RegisterState Actual{};
          Actual[Instruction.Dst] = Value;
          applyRegisterTransfer(Instruction, Actual, Program.ExecutableImage);
          const ProvenValueSet Result(Actual[Instruction.Dst]);
          if (!Output)
            Output = Result;
          else
            Output->join(Result);
        }
        Values = std::move(*Output);
      }
    }
    Witness.Values = Values;
    Proof.push_back(std::move(Witness));
    if (Values.unknown())
      return Values;
    Results.push_back(std::move(Values));
  }
  return std::move(Results.back());
}

llvm::Expected<size_t> ContextFlow::applyStoredExpressions(
    const MemoryPostconditions &Postconditions, const ContextState &Input,
    const ScratchState &EntryMemory, ScratchState &Output) {
  size_t Applied = 0;
  llvm::Error Failure = llvm::Error::success();
  Postconditions.visitValues([&](const RegisterValue &Address,
                                 const ProvenValueSet &,
                                 std::optional<size_t> Expression) {
    if (Failure || !Expression || !inputExpressionBound(*Expression, Input))
      return;
    const auto Destination =
        substituteOrigins(Address, Input.Registers, Program).singleton();
    if (Destination.ValueKind == RegisterValue::Kind::Unknown)
      return;
    const uint64_t Bytes = inputEvaluationBytes(*Expression);
    if (auto Error = retain(Bytes)) {
      Failure = std::move(Error);
      return;
    }
    const auto Release = llvm::scope_exit([&] { RetainedBytes -= Bytes; });
    std::vector<CallXInputExpressionValue> Proof;
    auto Value =
        evaluateInputExpression(*Expression, Input, EntryMemory, Proof);
    if (!Value) {
      Failure = Value.takeError();
      return;
    }
    Applied += applyStoredWord(Output, Destination, *Value, Program);
  });
  if (Failure)
    return std::move(Failure);
  return Applied;
}

llvm::Error ContextFlow::instantiateTargetInputs(FunctionContext &Caller,
                                                 const FunctionContext &Callee,
                                                 const ContextState &Input,
                                                 ContextCall &Edge) {
  if (Callee.Sites.empty())
    return llvm::Error::success();
  const auto &Templates =
      Callee.Generic ? Callee : *GenericContexts.at(Callee.EntryBlock);
  bool CheckedPath = false;
  std::optional<size_t> FirstCall;
  const auto SelectFirstCall = [&]() -> llvm::Expected<std::optional<size_t>> {
    // Local caller facts cannot authorize a callee's conditional site unless
    // they select its path. Follow only known successors, stop at every call,
    // and never turn an unknown branch or a repeated block into reachability.
    uint64_t Bytes = stateBytes(Input);
    if (auto Error = retain(Bytes))
      return std::move(Error);
    const auto Release = llvm::scope_exit([&] { RetainedBytes -= Bytes; });
    ContextState Prefix = Input;
    std::set<size_t> Seen;
    uint64_t SeenBytes = 0;
    size_t BlockID = Callee.EntryBlock;
    while (!Seen.contains(BlockID)) {
      constexpr uint64_t NodeBytes = sizeof(size_t) + 4 * sizeof(void *);
      if (auto Error = retain(NodeBytes))
        return std::move(Error);
      SeenBytes += NodeBytes;
      Bytes += NodeBytes;
      Seen.insert(BlockID);
      const auto &Block = Program.Med.Blocks[BlockID];
      for (size_t Slot = Block.StartSlot; Slot < Block.EndSlot; ++Slot) {
        const auto *Instruction = Index.find(Slot);
        if (!Instruction)
          continue;
        if (auto Error = visit())
          return std::move(Error);
        if (Instruction->Call != CallKind::None)
          return std::optional<size_t>(Slot);
        transferValues(*Instruction, Prefix);
        const uint64_t NewBytes = stateBytes(Prefix) + SeenBytes;
        if (NewBytes > Bytes) {
          if (auto Error = retain(NewBytes - Bytes))
            return std::move(Error);
        } else {
          RetainedBytes -= Bytes - NewBytes;
        }
        Bytes = NewBytes;
      }
      const auto *Last = Index.find(Block.EndSlot - 1);
      const auto Selected = knownBranchSuccessor(Last, Prefix);
      if (!Selected && Last &&
          Last->Semantics.Terminator == TerminatorKind::ConditionalJump)
        break;
      std::optional<size_t> Next;
      for (size_t E = Offsets[BlockID]; E < Offsets[BlockID + 1]; ++E) {
        if (Selected &&
            Program.Med.Blocks[Successors[E]].StartSlot != *Selected)
          continue;
        if (Next)
          return std::optional<size_t>{};
        Next = Successors[E];
      }
      if (!Next)
        break;
      BlockID = *Next;
    }
    return std::optional<size_t>{};
  };
  for (const auto &Template : Templates.Sites) {
    if (!Template.InputExpression || Template.TargetAddress ||
        Template.Result == CallXContextResult::ProvenTargetSet)
      continue;
    const size_t Root = *Template.InputExpression;
    if (!inputExpressionBound(Root, Input))
      continue;
    if (Caller.Generic) {
      if (!CheckedPath) {
        auto Selected = SelectFirstCall();
        if (!Selected)
          return Selected.takeError();
        FirstCall = *Selected;
        CheckedPath = true;
      }
      if (FirstCall != Template.CallXSlot)
        continue;
    }
    const uint64_t TemporaryBytes = inputEvaluationBytes(Root);
    if (auto Error = retain(TemporaryBytes))
      return Error;
    const auto Release =
        llvm::scope_exit([&] { RetainedBytes -= TemporaryBytes; });
    std::vector<CallXInputExpressionValue> Proof;
    auto Evaluated = evaluateInputExpression(Root, Input, Input.Scratch, Proof);
    if (!Evaluated)
      return Evaluated.takeError();
    auto Values = std::move(*Evaluated);
    if (Values.unknown())
      continue;
    CallXContextEvidence Site = Template;
    classifyTarget(Site, Values, {});
    if (Site.Result == CallXContextResult::UnknownTarget)
      continue;
    if (!Callee.Generic) {
      // A concrete summary can establish the same target without publishing
      // its input-expression witness. Keep that independently evaluated
      // witness when it agrees; this changes no return or memory transfer.
      const auto Actual = llvm::find_if(Callee.Sites, [&](const auto &Known) {
        return Known.CallXSlot == Site.CallXSlot && Known.TargetAddress &&
               Known.TargetAddress == Site.TargetAddress;
      });
      if (Actual == Callee.Sites.end())
        continue;
      const auto Expression = Site.InputExpression;
      Site = *Actual;
      Site.InputExpression = Expression;
    }
    Site.InputExpressionValues = std::move(Proof);
    uint64_t Bytes = 2 * sizeof(Site) +
                     Site.TargetValueAlternatives.allocatedBytes() +
                     Site.InputExpressionValues.capacity() *
                         sizeof(CallXInputExpressionValue);
    for (const auto &Value : Site.InputExpressionValues)
      Bytes += Value.Inputs.allocatedBytes() + Value.Values.allocatedBytes();
    if (auto Error = retain(Bytes))
      return Error;
    Caller.RetainedBytes += Bytes;
    Edge.InputExpressionTargets.push_back(std::move(Site));
    ++InstantiatedTargetInputExpressions;
  }
  return llvm::Error::success();
}

llvm::Expected<bool> ContextFlow::call(FunctionContext &Context,
                                       const MedInstruction &Instruction,
                                       ContextState &State, bool Collect) {
  State.Tagged.reset();
  if (Instruction.Call != CallKind::Indirect)
    return callTarget(Context, Instruction, State, Collect);
  const auto Targets = State.Registers[Instruction.CallRegister].values();
  if (Targets.size() < 2 ||
      !std::all_of(Targets.begin(), Targets.end(), [](const auto &Value) {
        return Value.ValueKind == RegisterValue::Kind::Constant;
      }))
    return callTarget(Context, Instruction, State, Collect);

  std::optional<ContextState> Joined;
  uint64_t JoinedBytes = 0;
  const auto Release = llvm::scope_exit([&] { RetainedBytes -= JoinedBytes; });
  for (const auto &Target : Targets) {
    ContextState Alternative = State;
    Alternative.Registers[Instruction.CallRegister] = Target;
    const uint64_t AlternativeBytes = stateBytes(Alternative);
    if (auto Error = retain(AlternativeBytes))
      return std::move(Error);
    const auto ReleaseAlternative =
        llvm::scope_exit([&] { RetainedBytes -= AlternativeBytes; });
    // Apply each output and record its dependency before requesting another
    // specialization: an unreferenced callee cache node may be reclaimed.
    auto Continues = callTarget(Context, Instruction, Alternative, Collect);
    if (!Continues)
      return Continues.takeError();
    if (!*Continues)
      continue;
    if (Joined) {
      if (auto Error = joinStates(*Joined, Alternative))
        return std::move(Error);
    } else {
      Joined = std::move(Alternative);
    }
    const uint64_t Bytes = stateBytes(*Joined);
    if (Bytes > JoinedBytes) {
      if (auto Error = retain(Bytes - JoinedBytes))
        return std::move(Error);
    } else {
      RetainedBytes -= JoinedBytes - Bytes;
    }
    JoinedBytes = Bytes;
  }
  if (!Joined)
    return false;
  State = std::move(*Joined);
  return true;
}

/// Replay a short memory-transfer path using actual caller values and the
/// canonical load/store owner. This creates no cached specialization. Only
/// known branch successors and a matched return establish a postcondition;
/// unknown branches, opaque calls and repeated register states do not.
llvm::Expected<std::optional<ContextFlow::MemoryTransferInstantiation>>
ContextFlow::instantiateMemoryTransferPath(const FunctionContext &Owner,
                                           size_t Entry,
                                           const ContextState &Input) {
  const size_t EntrySlot = Program.Med.Blocks[Entry].StartSlot;
  if (ActiveEntries.contains(EntrySlot) ||
      ActiveEntries.size() >= Program.Config.MaxCallDepth - 1)
    return std::optional<MemoryTransferInstantiation>{};
  if (!maximumCopyLength(Input.Registers[3]))
    return std::optional<MemoryTransferInstantiation>{};
  if (Input.Registers[3].values().size() > 1) {
    std::optional<MemoryTransferInstantiation> Joined;
    uint64_t JoinedBytes = 0;
    const auto InstantiationBytes =
        [](const MemoryTransferInstantiation &Path) {
          return stateBytes(Path.Output) + sizeof(MemoryTransferInstantiation) -
                 sizeof(ContextState) +
                 (Path.Effects.Writes.capacity() +
                  Path.Reads.Writes.capacity()) *
                     sizeof(MemoryWrite);
        };
    const auto ReleaseJoined =
        llvm::scope_exit([&] { RetainedBytes -= JoinedBytes; });
    // Prove all members of the existing finite domain. Source/destination
    // alternatives remain independent: no length/address correlation is
    // guessed.
    for (const auto &Length : Input.Registers[3].values()) {
      const uint64_t AlternativeBytes = stateBytes(Input);
      if (auto Error = retain(AlternativeBytes))
        return std::move(Error);
      const auto ReleaseAlternative =
          llvm::scope_exit([&] { RetainedBytes -= AlternativeBytes; });
      ContextState Alternative = Input;
      Alternative.Registers[3] = Length;
      Alternative.Origins[3] = Length;
      auto Path = instantiateMemoryTransferPath(Owner, Entry, Alternative);
      if (!Path)
        return Path.takeError();
      if (!*Path)
        return std::optional<MemoryTransferInstantiation>{};
      // The completed child is still live while its result is joined. Charge
      // it until its storage is released or transferred to the joined result.
      uint64_t PathBytes = InstantiationBytes(**Path);
      if (auto Error = retain(PathBytes))
        return std::move(Error);
      const auto ReleasePath =
          llvm::scope_exit([&] { RetainedBytes -= PathBytes; });
      if (Joined) {
        // The syscall witness describes one actual copy size and location.
        // Do not invent one when the proven cases execute different syscalls.
        if (Joined->CopySlot != (**Path).CopySlot ||
            Joined->CopySize != (**Path).CopySize)
          return std::optional<MemoryTransferInstantiation>{};
        if (auto Error = joinStates(Joined->Output, (**Path).Output))
          return std::move(Error);
        Joined->Effects.include((**Path).Effects, symbolicInputs(), Program);
        Joined->Reads.include((**Path).Reads, symbolicInputs(), Program);
        Joined->Instructions += (**Path).Instructions;
        Joined->Stores += (**Path).Stores;
      } else {
        Joined = std::move(**Path);
        JoinedBytes = PathBytes;
        PathBytes = 0;
      }
      const uint64_t Bytes = InstantiationBytes(*Joined);
      if (Bytes > JoinedBytes) {
        if (auto Error = retain(Bytes - JoinedBytes))
          return std::move(Error);
      } else {
        RetainedBytes -= JoinedBytes - Bytes;
      }
      JoinedBytes = Bytes;
    }
    return Joined;
  }
  const auto Requested = Input.Registers[3].singleton();
  constexpr uint64_t ResultOverhead = sizeof(MemoryTransferInstantiation) -
                                      sizeof(ContextState) + sizeof(size_t) +
                                      4 * sizeof(void *);
  uint64_t StateBytes = stateBytes(Input) + ResultOverhead;
  if (auto Error = retain(StateBytes))
    return std::move(Error);
  uint64_t SeenBytes = 0;
  const auto Release =
      llvm::scope_exit([&] { RetainedBytes -= StateBytes + SeenBytes; });
  ActiveEntries.insert(EntrySlot);
  const auto Leave = llvm::scope_exit([&] { ActiveEntries.erase(EntrySlot); });
  MemoryTransferInstantiation Result;
  Result.Output = Input;
  auto &State = Result.Output;
  const auto RetainState = [&]() -> llvm::Error {
    const uint64_t NewBytes =
        stateBytes(State) + ResultOverhead +
        (Result.Effects.Writes.capacity() + Result.Reads.Writes.capacity()) *
            sizeof(MemoryWrite);
    if (NewBytes > StateBytes) {
      if (auto Error = retain(NewBytes - StateBytes))
        return Error;
    } else {
      RetainedBytes -= StateBytes - NewBytes;
    }
    StateBytes = NewBytes;
    return llvm::Error::success();
  };
  State.InputExpressions = {};
  pushAbstractFrame(State, Program);
  pushAbstractFrame(State.Origins, Program);
  struct PathState {
    size_t Block;
    std::array<ProvenValueSet, kRegisterCount> Registers;

    bool operator<(const PathState &Other) const {
      if (Block != Other.Block)
        return Block < Other.Block;
      for (unsigned R = 0; R < kRegisterCount; ++R) {
        if (Registers[R] == Other.Registers[R])
          continue;
        return std::lexicographical_compare(
            Registers[R].values().begin(), Registers[R].values().end(),
            Other.Registers[R].values().begin(),
            Other.Registers[R].values().end(),
            [](const RegisterValue &L, const RegisterValue &R) {
              return std::tie(L.ValueKind, L.Value, L.Offset) <
                     std::tie(R.ValueKind, R.Value, R.Offset);
            });
      }
      return false;
    }
  };
  std::set<PathState> Seen;
  size_t BlockID = Entry;
  while (true) {
    uint64_t NodeBytes = sizeof(PathState) + 4 * sizeof(void *);
    for (const auto &Value : State.Registers)
      NodeBytes += Value.allocatedBytes();
    if (auto Error = retain(NodeBytes))
      return std::move(Error);
    SeenBytes += NodeBytes;
    // Ignoring memory here can reject a progressing memory-driven loop, but
    // cannot manufacture a returning path. Counted loops change registers.
    if (!Seen.insert({BlockID, State.Registers}).second)
      return std::optional<MemoryTransferInstantiation>{};
    const auto &Block = Program.Med.Blocks[BlockID];
    const MedInstruction *Last = nullptr;
    for (size_t Slot = Block.StartSlot; Slot < Block.EndSlot; ++Slot) {
      const auto *Instruction = Index.find(Slot);
      if (!Instruction)
        continue;
      if (auto Error = visit())
        return std::move(Error);
      // Every nested state keeps origins in the original caller's frame.
      // Recover a loaded frame pointer using the same owner as normal replay.
      refineOrigins(Owner, State);
      ++Result.Instructions;
      Last = Instruction;
      if (Instruction->Op == Operation::Exit) {
        // An empty path is useful for a zero-length nested copy, but still
        // needs an actual matched return; a zero argument alone proves nothing.
        if (!Result.CopySlot && !Result.Stores && Requested.Value != 0)
          return std::optional<MemoryTransferInstantiation>{};
        for (unsigned R = kFirstCalleeSavedRegister; R < kRegisterCount; ++R) {
          State.Registers[R] = Input.Registers[R];
          State.Origins[R] = Input.Origins[R];
          State.InputExpressions[R] = Input.InputExpressions[R];
        }
        if (auto Error = RetainState())
          return std::move(Error);
        return std::optional<MemoryTransferInstantiation>(std::move(Result));
      }
      if (Instruction->Call == CallKind::Internal) {
        if (!Instruction->CallTarget ||
            hasRegisteredLegacyRuntime(*Instruction))
          return std::optional<MemoryTransferInstantiation>{};
        const auto ChildEntry = ByEntry.find(*Instruction->CallTarget);
        if (ChildEntry == ByEntry.end() ||
            !Index.find(*Instruction->CallTarget))
          return std::optional<MemoryTransferInstantiation>{};
        const auto Generic = GenericContexts.find(ChildEntry->second);
        if (Generic == GenericContexts.end() || !Generic->second->Complete ||
            !Generic->second->HasMemoryTransfer)
          return std::optional<MemoryTransferInstantiation>{};
        auto Child =
            instantiateMemoryTransferPath(Owner, ChildEntry->second, State);
        if (!Child)
          return Child.takeError();
        if (!*Child ||
            ((**Child).CopySlot && (Result.CopySlot || Result.Stores)))
          return std::optional<MemoryTransferInstantiation>{};
        Result.Effects.include((**Child).Effects, symbolicInputs(), Program);
        Result.Reads.include((**Child).Reads, symbolicInputs(), Program);
        Result.Instructions += (**Child).Instructions;
        Result.Stores += (**Child).Stores;
        if ((**Child).CopySlot) {
          Result.CopySlot = (**Child).CopySlot;
          Result.CopySize = (**Child).CopySize;
        }
        State = std::move((**Child).Output);
      } else if (Instruction->Call != CallKind::None) {
        const auto Length = State.Registers[3].singleton();
        if (Result.CopySlot || Result.Stores ||
            Instruction->Call != CallKind::Syscall || !Instruction->Syscall ||
            (Instruction->Syscall->ID != Syscall::Memcpy &&
             Instruction->Syscall->ID != Syscall::Memmove) ||
            Length.ValueKind != RegisterValue::Kind::Constant ||
            Length.Value > kMaxModeledScratchBytes)
          return std::optional<MemoryTransferInstantiation>{};
        const auto Source = State.Origins[2].singleton();
        const auto Destination = State.Origins[1].singleton();
        const uint64_t BeforeBytes =
            sizeof(MemoryEffects) +
            State.InputWrites.Writes.capacity() * sizeof(MemoryWrite);
        if (auto Error = retain(BeforeBytes))
          return std::move(Error);
        const auto ReleaseBefore =
            llvm::scope_exit([&] { RetainedBytes -= BeforeBytes; });
        const auto Before = State.InputWrites;
        for (const auto &Address : possibleValues(State.Origins[1]))
          Result.Effects.addWrite(Address, Length.Value, Program,
                                  CapturedAddresses.get());
        for (const auto &Address : possibleValues(State.Origins[2]))
          if (Address.ValueKind != RegisterValue::Kind::Constant ||
              !Program.ExecutableImage.findRegion(Address.Value, Length.Value,
                                                  true))
            Result.Reads.addWrite(Address, Length.Value, Program,
                                  CapturedAddresses.get());
        State.InputWrites.include(Result.Effects, symbolicInputs(), Program);
        State.InputSpills.clear();
        transferValues(*Instruction, State);
        State.Postconditions.recordInputCopy(Source, Destination, Length.Value,
                                             Before);
        Result.CopySlot = Slot;
        Result.CopySize = Length.Value;
      } else {
        if (Instruction->Op == Operation::Load)
          addMemoryReadEffects(Result.Reads, *Instruction, State);
        if (Instruction->Op == Operation::Store) {
          addMemoryAccessEffects(Result.Effects, *Instruction, State,
                                 Instruction->Dst);
          State.InputWrites.include(Result.Effects, symbolicInputs(), Program);
          State.InputSpills.clear();
          ++Result.Stores;
        }
        transferValues(*Instruction, State);
      }
      if (auto Error = RetainState())
        return std::move(Error);
    }
    const auto Selected = knownBranchSuccessor(Last, State);
    if (!Selected && Last &&
        Last->Semantics.Terminator == TerminatorKind::ConditionalJump)
      return std::optional<MemoryTransferInstantiation>{};
    std::optional<size_t> Next;
    for (size_t E = Offsets[BlockID]; E < Offsets[BlockID + 1]; ++E) {
      const size_t Candidate = Successors[E];
      if (Selected && Program.Med.Blocks[Candidate].StartSlot != *Selected)
        continue;
      if (Next)
        return std::optional<MemoryTransferInstantiation>{};
      Next = Candidate;
    }
    if (!Next)
      return std::optional<MemoryTransferInstantiation>{};
    BlockID = *Next;
  }
}

llvm::Expected<bool> ContextFlow::callTarget(FunctionContext &Context,
                                             const MedInstruction &Instruction,
                                             ContextState &State,
                                             bool Collect) {
  State.Nullable.reset();
  // Resolved runtime calls keep the one canonical memory-effect owner.
  if (Instruction.Call == CallKind::Syscall) {
    Context.HasMemoryTransfer |=
        Instruction.Syscall && (Instruction.Syscall->ID == Syscall::Memcpy ||
                                Instruction.Syscall->ID == Syscall::Memmove);
    const auto BeforeWrites = State.InputWrites;
    const auto CopySource = State.Origins[2].singleton();
    const auto CopyDestination = State.Origins[1].singleton();
    const auto CopyLength = State.Registers[3].singleton();
    if (!Instruction.Syscall) {
      Context.PreservesMemory = false;
      if (Collect) {
        Context.Effects.Unknown = true;
        Context.ReadEffects.Unknown = true;
      }
      if (Context.Generic)
        State.InputWrites.Unknown = true;
      State.InputSpills.clear();
    } else {
      for (const auto &Window : getSyscallMemory(Instruction.Syscall->ID)) {
        const bool Writes = Window.Access == SyscallMemoryAccess::Write;
        auto &Effects = Writes ? Context.Effects : Context.ReadEffects;
        if (Writes) {
          Context.PreservesMemory = false;
          State.InputSpills.clear();
        }
        std::optional<uint64_t> Bytes = Window.fixedBytes();
        if (!Bytes)
          if (const auto Length = Window.lengthArgument()) {
            const auto Value =
                State.Registers[argumentRegister(*Length)].singleton();
            if (Value.ValueKind == RegisterValue::Kind::Constant)
              Bytes = Value.Value;
          }
        if (!Bytes) {
          if (Collect)
            Effects.Unknown = true;
          if (Context.Generic && Writes)
            State.InputWrites.Unknown = true;
          continue;
        }
        const auto &Origins = State.Origins[argumentRegister(Window.Argument)];
        for (const auto &Address : possibleValues(Origins)) {
          if (!Writes && Address.ValueKind == RegisterValue::Kind::Constant &&
              Program.ExecutableImage.findRegion(Address.Value, *Bytes, true))
            continue;
          if (Collect)
            Effects.addWrite(Address, *Bytes, Program, CapturedAddresses.get());
          if (Context.Generic && Writes)
            State.InputWrites.addWrite(Address, *Bytes, Program,
                                       CapturedAddresses.get());
        }
      }
    }
    transfer(Instruction, State, nullptr);
    if (Context.Generic && Instruction.Syscall &&
        (Instruction.Syscall->ID == Syscall::Memcpy ||
         Instruction.Syscall->ID == Syscall::Memmove) &&
        CopyLength.ValueKind == RegisterValue::Kind::Constant)
      State.Postconditions.recordInputCopy(CopySource, CopyDestination,
                                           CopyLength.Value, BeforeWrites);
    return true;
  }
  const auto BeforeWrites = State.InputWrites;
  ContextCall Edge;
  Edge.CallerSummaryID = Context.ID;
  Edge.CallerSlot = Instruction.Slot;
  Edge.CalleeEntrySlot = Instruction.CallTarget;
  if (Instruction.Call == CallKind::Indirect) {
    const RegisterValue Target =
        State.Registers[Instruction.CallRegister].singleton();
    if (Target.ValueKind == RegisterValue::Kind::Constant) {
      Edge.CallXTargetAddress = Target.Value;
      Edge.CalleeEntrySlot = rawTargetSlot(Target.Value);
    }
  }
  Edge.LegacyRuntime = hasRegisteredLegacyRuntime(Instruction);
  if (!Edge.CalleeEntrySlot)
    Edge.Barrier = CallXSummaryBarrier::UnknownTarget;
  else if (!ByEntry.contains(*Edge.CalleeEntrySlot) ||
           !Index.find(*Edge.CalleeEntrySlot))
    Edge.Barrier = CallXSummaryBarrier::UnsupportedRawEntry;
  else if (ActiveEntries.contains(*Edge.CalleeEntrySlot))
    Edge.Barrier = CallXSummaryBarrier::RecursiveCall;
  else if (ActiveEntries.size() >= Program.Config.MaxCallDepth - 1)
    Edge.Barrier = CallXSummaryBarrier::CallDepth;
  if ((Edge.Barrier == CallXSummaryBarrier::RecursiveCall ||
       Edge.Barrier == CallXSummaryBarrier::CallDepth) &&
      !Edge.LegacyRuntime &&
      !MayReturnFromBlock[ByEntry.at(*Edge.CalleeEntrySlot)]) {
    Edge.Barrier = CallXSummaryBarrier::NonReturningCall;
    if (Collect) {
      if (auto Error = retain(sizeof(Edge)))
        return std::move(Error);
      Context.RetainedBytes += sizeof(Edge);
      Context.Calls.push_back(Edge);
    }
    return false;
  }

  FunctionContext *Callee = nullptr;
  if (Edge.Barrier == CallXSummaryBarrier::None) {
    const size_t EntryBlock = ByEntry.at(*Edge.CalleeEntrySlot);
    // A literal CALLX may discover a block that was not a recognized function
    // root. Establish its generic summary before requesting a specialization.
    if (!Context.Generic && !GenericContexts.contains(EntryBlock) &&
        !SpecializationBudgetExhausted) {
      auto Generic = summarize(EntryBlock, ContextState{}, true);
      if (!Generic) {
        if (!SpecializationBudgetExhausted)
          return Generic.takeError();
        llvm::consumeError(Generic.takeError());
      }
    }
    const auto Generic = GenericContexts.find(EntryBlock);
    if (!Context.Generic &&
        (Generic == GenericContexts.end() || !Generic->second->Complete))
      Edge.Barrier = CallXSummaryBarrier::SummaryBudget;
  }
  if (Edge.Barrier == CallXSummaryBarrier::None) {
    ContextState Input = Context.Generic ? ContextState{} : State;
    if (Edge.LegacyRuntime) {
      // Legacy runtime dispatch precedes the function. Its effects cannot be
      // bypassed using the file-local target or the original arguments.
      Input.Scratch.clear();
      for (unsigned R = 0; R < callClobberedRegisterCount(Instruction.Call);
           ++R)
        Input.Registers[R] = {};
    }
    pushAbstractFrame(Input, Program);
    const size_t EntryBlock = ByEntry.at(*Edge.CalleeEntrySlot);
    CallXInputGeneralization Generalization = CallXInputGeneralization::None;
    auto Result = summarize(EntryBlock, Input, Context.Generic, &Generalization,
                            &Edge.ReadProjectedInput);
    if (!Result) {
      if (Context.Generic || !SpecializationBudgetExhausted)
        return Result.takeError();
      llvm::consumeError(Result.takeError());
      Result = GenericContexts.at(EntryBlock);
      Generalization = CallXInputGeneralization::ResourceBudget;
    }
    Callee = *Result;
    if (!Edge.LegacyRuntime)
      Context.HasMemoryTransfer |= Callee->HasMemoryTransfer;
    Edge.CalleeSummaryID = Callee->ID;
    Edge.GeneralizedInputs = Callee->Generic;
    Edge.InputGeneralization = Generalization;
    if (Collect && !Callee->Sites.empty() && !Edge.LegacyRuntime &&
        !SpecializationBudgetExhausted) {
      uint64_t InputBytes = 0;
      if (Context.Generic) {
        InputBytes = sizeof(Input.Registers) +
                     State.Scratch.Values.retainedByteEstimate();
        for (const auto &Value : State.Registers)
          InputBytes += Value.allocatedBytes();
        if (auto Error = retain(InputBytes))
          return std::move(Error);
      }
      const auto ReleaseInput =
          llvm::scope_exit([&] { RetainedBytes -= InputBytes; });
      if (Context.Generic) {
        Input.Registers = State.Registers;
        Input.Scratch.Values = State.Scratch.Values;
        pushAbstractFrame(Input, Program);
      }
      if (auto Error = instantiateTargetInputs(Context, *Callee, Input, Edge)) {
        if (!SpecializationBudgetExhausted)
          return std::move(Error);
        llvm::consumeError(std::move(Error));
      }
    }
  }
  const bool ObservableCalleeReads =
      Callee && needsSummaryInputGuards(*Callee, Edge);

  const auto IncludeCalleeReads = [&] {
    if (!Collect || !ObservableCalleeReads || Edge.LegacyRuntime)
      return;
    auto Origins = State.Origins;
    pushAbstractFrame(Origins, Program);
    RegisterState EntryOrigins{};
    for (unsigned R = 0; R < kRegisterCount; ++R)
      EntryOrigins[R] = Origins[R].singleton();
    Context.ReadEffects.Unknown |= Callee->ReadEffects.Unknown;
    Context.ReadEffects.WritesFrames |= Callee->ReadEffects.WritesFrames;
    for (const auto &Read : Callee->ReadEffects.Writes) {
      const auto Address =
          substituteOrigin(Read.Address, EntryOrigins, Program);
      if (Address.ValueKind == RegisterValue::Kind::Constant &&
          Program.ExecutableImage.findRegion(Address.Value, Read.Size, true))
        continue;
      Context.ReadEffects.addWrite(Address, Read.Size, Program,
                                   CapturedAddresses.get());
    }
  };
  if (Callee && !Callee->Return) {
    IncludeCalleeReads();
    // Keep caller-conditioned sites reached before termination. Its writes
    // cannot affect a returning sibling path of this caller's summary.
    if (Collect) {
      if (auto Error = retain(sizeof(Edge)))
        return std::move(Error);
      Context.RetainedBytes += sizeof(Edge);
      Context.Calls.push_back(Edge);
      ++Callee->IncomingReferences;
    }
    return false;
  }
  const auto CopyLength = maximumCopyLength(State.Registers[3]);
  bool CopiesCapturedValue = false;
  bool CopiesKnownFrameRange = false;
  bool UpdatesKnownDescriptor = false;
  // A specialized loop can also widen its destination to unknown. The
  // generic body identifies the transfer wrapper even when the specialized
  // path uses only inline stores and never reaches its copy syscall.
  if (Callee && (Callee->Generic || Callee->Effects.Unknown) && CopyLength &&
      GenericContexts.at(Callee->EntryBlock)->HasMemoryTransfer) {
    for (unsigned R : {1u, 2u})
      CopiesCapturedValue |= llvm::any_of(
          State.Registers[R].values(), [](const RegisterValue &Value) {
            return Value.ValueKind == RegisterValue::Kind::SymbolicAddress;
          });
    const auto Source = memoryAddress(State.Registers[2].singleton());
    const auto Destination = memoryAddress(State.Registers[1].singleton());
    // A bounded append can read and update a descriptor before returning.
    // Admit only a known holder; replay proves every branch and nested call.
    UpdatesKnownDescriptor =
        Callee->HasInlineStores && Destination &&
        State.Scratch.Values.readValues(*Destination).singleton().ValueKind ==
            RegisterValue::Kind::StackAddress;
    // An inline stack-copy loop can erase unrelated saved pointers when its
    // summary destination widens. Replay the known address/length path even
    // when the copied bytes are unknown. Plain syscall wrappers already have
    // transfer summaries; replaying those adds unrelated scratch snapshots to
    // later specializations and can displace their useful inputs.
    CopiesKnownFrameRange =
        Callee->HasInlineStores && Source && Destination &&
        Source->ValueKind == RegisterValue::Kind::StackAddress &&
        Destination->ValueKind == RegisterValue::Kind::StackAddress &&
        State.Scratch.Values.canModelRange(*Source, *CopyLength) &&
        State.Scratch.Values.canModelRange(*Destination, *CopyLength) &&
        (!State.Scratch.Values.empty() || !State.OriginScratch.Values.empty());
    if (Source && !CopiesCapturedValue)
      State.Scratch.Values.visitCells([&](const RegisterValue &Address,
                                          const ProvenValueSet &Value) {
        if (Address.ValueKind != Source->ValueKind ||
            Address.ValueKind == RegisterValue::Kind::SymbolicAddress)
          return;
        const uint64_t Offset = Address.Value - Source->Value;
        CopiesCapturedValue |=
            Offset < *CopyLength && sizeof(uint64_t) <= *CopyLength - Offset &&
            llvm::any_of(
                Value.values(),
                [](const RegisterValue &Stored) {
                  return Stored.ValueKind ==
                         RegisterValue::Kind::SymbolicAddress;
                });
      });
  }
  if ((CopiesCapturedValue || CopiesKnownFrameRange ||
       UpdatesKnownDescriptor) &&
      !Edge.LegacyRuntime && !SpecializationBudgetExhausted) {
    auto Copy =
        instantiateMemoryTransferPath(Context, Callee->EntryBlock, State);
    if (!Copy)
      return Copy.takeError();
    if (*Copy) {
      Context.PreservesMemory = false;
      Edge.AppliedMemoryEffects = true;
      Edge.InstantiatedCopySlot = (**Copy).CopySlot;
      Edge.InstantiatedCopyBytes = (**Copy).CopySize;
      if ((**Copy).Stores) {
        Edge.InstantiatedMemoryPathInstructions = (**Copy).Instructions;
        Edge.InstantiatedMemoryPathStores = (**Copy).Stores;
      }
      if (Collect) {
        Context.ReadEffects.include((**Copy).Reads, symbolicInputs(), Program);
        Context.Effects.include((**Copy).Effects, symbolicInputs(), Program);
        if (auto Error = retain(sizeof(Edge)))
          return std::move(Error);
        Context.RetainedBytes += sizeof(Edge);
        Context.Calls.push_back(Edge);
        ++Callee->IncomingReferences;
      }
      State = std::move((**Copy).Output);
      if (!Context.Generic)
        State.Postconditions.discardInputCopies();
      return true;
    }
  }
  IncludeCalleeReads();
  Context.PreservesMemory &=
      Callee && Callee->PreservesMemory && !Edge.LegacyRuntime;
  auto OriginEntry = State.Origins;
  pushAbstractFrame(OriginEntry, Program);
  RegisterState ScalarOriginEntry{};
  for (unsigned R = 0; R < kRegisterCount; ++R)
    ScalarOriginEntry[R] = OriginEntry[R].singleton();
  ContextState ValueEntry;
  ValueEntry.Registers = State.Registers;
  pushAbstractFrame(ValueEntry, Program);
  std::optional<int64_t> ReturnFrameDelta = int64_t{0};
  if (Callee && Callee->Return) {
    const auto Formal =
        Callee->Input.Registers[kFramePointerRegister].singleton();
    const auto Actual = ValueEntry.Registers[kFramePointerRegister].singleton();
    if (Formal.ValueKind == RegisterValue::Kind::StackAddress) {
      int64_t Delta;
      if (Actual.ValueKind != RegisterValue::Kind::StackAddress ||
          Actual.Value != Formal.Value ||
          llvm::SubOverflow(Actual.Offset, Formal.Offset, Delta))
        ReturnFrameDelta.reset();
      else
        ReturnFrameDelta = Delta;
    }
  }
  ProvenValueSet ReturnOrigins;
  llvm::Error CaptureError = llvm::Error::success();
  std::set<size_t> ImportedCaptures;
  std::optional<bool> NullableWasNew;
  std::map<uint64_t, uint64_t> NewCaptures;
  uint64_t NewCaptureBytes = 0;
  const auto ReleaseNewCaptures =
      llvm::scope_exit([&] { RetainedBytes -= NewCaptureBytes; });
  const auto BindCapture = [&](RegisterValue Value) {
    if (Edge.LegacyRuntime || !Callee ||
        CaptureOwners[Value.Value] != Callee->ID)
      return Value;
    auto Bound = captureAddress(Context.ID, Instruction.Slot, Value.Value,
                                (*CapturedAddresses)[Value.Value]);
    if (!Bound) {
      CaptureError =
          llvm::joinErrors(std::move(CaptureError), Bound.takeError());
      return RegisterValue{};
    }
    ImportedCaptures.insert(Bound->Value);
    if (Callee->Return && Callee->Return->Nullable &&
        Value.Value == Callee->Return->Nullable->Capture && !NullableWasNew)
      NullableWasNew = !State.definesCapture(Bound->Value);
    if (Callee->TaggedReturn && !State.definesCapture(Bound->Value)) {
      constexpr uint64_t Bytes =
          sizeof(decltype(NewCaptures)::value_type) + 4 * sizeof(void *);
      if (auto Error = retain(Bytes)) {
        CaptureError =
            llvm::joinErrors(std::move(CaptureError), std::move(Error));
        return RegisterValue{};
      }
      NewCaptureBytes += Bytes;
      NewCaptures.emplace(Value.Value, Bound->Value);
    }
    State.defineCapture(Bound->Value);
    Bound->Offset = Value.Offset;
    return *Bound;
  };
  MemoryPostconditions ReturnedPostconditions;
  if (Callee && Callee->Return && !Edge.LegacyRuntime) {
    ReturnedPostconditions = Callee->Return->Postconditions;
    ReturnedPostconditions.transformSymbols(BindCapture);
  }
  if (Callee && Callee->Return && !Edge.LegacyRuntime)
    ReturnOrigins = substituteOrigins(
        mapSymbolicValues(Callee->Return->Origins[0], BindCapture), OriginEntry,
        Program);
  if (CaptureError)
    return std::move(CaptureError);
  std::optional<NullableCapture> ReturnedNullable;
  uint64_t ReturnedNullableBytes = 0;
  const auto ReleaseReturnedNullable =
      llvm::scope_exit([&] { RetainedBytes -= ReturnedNullableBytes; });
  if (Callee && Callee->Return && Callee->Return->Nullable &&
      (Callee->Return->Nullable->Registers & registerBit(0)) &&
      !Edge.LegacyRuntime) {
    const auto &Template = *Callee->Return->Nullable;
    const auto Capture = BindCapture(
        {RegisterValue::Kind::SymbolicAddress, Template.Capture, 0});
    if (CaptureError)
      return std::move(CaptureError);
    MemoryPostconditions Fields = Template.Postconditions;
    Fields.transformSymbols(BindCapture);
    if (CaptureError)
      return std::move(CaptureError);
    ScratchState Values;
    Values.Values.setCoordinates(Coordinates);
    Fields.apply(Values, ValueEntry.Registers, Program, &State.Scratch);
    auto Objects = Template.Values;
    rebaseReturnedFrame(Objects, ReturnFrameDelta);
    if (Callee->Generic &&
        Edge.InputGeneralization !=
            CallXInputGeneralization::EquivalentGenericInputs)
      Objects.discardFrameCoordinates();
    Objects.transformSymbols(BindCapture);
    if (CaptureError)
      return std::move(CaptureError);
    Values.Values.includeCells(Objects, [](const auto &) { return true; });
    ReturnedNullable = NullableCapture{Capture.Value,
                                       registerBit(0),
                                       Template.ConditionalDefinition &&
                                           NullableWasNew.value_or(false),
                                       std::move(Values.Values),
                                       {}};
    ReturnedNullable->Postconditions.include(Fields, OriginEntry, Program);
    ReturnedNullableBytes = ReturnedNullable->retainedByteEstimate() +
                            Fields.retainedByteEstimate() +
                            Objects.retainedByteEstimate();
    if (auto Error = retain(ReturnedNullableBytes)) {
      ReturnedNullableBytes = 0;
      return std::move(Error);
    }
  }
  if (CaptureError)
    return std::move(CaptureError);
  std::optional<TaggedMemory> ReturnedTagged;
  uint64_t ReturnedTaggedBytes = 0;
  const auto ReleaseReturnedTagged =
      llvm::scope_exit([&] { RetainedBytes -= ReturnedTaggedBytes; });
  if (Callee && Callee->TaggedReturn && !Edge.LegacyRuntime) {
    const auto &Template = *Callee->TaggedReturn;
    const auto Address = memoryAddress(
        substituteOrigins(mapSymbolicValues(Template.Address, BindCapture),
                          ValueEntry.Registers, Program)
            .singleton());
    StoredRegisterMemory Owner;
    Owner.setCoordinates(Coordinates);
    if (Address && Owner.canModelRange(*Address, Template.Width)) {
      ReturnedTagged = TaggedMemory{*Address, Template.Width, 0, {}, {}};
      for (const auto &Original : Template.Cases) {
        MemoryPostconditions Fields = Original.Postconditions;
        Fields.transformSymbols(BindCapture);
        ScratchState Values;
        Values.Values.setCoordinates(Coordinates);
        Fields.apply(Values, ValueEntry.Registers, Program, &State.Scratch);
        auto Applied =
            applyStoredExpressions(Fields, ValueEntry, State.Scratch, Values);
        if (!Applied)
          return Applied.takeError();
        auto Objects = Original.Values;
        rebaseReturnedFrame(Objects, ReturnFrameDelta);
        Objects.transformSymbols(BindCapture);
        Values.Values.includeCells(Objects, [](const auto &) { return true; });
        const uint64_t Temporary = Fields.retainedByteEstimate() +
                                   Values.retainedByteEstimate() +
                                   Objects.retainedByteEstimate();
        if (auto Error = retain(Temporary))
          return std::move(Error);
        const auto Release =
            llvm::scope_exit([&] { RetainedBytes -= Temporary; });
        const auto Tag =
            Values.Values.readValues(*Address, Template.Width).singleton();
        if (Tag.ValueKind != RegisterValue::Kind::Constant ||
            Tag.Value != Original.Tag) {
          ReturnedTagged.reset();
          break;
        }
        TaggedMemoryCase Case;
        Case.Tag = Original.Tag;
        Case.Values = std::move(Values.Values);
        // Callee expression IDs were evaluated against this call's entry.
        // Only substituted origins can enter the caller's formal lane.
        Fields.discardExpressions();
        Case.Postconditions.include(Fields, OriginEntry, Program);
        ReturnedTagged->Cases.push_back(std::move(Case));
        const uint64_t Bytes = ReturnedTagged->retainedByteEstimate();
        if (auto Error = retain(Bytes - ReturnedTaggedBytes))
          return std::move(Error);
        ReturnedTaggedBytes = Bytes;
      }
      if (ReturnedTagged)
        Edge.InstantiatedTaggedOutputCases = ReturnedTagged->Cases.size();
    }
  }
  if (CaptureError)
    return std::move(CaptureError);
  std::optional<size_t> ReturnExpression;
  const bool NeedsReturnExpression = LiveCallReturns[Instruction.Slot];
  ProvenValueSet InstantiatedReturn;
  if (Callee && Callee->Generic && Callee->Return && !Edge.LegacyRuntime &&
      NeedsReturnExpression &&
      Callee->Return->InputExpressions[kReturnRegister] &&
      substituteOrigins(Callee->Return->Origins[kReturnRegister],
                        ValueEntry.Registers, Program)
          .unknown()) {
    const size_t Expression =
        *Callee->Return->InputExpressions[kReturnRegister];
    if (inputExpressionBound(Expression, ValueEntry)) {
      // A loaded return describes the callee's entry memory. Consume it before
      // applying writes, including writes that alias its already captured load.
      const uint64_t Bytes = inputEvaluationBytes(Expression);
      if (auto Error = retain(Bytes))
        return std::move(Error);
      const auto Release = llvm::scope_exit([&] { RetainedBytes -= Bytes; });
      std::vector<CallXInputExpressionValue> Proof;
      auto Value =
          evaluateInputExpression(Expression, ValueEntry, State.Scratch, Proof);
      if (!Value)
        return Value.takeError();
      InstantiatedReturn = std::move(*Value);
      Edge.InstantiatedReturnInputExpression = !InstantiatedReturn.unknown();
      if (Collect && Edge.InstantiatedReturnInputExpression)
        ++InstantiatedReturnInputExpressions;
    }
  }
  if (Context.Generic && Callee && Callee->Return && !Edge.LegacyRuntime &&
      NeedsReturnExpression && ReturnOrigins.unknown() &&
      Callee->Return->InputExpressions[kReturnRegister]) {
    // Complete substituted origins already describe this return. Rebuilding
    // their instruction chain repeats work at every solver replay and can
    // consume the fixed budget before unrelated caller proofs are reached.
    auto Expression = composeReturnExpression(
        *Callee->Return->InputExpressions[kReturnRegister], State, OriginEntry);
    if (!Expression)
      return Expression.takeError();
    ReturnExpression = *Expression;
  }
  std::vector<std::pair<RegisterValue, size_t>> ComposedFields;
  uint64_t ComposedBytes = 0;
  const auto ReleaseComposed =
      llvm::scope_exit([&] { RetainedBytes -= ComposedBytes; });
  if (Context.Generic && Callee && Callee->Return && !Edge.LegacyRuntime) {
    ComposedBytes = ReturnedPostconditions.size() *
                    sizeof(decltype(ComposedFields)::value_type);
    if (auto Error = retain(ComposedBytes)) {
      ComposedBytes = 0;
      return std::move(Error);
    }
    ComposedFields.reserve(ReturnedPostconditions.size());
    llvm::Error Failure = llvm::Error::success();
    ReturnedPostconditions.visitValues([&](const RegisterValue &Address,
                                           const ProvenValueSet &Origins,
                                           std::optional<size_t> Expression) {
      if (Failure)
        return;
      if (!Expression) {
        if (!substituteOrigins(Origins, OriginEntry, Program).unknown())
          return;
        auto Root = inputOriginExpression(Origins);
        if (!Root) {
          Failure = Root.takeError();
          return;
        }
        Expression = *Root;
      }
      if (!Expression)
        return;
      auto Bound = composeReturnExpression(*Expression, State, OriginEntry);
      if (!Bound) {
        Failure = Bound.takeError();
        return;
      }
      if (*Bound)
        ComposedFields.emplace_back(Address, **Bound);
    });
    if (Failure)
      return std::move(Failure);
  }
  if (Collect) {
    if (!Callee || Edge.LegacyRuntime) {
      Context.Effects.Unknown = true;
      Context.ReadEffects.Unknown = true;
    } else {
      Context.Effects.include(Callee->Effects, ScalarOriginEntry, Program,
                              CapturedAddresses.get());
    }
  }
  // This is the canonical caller ABI: r0-r5 are clobbered, r6-r10 survive.
  // A converged callee supplies only its return carrier and memory effects.
  ScratchState Preserved;
  ScratchState PreservedOrigins = State.OriginScratch;
  const bool PreserveInput =
      Callee && Callee->PreservesMemory && !Edge.LegacyRuntime;
  if (Context.Generic && !PreserveInput) {
    if (Callee && !Edge.LegacyRuntime)
      State.InputWrites.include(Callee->Effects, ScalarOriginEntry, Program,
                                CapturedAddresses.get());
    else
      State.InputWrites.Unknown = true;
  }
  const bool EquivalentLeaf = Edge.InputGeneralization ==
                              CallXInputGeneralization::EquivalentGenericInputs;
  const bool ApplyEffects =
      Callee &&
      ((Callee->Generic && !EquivalentLeaf) || Edge.ReadProjectedInput) &&
      !Callee->Effects.Unknown && !Edge.LegacyRuntime;
  if (PreserveInput || ApplyEffects)
    Preserved = State.Scratch;
  else if (Callee && !Edge.LegacyRuntime) {
    // These cells were projected out of the callee input. Keep the caller's
    // current facts only where the callee's complete may-writes cannot touch.
    Preserved.Values = State.Scratch.Values;
    Preserved.Values.discardWordCells();
    Callee->Effects.invalidate(Preserved, ValueEntry.Registers, Program);
  }
  if (!PreserveInput && ApplyEffects) {
    Edge.AppliedMemoryEffects = true;
    Edge.PreservedOlderFrames =
        Callee->Effects.invalidate(Preserved, ValueEntry.Registers, Program) &&
        Callee->Effects.WritesFrames;
  }
  if (!PreserveInput) {
    ScratchState Spills;
    Spills.Values = std::move(State.InputSpills);
    if (Callee && !Edge.LegacyRuntime)
      Callee->Effects.invalidate(Spills, ValueEntry.Registers, Program);
    else
      Spills.clear();
    State.InputSpills = std::move(Spills.Values);
    if (Callee && !Edge.LegacyRuntime)
      Callee->Effects.invalidate(PreservedOrigins, ValueEntry.Registers,
                                 Program);
    else
      PreservedOrigins.clear();
  }
  MemoryPostconditions PreservedPostconditions = State.Postconditions;
  ScratchState ReturnScratch;
  if (Callee && Callee->Return) {
    ReturnScratch = PreserveInput || ApplyEffects ? std::move(Preserved)
                    : Edge.ReadProjectedInput     ? ScratchState{}
                                                  : Callee->Return->Scratch;
    if (!PreserveInput && !ApplyEffects) {
      rebaseReturnedFrame(ReturnScratch.Values, ReturnFrameDelta);
      ReturnScratch.Values.transformSymbols(BindCapture);
      ReturnScratch.Values.includeCells(
          Preserved.Values, [](const RegisterValue &) { return true; });
    }
    if (Callee->Generic && !EquivalentLeaf && !PreserveInput && !ApplyEffects) {
      // Generic frame coordinates are a private symbolic anchor. Only
      // explicitly substituted postconditions can restore caller-frame cells.
      ReturnScratch.Memory.clear();
      ReturnScratch.Values.discardFrameCoordinates();
    }
    if (!Edge.LegacyRuntime) {
      PreservedPostconditions.invalidate(Callee->Effects, ScalarOriginEntry,
                                         Program);
      Edge.AppliedStoredValues = ReturnedPostconditions.apply(
          ReturnScratch, ValueEntry.Registers, Program, &State.Scratch);
      auto Stored = applyStoredExpressions(ReturnedPostconditions, ValueEntry,
                                           State.Scratch, ReturnScratch);
      if (!Stored)
        return Stored.takeError();
      Edge.AppliedStoredValues += *Stored;
      if (Collect)
        InstantiatedStoredValueInputExpressions += *Stored;
      // IDs belong to the callee's entry. Consume them against its actual
      // entry snapshot, then keep only explicitly composed caller IDs.
      ReturnedPostconditions.discardExpressions();
      for (const auto &[Address, Expression] : ComposedFields)
        ReturnedPostconditions.storeExpression(Address, Expression);
      // Only generic mode tracks the complete entry-relative write history.
      // A specialized caller can consume this copy against its current memory,
      // but must not later replay it against its own earlier entry snapshot.
      if (!Context.Generic)
        ReturnedPostconditions.discardInputCopies();
      PreservedPostconditions.include(ReturnedPostconditions, OriginEntry,
                                      Program, &BeforeWrites,
                                      CapturedAddresses.get());
    } else {
      PreservedPostconditions.clear();
    }
  } else {
    PreservedPostconditions.clear();
  }
  if (Callee && Callee->Return && !Edge.LegacyRuntime) {
    auto OwnedObjects = Callee->Return->Scratch.Values;
    rebaseReturnedFrame(OwnedObjects, ReturnFrameDelta);
    OwnedObjects.transformSymbols(BindCapture);
    ReturnScratch.Values.includeCells(
        OwnedObjects, [&](const RegisterValue &Address) {
          return Address.ValueKind == RegisterValue::Kind::SymbolicAddress &&
                 ImportedCaptures.contains(Address.Value);
        });
  }
  if (CaptureError)
    return std::move(CaptureError);
  if (Collect) {
    if (auto Error = retain(sizeof(Edge)))
      return std::move(Error);
    Context.RetainedBytes += sizeof(Edge);
    Context.Calls.push_back(Edge);
    if (Edge.CalleeSummaryID)
      ++Callee->IncomingReferences;
  }
  transfer(Instruction, State, nullptr);
  if (Callee) {
    State.Registers[0] =
        translateFrameValues(Callee->Return->Registers[0], ReturnFrameDelta);
    State.Scratch = std::move(ReturnScratch);
    State.Scratch.Memory.clear();
    if (Callee->Generic && !EquivalentLeaf) {
      const auto Instantiated =
          Edge.LegacyRuntime ? ProvenValueSet{}
                             : substituteOrigins(Callee->Return->Origins[0],
                                                 ValueEntry.Registers, Program);
      if (!Instantiated.unknown())
        State.Registers[0] = Instantiated;
      else
        for (const auto &Value : State.Registers[0].values())
          if (Value.ValueKind == RegisterValue::Kind::StackAddress) {
            State.Registers[0] = {};
            break;
          }
    }
    if (!InstantiatedReturn.unknown())
      State.Registers[kReturnRegister] = std::move(InstantiatedReturn);
    State.Registers[0] = mapSymbolicValues(State.Registers[0], BindCapture);
    std::optional<ProvenValueSet> CapturedReturn;
    std::optional<uint64_t> NewCapture;
    for (auto Value : State.Registers[0].values()) {
      if (NeedsReturnExpression &&
          Value.ValueKind == RegisterValue::Kind::UnsignedRange &&
          Value.Value >= kHeapStart &&
          Value.Value < kHeapStart + kMemoryRegionSize) {
        auto Captured = captureAddress(Context.ID, Instruction.Slot, UINT64_MAX,
                                       *unsignedValueRange(Value));
        if (!Captured)
          return Captured.takeError();
        Value = *Captured;
        const auto Bounds = (*CapturedAddresses)[Value.Value];
        if (!State.definesCapture(Value.Value) && Bounds.Lower != 0 &&
            Bounds.Lower <= Bounds.Upper)
          NewCapture = Value.Value;
        State.defineCapture(Value.Value);
      }
      if (!CapturedReturn)
        CapturedReturn = Value;
      else
        CapturedReturn->join(Value);
    }
    if (CapturedReturn && !Edge.LegacyRuntime)
      State.Registers[0] = *CapturedReturn;
    const auto IsNullableReturn = [&](uint64_t Capture) {
      return State.Registers[0].values().size() == 2 &&
             llvm::all_of(State.Registers[0].values(), [&](const auto &Value) {
               return (Value.ValueKind == RegisterValue::Kind::Constant &&
                       Value.Value == 0) ||
                      (Value.ValueKind ==
                           RegisterValue::Kind::SymbolicAddress &&
                       Value.Value == Capture && Value.Offset == 0);
             });
    };
    if (NewCapture && !Edge.LegacyRuntime && IsNullableReturn(*NewCapture))
      State.Nullable =
          NullableCapture{*NewCapture, registerBit(0), true, {}, {}};
    if (ReturnedNullable && IsNullableReturn(ReturnedNullable->Capture))
      State.Nullable = std::move(ReturnedNullable);
    if (CaptureError)
      return std::move(CaptureError);
    if (ReturnedTagged) {
      // Binding other return lanes can introduce additional escaping names.
      // Complete the per-case history after all bindings, without creating
      // names for private objects that never escaped the callee.
      for (const auto &[Source, Bound] : NewCaptures) {
        ReturnedTagged->ConditionalCaptures.push_back(Bound);
        for (size_t I = 0; I < ReturnedTagged->Cases.size(); ++I) {
          const auto &Definitions =
              Callee->TaggedReturn->Cases[I].DefinedCaptures;
          if (std::binary_search(Definitions.begin(), Definitions.end(),
                                 Source))
            ReturnedTagged->Cases[I].DefinedCaptures.push_back(Bound);
        }
      }
      llvm::sort(ReturnedTagged->ConditionalCaptures);
      for (auto &Case : ReturnedTagged->Cases)
        llvm::sort(Case.DefinedCaptures);
      const uint64_t Bytes = ReturnedTagged->retainedByteEstimate();
      if (auto Error = retain(Bytes - ReturnedTaggedBytes))
        return std::move(Error);
      ReturnedTaggedBytes = Bytes;
    }
    if (!State.Registers[0].unknown() &&
        llvm::all_of(State.Registers[0].values(),
                     [](const RegisterValue &Value) {
                       return Value.ValueKind ==
                                  RegisterValue::Kind::Constant ||
                              Value.ValueKind ==
                                  RegisterValue::Kind::SymbolicAddress;
                     }) &&
        llvm::any_of(
            State.Registers[0].values(), [](const RegisterValue &Value) {
              return Value.ValueKind == RegisterValue::Kind::SymbolicAddress;
            }))
      ReturnOrigins = State.Registers[0];
    State.Origins[0] = ReturnOrigins;
    State.OriginScratch = std::move(PreservedOrigins);
    State.OriginScratch.Memory.clear();
    State.Postconditions = std::move(PreservedPostconditions);
    State.Tagged = std::move(ReturnedTagged);
  }
  if (Context.Generic) {
    for (unsigned R = 0; R < callClobberedRegisterCount(Instruction.Call); ++R)
      State.InputExpressions[R].reset();
    if (NeedsReturnExpression && !ReturnExpression && !Edge.LegacyRuntime) {
      auto Expression = inputOriginExpression(State.Origins[kReturnRegister]);
      if (!Expression)
        return Expression.takeError();
      ReturnExpression = *Expression;
    }
    State.InputExpressions[kReturnRegister] = ReturnExpression;
  }
  return true;
}

llvm::Expected<bool> ContextFlow::replay(FunctionContext &Context,
                                         size_t BlockID, ContextState &State,
                                         uint16_t &Defined, bool Collect) {
  const MedBlock &Block = Program.Med.Blocks[BlockID];
  struct ShiftedOrigin {
    size_t Slot;
    unsigned Register;
    uint64_t Amount;
    RegisterValue Origin;
  };
  // An adjacent inverse shift pair in this block can preserve an origin when
  // the left shift loses no bits. Keep only this local transfer witness; an
  // alternate block entry or intervening instruction cannot inherit it.
  std::optional<ShiftedOrigin> PreviousShift;
  MissingRegisters Missing;
  if (Collect)
    for (unsigned R = 0; R < kRegisterCount; ++R)
      if (State.Registers[R].unknown()) {
        Missing[R].Kind = BlockID == Context.EntryBlock
                              ? CallXMissingFactKind::IncomingRegister
                              : CallXMissingFactKind::BlockEntry;
        Missing[R].InputRegister = R;
        if (BlockID != Context.EntryBlock)
          Missing[R].Slot = Block.StartSlot;
      }
  for (size_t Slot = Block.StartSlot; Slot < Block.EndSlot; ++Slot) {
    const MedInstruction *Instruction = Index.find(Slot);
    if (!Instruction)
      continue;
    if (auto Error = visit())
      return std::move(Error);
    refineOrigins(Context, State);
    const auto Shift = std::exchange(PreviousShift, std::nullopt);
    std::optional<RegisterValue> RestoredOrigin;
    if (Instruction->Width == kDoubleWordBitWidth &&
        Instruction->Form == OperandForm::DstImm &&
        (Instruction->Op == Operation::LSh ||
         Instruction->Op == Operation::RSh)) {
      const uint64_t Amount = normalizeImmediate(
          Instruction->Immediate, Instruction->Semantics.Immediate);
      if (Instruction->Op == Operation::RSh && Shift &&
          Shift->Slot + 1 == Slot && Shift->Register == Instruction->Dst &&
          Shift->Amount == Amount)
        RestoredOrigin = Shift->Origin;
      if (Instruction->Op == Operation::LSh) {
        const auto Origin = State.Origins[Instruction->Dst].singleton();
        const auto &Values = State.Registers[Instruction->Dst];
        // The owning instruction validator bounds immediate shifts to 0..63.
        if (hasValueIdentity(Origin) && !Values.unknown() &&
            llvm::all_of(Values.values(), [&](const RegisterValue &Value) {
              const auto Bounds =
                  unsignedValueRange(Value, CapturedAddresses.get());
              return Bounds && Bounds->Upper <= (UINT64_MAX >> Amount);
            }))
          PreviousShift = ShiftedOrigin{Slot, Instruction->Dst, Amount, Origin};
      }
    }
    if (Collect && Instruction->Op == Operation::CallX &&
        Instruction->Call == CallKind::Indirect) {
      auto Site = evidence(Context, *Instruction, State, Missing,
                           BlockID != Context.EntryBlock);
      if (Context.Generic && !Site.InputExpression &&
          Site.Result == CallXContextResult::UnknownTarget) {
        auto Expression =
            inputOriginExpression(State.Origins[Instruction->CallRegister]);
        if (!Expression)
          return Expression.takeError();
        Site.InputExpression = *Expression;
      }
      const uint64_t Bytes =
          sizeof(Site) + Site.TargetValueAlternatives.allocatedBytes();
      if (auto Error = retain(Bytes))
        return std::move(Error);
      Context.RetainedBytes += Bytes;
      Context.Sites.push_back(std::move(Site));
    }
    if (Instruction->Call != CallKind::None) {
      const size_t FirstEdge = Context.Calls.size();
      auto Continues = call(Context, *Instruction, State, Collect);
      if (!Continues)
        return Continues.takeError();
      CallInputs Callee;
      uint16_t ObservableRegisters = AllRegisters;
      if (Collect) {
        Callee = {0, false};
        ObservableRegisters = 0;
        for (size_t I = FirstEdge; I < Context.Calls.size(); ++I) {
          const auto &Edge = Context.Calls[I];
          if (Edge.Barrier != CallXSummaryBarrier::None || Edge.LegacyRuntime) {
            Callee = {};
            ObservableRegisters = AllRegisters;
            break;
          }
          const auto &Summary = *Contexts[*Edge.CalleeSummaryID];
          Callee.Registers |= usedCalleeInputs(
              *Instruction, Summary.EntryBlock,
              Summary.Generic ? InputRegisters[Summary.EntryBlock]
                              : Summary.ReuseInputRegisters);
          Callee.Memory |= Summary.Generic ? ReadsMemory[Summary.EntryBlock]
                                           : Summary.ReuseReadsMemory;
          if (needsSummaryInputGuards(Summary, Edge))
            ObservableRegisters |= usedCalleeInputs(
                *Instruction, Summary.EntryBlock,
                Summary.Generic ? InputRegisters[Summary.EntryBlock]
                                : Summary.ObservableInputRegisters);
        }
      }
      const auto Dependencies = instructionDependencies(*Instruction, Callee);
      if (Collect) {
        Context.ReuseInputRegisters |= Dependencies.Reads & ~Defined;
        Context.ObservableInputRegisters |=
            instructionDependencies(*Instruction, {ObservableRegisters, false})
                .Reads &
            ~Defined;
        Context.ReuseReadsMemory |= Dependencies.Memory;
      }
      Defined |= Dependencies.Writes;
      if (!*Continues)
        return false;
      if (Collect)
        for (unsigned R = 0; R < callClobberedRegisterCount(Instruction->Call);
             ++R) {
          Missing[R] = {};
          if (State.Registers[R].unknown()) {
            Missing[R].Kind = CallXMissingFactKind::FunctionReturn;
            Missing[R].Slot = Slot;
            Missing[R].InputRegister = R;
          }
        }
    } else {
      const auto Dependencies = instructionDependencies(*Instruction);
      if (Collect) {
        Context.ReuseInputRegisters |= Dependencies.Reads & ~Defined;
        Context.ObservableInputRegisters |= Dependencies.Reads & ~Defined;
        Context.ReuseReadsMemory |= Dependencies.Memory;
      }
      Defined |= Dependencies.Writes;
      if (Instruction->Op == Operation::Store) {
        Context.HasInlineStores = true;
        Context.PreservesMemory = false;
      }
      if (Collect && Instruction->Op == Operation::Store)
        addMemoryAccessEffects(Context.Effects, *Instruction, State,
                               Instruction->Dst);
      if (Collect && Instruction->Op == Operation::Load) {
        addMemoryReadEffects(Context.ReadEffects, *Instruction, State);
      }
      if (StoredReturnValues[Slot]) {
        const unsigned Source = Instruction->Src;
        const auto Value = State.Registers[Source].singleton();
        if (Value.ValueKind == RegisterValue::Kind::UnsignedRange &&
            Value.Value >= kHeapStart &&
            Value.Value < kHeapStart + kMemoryRegionSize) {
          // Name the stored value after predecessor ranges have joined. The
          // store and later return then share one identity, even when several
          // bounds checks or a covered literal reach this instruction.
          auto Capture = captureAddress(Context.ID, Slot, Source,
                                        *unsignedValueRange(Value));
          if (!Capture)
            return Capture.takeError();
          const auto Origin = State.Origins[Source].singleton();
          for (unsigned R = 0; R < kRegisterCount; ++R)
            if (R == Source || (hasValueIdentity(Origin) &&
                                State.Origins[R].singleton() == Origin)) {
              State.Registers[R] = *Capture;
              State.Origins[R] = *Capture;
            }
          State.defineCapture(Capture->Value);
        }
      }
      if (Context.Generic)
        if (auto Error = transferInputExpression(*Instruction, State))
          return std::move(Error);
      transfer(*Instruction, State, Collect ? &Missing : nullptr);
      if (RestoredOrigin)
        State.Origins[Instruction->Dst] = *RestoredOrigin;
    }
  }
  return true;
}

llvm::Expected<bool>
ContextFlow::memoryInputsMatch(const FunctionContext &Candidate,
                               const ContextState &Input,
                               bool &CoveringInputs) {
  // A summary computed with fewer known cells covers an input that keeps
  // every old fact and only adds facts, even when its read range is unknown.
  // Changed or forgotten old cells still need the read-footprint proof below.
  bool Extends = true;
  Candidate.Input.Scratch.Values.visitCells(
      [&](const RegisterValue &Address, const ProvenValueSet &Value) {
        if (Extends && Input.Scratch.Values.readValues(Address) != Value)
          Extends = false;
      });
  if (Extends) {
    CoveringInputs = true;
    return true;
  }
  if (Candidate.ReadEffects.Unknown)
    return false;
  const auto &Before = Candidate.Input.Scratch;
  const auto &After = Input.Scratch;
  const uint64_t Bytes = 2 * sizeof(ScratchState) +
                         Before.retainedByteEstimate() +
                         After.retainedByteEstimate();
  if (auto Error = retain(Bytes))
    return std::move(Error);
  const auto Release = llvm::scope_exit([&] { RetainedBytes -= Bytes; });
  ScratchState UnreadBefore = Before;
  ScratchState UnreadAfter = After;
  Candidate.ReadEffects.invalidate(UnreadBefore, Candidate.Input.Registers,
                                   Program);
  Candidate.ReadEffects.invalidate(UnreadAfter, Input.Registers, Program);
  bool Same = true;
  const auto Check = [&](const ScratchState &Original,
                         const ScratchState &Unread,
                         const ScratchState &Other) {
    Original.Values.visitCells(
        [&](const RegisterValue &Address, const ProvenValueSet &Value) {
          if (Unread.Values.readValues(Address).unknown() &&
              Other.Values.readValues(Address) != Value)
            Same = false;
        });
  };
  Check(Before, UnreadBefore, After);
  Check(After, UnreadAfter, Before);
  return Same;
}

llvm::Expected<FunctionContext *>
ContextFlow::summarize(size_t Entry, const ContextState &Incoming, bool Generic,
                       CallXInputGeneralization *Generalization,
                       bool *ReadProjectedInput) {
  if (ReadProjectedInput)
    *ReadProjectedInput = false;
  const auto Generalized = [&](CallXInputGeneralization Reason) {
    if (Generalization)
      *Generalization = Reason;
    return GenericContexts.at(Entry);
  };
  if (Generalization)
    *Generalization = Generic ? CallXInputGeneralization::UnknownCaller
                              : CallXInputGeneralization::None;
  ContextState Input = Generic ? genericInput() : Incoming;
  // Narrow output cells propagate within the caller, but arbitrary incoming
  // byte snapshots must not displace its four full-word specializations.
  // Project them to unknown at the entry boundary until narrow read
  // dependencies can be represented. Caller memory is restored only after
  // may-write checks.
  Input.Scratch.Memory.clear();
  Input.Scratch.Values.discardNarrowCells();
  const auto Origins = symbolicInputs();
  for (unsigned R = 0; R < kRegisterCount; ++R)
    Input.Origins[R] = Origins[R];
  Input.OriginScratch.clear();
  Input.Postconditions.clear();
  Input.Tagged.reset();
  // Incoming names belong to callers, not to the invocation being summarized.
  // Their fields still use the ordinary must-fact join within this summary.
  Input.DefinedCaptures = std::vector<uint64_t>{};
  Input.Nullable.reset();
  Input.InputExpressions = {};
  Input.InputSpills.clear();
  Input.InputWrites = {};
  Input.Scratch.Values.setCoordinates(Coordinates);
  Input.OriginScratch.Values.setCoordinates(Coordinates);
  Input.InputSpills.setCoordinates(Coordinates);
  if (Generic)
    for (unsigned R = 0; R < kRegisterCount; ++R) {
      CallXInputExpression Expression;
      Expression.Register = R;
      auto ID = inputExpression(Expression);
      if (!ID)
        return ID.takeError();
      Input.InputExpressions[R] = *ID;
    }
  for (unsigned R = 0; R < kRegisterCount; ++R)
    if (!(InputRegisters[Entry] & (uint16_t{1} << R))) {
      ProjectedRegisters += !Generic && !Input.Registers[R].unknown();
      Input.Registers[R] = {};
    }
  if (!Generic) {
    if (!ReadsMemory[Entry] && GenericContexts.contains(Entry) &&
        GenericContexts.at(Entry)->PreservesMemory) {
      ProjectedScratchSnapshots += !Input.Scratch.empty();
      Input.Scratch.clear();
    }
    const auto Frame = Input.Registers[kFramePointerRegister].singleton();
    if (Frame.ValueKind == RegisterValue::Kind::StackAddress &&
        Frame.Offset != 0 && Frame.Offset != INT64_MIN &&
        translateFrameInput(Input, -Frame.Offset))
      ++CanonicalizedFrameInputs;
    const auto It = GenericContexts.find(Entry);
    // Generic mode drops locally known inputs at function calls. Equal entry
    // facts therefore authorize direct reuse only for a function-call-free
    // body; its scalar/syscall transfers are identical in both modes.
    if (It != GenericContexts.end() && It->second->Complete &&
        It->second->Calls.empty() &&
        Input.Registers == It->second->Input.Registers &&
        Input.Scratch == It->second->Input.Scratch) {
      ++EquivalentGenericReuses;
      ++CacheHits;
      return Generalized(CallXInputGeneralization::EquivalentGenericInputs);
    }
  }
  llvm::FoldingSetNodeID Key;
  profileInput(Key, Entry, Generic, Input);
  void *InsertPosition = nullptr;
  auto *Found = Cache.FindNodeOrInsertPos(Key, InsertPosition);
  if (Found && Found->Complete && !Generic)
    if (auto Retry = unblockedRecursiveCall(*Found)) {
      auto Released = generalizeRedundantContext(Entry, Input, Retry);
      if (!Released)
        return Released.takeError();
      if (*Released)
        Found = Cache.FindNodeOrInsertPos(Key, InsertPosition);
    }
  if (Found) {
    if (!Found->Complete) {
      if (!Generic && SpecializationBudgetExhausted)
        return Generalized(CallXInputGeneralization::ResourceBudget);
      return llvm::make_error<llvm::StringError>(
          kCallXContextMemoryBudgetDiagnostic, llvm::inconvertibleErrorCode());
    }
    ++CacheHits;
    return Found;
  }
  // Only real caller edges use this conditional projection. Standalone roots
  // retain their own entry assumptions, and generic mode still drops locally
  // known inputs at nested calls, so it is not interchangeable with this mode.
  if (!Generic && Generalization && Specializations.contains(Entry)) {
    const bool AtCapacity = Specializations.at(Entry).size() ==
                            kCallXContextSpecializationsPerEntry;
    for (auto *Candidate : Specializations.at(Entry)) {
      if (!Candidate->Complete)
        continue;
      bool Same = true;
      bool CoveringInputs = false;
      bool RelaxedInputGuards = false;
      for (unsigned R = 0; R < kRegisterCount; ++R)
        if ((Candidate->ReuseInputRegisters & registerBit(R)) &&
            Candidate->Input.Registers[R] != Input.Registers[R]) {
          if (AtCapacity &&
              !(Candidate->ObservableInputRegisters & registerBit(R))) {
            RelaxedInputGuards = true;
            continue;
          }
          const auto Value = Input.Registers[R].singleton();
          if (!AtCapacity ||
              Value.ValueKind != RegisterValue::Kind::UnsignedRange) {
            Same = false;
            break;
          }
          const auto &Before = Candidate->Input.Registers[R];
          const auto Bounds =
              Before.unknown()
                  ? std::optional<UnsignedValueRange>(UnsignedValueRange{})
                  : unsignedValueRange(Before.singleton());
          const auto Narrow = *unsignedValueRange(Value);
          // A completed summary for a wider numeric input remains sound for
          // this interval. Keep pointer identities and nonnumeric inputs
          // exact; wrapping ranges need a different containment proof.
          if (!Bounds || Bounds->Lower > Bounds->Upper ||
              Narrow.Lower > Narrow.Upper || Bounds->Lower > Narrow.Lower ||
              Bounds->Upper < Narrow.Upper) {
            Same = false;
            break;
          }
          CoveringInputs = true;
        }
      if (!Same)
        continue;
      const bool DifferentMemory =
          (Candidate->ReuseReadsMemory || !Candidate->PreservesMemory) &&
          Candidate->Input.Scratch != Input.Scratch;
      if (DifferentMemory || CoveringInputs || RelaxedInputGuards) {
        // Keep exact scratch variants while space remains. The projected
        // output uses may-write effects, which can be less precise than a
        // fresh execution even when the changed input fields are unread.
        if (!AtCapacity || (DifferentMemory && !ReadProjectedInput))
          continue;
        const auto &Generic = *GenericContexts.at(Entry);
        const bool RefinesReturn =
            Candidate->Return &&
            !Candidate->Return->Registers[kReturnRegister].unknown() &&
            (!Generic.Return || Candidate->Return->Registers[kReturnRegister] !=
                                    Generic.Return->Registers[kReturnRegister]);
        const bool AddsFields =
            Candidate->Return &&
            (!Generic.Return || Candidate->Return->Postconditions.size() >
                                    Generic.Return->Postconditions.size());
        // Projected effects can preserve extra unrelated caller snapshots.
        // Spend that precision only for a stronger scalar or field result.
        if (!RefinesReturn && !AddsFields)
          continue;
        if (DifferentMemory) {
          auto Matches = memoryInputsMatch(*Candidate, Input, CoveringInputs);
          if (!Matches)
            return Matches.takeError();
          if (!*Matches)
            continue;
          *ReadProjectedInput = true;
        }
      }
      ++CacheHits;
      if (CoveringInputs) {
        ++CoveringSpecializedReuses;
        *Generalization = CallXInputGeneralization::CoveringSpecializedInputs;
      } else {
        ++EquivalentSpecializedReuses;
        *Generalization = CallXInputGeneralization::EquivalentSpecializedInputs;
      }
      return Candidate;
    }
  }
  if (!Generic && SpecializationBudgetExhausted)
    return Generalized(CallXInputGeneralization::ResourceBudget);
  if (!Generic && Specializations.contains(Entry) &&
      Specializations.at(Entry).size() ==
          kCallXContextSpecializationsPerEntry) {
    // A loop widening can supersede its earlier precise input. Previous
    // roots may also leave unused variants or orphan parents. Preserve
    // unrelated alternatives within this root; reclaim older graphs only
    // when a new specialization actually needs their space.
    const auto FindVictim = [&]() -> FunctionContext * {
      if (ActiveEntries.contains(Program.Med.Blocks[Entry].StartSlot))
        return nullptr;
      for (auto *Candidate : Specializations.at(Entry)) {
        if (!Candidate->isUnreferencedSolverNode())
          continue;
        if (Candidate->RootGeneration < RootGeneration)
          return Candidate;
        ContextState Widened = Candidate->Input;
        Widened.join(Input, Program, Coordinates);
        if (Widened.Registers == Input.Registers &&
            Widened.Scratch == Input.Scratch)
          return Candidate;
      }
      return nullptr;
    };
    auto Victim = FindVictim();
    if (!Victim) {
      reclaimOrphanedContexts(Entry);
      Victim = FindVictim();
    }
    if (Specializations.at(Entry).size() ==
        kCallXContextSpecializationsPerEntry) {
      if (Victim)
        reclaimContext(*Victim);
      else {
        auto Reclaimed = generalizeRedundantContext(Entry, Input);
        if (!Reclaimed)
          return Reclaimed.takeError();
        if (!*Reclaimed)
          return Generalized(CallXInputGeneralization::SpecializationLimit);
      }
    }
    // Removing a FoldingSet node invalidates the earlier insertion position.
    Cache.FindNodeOrInsertPos(Key, InsertPosition);
  }
  const uint64_t ContextBytes = sizeof(FunctionContext) + stateBytes(Input);
  // Stable IDs retain vector slots and reclamation/graph traversal metadata
  // even after an unreferenced solver-only node has been reclaimed.
  constexpr uint64_t StableIDBytes = 3 * sizeof(size_t);
  const uint64_t VariantIndexBytes =
      !Generic && !Specializations.contains(Entry)
          ? sizeof(decltype(Specializations)::value_type) + 4 * sizeof(void *)
          : 0;
  if (auto Error = retain(ContextBytes + StableIDBytes + VariantIndexBytes))
    return std::move(Error);
  auto Owned =
      std::make_unique<FunctionContext>(Contexts.size(), Entry, Input, Generic);
  Owned->RootGeneration = RootGeneration;
  FunctionContext &Context = *Owned;
  Context.RetainedBytes = ContextBytes;
  Cache.InsertNode(&Context, InsertPosition);
  Contexts.push_back(std::move(Owned));
  if (Generic)
    GenericContexts.emplace(Entry, &Context);
  else
    Specializations[Entry].push_back(&Context);
  const size_t EntrySlot = Program.Med.Blocks[Entry].StartSlot;
  ActiveEntries.insert(EntrySlot);
  const auto Leave = llvm::scope_exit([&] { ActiveEntries.erase(EntrySlot); });

  std::map<size_t, BlockState> States;
  std::deque<size_t> Worklist;
  // Leaf CFGs merge predecessor ranges before naming stored return values.
  // Concrete indirect sites also converge before requesting their callees
  // again. Other callers keep FIFO order so their early demands do not
  // displace later bounded callee specializations.
  const bool Ordered = !ContainsCalls[Entry] ||
                       (!Generic && !GenericContexts.at(Entry)->Sites.empty());
  const auto Later = [&](size_t Left, size_t Right) {
    return BlockOrder[Left] > BlockOrder[Right];
  };
  const auto Queue = [&](size_t Block) {
    Worklist.push_back(Block);
    if (Ordered)
      std::push_heap(Worklist.begin(), Worklist.end(), Later);
  };
  uint64_t SolverBytes = 0;
  const auto Release = llvm::scope_exit([&] { RetainedBytes -= SolverBytes; });
  const auto StateCharge = [](const ContextState &State) {
    return stateBytes(State) + sizeof(decltype(States)::value_type) +
           6 * sizeof(void *);
  };
  const auto Enqueue = [&](size_t BlockID, const ContextState &Incoming,
                           uint16_t Defined) -> llvm::Error {
    const auto ProjectInputs = [&](ContextState &State) {
      const uint16_t Inputs =
          InputRegisters[BlockID] | registerBit(kFramePointerRegister);
      for (unsigned R = 0; R < kRegisterCount; ++R)
        if (!(Inputs & registerBit(R))) {
          State.Registers[R] = {};
          State.Origins[R] = {};
          State.InputExpressions[R].reset();
        }
      if (State.Tagged)
        State.Tagged->Registers &= Inputs;
      if (State.Nullable) {
        State.Nullable->Registers &= Inputs;
        if (!State.Nullable->Registers)
          State.Nullable.reset();
      }
      if (!ReadsMemory[BlockID])
        State.InputWrites = {};
    };
    auto It = States.find(BlockID);
    if (It == States.end()) {
      ContextState Input = Incoming;
      ProjectInputs(Input);
      const uint64_t Bytes = StateCharge(Input);
      if (auto Error = retain(Bytes))
        return Error;
      SolverBytes += Bytes;
      States.emplace(BlockID, BlockState{std::move(Input), true, Defined});
      Queue(BlockID);
      return llvm::Error::success();
    }
    ContextState Merged = It->second.Entry;
    if (auto Error = joinStates(Merged, Incoming))
      return Error;
    ProjectInputs(Merged);
    const uint16_t MergedDefinitions = It->second.DefinitelyWritten & Defined;
    if (Merged.Registers == It->second.Entry.Registers &&
        Merged.Scratch == It->second.Entry.Scratch &&
        Merged.Origins == It->second.Entry.Origins &&
        Merged.OriginScratch == It->second.Entry.OriginScratch &&
        Merged.Postconditions == It->second.Entry.Postconditions &&
        Merged.InputExpressions == It->second.Entry.InputExpressions &&
        Merged.InputSpills == It->second.Entry.InputSpills &&
        Merged.InputWrites == It->second.Entry.InputWrites &&
        Merged.Tagged == It->second.Entry.Tagged &&
        Merged.DefinedCaptures == It->second.Entry.DefinedCaptures &&
        Merged.Nullable == It->second.Entry.Nullable &&
        MergedDefinitions == It->second.DefinitelyWritten)
      return llvm::Error::success();
    const uint64_t OldCharge = StateCharge(It->second.Entry);
    const uint64_t NewCharge = StateCharge(Merged);
    if (NewCharge > OldCharge) {
      if (auto Error = retain(NewCharge - OldCharge))
        return Error;
      SolverBytes += NewCharge - OldCharge;
    } else {
      RetainedBytes -= OldCharge - NewCharge;
      SolverBytes -= OldCharge - NewCharge;
    }
    It->second.Entry = std::move(Merged);
    It->second.DefinitelyWritten = MergedDefinitions;
    if (!It->second.Queued) {
      Queue(BlockID);
      It->second.Queued = true;
    }
    return llvm::Error::success();
  };
  if (auto Error = Enqueue(Entry, Input, 0))
    return std::move(Error);
  while (!Worklist.empty()) {
    size_t BlockID;
    if (Ordered) {
      std::pop_heap(Worklist.begin(), Worklist.end(), Later);
      BlockID = Worklist.back();
      Worklist.pop_back();
    } else {
      BlockID = Worklist.front();
      Worklist.pop_front();
    }
    BlockState &Block = States.at(BlockID);
    Block.Queued = false;
    ContextState State = Block.Entry;
    uint16_t Defined = Block.DefinitelyWritten;
    auto Continues = replay(Context, BlockID, State, Defined, false);
    if (!Continues)
      return Continues.takeError();
    if (*Continues) {
      const auto &MedBlock = Program.Med.Blocks[BlockID];
      const auto Selected =
          knownBranchSuccessor(Index.find(MedBlock.EndSlot - 1), State);
      for (size_t E = Offsets[BlockID]; E < Offsets[BlockID + 1]; ++E) {
        if (Selected &&
            Program.Med.Blocks[Successors[E]].StartSlot != *Selected) {
          ++SkippedConstantBranchSuccessors;
          continue;
        }
        ContextState SuccessorState = State;
        const size_t NextSlot = Program.Med.Blocks[Successors[E]].StartSlot;
        auto TaggedReachable =
            refineTaggedBranch(MedBlock, NextSlot, SuccessorState);
        if (!TaggedReachable)
          return TaggedReachable.takeError();
        if (!*TaggedReachable)
          continue;
        if (auto Error =
                refineLoadedEquality(MedBlock, NextSlot, SuccessorState))
          return std::move(Error);
        if (!refineAddressBranch(Index.find(MedBlock.EndSlot - 1), NextSlot,
                                 SuccessorState, Program, *CapturedAddresses,
                                 ReturnCarriers[Successors[E]]))
          continue;
        // Reverse postorder identifies a DFS backedge even when the bytecode
        // places an acyclic continuation before its allocating predecessor.
        // Every cycle still crosses such an edge and loses capture identity.
        if (BlockOrder[Successors[E]] <= BlockOrder[BlockID])
          widenCapturedValues(SuccessorState, *CapturedAddresses);
        if (auto Error = Enqueue(Successors[E], SuccessorState, Defined))
          return std::move(Error);
      }
    }
  }
  // A common store-tag/exit tail destroys the correlation at its entry join.
  // Recover its complete partition from converged predecessor exits only.
  // Other return shapes remain in the ordinary pointwise domain.
  std::optional<size_t> TaggedTail;
  const MedInstruction *TagStore = nullptr;
  size_t ReturnBlocks = 0;
  for (const auto &[BlockID, Block] : States) {
    const auto &MedBlock = Program.Med.Blocks[BlockID];
    const auto *Last = Index.find(MedBlock.EndSlot - 1);
    if (!Last || Last->Op != Operation::Exit)
      continue;
    ++ReturnBlocks;
    const auto *First = Index.find(MedBlock.StartSlot);
    if (First && First->Op == Operation::Store &&
        First->Form == OperandForm::StoreReg &&
        First->Slot + First->SlotWidth == Last->Slot && BlockID != Entry) {
      TaggedTail = BlockID;
      TagStore = First;
    }
  }
  if (ReturnBlocks != 1)
    TaggedTail.reset();
  std::optional<TaggedMemory> TaggedReturn;
  uint64_t TaggedBytes = 0;
  bool RejectedTag = false;
  const auto ReleaseTagged =
      llvm::scope_exit([&] { RetainedBytes -= TaggedBytes; });
  const auto CollectTagged = [&](size_t BlockID,
                                 const ContextState &State) -> llvm::Error {
    if (!TaggedTail || RejectedTag ||
        !llvm::is_contained(llvm::ArrayRef(Successors)
                                .slice(Offsets[BlockID],
                                       Offsets[BlockID + 1] - Offsets[BlockID]),
                            *TaggedTail))
      return llvm::Error::success();
    const auto &MedBlock = Program.Med.Blocks[BlockID];
    const auto Selected =
        knownBranchSuccessor(Index.find(MedBlock.EndSlot - 1), State);
    const size_t TailSlot = Program.Med.Blocks[*TaggedTail].StartSlot;
    if (Selected && *Selected != TailSlot)
      return llvm::Error::success();
    const uint64_t Temporary = stateBytes(State);
    if (auto Error = retain(Temporary))
      return Error;
    const auto Release = llvm::scope_exit([&] { RetainedBytes -= Temporary; });
    ContextState Path = State;
    auto TaggedReachable = refineTaggedBranch(MedBlock, TailSlot, Path);
    if (!TaggedReachable)
      return TaggedReachable.takeError();
    if (!*TaggedReachable)
      return llvm::Error::success();
    if (auto Error = refineLoadedEquality(MedBlock, TailSlot, Path))
      return Error;
    if (!refineAddressBranch(Index.find(MedBlock.EndSlot - 1), TailSlot, Path,
                             Program, *CapturedAddresses,
                             ReturnCarriers[*TaggedTail]))
      return llvm::Error::success();
    if (BlockOrder[*TaggedTail] <= BlockOrder[BlockID])
      widenCapturedValues(Path, *CapturedAddresses);
    const unsigned Width = TagStore->Width / kBitsPerByte;
    const auto Tag =
        truncateStoredValue(Path.Registers[TagStore->Src], Width).singleton();
    refineOrigins(Context, Path);
    RegisterState Entry{};
    Entry[0] = Path.Origins[TagStore->Dst].singleton();
    const auto Address = substituteOrigin(
        {RegisterValue::Kind::StackAddress, 1, TagStore->Offset}, Entry,
        Program);
    if (Tag.ValueKind != RegisterValue::Kind::Constant ||
        Address.ValueKind == RegisterValue::Kind::Unknown ||
        (TaggedReturn &&
         (TaggedReturn->Address != Address || TaggedReturn->Width != Width))) {
      RejectedTag = true;
      return llvm::Error::success();
    }
    if (auto Error = visit())
      return Error;
    transfer(*TagStore, Path, nullptr);
    if (!TaggedReturn)
      TaggedReturn = TaggedMemory{Address, Width, 0, {}, {}};
    TaggedMemoryCase Case;
    Case.Tag = Tag.Value;
    Case.Postconditions = std::move(Path.Postconditions);
    Case.Postconditions.discardInputCopies();
    Case.Postconditions.store(Address, Width, Tag);
    Case.Values.setCoordinates(Coordinates);
    Case.Values.includeCells(Path.Scratch.Values, [&](const auto &Cell) {
      return Cell.ValueKind == RegisterValue::Kind::SymbolicAddress &&
             CaptureOwners[Cell.Value] == Context.ID;
    });
    Case.DefinedCaptures = std::move(Path.DefinedCaptures);
    std::erase_if(Case.DefinedCaptures, [&](uint64_t Capture) {
      return CaptureOwners[Capture] != Context.ID;
    });
    auto Existing = llvm::find_if(TaggedReturn->Cases, [&](const auto &Old) {
      return Old.Tag == Case.Tag;
    });
    if (Existing == TaggedReturn->Cases.end()) {
      if (TaggedReturn->Cases.size() == kCallXContextValueAlternatives) {
        RejectedTag = true;
        return llvm::Error::success();
      }
      TaggedReturn->Cases.push_back(std::move(Case));
    } else {
      Existing->Values.joinValues(Case.Values);
      Existing->Postconditions.meet(Case.Postconditions);
      for (uint64_t Capture : Case.DefinedCaptures) {
        auto &Captures = Existing->DefinedCaptures;
        const auto Position =
            std::lower_bound(Captures.begin(), Captures.end(), Capture);
        if (Position == Captures.end() || *Position != Capture)
          Captures.insert(Position, Capture);
      }
    }
    const uint64_t Bytes = TaggedReturn->retainedByteEstimate();
    if (Bytes > TaggedBytes) {
      if (auto Error = retain(Bytes - TaggedBytes))
        return Error;
    } else {
      RetainedBytes -= TaggedBytes - Bytes;
    }
    TaggedBytes = Bytes;
    return llvm::Error::success();
  };
  // Publish no provisional call or target: replay only the fixed-point entry
  // states. Cache nodes created while converging are not automatically roots.
  for (const auto &[BlockID, Block] : States) {
    ContextState State = Block.Entry;
    uint16_t Defined = Block.DefinitelyWritten;
    auto Continues = replay(Context, BlockID, State, Defined, true);
    if (!Continues)
      return Continues.takeError();
    if (!*Continues)
      continue;
    if (auto Error = CollectTagged(BlockID, State))
      return std::move(Error);
    const MedBlock &MedBlock = Program.Med.Blocks[BlockID];
    const MedInstruction *Last = Index.find(MedBlock.EndSlot - 1);
    if (!Last || Last->Op != Operation::Exit)
      continue;
    if (!Context.Return) {
      Context.Return = std::move(State);
    } else {
      if (auto Error = joinStates(*Context.Return, State))
        return std::move(Error);
    }
  }
  if (!RejectedTag && TaggedReturn && TaggedReturn->Cases.size() > 1) {
    llvm::sort(TaggedReturn->Cases,
               [](const auto &A, const auto &B) { return A.Tag < B.Tag; });
    Context.TaggedReturn = std::move(TaggedReturn);
    Context.RetainedBytes += TaggedBytes;
    TaggedBytes = 0;
  }
  Context.HasTargetEvidence = llvm::any_of(Context.Sites, [](const auto &Site) {
    return Site.TargetAddress || !Site.TargetValueAlternatives.unknown();
  });
  for (const auto &Edge : Context.Calls)
    Context.HasTargetEvidence |=
        !Edge.InputExpressionTargets.empty() ||
        (Edge.CalleeSummaryID &&
         Contexts[*Edge.CalleeSummaryID]->HasTargetEvidence);
  if (Context.Return) {
    if (auto Error = retain(stateBytes(*Context.Return)))
      return std::move(Error);
    Context.RetainedBytes += stateBytes(*Context.Return);
  }
  const uint64_t AccessBytes = (Context.Effects.Writes.capacity() +
                                Context.ReadEffects.Writes.capacity()) *
                               sizeof(MemoryWrite);
  if (auto Error = retain(AccessBytes))
    return std::move(Error);
  Context.RetainedBytes += AccessBytes;
  Context.WritesOnlyOwnAndChildFrames =
      !Context.Effects.Unknown && Context.Effects.Writes.empty();
  Context.PreservesMemory =
      Context.WritesOnlyOwnAndChildFrames && !Context.Effects.WritesFrames;
  Context.Complete = true;
  return &Context;
}

llvm::Error ContextFlow::collectEntryPrefixes(CallXContextReport &Report) {
  std::map<size_t, size_t> Prefixes;
  for (const MedInstruction &Instruction : Program.Med.Instructions) {
    if (Instruction.Call != CallKind::Internal || !Instruction.CallTarget)
      continue;
    const auto It = ByEntry.find(*Instruction.CallTarget);
    if (It == ByEntry.end())
      continue;
    const auto *Last = Index.find(Program.Med.Blocks[It->second].EndSlot - 1);
    if (Last && Last->Op == Operation::CallX &&
        Last->Call == CallKind::Indirect)
      Prefixes.emplace(*Instruction.CallTarget, It->second);
  }
  Report.EntryPrefixCallXCount = Prefixes.size();
  if (Prefixes.empty())
    return llvm::Error::success();
  // Preserve the independently caller-seeded prefix proofs when unrelated
  // return paths consume a whole-function summary's variant capacity.
  const ScratchFlow Flow(Program, Index, ScratchLoadPolicy::ProvenValues);
  Report.PrefixScratchPrecision = Flow.statistics().Precision;
  const uint64_t FlowBytes = Flow.statistics().PeakRetainedByteEstimate;
  if (auto Error = retain(FlowBytes))
    return Error;
  const auto Release = llvm::scope_exit([&] { RetainedBytes -= FlowBytes; });
  for (const MedBlock &Block : Program.Med.Blocks) {
    ContextState Caller;
    const RegisterState *Registers = Flow.entryRegisters(Block.ID);
    for (unsigned R = 0; R < kRegisterCount; ++R)
      Caller.Registers[R] = Registers ? (*Registers)[R] : Block.Inputs[R];
    Caller.Scratch = Flow.entryState(Block.ID);
    for (size_t Slot = Block.StartSlot; Slot < Block.EndSlot; ++Slot) {
      const MedInstruction *Instruction = Index.find(Slot);
      if (!Instruction)
        continue;
      if (auto Error = visit())
        return Error;
      if (Instruction->Call == CallKind::Internal && Instruction->CallTarget &&
          Prefixes.contains(*Instruction->CallTarget)) {
        const size_t Entry = Prefixes.at(*Instruction->CallTarget);
        ContextState Callee = Caller;
        pushAbstractFrame(Callee, Program);
        FunctionContext Context(0, Entry, Callee, false);
        MissingRegisters Missing;
        for (unsigned R = 0; R < kRegisterCount; ++R)
          if (Callee.Registers[R].unknown()) {
            Missing[R].Kind = CallXMissingFactKind::IncomingRegister;
            Missing[R].InputRegister = R;
          }
        const MedBlock &Prefix = Program.Med.Blocks[Entry];
        CallXContextEvidence Evidence;
        for (size_t S = Prefix.StartSlot; S < Prefix.EndSlot; ++S) {
          const auto *Current = Index.find(S);
          if (!Current)
            continue;
          if (auto Error = visit())
            return Error;
          if (S == Prefix.EndSlot - 1)
            Evidence = evidence(Context, *Current, Callee, Missing, false);
          else
            transfer(*Current, Callee, &Missing);
        }
        Evidence.EntryPrefixOnly = true;
        Evidence.CallerSlot = Slot;
        if (hasRegisteredLegacyRuntime(*Instruction)) {
          Evidence.Result = CallXContextResult::LegacyRuntimeMayMutateArguments;
          Evidence.TargetAddress.reset();
          Evidence.TargetSlot.reset();
          Evidence.TargetValueAlternatives = {};
          Evidence.Missing = {};
        }
        if (Report.Contexts.size() + Report.UnresolvedCallXSlots.size() ==
            kCallXContextOutputElementBudget)
          return llvm::make_error<llvm::StringError>(
              kCallXContextOutputBudgetDiagnostic,
              llvm::inconvertibleErrorCode());
        if (auto Error = retain(sizeof(Evidence)))
          return Error;
        Report.Contexts.push_back(std::move(Evidence));
      }
      transfer(*Instruction, Caller, nullptr);
    }
  }
  return llvm::Error::success();
}

llvm::Expected<CallXContextReport> ContextFlow::run() {
  CallXContextReport Report;
  if (auto Error = retain(sizeof(StoredMemoryCoordinates) +
                          sizeof(SymbolicAddressSpace) + 4 * sizeof(void *) +
                          BlockOrder.capacity() * sizeof(size_t)))
    return std::move(Error);
  std::set<size_t> Roots;
  std::vector<bool> HasPredecessor(Program.Med.Blocks.size());
  for (size_t Successor : Successors)
    HasPredecessor[Successor] = true;
  for (const Function &Function : Program.High.Functions)
    Roots.insert(Function.EntrySlot);
  Roots.insert(Program.Low.EntrySlot);
  for (const MedBlock &Block : Program.Med.Blocks)
    if (!HasPredecessor[Block.ID])
      Roots.insert(Block.StartSlot);
  for (const MedInstruction &Instruction : Program.Med.Instructions) {
    if (Instruction.Call == CallKind::Internal && Instruction.CallTarget) {
      Roots.insert(*Instruction.CallTarget);
    }
    if (Instruction.Op == Operation::CallX &&
        Instruction.Call == CallKind::Indirect) {
      if (Report.UnresolvedCallXSlots.size() ==
          kCallXContextOutputElementBudget)
        return llvm::make_error<llvm::StringError>(
            kCallXContextOutputBudgetDiagnostic,
            llvm::inconvertibleErrorCode());
      Report.UnresolvedCallXSlots.push_back(Instruction.Slot);
    }
  }
  if (Report.UnresolvedCallXSlots.empty())
    return Report;
  if (auto Error = computeReturnReachability())
    return std::move(Error);
  if (auto Error = computeInputDependencies())
    return std::move(Error);
  if (auto Error = collectEntryPrefixes(Report))
    return std::move(Error);
  // Build sound all-unknown-input summaries first. They are reusable across
  // arbitrary caller facts and remain available if specialization widens.
  for (size_t Slot : Roots) {
    const auto It = ByEntry.find(Slot);
    if (It == ByEntry.end())
      continue;
    auto Root = summarize(It->second, ContextState{}, true);
    if (!Root)
      return Root.takeError();
    publishRoot(**Root);
  }
  // Preserve entry order: eagerly specializing small unknown-input wrappers
  // can pin all variants before their concrete callers are analyzed.
  for (size_t Slot : Roots) {
    const auto It = ByEntry.find(Slot);
    if (It == ByEntry.end())
      continue;
    if (!ReachesCallX[It->second]) {
      // Generic roots remain published. A root with no statically reachable
      // unresolved CALLX cannot add site evidence; any caller can still demand
      // its specialized return/memory summary through summarize().
      ++SkippedNonCallXRoots;
      continue;
    }
    ++RootGeneration;
    ContextState Input;
    // Whole-function replay starts at the loader's virtual predecessor. The
    // global block fixed point may already have joined unrelated roots or
    // backedges into this block. Our CFG solver joins real backedges itself;
    // ordinary calls to this same slot retain their actual caller inputs.
    const auto Boundary = Slot == Program.Low.EntrySlot
                              ? programEntryRegisterFacts(Program)
                              : Program.Med.Blocks[It->second].Inputs;
    for (unsigned R = 0; R < kRegisterCount; ++R)
      Input.Registers[R] = Boundary[R];
    auto Root = summarize(It->second, Input);
    if (!Root) {
      if (!SpecializationBudgetExhausted)
        return Root.takeError();
      llvm::consumeError(Root.takeError());
      continue;
    }
    publishRoot(**Root);
  }

  const size_t Unreached = std::numeric_limits<size_t>::max();
  std::vector<size_t> Depth(Contexts.size(), Unreached);
  std::deque<size_t> Worklist;
  for (const auto &Context : Contexts)
    if (Context && Context->IsRoot) {
      Depth[Context->ID] = 0;
      Worklist.push_back(Context->ID);
    }
  while (!Worklist.empty()) {
    const size_t ID = Worklist.front();
    Worklist.pop_front();
    for (const auto &Call : Contexts[ID]->Calls)
      if (Call.CalleeSummaryID &&
          Depth[*Call.CalleeSummaryID] > Depth[ID] + 1) {
        Depth[*Call.CalleeSummaryID] = Depth[ID] + 1;
        Worklist.push_back(*Call.CalleeSummaryID);
      }
  }
  size_t InputExpressionProofElements = 0;
  size_t ReturnInputElements = 0;
  size_t MemoryGuardElements = InputMemoryGuards.size();
  for (const auto &Guard : InputMemoryGuards)
    MemoryGuardElements += Guard.Writes.size();
  const auto AddSites = [&](const FunctionContext &Context,
                            const ContextCall *Edge) {
    size_t Instantiated = 0;
    for (auto Site : Context.Sites) {
      if (Edge && Instantiated < Edge->InputExpressionTargets.size() &&
          Edge->InputExpressionTargets[Instantiated].CallXSlot ==
              Site.CallXSlot)
        Site = Edge->InputExpressionTargets[Instantiated++];
      Site.IsRootContext = !Edge;
      if (Edge) {
        Site.CallerSlot = Edge->CallerSlot;
        Site.CallerSummaryID = Edge->CallerSummaryID;
        Site.InputGeneralization = Edge->InputGeneralization;
        if (Edge->LegacyRuntime) {
          Site.Result = CallXContextResult::LegacyRuntimeMayMutateArguments;
          Site.TargetAddress.reset();
          Site.TargetSlot.reset();
          Site.TargetValueAlternatives = {};
          Site.Missing = {};
        }
      }
      InputExpressionProofElements += Site.InputExpressionValues.size();
      Report.Contexts.push_back(std::move(Site));
    }
  };
  for (const auto &Context : Contexts) {
    if (!Context)
      continue;
    if (Depth[Context->ID] == Unreached)
      continue;
    CallXFunctionSummary Summary;
    Summary.ID = Context->ID;
    Summary.EntrySlot = Program.Med.Blocks[Context->EntryBlock].StartSlot;
    Summary.InputRegisterMask = InputRegisters[Context->EntryBlock];
    Summary.ReadsInputMemory = ReadsMemory[Context->EntryBlock];
    Summary.ReuseInputRegisterMask = Context->ObservableInputRegisters;
    Summary.ReuseReadsInputMemory = Context->ReuseReadsMemory;
    for (unsigned R = 0; R < kRegisterCount; ++R) {
      Summary.Inputs[R] = Context->Input.Registers[R].singleton();
      if (Context->Input.Registers[R].values().size() > 1)
        Summary.InputAlternatives.emplace_back(R, Context->Input.Registers[R]);
    }
    Summary.MayReturn = Context->Return.has_value();
    if (Context->Return) {
      Summary.ReturnValue = Context->Return->Registers[0].singleton();
      Summary.ReturnAlternatives = Context->Return->Registers[0];
      Summary.ReturnValueExpression = Context->Return->InputExpressions[0];
      const auto Origin = Context->Return->Origins[0].singleton();
      if (Origin.ValueKind == RegisterValue::Kind::StackAddress)
        Summary.ReturnInputExpression = std::make_pair(
            static_cast<unsigned>(Origin.Value - 1), Origin.Offset);
      if (Context->Return->Origins[0].values().size() > 1)
        for (const auto &Value : Context->Return->Origins[0].values()) {
          CallXInputExpression Expression;
          if (Value.ValueKind == RegisterValue::Kind::Constant) {
            Expression.Kind = CallXInputExpressionKind::Constant;
            Expression.Constant = Value;
          } else if (Value.ValueKind == RegisterValue::Kind::StackAddress) {
            Expression.Register = Value.Value - 1;
            Expression.Offset = Value.Offset;
          } else {
            Summary.ReturnInputAlternatives.clear();
            break;
          }
          Summary.ReturnInputAlternatives.push_back(Expression);
        }
      Summary.OutputScratchWords = Context->Return->Scratch.Values.wordCount();
      Summary.OutputNarrowScalarCells =
          Context->Return->Scratch.Values.size() - Summary.OutputScratchWords;
      Summary.OutputParameterizedWords =
          Context->Return->Postconditions.wordCount();
      Summary.OutputNarrowParameterizedFields =
          Context->Return->Postconditions.size() -
          Summary.OutputParameterizedWords;
      Summary.OutputFieldValueExpressions =
          Context->Return->Postconditions.expressionCount();
    }
    Summary.OutputMemoryCopies =
        Context->Return ? Context->Return->Postconditions.copyCount() : 0;
    Summary.OutputTaggedCases =
        Context->TaggedReturn ? Context->TaggedReturn->Cases.size() : 0;
    ReturnInputElements += Summary.OutputMemoryCopies;
    Summary.InputScratchWords = Context->Input.Scratch.Values.wordCount();
    Summary.InputNarrowScalarCells =
        Context->Input.Scratch.Values.size() - Summary.InputScratchWords;
    Summary.IsRoot = Context->IsRoot;
    Summary.GeneralizedInputs = Context->Generic;
    Summary.PreservesCallerMemory = Context->PreservesMemory;
    Summary.WritesOnlyOwnAndChildFrames = Context->WritesOnlyOwnAndChildFrames;
    Summary.MayWriteUnknownMemory = Context->Effects.Unknown;
    Summary.MayWriteCalleeFrames = Context->Effects.WritesFrames;
    for (const auto &Write : Context->Effects.Writes) {
      CallXMemoryWrite Effect;
      Effect.Size = Write.Size;
      Effect.Address = Write.Address.Value;
      Effect.Offset = Write.Address.Offset;
      if (Write.Address.ValueKind == RegisterValue::Kind::Constant)
        Effect.Base = CallXMemoryWriteBase::Absolute;
      else if (Write.Address.ValueKind == RegisterValue::Kind::UnsignedRange)
        Effect.Base = CallXMemoryWriteBase::UnsignedRange;
      else if (Write.Address.Value == kRegisterCount)
        Effect.Base = CallXMemoryWriteBase::FramePointer;
      else {
        Effect.Base = CallXMemoryWriteBase::InputRegister;
        Effect.Register = Write.Address.Value - 1;
      }
      Summary.MemoryWrites.push_back(Effect);
    }
    Summary.MinimumDepth = Depth[Context->ID];
    ReturnInputElements += Summary.ReturnInputAlternatives.size();
    // Charge the added public metadata while the private summaries still live.
    if (auto Error = retain(sizeof(Summary.OutputTaggedCases) +
                            sizeof(Summary.ReturnValueExpression) +
                            sizeof(Summary.OutputFieldValueExpressions) +
                            sizeof(Summary.ReturnInputAlternatives) +
                            Summary.ReturnInputAlternatives.capacity() *
                                sizeof(CallXInputExpression)))
      return std::move(Error);
    Report.Summaries.push_back(std::move(Summary));
    if (Context->IsRoot)
      AddSites(*Context, nullptr);
    for (const auto &Edge : Context->Calls) {
      if (auto Error = retain(sizeof(Edge.CompletedResultBinding) +
                              sizeof(Edge.InstantiatedTaggedOutputCases)))
        return std::move(Error);
      Report.SummaryEdges.push_back(Edge);
      if (Edge.CalleeSummaryID)
        AddSites(*Contexts[*Edge.CalleeSummaryID], &Edge);
    }
    if (Report.UnresolvedCallXSlots.size() + Report.Contexts.size() +
            Report.Summaries.size() + Report.SummaryEdges.size() +
            InputExpressions.size() + InputExpressionProofElements +
            ReturnInputElements + MemoryGuardElements +
            CapturedAddresses->size() >
        kCallXContextOutputElementBudget)
      return llvm::make_error<llvm::StringError>(
          kCallXContextOutputBudgetDiagnostic, llvm::inconvertibleErrorCode());
  }
  Report.InstructionVisits = Visits;
  Report.SummaryCacheHits = CacheHits;
  Report.PeakRetainedBytes = PeakRetainedBytes;
  Report.SpecializationBudgetExhausted = SpecializationBudgetExhausted;
  Report.ProjectedRegisters = ProjectedRegisters;
  Report.ProjectedScratchSnapshots = ProjectedScratchSnapshots;
  Report.InputDependencyVisits = DependencyVisits;
  Report.SummaryCacheEvictions = CacheEvictions;
  Report.GeneralizedPublishedSummaries = GeneralizedPublishedSummaries;
  Report.UnblockedRecursionRetries = UnblockedRecursionRetries;
  Report.EquivalentGenericReuses = EquivalentGenericReuses;
  Report.EquivalentSpecializedReuses = EquivalentSpecializedReuses;
  Report.CoveringSpecializedReuses = CoveringSpecializedReuses;
  Report.SkippedNonCallXRootSpecializations = SkippedNonCallXRoots;
  Report.SkippedConstantBranchSuccessors = SkippedConstantBranchSuccessors;
  Report.CanonicalizedFrameInputs = CanonicalizedFrameInputs;
  Report.CapturedAddresses = *CapturedAddresses;
  Report.InputExpressions = std::move(InputExpressions);
  for (const auto &Guard : InputMemoryGuards) {
    CallXInputMemoryGuard Output;
    Output.WritesCalleeFrames = Guard.WritesFrames;
    for (const auto &Write : Guard.Writes) {
      CallXMemoryWrite Effect;
      Effect.Size = Write.Size;
      Effect.Address = Write.Address.Value;
      Effect.Offset = Write.Address.Offset;
      if (Write.Address.ValueKind == RegisterValue::Kind::Constant)
        Effect.Base = CallXMemoryWriteBase::Absolute;
      else if (Write.Address.ValueKind == RegisterValue::Kind::UnsignedRange)
        Effect.Base = CallXMemoryWriteBase::UnsignedRange;
      else if (Write.Address.Value == kRegisterCount)
        Effect.Base = CallXMemoryWriteBase::FramePointer;
      else {
        Effect.Base = CallXMemoryWriteBase::InputRegister;
        Effect.Register = Write.Address.Value - 1;
      }
      Output.Writes.push_back(Effect);
    }
    Report.InputMemoryGuards.push_back(std::move(Output));
  }
  Report.InstantiatedTargetInputExpressions =
      InstantiatedTargetInputExpressions;
  Report.InstantiatedStoredValueInputExpressions =
      InstantiatedStoredValueInputExpressions;
  Report.InstantiatedReturnInputExpressions =
      InstantiatedReturnInputExpressions;
  Report.ReturnReachabilityVisits = ReturnReachabilityVisits;
  return Report;
}

} // namespace

llvm::Expected<CallXContextReport>
recoverCallXContexts(const SBFProgram &Program) {
  return ContextFlow(Program).run();
}

} // namespace neverd::sbf
