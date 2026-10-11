//===- RegistrationFrame.cpp - x86 EH frame value transfer ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationFrame.h"

#include "neverd/ir/low/LowIR.h"
#include "neverd/lift/X86Regs.h"

namespace neverd::registration_state {

FrameValue join(FrameValue Left, const FrameValue &Right) {
  if (Left == Right)
    return Left;
  const bool MayBeFrame = Left.MayBeFrame || Right.MayBeFrame;
  return {{},
          {},
          false,
          MayBeFrame,
          MayBeFrame && (!Left.MayBeFrame || Left.FrameOnlyFromCall) &&
              (!Right.MayBeFrame || Right.FrameOnlyFromCall)};
}

namespace {
bool mergeCells(std::map<int32_t, FrameValue> &Cells,
                const std::map<int32_t, FrameValue> &Other) {
  bool Changed = false;
  for (auto &[Offset, Value] : Cells) {
    const auto It = Other.find(Offset);
    const FrameValue Merged =
        join(Value, It == Other.end() ? FrameValue{} : It->second);
    Changed |= Merged != Value;
    Value = Merged;
  }
  for (const auto &[Offset, Value] : Other)
    if (!Cells.count(Offset)) {
      const FrameValue Merged = join({}, Value);
      if (Merged != FrameValue{}) {
        Cells.emplace(Offset, Merged);
        Changed = true;
      }
    }
  return Changed;
}

} // namespace

bool FrameState::merge(const FrameState &Other) {
  bool Changed = false;
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
  return Changed;
}

void FrameState::forgetCellValues() {
  for (auto *Space : {&Cells, &EntryCells, &CallbackCells})
    for (auto &[Offset, Value] : *Space)
      Value = join(Value, {});
}

void FrameTransfer::beginInstruction(va_t Address) {
  if (Address != Instruction) {
    Temps.clear();
    Instruction = Address;
  }
}

FrameValue FrameTransfer::read(const NdVar &Value) const {
  if (Value.isConst())
    return FrameValue::constant(static_cast<uint32_t>(Value.Offset));
  if (Value.isReg() && Value.Offset < 8 * x86reg::GeneralRegStride) {
    const FrameValue &Register =
        State.Registers[Value.Offset / x86reg::GeneralRegStride];
    if (Value.Offset % x86reg::GeneralRegStride == 0 && Value.Size >= 4)
      return Register;
    return {{}, {}, false, Register.MayBeFrame};
  }
  if (Value.isReg()) {
    FrameValue Result;
    for (uint64_t Byte = 0; Byte < Value.Size; ++Byte)
      if (auto It = State.OtherRegisterBytes.find(Value.Offset + Byte);
          It != State.OtherRegisterBytes.end())
        Result = join(Result, It->second);
      else if (State.OtherRegistersMayBeFrame)
        Result = join(Result, {{}, {}, false, true, true});
    return Result;
  }
  if (Value.isTemp()) {
    auto It = Temps.find(Value.Offset);
    if (It != Temps.end())
      return It->second;
  }
  return {};
}

FrameValue FrameTransfer::evaluate(const LowOp &Op, bool Installed) const {
  FrameValue Result;
  for (unsigned I = 0; I < Op.NumInputs; ++I) {
    const FrameValue Input = read(Op.Inputs[I]);
    if (Input.MayBeFrame) {
      Result.FrameOnlyFromCall =
          (!Result.MayBeFrame || Result.FrameOnlyFromCall) &&
          Input.FrameOnlyFromCall;
      Result.MayBeFrame = true;
    }
  }
  if ((Op.Opcode == NdOp::COPY || Op.Opcode == NdOp::INT_ZEXT) &&
      Op.NumInputs == 1) {
    const auto Input = read(Op.Inputs[0]);
    if (Op.Output.Size >= 4 && Op.Inputs[0].Size >= 4)
      return Input;
    return {{}, {}, false, Input.MayBeFrame};
  }
  if (Op.Opcode == NdOp::SUBBYTES && Op.NumInputs == 2 &&
      Op.Inputs[1].isConst() && Op.Inputs[1].Offset == 0 && Op.Output.Size == 4)
    return read(Op.Inputs[0]);
  if (Op.Opcode == NdOp::LOAD && Op.NumInputs == 1) {
    const FrameValue Address = read(Op.Inputs[0]);
    if (Op.MemoryAddressSpace == NdMemoryAddressSpace::X86FS &&
        Address.Constant == uint32_t{0} && Op.Output.Size == 4)
      return Installed ? FrameValue::frame(RegistrationOffset)
                       : FrameValue::previousChain();
    if (SecurityCookieVA &&
        Op.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
        Address.Constant == SecurityCookieVA && Op.Output.Size == 4) {
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
          Left.CookieFrameOffset == Right.CookieFrameOffset)
        return FrameValue::constant(Left.CookieXOR ^ Right.CookieXOR);
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
         Cookie.CookieXOR == 0;
}

void FrameTransfer::write(const LowOp &Op, FrameValue Value,
                          va_t CookieCheckVA) {
  if (isCookieCheck(Op, CookieCheckVA)) {
    // The authenticated success path changes flags, but no GPR or stack
    // cell. In particular the lifter's generic EAX result is not a write.
    State.OtherRegisterBytes.clear();
    State.OtherRegistersMayBeFrame = true;
    return;
  }
  if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
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
  if (Op.Output.isTemp())
    Temps[Op.Output.Offset] = Value;
  else if (Op.Output.isReg() &&
           Op.Output.Offset < 8 * x86reg::GeneralRegStride) {
    FrameValue &Register =
        State.Registers[Op.Output.Offset / x86reg::GeneralRegStride];
    if (Op.Output.Offset % x86reg::GeneralRegStride == 0 && Op.Output.Size >= 4)
      Register = Value;
    else
      Register = {{}, {}, false, Register.MayBeFrame || Value.MayBeFrame};
  } else if (Op.Output.isReg()) {
    for (uint64_t Byte = 0; Byte < Op.Output.Size; ++Byte)
      if (Value.MayBeFrame || State.OtherRegistersMayBeFrame)
        State.OtherRegisterBytes[Op.Output.Offset + Byte] = {
            {}, {}, false, Value.MayBeFrame, Value.FrameOnlyFromCall};
      else
        State.OtherRegisterBytes.erase(Op.Output.Offset + Byte);
  }
}

} // namespace neverd::registration_state
