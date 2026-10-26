//===- SBFCallXMemoryEffects.cpp - Parametric scratch writes --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Composes argument-relative write intervals and applies them to caller facts.
/// The analysis may forget bytes between writes, but never assumes a pointer
/// from account memory or an opaque call is disjoint from a caller's frame.
///
//===----------------------------------------------------------------------===//

#include "SBFCallXMemoryEffects.h"

#include "neverd/sbf/runtime/SBFVMConfig.h"

#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <bit>
#include <limits>
#include <optional>
#include <utility>

namespace neverd::sbf::callx_detail {

RegisterState symbolicInputs() {
  RegisterState Origins{};
  for (unsigned R = 0; R < kRegisterCount; ++R)
    Origins[R] = {RegisterValue::Kind::StackAddress, R + 1, 0};
  return Origins;
}

RegisterValue substituteOrigin(const RegisterValue &Origin,
                               const RegisterState &Entry,
                               const SBFProgram &Program) {
  if (Origin.ValueKind == RegisterValue::Kind::Constant)
    return Origin;
  if (Origin.ValueKind != RegisterValue::Kind::StackAddress)
    return {};
  RegisterState Value{};
  Value[0] = Entry[Origin.Value - 1];
  MedInstruction Add;
  Add.Op = Operation::Add;
  Add.Form = OperandForm::DstImm;
  Add.Width = kDoubleWordBitWidth;
  Add.Immediate = std::bit_cast<uint64_t>(Origin.Offset);
  Add.Semantics.Immediate = ImmediateExtension::Full64;
  Add.Semantics.WritesDestination = true;
  applyRegisterTransfer(Add, Value, Program.ExecutableImage);
  return Value[0];
}

void MemoryEffects::addWrite(RegisterValue Address, uint64_t Size,
                             const SBFProgram &Program) {
  if (Unknown || Size == 0)
    return;
  const bool Absolute = Address.ValueKind == RegisterValue::Kind::Constant;
  if (!Absolute && Address.ValueKind != RegisterValue::Kind::StackAddress) {
    Unknown = true;
    Writes.clear();
    return;
  }
  int64_t End = 0;
  if ((Absolute &&
       Size > std::numeric_limits<uint64_t>::max() - Address.Value) ||
      (!Absolute &&
       (Size > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
        llvm::AddOverflow(Address.Offset, static_cast<int64_t>(Size), End)))) {
    Unknown = true;
    Writes.clear();
    return;
  }
  if (!Absolute && Address.Value == kRegisterCount &&
      !versionHasFeature(Program.Low.TheVersion,
                         VersionFeature::ManualStackFrames) &&
      Address.Offset >= -static_cast<int64_t>(Program.Config.StackFrameSize)) {
    // The current automatic frame and every higher coordinate belong to this
    // callee or descendants. All such cells are invalidated together at use.
    WritesFrames = true;
    return;
  }
  for (auto &Write : Writes) {
    if (Write.Address.ValueKind != Address.ValueKind ||
        (!Absolute && Write.Address.Value != Address.Value))
      continue;
    if (Absolute) {
      const uint64_t Last =
          std::max(Write.Address.Value + Write.Size, Address.Value + Size);
      Write.Address.Value = std::min(Write.Address.Value, Address.Value);
      Write.Size = Last - Write.Address.Value;
      if (Write.Size >
          static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        Unknown = true;
        Writes.clear();
      }
    } else {
      const int64_t Last = std::max(
          Write.Address.Offset + static_cast<int64_t>(Write.Size), End);
      Write.Address.Offset = std::min(Write.Address.Offset, Address.Offset);
      // Use unsigned subtraction: a conservative interval can span zero.
      Write.Size = static_cast<uint64_t>(Last) -
                   static_cast<uint64_t>(Write.Address.Offset);
      if (Write.Size >
          static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        Unknown = true;
        Writes.clear();
      }
    }
    return;
  }
  Writes.push_back({Address, Size});
}

void MemoryEffects::include(const MemoryEffects &Callee,
                            const RegisterState &Entry,
                            const SBFProgram &Program) {
  if (Callee.Unknown) {
    Unknown = true;
    Writes.clear();
    return;
  }
  WritesFrames |= Callee.WritesFrames;
  for (const auto &Write : Callee.Writes)
    addWrite(substituteOrigin(Write.Address, Entry, Program), Write.Size,
             Program);
}

bool MemoryEffects::invalidate(
    ScratchState &Scratch,
    const std::array<ProvenValueSet, kRegisterCount> &Entry,
    const SBFProgram &Program) const {
  if (Unknown) {
    Scratch.clear();
    return false;
  }
  // Descriptor bytes do not have coordinate tags; preserve only typed values
  // when applying a symbolic footprint across a call.
  Scratch.Memory.clear();
  if (WritesFrames) {
    const auto Frame = Entry[kFramePointerRegister].singleton();
    const auto FP = effectiveAddress(Frame, 0);
    if (!FP || Frame.ValueKind != RegisterValue::Kind::StackAddress ||
        *FP < Program.Config.StackFrameSize) {
      Scratch.clear();
      return false;
    }
    Scratch.Values.invalidateFrom(*FP - Program.Config.StackFrameSize,
                                  MemoryCoordinate::FrameRelative);
  }
  RegisterState Actual{};
  for (unsigned R = 0; R < kRegisterCount; ++R)
    Actual[R] = Entry[R].singleton();
  for (const auto &Write : Writes) {
    const auto Values =
        Write.Address.ValueKind == RegisterValue::Kind::StackAddress
            ? Entry[Write.Address.Value - 1].values()
            : llvm::ArrayRef<RegisterValue>(Write.Address);
    if (Values.empty()) {
      Scratch.clear();
      return false;
    }
    for (const auto &Value : Values) {
      auto Registers = Actual;
      if (Write.Address.ValueKind == RegisterValue::Kind::StackAddress)
        Registers[Write.Address.Value - 1] = Value;
      const auto Address = substituteOrigin(Write.Address, Registers, Program);
      const auto Number = effectiveAddress(Address, 0);
      if (!Number) {
        Scratch.clear();
        return false;
      }
      Scratch.Values.invalidate(*Number, Write.Size,
                                Address.ValueKind ==
                                        RegisterValue::Kind::StackAddress
                                    ? MemoryCoordinate::FrameRelative
                                    : MemoryCoordinate::Absolute);
    }
  }
  return true;
}

void MemoryPostconditions::invalidate(const RegisterValue &Address,
                                      uint64_t Size) {
  const bool Absolute = Address.ValueKind == RegisterValue::Kind::Constant;
  int64_t End = 0;
  if ((Absolute &&
       Size > std::numeric_limits<uint64_t>::max() - Address.Value) ||
      (!Absolute &&
       (Address.ValueKind != RegisterValue::Kind::StackAddress ||
        Size > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
        llvm::AddOverflow(Address.Offset, static_cast<int64_t>(Size), End)))) {
    clear();
    return;
  }
  std::erase_if(Values, [&](const auto &Cell) {
    const auto &[Kind, Base, Offset] = Cell.first;
    // Independent formal names prove no separation of the actual pointers.
    if (Kind != Address.ValueKind)
      return true;
    if (Absolute)
      return Base < Address.Value + Size &&
             Address.Value < Base + sizeof(uint64_t);
    return Base != Address.Value ||
           (Offset < End &&
            Address.Offset < Offset + static_cast<int64_t>(sizeof(uint64_t)));
  });
}

void MemoryPostconditions::store(const RegisterValue &Address, uint64_t Size,
                                 const RegisterValue &Value) {
  invalidate(Address, Size);
  int64_t End;
  if (Size == sizeof(uint64_t) &&
      ((Address.ValueKind == RegisterValue::Kind::StackAddress &&
        !llvm::AddOverflow(Address.Offset, static_cast<int64_t>(Size), End)) ||
       (Address.ValueKind == RegisterValue::Kind::Constant &&
        isScratchRange(Address.Value, Size))) &&
      (Value.ValueKind == RegisterValue::Kind::Constant ||
       Value.ValueKind == RegisterValue::Kind::StackAddress))
    Values[{Address.ValueKind, Address.Value, Address.Offset}] = Value;
}

RegisterValue MemoryPostconditions::read(const RegisterValue &Address) const {
  const auto Found =
      Values.find({Address.ValueKind, Address.Value, Address.Offset});
  return Found == Values.end() ? RegisterValue{} : Found->second;
}

void MemoryPostconditions::meet(const MemoryPostconditions &Other) {
  std::erase_if(Values, [&](const auto &Cell) {
    const auto Found = Other.Values.find(Cell.first);
    return Found == Other.Values.end() || Found->second != Cell.second;
  });
}

void MemoryPostconditions::invalidate(const MemoryEffects &Effects,
                                      const RegisterState &Entry,
                                      const SBFProgram &Program) {
  if (Effects.Unknown || Effects.WritesFrames) {
    clear();
    return;
  }
  for (const auto &Write : Effects.Writes)
    invalidate(substituteOrigin(Write.Address, Entry, Program), Write.Size);
}

void MemoryPostconditions::include(const MemoryPostconditions &Callee,
                                   const RegisterState &Entry,
                                   const SBFProgram &Program) {
  for (const auto &[Cell, Value] : Callee.Values) {
    const auto &[Kind, Base, Offset] = Cell;
    const RegisterValue Address{Kind, Base, Offset};
    store(substituteOrigin(Address, Entry, Program), sizeof(uint64_t),
          substituteOrigin(Value, Entry, Program));
  }
}

size_t MemoryPostconditions::apply(
    ScratchState &Scratch,
    const std::array<ProvenValueSet, kRegisterCount> &Entry,
    const SBFProgram &Program) const {
  const auto Instantiate = [&](const RegisterValue &Origin) {
    if (Origin.ValueKind == RegisterValue::Kind::Constant)
      return ProvenValueSet(Origin);
    std::optional<ProvenValueSet> Result;
    for (const auto &Value : Entry[Origin.Value - 1].values()) {
      RegisterState Actual{};
      Actual[Origin.Value - 1] = Value;
      const auto Substituted = substituteOrigin(Origin, Actual, Program);
      if (!Result)
        Result = Substituted;
      else
        Result->join(Substituted);
    }
    return Result.value_or(ProvenValueSet{});
  };
  MedInstruction Store;
  Store.Op = Operation::Store;
  Store.Form = OperandForm::StoreReg;
  Store.Dst = 0;
  Store.Src = 1;
  Store.Width = Store.Semantics.MemoryWidth = kDoubleWordBitWidth;
  size_t Applied = 0;
  for (const auto &[Cell, Value] : Values) {
    const auto &[Kind, Base, Offset] = Cell;
    const auto Address = Instantiate({Kind, Base, Offset}).singleton();
    const auto Stored = Instantiate(Value);
    if (Address.ValueKind == RegisterValue::Kind::Unknown || Stored.unknown())
      continue;
    std::optional<ScratchState> Output;
    for (const auto &Alternative : Stored.values()) {
      MachineState State;
      State.Scratch = Scratch;
      State.Registers[Store.Dst] = Address;
      State.Registers[Store.Src] = Alternative;
      // Reuse the store owner for scratch-range checks, coordinate aliasing,
      // pointer tags and byte serialization after substituting real inputs.
      applyTransfer(Store, State, Program.ExecutableImage,
                    ScratchLoadPolicy::ProvenValues);
      if (!Output)
        Output = std::move(State.Scratch);
      else {
        Output->Memory.meet(State.Scratch.Memory);
        Output->Values.joinValues(State.Scratch.Values);
      }
    }
    Scratch = std::move(*Output);
    const auto Number = effectiveAddress(Address, 0);
    if (Number &&
        !Scratch.Values
             .readValues(*Number,
                         Address.ValueKind == RegisterValue::Kind::StackAddress
                             ? MemoryCoordinate::FrameRelative
                             : MemoryCoordinate::Absolute)
             .unknown())
      ++Applied;
  }
  return Applied;
}

uint64_t MemoryPostconditions::retainedByteEstimate() const {
  return Values.size() *
         (sizeof(decltype(Values)::value_type) + 4 * sizeof(void *));
}

} // namespace neverd::sbf::callx_detail
