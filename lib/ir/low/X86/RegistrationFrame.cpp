//===- RegistrationFrame.cpp - x86 EH frame value transfer ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationFrame.h"

#include "RegistrationFrameEffects.h"

#include "neverd/ir/RegistrationState.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/low/LowScalarConstant.h"
#include "neverd/ir/low/X86ArithmeticFlags.h"
#include "neverd/lift/X86Regs.h"

#include <algorithm>

namespace neverd::registration_state {

bool hasX86ScalarCallOutput(const LowOp &Op) {
  return Op.Opcode == NdOp::CALL && Op.Output.Size == 4 &&
         (Op.Output.isTemp() ||
          (Op.Output.isReg() && Op.Output.Offset == x86reg::RAX));
}

bool isX86VolatileFrameRegister(unsigned Index) {
  return Index == x86reg::RAX / x86reg::GeneralRegStride ||
         Index == x86reg::RCX / x86reg::GeneralRegStride ||
         Index == x86reg::RDX / x86reg::GeneralRegStride;
}

FrameValue projectX86CallReturnValue(const FrameValue &Value) {
  if (Value == FrameValue{} ||
      (Value.Constant && Value == FrameValue::constant(*Value.Constant)) ||
      (Value.ImageAddress &&
       Value == FrameValue::imageAddress(*Value.ImageAddress)))
    return Value;
  auto Opaque = FrameValue::taint(true);
  Opaque.FrameOnlyFromCall = true;
  return Opaque;
}

FrameValue join(FrameValue Left, const FrameValue &Right) {
  if (Left == Right)
    return Left;
  const bool MayBeFrame = Left.MayBeFrame || Right.MayBeFrame;
  FrameValue Result{{},
                    {},
                    false,
                    MayBeFrame,
                    MayBeFrame &&
                        (!Left.MayBeFrame || Left.FrameOnlyFromCall) &&
                        (!Right.MayBeFrame || Right.FrameOnlyFromCall)};
  Result.FrameOnlyFromSEHScalar =
      MayBeFrame && (!Left.MayBeFrame || Left.FrameOnlyFromSEHScalar) &&
      (!Right.MayBeFrame || Right.FrameOnlyFromSEHScalar);
  return Result;
}

namespace {
bool mergeCells(FrameCells &Snapshot, const FrameCells &Other) {
  if (Snapshot.empty() && Other.empty())
    return false;
  // Shared snapshots are already equal. Explicit empty cells still need the
  // canonical erasure below; equality alone must not retain those dead facts.
  if (Snapshot == Other &&
      std::none_of(Snapshot.begin(), Snapshot.end(), [](const auto &Cell) {
        return Cell.second == FrameValue{};
      }))
    return false;
  bool Changed = false;
  Snapshot.edit([&](FrameCells::Map &Cells) {
    for (auto Cell = Cells.begin(); Cell != Cells.end();) {
      const auto It = Other.find(Cell->first);
      const FrameValue Merged =
          join(Cell->second, It == Other.end() ? FrameValue{} : It->second);
      Changed |= Merged != Cell->second;
      // An absent cell already means no exact value and no frame taint. Keep
      // that canonical representation: dead local values must not accumulate
      // in every subsequent CFG state or consume the proof work budget.
      if (Merged == FrameValue{})
        Cell = Cells.erase(Cell);
      else {
        Cell->second = Merged;
        ++Cell;
      }
    }
    for (const auto &[Offset, Value] : Other)
      if (!Cells.count(Offset)) {
        const FrameValue Merged = join({}, Value);
        if (Merged != FrameValue{}) {
          Cells.emplace(Offset, Merged);
          Changed = true;
        }
      }
  });
  return Changed;
}

} // namespace

bool FrameState::merge(const FrameState &Other) {
  bool Changed = ImportBindingsIntact && !Other.ImportBindingsIntact;
  ImportBindingsIntact &= Other.ImportBindingsIntact;
  for (size_t I = 0; I < Registers.size(); ++I) {
    const FrameValue Merged = join(Registers[I], Other.Registers[I]);
    Changed |= Merged != Registers[I];
    Registers[I] = Merged;
  }
  const FrameValue Default = OtherRegistersMayBeFrame
                                 ? FrameValue{{}, {}, false, true, true}
                                 : FrameValue{};
  const FrameValue OtherDefault = Other.OtherRegistersMayBeFrame
                                      ? FrameValue{{}, {}, false, true, true}
                                      : FrameValue{};
  for (auto &[Offset, Value] : OtherRegisterBytes)
    if (!Other.OtherRegisterBytes.count(Offset)) {
      const auto Merged = join(Value, OtherDefault);
      Changed |= Value != Merged;
      Value = Merged;
    }
  for (const auto &[Offset, Value] : Other.OtherRegisterBytes) {
    auto [It, New] = OtherRegisterBytes.emplace(Offset, join(Default, Value));
    if (New)
      Changed = true;
    else {
      const auto Merged = join(It->second, Value);
      Changed |= It->second != Merged;
      It->second = Merged;
    }
  }
  Changed |= Other.OtherRegistersMayBeFrame && !OtherRegistersMayBeFrame;
  OtherRegistersMayBeFrame |= Other.OtherRegistersMayBeFrame;
  Changed |= mergeCells(Cells, Other.Cells);
  Changed |= mergeCells(EntryCells, Other.EntryCells);
  Changed |= mergeCells(CallbackCells, Other.CallbackCells);
  for (auto It = InitializedCallbackBytes.begin();
       It != InitializedCallbackBytes.end();)
    if (!Other.InitializedCallbackBytes.count(*It)) {
      It = InitializedCallbackBytes.erase(It);
      Changed = true;
    } else
      ++It;
  if (CallbackEntry != Other.CallbackEntry && CallbackEntry) {
    CallbackEntry.reset();
    Changed = true;
  }
  if (SEHFilterEntry != Other.SEHFilterEntry && SEHFilterEntry) {
    SEHFilterEntry.reset();
    Changed = true;
  }
  return Changed;
}

void FrameState::forgetCellValues() {
  for (auto *Space : {&Cells, &EntryCells, &CallbackCells})
    if (std::any_of(Space->begin(), Space->end(), [](const auto &Cell) {
          return Cell.second == FrameValue{} ||
                 join(Cell.second, {}) != Cell.second;
        }))
      Space->edit([](FrameCells::Map &Cells) {
        for (auto It = Cells.begin(); It != Cells.end();) {
          const auto Value = join(It->second, {});
          if (Value == FrameValue{})
            It = Cells.erase(It);
          else {
            It->second = Value;
            ++It;
          }
        }
      });
}

void FrameTransfer::beginInstruction(va_t Address) {
  if (Address != Instruction) {
    Temps.clear();
    Instruction = Address;
  }
}

FrameValue FrameTransfer::read(const NdVar &Value) const {
  if (Value.isConst() && isAddressProvenance(Value.Provenance))
    return isExactAddressProvenance(Value.Provenance) && Value.Size == 4 &&
                   Value.Offset <= UINT32_MAX
               ? FrameValue::imageAddress(uint32_t(Value.Offset))
               : FrameValue{};
  if (Value.isConst())
    return FrameValue::constant(static_cast<uint32_t>(Value.Offset));
  if (Value.isReg() && Value.Offset < 8 * x86reg::GeneralRegStride) {
    const FrameValue &Register =
        State.Registers[Value.Offset / x86reg::GeneralRegStride];
    if (Value.Offset % x86reg::GeneralRegStride == 0 && Value.Size >= 4)
      return Register;
    const auto Byte = Value.Offset % x86reg::GeneralRegStride;
    if (Register.Constant && !Register.MayBeFrame && Value.Size &&
        Byte + Value.Size <= 4)
      return FrameValue::constant((*Register.Constant >> (Byte * 8)) &
                                  (UINT32_MAX >> ((4 - Value.Size) * 8)));
    return FrameValue::taint(Register.MayBeFrame,
                             Register.FrameOnlyFromSEHScalar);
  }
  if (Value.isReg()) {
    FrameValue Result;
    std::optional<uint32_t> Constant = Value.Size && Value.Size <= 4
                                           ? std::optional<uint32_t>{0}
                                           : std::nullopt;
    for (uint64_t Byte = 0; Byte < Value.Size; ++Byte)
      if (auto It = State.OtherRegisterBytes.find(Value.Offset + Byte);
          It != State.OtherRegisterBytes.end()) {
        Result = join(Result, It->second);
        if (Constant && It->second.Constant && !It->second.MayBeFrame)
          *Constant |= (*It->second.Constant & UINT8_MAX) << (Byte * 8);
        else
          Constant.reset();
      } else {
        Constant.reset();
        if (State.OtherRegistersMayBeFrame)
          Result = join(Result, {{}, {}, false, true, true});
      }
    if (Constant && !Result.MayBeFrame)
      return FrameValue::constant(*Constant);
    return Result;
  }
  if (Value.isTemp()) {
    auto It = Temps.find(Value.Offset);
    if (It != Temps.end()) {
      if ((It->second.SEHException || It->second.ThreadAddress) &&
          Value.Size < 4)
        return FrameValue::taint(It->second.MayBeFrame);
      return It->second;
    }
  }
  return {};
}

void FrameTransfer::invalidateImportBindings(
    const LowOp &Op, const std::set<va_t> &ImportSlots) {
  if (!State.ImportBindingsIntact || ImportSlots.empty())
    return;
  const auto Memory = lowMemoryOperands(Op);
  if (!Memory.Address || !Memory.StoredValue)
    return;
  const auto Address = read(*Memory.Address);
  if (Op.MemoryAddressSpace != NdMemoryAddressSpace::Default) {
    // Only the exact PE32 exception-chain word is known to be outside the
    // image. Other segment-relative stores have no proved IAT alias bound.
    if (Op.MemoryAddressSpace != NdMemoryAddressSpace::X86FS ||
        Address.Constant != uint32_t{0} || Memory.AccessSize != 4)
      State.ImportBindingsIntact = false;
    return;
  }
  if (Address.Offset || Address.EntryOffset || Address.CallbackAddress)
    return;
  const auto VA = Address.preferredAddress();
  if (!VA) {
    State.ImportBindingsIntact = false;
    return;
  }
  for (va_t Slot : ImportSlots)
    if (uint64_t(*VA) < Slot + 4 && Slot < uint64_t(*VA) + Memory.AccessSize) {
      State.ImportBindingsIntact = false;
      return;
    }
}

FrameValue FrameTransfer::evaluate(const LowOp &Op, bool Installed,
                                   std::optional<va_t> ImportLoad) const {
  FrameValue Result;
  for (unsigned I = 0; I < Op.NumInputs; ++I) {
    const FrameValue Input = read(Op.Inputs[I]);
    if (Input.MayBeFrame) {
      Result.FrameOnlyFromCall =
          (!Result.MayBeFrame || Result.FrameOnlyFromCall) &&
          Input.FrameOnlyFromCall;
      Result.FrameOnlyFromSEHScalar =
          (!Result.MayBeFrame || Result.FrameOnlyFromSEHScalar) &&
          Input.FrameOnlyFromSEHScalar;
      Result.MayBeFrame = true;
    }
  }
  if (auto Relative = evaluateRelativeScalar(Op))
    return *Relative;
  if (Op.Output.Size <= 4)
    if (const auto Constant = lowScalarConstantResult(
            Op, [&](const NdVar &V) -> std::optional<uint64_t> {
              if (!V.Size || V.Size > 4)
                return std::nullopt;
              const auto Value = read(V);
              if (Value.MayBeFrame || Value.SecurityCookie || Value.ReturnPC)
                return std::nullopt;
              return Value.Constant;
            }))
      return FrameValue::constant(uint32_t(*Constant));
  if ((Op.Opcode == NdOp::COPY || Op.Opcode == NdOp::INT_ZEXT) &&
      Op.NumInputs == 1) {
    const auto Input = read(Op.Inputs[0]);
    if (Op.Output.Size >= 4 && Op.Inputs[0].Size >= 4)
      return Input;
    return FrameValue::taint(Input.MayBeFrame, Input.FrameOnlyFromSEHScalar);
  }
  if (Op.Opcode == NdOp::SUBBYTES && Op.NumInputs == 2 &&
      Op.Inputs[1].isConst() && Op.Inputs[1].Offset == 0 && Op.Output.Size == 4)
    return read(Op.Inputs[0]);
  if (Op.Opcode == NdOp::LOAD && Op.NumInputs == 1) {
    if (ImportLoad && *ImportLoad <= UINT32_MAX && State.ImportBindingsIntact &&
        Op.Output.Size == 4 &&
        Op.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
        Op.MemoryOrdering == NdMemoryOrdering::None)
      return FrameValue::importTarget(uint32_t(*ImportLoad));
    const FrameValue Address = read(Op.Inputs[0]);
    using Thread = RegistrationSEHThreadAccess;
    if (Op.MemoryAddressSpace == NdMemoryAddressSpace::X86FS &&
        Address.Constant == Thread::SelfOffset &&
        Thread::isAddressWidth(Op.Inputs[0].Size) &&
        Op.Output.Size == Thread::WordBytes &&
        Op.MemoryOrdering == NdMemoryOrdering::None && Op.Seq >= 0)
      return FrameValue::threadBlock({Op.Addr, Op.Seq, 0});
    if (Address.ThreadAddress) {
      if (Op.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
          Address.ThreadAddress->Offset == Thread::StackBaseOffset &&
          Thread::isAddressWidth(Op.Inputs[0].Size) &&
          Op.Output.Size == Thread::WordBytes &&
          Op.MemoryOrdering == NdMemoryOrdering::None)
        return FrameValue::taint(true, true);
      return FrameValue::taint(true);
    }
    if (Address.SEHException) {
      if (Op.MemoryAddressSpace == NdMemoryAddressSpace::Default)
        if (const auto Value =
                State.sehExceptionLoad(*Address.SEHException, Op.Output.Size))
          return *Value;
      return {{}, {}, false, true};
    }
    if (Op.MemoryAddressSpace == NdMemoryAddressSpace::X86FS &&
        Address.Constant == uint32_t{0} && Op.Output.Size == 4)
      return Installed ? FrameValue::frame(RegistrationOffset)
                       : FrameValue::previousChain();
    if (SecurityCookieVA &&
        Op.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
        Address.preferredAddress() == SecurityCookieVA && Op.Output.Size == 4) {
      FrameValue Cookie;
      Cookie.SecurityCookie = true;
      return Cookie;
    }
    if (Op.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
        Address.MayBeFrame) {
      if (Address.CallbackAddress &&
          Address.CallbackAddress->Entry == State.CallbackEntry)
        return State.loadCallback(Address.CallbackAddress->Offset,
                                  Op.Output.Size);
      return Address.EntryOffset && *Address.EntryOffset >= -12
                 ? State.loadEntry(*Address.EntryOffset, Op.Output.Size)
                 : State.load(Address.Offset, Op.Output.Size);
    }
    return {};
  }
  if (auto RealignedValue = evaluateRealignment(Op))
    return *RealignedValue;
  if (Op.Opcode == NdOp::INT_XOR && Op.NumInputs == 2 && Op.Output.Size == 4 &&
      Op.Inputs[0].Size == 4 && Op.Inputs[1].Size == 4) {
    auto Left = read(Op.Inputs[0]);
    auto Right = read(Op.Inputs[1]);
    if (!Left.SecurityCookie && Right.SecurityCookie)
      std::swap(Left, Right);
    if (Left.SecurityCookie) {
      if (Right.SecurityCookie &&
          Left.CookieFrameOffset == Right.CookieFrameOffset &&
          Left.CookieImageXOR == Right.CookieImageXOR)
        return FrameValue::constant(Left.CookieXOR ^ Right.CookieXOR);
      if (Right.ImageAddress) {
        if (!Left.CookieImageXOR)
          Left.CookieImageXOR = Right.ImageAddress;
        else if (Left.CookieImageXOR == Right.ImageAddress)
          Left.CookieImageXOR.reset();
        else
          return Result;
        return Left;
      }
      if (Right.Constant) {
        Left.CookieXOR ^= *Right.Constant;
        return Left;
      }
      if (Right.Offset && !Left.CookieFrameOffset) {
        Left.CookieFrameOffset = Right.Offset;
        Left.MayBeFrame = true;
        return Left;
      }
      if (Right.Offset && Left.CookieFrameOffset == Right.Offset) {
        Left.CookieFrameOffset.reset();
        Left.MayBeFrame = false;
        return Left;
      }
    }
    if (Left.Constant && Right.Constant)
      return FrameValue::constant(*Left.Constant ^ *Right.Constant);
  }
  if ((Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB) &&
      Op.NumInputs == 2) {
    const FrameValue Left = read(Op.Inputs[0]);
    const FrameValue Right = read(Op.Inputs[1]);
    if (Op.Output.Size == 4 && Op.Inputs[0].Size == 4 &&
        Op.Inputs[1].Size == 4) {
      if (Left.ImageAddress && Right.Constant)
        return FrameValue::imageAddress(
            *Left.ImageAddress +
            (Op.Opcode == NdOp::INT_ADD ? *Right.Constant : -*Right.Constant));
      if (Op.Opcode == NdOp::INT_ADD && Left.Constant && Right.ImageAddress)
        return FrameValue::imageAddress(*Left.Constant + *Right.ImageAddress);
    }
    auto AddExceptionOffset = [&](const FrameValue &Base,
                                  int64_t Delta) -> std::optional<FrameValue> {
      if (!Base.SEHException || Op.Output.Size < 4)
        return std::nullopt;
      auto Address = *Base.SEHException;
      const int64_t Offset = int64_t(Address.Offset) + Delta;
      if (Offset < 0 || Offset > 4)
        return FrameValue{{}, {}, false, true};
      Address.Offset = int32_t(Offset);
      return FrameValue::sehException(Address);
    };
    auto AddThreadOffset = [&](const FrameValue &Base, int64_t Delta) {
      if (Op.Output.Size != 4 || Op.Inputs[0].Size != 4 ||
          Op.Inputs[1].Size != 4)
        return FrameValue{};
      auto Address = *Base.ThreadAddress;
      const int64_t Offset = int64_t(Address.Offset) + Delta;
      if (Offset < INT32_MIN || Offset > INT32_MAX)
        return FrameValue{};
      Address.Offset = int32_t(Offset);
      return FrameValue::threadBlock(Address);
    };
    if (Left.ThreadAddress && Right.Constant)
      return AddThreadOffset(Left, Op.Opcode == NdOp::INT_ADD
                                       ? int64_t(int32_t(*Right.Constant))
                                       : -int64_t(int32_t(*Right.Constant)));
    if (Op.Opcode == NdOp::INT_ADD && Left.Constant && Right.ThreadAddress)
      return AddThreadOffset(Right, int32_t(*Left.Constant));
    if (Left.SEHException && Right.Constant)
      if (auto Value = AddExceptionOffset(
              Left, Op.Opcode == NdOp::INT_ADD
                        ? int64_t(int32_t(*Right.Constant))
                        : -int64_t(int32_t(*Right.Constant))))
        return *Value;
    if (Op.Opcode == NdOp::INT_ADD && Left.Constant && Right.SEHException)
      if (auto Value = AddExceptionOffset(Right, int32_t(*Left.Constant)))
        return *Value;
    if (Left.CallbackAddress && Right.Constant)
      return FrameValue::callbackFrame(
          Left.CallbackAddress->Entry,
          int32_t(uint32_t(Left.CallbackAddress->Offset) +
                  (Op.Opcode == NdOp::INT_ADD ? *Right.Constant
                                              : -*Right.Constant)));
    if (Op.Opcode == NdOp::INT_ADD && Left.Constant && Right.CallbackAddress)
      return FrameValue::callbackFrame(
          Right.CallbackAddress->Entry,
          int32_t(*Left.Constant + uint32_t(Right.CallbackAddress->Offset)));
    if (Left.EntryOffset && Right.Constant)
      return FrameValue::entryFrame(static_cast<int32_t>(
          uint32_t(*Left.EntryOffset) +
          (Op.Opcode == NdOp::INT_ADD ? *Right.Constant : -*Right.Constant)));
    if (Op.Opcode == NdOp::INT_ADD && Left.Constant && Right.EntryOffset)
      return FrameValue::entryFrame(
          static_cast<int32_t>(*Left.Constant + uint32_t(*Right.EntryOffset)));
    if (Left.Offset && Right.Constant) {
      auto Result = FrameValue::frame(static_cast<int32_t>(
          uint32_t(*Left.Offset) +
          (Op.Opcode == NdOp::INT_ADD ? *Right.Constant : -*Right.Constant)));
      Result.ExceptionObject = Left.ExceptionObject;
      return Result;
    }
    if (Op.Opcode == NdOp::INT_ADD && Left.Constant && Right.Offset) {
      auto Result = FrameValue::frame(
          static_cast<int32_t>(*Left.Constant + uint32_t(*Right.Offset)));
      Result.ExceptionObject = Right.ExceptionObject;
      return Result;
    }
    if (Left.Constant && Right.Constant)
      return FrameValue::constant(Op.Opcode == NdOp::INT_ADD
                                      ? *Left.Constant + *Right.Constant
                                      : *Left.Constant - *Right.Constant);
  }
  return Result;
}

bool FrameTransfer::isCookieCheck(const LowOp &Op, va_t CookieCheckVA) const {
  const auto &Cookie = State.Registers[x86reg::RCX / x86reg::GeneralRegStride];
  return CookieCheckVA && Op.Opcode == NdOp::CALL && Op.NumInputs == 1 &&
         Op.Inputs[0].isConst() && Op.Inputs[0].Size == 4 &&
         Op.Inputs[0].Offset == CookieCheckVA && Cookie.SecurityCookie &&
         !Cookie.CookieFrameOffset && !Cookie.MayBeFrame &&
         !Cookie.CookieImageXOR && Cookie.CookieXOR == 0;
}

void FrameTransfer::write(const LowOp &Op, FrameValue Value, va_t CookieCheckVA,
                          bool PreserveImportedTargets,
                          bool PreserveImportBindings) {
  if (isCookieCheck(Op, CookieCheckVA)) {
    // The authenticated success path compares equal values, then returns
    // without changing another register. The generic EAX result is no write.
    for (const auto &[Flag, Bit] : kX86EqualComparisonFlags)
      State.OtherRegisterBytes[Flag] = FrameValue::constant(Bit);
    return;
  }
  if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
    State.ImportBindingsIntact &= PreserveImportBindings;
    if (!PreserveImportedTargets) {
      for (auto &Register : State.Registers)
        Register.ImportTarget.reset();
      for (auto &[Offset, Temporary] : Temps)
        Temporary.ImportTarget.reset();
    }
    State.Registers[x86reg::RAX / x86reg::GeneralRegStride] = {
        {}, {}, false, true, true};
    State.Registers[x86reg::RCX / x86reg::GeneralRegStride] = {
        {}, {}, false, true, true};
    State.Registers[x86reg::RDX / x86reg::GeneralRegStride] = {
        {}, {}, false, true, true};
    // LowIR's call alone does not prove the callee's PE32 stack-pop ABI.
    // A later explicit SP restoration can recover the frame value.
    State.Registers[x86reg::RSP / x86reg::GeneralRegStride] = {
        {}, {}, false, true};
    State.OtherRegisterBytes.clear();
    State.OtherRegistersMayBeFrame = true;
    Value = {{}, {}, false, true, true};
  }
  if (Op.Opcode == NdOp::INTRINSIC && !isFramePreservingIntrinsic(Op)) {
    State.ImportBindingsIntact = false;
    for (auto &Register : State.Registers)
      if (Register.ImportTarget)
        Register = {};
    for (auto &[Offset, Temporary] : Temps)
      if (Temporary.ImportTarget)
        Temporary = {};
  }
  if (Op.Output.isTemp())
    Temps[Op.Output.Offset] = Value;
  else if (Op.Output.isReg() &&
           Op.Output.Offset < 8 * x86reg::GeneralRegStride) {
    FrameValue &Register =
        State.Registers[Op.Output.Offset / x86reg::GeneralRegStride];
    if (Op.Output.Offset % x86reg::GeneralRegStride == 0 && Op.Output.Size >= 4)
      Register = Value;
    else {
      const auto Merged = join(Register, Value);
      Register =
          FrameValue::taint(Merged.MayBeFrame, Merged.FrameOnlyFromSEHScalar);
    }
  } else if (Op.Output.isReg()) {
    for (uint64_t Byte = 0; Byte < Op.Output.Size; ++Byte)
      if (Value.Constant && !Value.MayBeFrame && Op.Output.Size <= 4)
        State.OtherRegisterBytes[Op.Output.Offset + Byte] =
            FrameValue::constant((*Value.Constant >> (Byte * 8)) & UINT8_MAX);
      else if (Value.MayBeFrame || State.OtherRegistersMayBeFrame) {
        State.OtherRegisterBytes[Op.Output.Offset + Byte] = {
            {}, {}, false, Value.MayBeFrame, Value.FrameOnlyFromCall};
        State.OtherRegisterBytes[Op.Output.Offset + Byte]
            .FrameOnlyFromSEHScalar = Value.FrameOnlyFromSEHScalar;
      } else
        State.OtherRegisterBytes.erase(Op.Output.Offset + Byte);
  }
}

} // namespace neverd::registration_state
