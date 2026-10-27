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

#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/ConstantRange.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <bit>
#include <limits>
#include <optional>
#include <utility>

namespace neverd::sbf::callx_detail {
namespace {

bool isOwnOrChildFrame(const RegisterValue &Address,
                       const SBFProgram &Program) {
  return Address.ValueKind == RegisterValue::Kind::StackAddress &&
         Address.Value == kRegisterCount &&
         !versionHasFeature(Program.Low.TheVersion,
                            VersionFeature::ManualStackFrames) &&
         Address.Offset >= -static_cast<int64_t>(Program.Config.StackFrameSize);
}

} // namespace

RegisterState symbolicInputs() {
  RegisterState Origins{};
  for (unsigned R = 0; R < kRegisterCount; ++R)
    Origins[R] = {RegisterValue::Kind::StackAddress, R + 1, 0};
  return Origins;
}

RegisterValue substituteOrigin(const RegisterValue &Origin,
                               const RegisterState &Entry,
                               const SBFProgram &Program) {
  if (Origin.ValueKind == RegisterValue::Kind::Constant ||
      Origin.ValueKind == RegisterValue::Kind::UnsignedRange ||
      Origin.ValueKind == RegisterValue::Kind::SymbolicAddress)
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

ProvenValueSet
substituteOrigins(const ProvenValueSet &Origins,
                  const std::array<ProvenValueSet, kRegisterCount> &Entry,
                  const SBFProgram &Program) {
  std::optional<ProvenValueSet> Result;
  for (const auto &Origin : Origins.values()) {
    std::optional<ProvenValueSet> Substituted;
    if (Origin.ValueKind == RegisterValue::Kind::Constant ||
        Origin.ValueKind == RegisterValue::Kind::UnsignedRange ||
        Origin.ValueKind == RegisterValue::Kind::SymbolicAddress) {
      Substituted = Origin;
    } else if (Origin.ValueKind == RegisterValue::Kind::StackAddress) {
      for (const auto &Value : Entry[Origin.Value - 1].values()) {
        RegisterState Actual{};
        Actual[Origin.Value - 1] = Value;
        const auto Alternative = substituteOrigin(Origin, Actual, Program);
        if (!Substituted)
          Substituted = Alternative;
        else
          Substituted->join(Alternative);
      }
    }
    if (!Result)
      Result = Substituted.value_or(ProvenValueSet{});
    else
      Result->join(Substituted.value_or(ProvenValueSet{}));
  }
  return Result.value_or(ProvenValueSet{});
}

void MemoryEffects::addWrite(RegisterValue Address, uint64_t Size,
                             const SBFProgram &Program,
                             const SymbolicAddressSpace *Addresses) {
  if (Unknown || Size == 0)
    return;
  if (Address.ValueKind == RegisterValue::Kind::SymbolicAddress) {
    const auto Bounds = addressValueRange(Address, Addresses);
    Address = Bounds ? unsignedRangeValue(*Bounds) : RegisterValue{};
  }
  if (Address.ValueKind == RegisterValue::Kind::UnsignedRange) {
    const auto Bounds = *addressValueRange(Address);
    auto Range = llvm::ConstantRange::getNonEmpty(
        llvm::APInt(64, Bounds.Lower), llvm::APInt(64, Bounds.Upper + 1));
    if (Size > 1)
      Range = Range.add(
          llvm::ConstantRange(llvm::APInt(64, 0), llvm::APInt(64, Size)));
    for (auto &Write : Writes)
      if (Write.Address.ValueKind == RegisterValue::Kind::UnsignedRange) {
        const auto Previous = *addressValueRange(Write.Address);
        Range = Range.unionWith(llvm::ConstantRange::getNonEmpty(
            llvm::APInt(64, Previous.Lower),
            llvm::APInt(64, Previous.Upper + 1)));
        Write.Address =
            unsignedRangeValue({Range.getLower().getZExtValue(),
                                Range.getUpper().getZExtValue() - 1});
        Write.Size = 1;
        if (Range.isFullSet()) {
          Unknown = true;
          Writes.clear();
        }
        return;
      }
    if (Range.isFullSet()) {
      Unknown = true;
      Writes.clear();
    } else {
      Writes.push_back(
          {unsignedRangeValue({Range.getLower().getZExtValue(),
                               Range.getUpper().getZExtValue() - 1}),
           1});
    }
    return;
  }
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
  if (isOwnOrChildFrame(Address, Program)) {
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
                            const SBFProgram &Program,
                            const SymbolicAddressSpace *Addresses) {
  if (Callee.Unknown) {
    Unknown = true;
    Writes.clear();
    return;
  }
  WritesFrames |= Callee.WritesFrames;
  for (const auto &Write : Callee.Writes)
    addWrite(substituteOrigin(Write.Address, Entry, Program), Write.Size,
             Program, Addresses);
}

bool MemoryEffects::invalidatesEntryFrame(const RegisterValue &Address,
                                          const SBFProgram &Program) const {
  return WritesFrames && isOwnOrChildFrame(Address, Program);
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
      if (Address.ValueKind == RegisterValue::Kind::UnsignedRange ||
          Address.ValueKind == RegisterValue::Kind::SymbolicAddress) {
        Scratch.Values.invalidate(Address, Write.Size);
        continue;
      }
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
  if (Size == 0)
    return;
  const bool Absolute = Address.ValueKind == RegisterValue::Kind::Constant;
  int64_t End = 0;
  if ((Absolute &&
       Size > std::numeric_limits<uint64_t>::max() - Address.Value) ||
      (!Absolute &&
       ((Address.ValueKind != RegisterValue::Kind::StackAddress &&
         Address.ValueKind != RegisterValue::Kind::SymbolicAddress) ||
        Size > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
        llvm::AddOverflow(Address.Offset, static_cast<int64_t>(Size), End)))) {
    clear();
    return;
  }
  llvm::erase_if(Copies, [&](const InputCopy &Copy) {
    const auto &Other = Copy.Destination;
    if (Other.ValueKind != Address.ValueKind)
      return true;
    if (Absolute)
      return Other.Value < Address.Value + Size &&
             Address.Value < Other.Value + Copy.Size;
    return Other.Value != Address.Value ||
           (Other.Offset < End &&
            Address.Offset < Other.Offset + static_cast<int64_t>(Copy.Size));
  });
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
                                 const ProvenValueSet &Value) {
  invalidate(Address, Size);
  int64_t End;
  if (Size == sizeof(uint64_t) &&
      (((Address.ValueKind == RegisterValue::Kind::StackAddress ||
         Address.ValueKind == RegisterValue::Kind::SymbolicAddress) &&
        !llvm::AddOverflow(Address.Offset, static_cast<int64_t>(Size), End)) ||
       (Address.ValueKind == RegisterValue::Kind::Constant &&
        isScratchRange(Address.Value, Size))) &&
      !Value.unknown() &&
      std::all_of(Value.values().begin(), Value.values().end(),
                  [](const auto &Alternative) {
                    return Alternative.ValueKind ==
                               RegisterValue::Kind::Constant ||
                           Alternative.ValueKind ==
                               RegisterValue::Kind::StackAddress ||
                           Alternative.ValueKind ==
                               RegisterValue::Kind::UnsignedRange ||
                           Alternative.ValueKind ==
                               RegisterValue::Kind::SymbolicAddress;
                  }))
    Values[{Address.ValueKind, Address.Value, Address.Offset}] = Value;
}

ProvenValueSet MemoryPostconditions::read(const RegisterValue &Address) const {
  const auto Found =
      Values.find({Address.ValueKind, Address.Value, Address.Offset});
  return Found == Values.end() ? ProvenValueSet{} : Found->second;
}

void MemoryPostconditions::copyFrom(const RegisterValue &Source,
                                    const RegisterValue &Destination,
                                    uint64_t Size, const SBFProgram &Program) {
  std::vector<std::pair<RegisterValue, ProvenValueSet>> Copied;
  for (const auto &[Cell, Value] : Values) {
    const auto &[Kind, Base, Offset] = Cell;
    if (Kind != Source.ValueKind ||
        (Kind != RegisterValue::Kind::Constant && Base != Source.Value))
      continue;
    const uint64_t Delta = Kind == RegisterValue::Kind::Constant
                               ? Base - Source.Value
                               : static_cast<uint64_t>(Offset) -
                                     static_cast<uint64_t>(Source.Offset);
    if (Delta >= Size || sizeof(uint64_t) > Size - Delta || Delta > INT64_MAX)
      continue;
    RegisterState Registers{};
    Registers[0] = Destination;
    Copied.emplace_back(substituteOrigin({RegisterValue::Kind::StackAddress, 1,
                                          static_cast<int64_t>(Delta)},
                                         Registers, Program),
                        Value);
  }
  invalidate(Destination, Size);
  for (const auto &[Address, Value] : Copied)
    if (Address.ValueKind == RegisterValue::Kind::Constant ||
        Address.ValueKind == RegisterValue::Kind::StackAddress ||
        Address.ValueKind == RegisterValue::Kind::SymbolicAddress)
      Values[{Address.ValueKind, Address.Value, Address.Offset}] = Value;
}

void MemoryPostconditions::recordInputCopy(const RegisterValue &Source,
                                           const RegisterValue &Destination,
                                           uint64_t Size,
                                           const MemoryEffects &Before) {
  const auto ValidAddress = [Size](const RegisterValue &Address) {
    if (Address.ValueKind == RegisterValue::Kind::Constant)
      return Size <= UINT64_MAX - Address.Value;
    int64_t End;
    return (Address.ValueKind == RegisterValue::Kind::StackAddress ||
            Address.ValueKind == RegisterValue::Kind::SymbolicAddress) &&
           Size <= static_cast<uint64_t>(INT64_MAX) &&
           !llvm::AddOverflow(Address.Offset, static_cast<int64_t>(Size), End);
  };
  if (Before.Unknown || Size == 0 || Size > kMaxModeledScratchBytes ||
      !ValidAddress(Source) || !ValidAddress(Destination))
    return;
  const InputCopy Copy{Source, Destination, Size, Before};
  if (std::find(Copies.begin(), Copies.end(), Copy) == Copies.end())
    Copies.push_back(Copy);
}

void MemoryPostconditions::meet(const MemoryPostconditions &Other) {
  llvm::erase_if(Copies, [&](const InputCopy &Copy) {
    return std::find(Other.Copies.begin(), Other.Copies.end(), Copy) ==
           Other.Copies.end();
  });
  for (auto It = Values.begin(); It != Values.end();) {
    const auto Found = Other.Values.find(It->first);
    if (Found != Other.Values.end())
      It->second.join(Found->second);
    if (Found == Other.Values.end() || It->second.unknown())
      It = Values.erase(It);
    else
      ++It;
  }
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

void MemoryPostconditions::include(
    const MemoryPostconditions &Callee,
    const std::array<ProvenValueSet, kRegisterCount> &Entry,
    const SBFProgram &Program, const MemoryEffects *Before,
    const SymbolicAddressSpace *Addresses) {
  for (const auto &Copy : Callee.Copies) {
    RegisterState Actual{};
    for (unsigned R = 0; R < kRegisterCount; ++R)
      Actual[R] = Entry[R].singleton();
    MemoryEffects Guard = Before ? *Before : MemoryEffects{};
    Guard.include(Copy.Before, Actual, Program, Addresses);
    recordInputCopy(
        substituteOrigins(Copy.Source, Entry, Program).singleton(),
        substituteOrigins(Copy.Destination, Entry, Program).singleton(),
        Copy.Size, Guard);
  }
  for (const auto &[Cell, Value] : Callee.Values) {
    const auto &[Kind, Base, Offset] = Cell;
    const RegisterValue Address{Kind, Base, Offset};
    store(substituteOrigins(Address, Entry, Program).singleton(),
          sizeof(uint64_t), substituteOrigins(Value, Entry, Program));
  }
}

size_t MemoryPostconditions::apply(
    ScratchState &Scratch,
    const std::array<ProvenValueSet, kRegisterCount> &Entry,
    const SBFProgram &Program, const ScratchState *EntryScratch) const {
  if (EntryScratch)
    for (const auto &Copy : Copies) {
      ScratchState Source = *EntryScratch;
      Copy.Before.invalidate(Source, Entry, Program);
      const auto From =
          substituteOrigins(Copy.Source, Entry, Program).singleton();
      const auto To =
          substituteOrigins(Copy.Destination, Entry, Program).singleton();
      if (memoryAddress(From) && memoryAddress(To)) {
        const auto Destination = memoryAddress(To);
        if (Destination->ValueKind == RegisterValue::Kind::SymbolicAddress ||
            isScratchRange(Destination->Value, Copy.Size))
          Scratch.Values.copyFrom(Source.Values, From, To, Copy.Size);
        else
          Scratch.clear();
      }
    }
  MedInstruction Store;
  Store.Op = Operation::Store;
  Store.Form = OperandForm::StoreReg;
  Store.Dst = 0;
  Store.Src = 1;
  Store.Width = Store.Semantics.MemoryWidth = kDoubleWordBitWidth;
  size_t Applied = 0;
  for (const auto &[Cell, Value] : Values) {
    const auto &[Kind, Base, Offset] = Cell;
    const auto Address =
        substituteOrigins(RegisterValue{Kind, Base, Offset}, Entry, Program)
            .singleton();
    const auto Stored = substituteOrigins(Value, Entry, Program);
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
    if (!Scratch.Values.readValues(Address).unknown())
      ++Applied;
  }
  return Applied;
}

void MemoryPostconditions::transformSymbols(
    llvm::function_ref<RegisterValue(RegisterValue)> Map) {
  llvm::erase_if(Copies, [&](InputCopy &Copy) {
    if (Copy.Source.ValueKind == RegisterValue::Kind::SymbolicAddress)
      Copy.Source = Map(Copy.Source);
    if (Copy.Destination.ValueKind == RegisterValue::Kind::SymbolicAddress)
      Copy.Destination = Map(Copy.Destination);
    return (Copy.Source.ValueKind != RegisterValue::Kind::Constant &&
            Copy.Source.ValueKind != RegisterValue::Kind::StackAddress &&
            Copy.Source.ValueKind != RegisterValue::Kind::SymbolicAddress) ||
           (Copy.Destination.ValueKind != RegisterValue::Kind::Constant &&
            Copy.Destination.ValueKind != RegisterValue::Kind::StackAddress &&
            Copy.Destination.ValueKind != RegisterValue::Kind::SymbolicAddress);
  });
  decltype(Values) Transformed;
  for (const auto &[Cell, Value] : Values) {
    const auto &[Kind, Base, Offset] = Cell;
    RegisterValue Address{Kind, Base, Offset};
    if (Kind == RegisterValue::Kind::SymbolicAddress)
      Address = Map(Address);
    if (Address.ValueKind != RegisterValue::Kind::Constant &&
        Address.ValueKind != RegisterValue::Kind::StackAddress &&
        Address.ValueKind != RegisterValue::Kind::SymbolicAddress)
      continue;
    std::optional<ProvenValueSet> Stored;
    for (auto Alternative : Value.values()) {
      if (Alternative.ValueKind == RegisterValue::Kind::SymbolicAddress)
        Alternative = Map(Alternative);
      if (!Stored)
        Stored = Alternative;
      else
        Stored->join(Alternative);
    }
    if (Stored && !Stored->unknown())
      Transformed[{Address.ValueKind, Address.Value, Address.Offset}] = *Stored;
  }
  Values = std::move(Transformed);
}

uint64_t MemoryPostconditions::retainedByteEstimate() const {
  uint64_t Allocated = Copies.capacity() * sizeof(InputCopy);
  for (const auto &Copy : Copies)
    Allocated += Copy.Before.Writes.capacity() * sizeof(MemoryWrite);
  for (const auto &[Cell, Value] : Values)
    Allocated += Value.allocatedBytes();
  return Allocated + Values.size() * (sizeof(decltype(Values)::value_type) +
                                      4 * sizeof(void *));
}

} // namespace neverd::sbf::callx_detail
