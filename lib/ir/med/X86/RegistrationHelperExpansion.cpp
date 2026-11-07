//===- RegistrationHelperExpansion.cpp - Physical helper effects --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Decode checked shared helpers with exact physical stack and scratch effects.
//===----------------------------------------------------------------------===//

#include "RegistrationHelperExpansion.h"

#include "neverd/Limits.h"
#include "neverd/decode/Decoder.h"
#include "neverd/decode/InstructionRelocations.h"
#include "neverd/lift/LiftCommon.h"
#include "neverd/lift/X86/X86Regs.h"

#include <algorithm>
#include <map>

namespace neverd::x86_registration {

std::optional<uint64_t> nextRegistrationHelperTemp(const LowFunc &Source) {
  uint64_t Next = 0;
  size_t Work = 0;
  const auto Reserve = [&](const NdVar &Value) {
    if (++Work > limits::kMaxRegistrationEHStateWork)
      return false;
    if (!Value.isTemp())
      return true;
    const uint64_t Bytes = std::max<uint64_t>(Value.Size, 1);
    if (Value.Offset < TmpBase || Value.Offset > UINT64_MAX - Bytes)
      return false;
    const uint64_t End = Value.Offset + Bytes - TmpBase;
    Next = std::max(Next, End / TmpStride + bool(End % TmpStride));
    return true;
  };
  for (const auto &Block : Source.Blocks)
    for (const auto &Op : Block.Ops) {
      if (!Reserve(Op.Output))
        return std::nullopt;
      for (unsigned I = 0; I != Op.NumInputs; ++I)
        if (!Reserve(Op.Inputs[I]))
          return std::nullopt;
    }
  return Next;
}

std::optional<std::pair<std::vector<LowOp>, uint64_t>>
expandRegistrationHelper(const BinaryImage &Image, va_t SourceEntry,
                         va_t CallVA, va_t ReturnVA,
                         ExceptionAddressRange Helper, uint64_t TempId) {
  if (!Helper.isValid() ||
      Helper.size() > limits::kMaxRegistrationEHStateWork ||
      CallVA > UINT32_MAX - 5 || ReturnVA != CallVA + 5)
    return std::nullopt;
  Decoder Dec;
  if (!Dec.init(Image))
    return std::nullopt;
  Dec.setStrict(true);
  auto Lift = [&](va_t Address, va_t End,
                  std::vector<LowOp> &Ops) -> std::optional<uint16_t> {
    const auto *Bytes = Image.readVA(Address, End - Address);
    DecodedInsn Insn;
    if (!Bytes || !Dec.decodeOne(Bytes, End - Address, Address, Insn))
      return std::nullopt;
    const auto Relocations = instructionRelocations(Image, Insn);
    try {
      Dec.liftToLow(Insn, Ops, Relocations.Addresses, Relocations.Scalars);
    } catch (const UnliftedInstruction &) {
      return std::nullopt;
    }
    return Insn.Size;
  };
  std::vector<LowOp> Ops;
  LiftStateBase S(CallVA, 5, Ops);
  S.TmpId = TempId;
  const auto SP = NdVar::reg(x86reg::RSP, 4);
  S.emit(NdOp::INT_SUB, SP, {SP, NdVar::scalar(4, 4)});
  S.emit(NdOp::STORE, {}, {SP, NdVar::codeAddress(ReturnVA, 4, SourceEntry)});
  va_t Address = Helper.Begin;
  bool Returned = false;
  size_t Work = 0;
  while (Address < Helper.End) {
    if (++Work > limits::kMaxRegistrationEHStateWork)
      return std::nullopt;
    std::vector<LowOp> Body;
    const auto Size = Lift(Address, Helper.End, Body);
    if (!Size)
      return std::nullopt;
    Address += *Size;
    if (Body.size() == 1 && Body.front().Opcode == NdOp::RETURN &&
        Address == Helper.End) {
      // The ordinary RET lift omits its physical PC pop, just as CALL omits
      // the push above. Keep both here when composing the checked helper.
      S.emit(NdOp::INT_ADD, SP, {SP, NdVar::scalar(4, 4)});
      Returned = true;
      break;
    }
    // Fresh scratch storage per decoded helper instruction must not clobber
    // the source body's temporaries or another expanded helper occurrence.
    std::map<uint64_t, NdVar> Temps;
    auto Remap = [&](NdVar &Value) {
      if (!Value.isTemp() || !Value.Size)
        return true;
      auto It = Temps.find(Value.Offset);
      if (It == Temps.end()) {
        const uint64_t Slots =
            (uint64_t(Value.Size) + TmpStride - 1) / TmpStride;
        constexpr uint64_t Last = (UINT64_MAX - TmpBase) / TmpStride;
        if (S.TmpId > Last || Slots - 1 > Last - S.TmpId)
          return false;
        It = Temps.emplace(Value.Offset, S.makeTemp(Value.Size)).first;
      }
      if (It->second.Size != Value.Size)
        return false;
      Value = It->second;
      return true;
    };
    for (auto &Effect : Body) {
      if (++Work > limits::kMaxRegistrationEHStateWork ||
          Effect.Opcode == NdOp::RETURN || Effect.Opcode == NdOp::CALL ||
          Effect.Opcode == NdOp::INDIR_CALL || Effect.Opcode == NdOp::BRANCH ||
          Effect.Opcode == NdOp::COND_BR || Effect.Opcode == NdOp::INDIR_BR ||
          !Remap(Effect.Output))
        return std::nullopt;
      for (unsigned I = 0; I != Effect.NumInputs; ++I)
        if (!Remap(Effect.Inputs[I]))
          return std::nullopt;
      Effect.Addr = CallVA;
      Effect.Seq = S.Seq++;
      Ops.push_back(std::move(Effect));
    }
  }
  if (!Returned)
    return std::nullopt;
  return std::make_pair(std::move(Ops), S.TmpId);
}

} // namespace neverd::x86_registration
