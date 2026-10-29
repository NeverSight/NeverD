//===- MedVariadicX86.cpp - x86 variadic prologues ------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The x86 variadic prologue recognizers: the System V x86-64 va_start
/// word with its register save area, and the i386 stack va_list home slot.
///
//===----------------------------------------------------------------------===//

#include "../MedVariadicDetail.h"

#include "neverd/Limits.h"
#include "neverd/ir/med/MedCallingConvDetail.h"

#include "llvm/ADT/SmallVector.h"

#include <map>
#include <optional>
#include <set>

namespace neverd {
namespace med_variadic_detail {

using med_calling_conv_detail::computeForwardValueClosure;
using med_calling_conv_detail::containsValue;

/// The va_start GP/FP-offset word with a parameter-register spill.
bool hasX64VariadicPrologue(VariadicScan &S, uint64_t &VaStartWord) {
  const MedFunc &Func = S.Func;
  const TargetRegInfo &TRI = S.TRI;
  bool Marked = false;
  // The va_start GP/FP-offset word identifies a variadic prologue; pairing it
  // with a parameter-register spill keeps a stray constant from qualifying.
  bool HasVaWord = false;
  // GCC may also store the word's two 32-bit fields one at a time, gp_offset
  // at the va_list and fp_offset right after it: the constant each 4-byte
  // store writes, by its entry-SP offset.
  std::map<int64_t, uint64_t> FieldStores;
  for (const auto &Blk : Func.Blocks)
    for (const auto &Op : Blk.Ops) {
      if (Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
        continue;
      for (uint8_t I = 0; I < Op.NumInputs; ++I)
        if (Op.Inputs[I].isConst() && isX64VaStartWord(Op.Inputs[I].ConstVal)) {
          HasVaWord = true;
          VaStartWord = Op.Inputs[I].ConstVal;
        }
      if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 2 &&
          Op.Inputs[1].isConst() && Op.Inputs[1].Size == sizeof(uint32_t))
        if (auto Delta = entrySpDelta(S, Op.Inputs[0], 0))
          FieldStores[*Delta] = Op.Inputs[1].ConstVal & 0xFFFFFFFFu;
    }
  for (const auto &[Delta, GpOffset] : FieldStores)
    if (auto FpOffset =
            FieldStores.find(Delta + static_cast<int64_t>(sizeof(uint32_t)));
        FpOffset != FieldStores.end() &&
        isX64VaStartWord(GpOffset | (FpOffset->second << 32))) {
      HasVaWord = true;
      VaStartWord = GpOffset | (FpOffset->second << 32);
    }
  Marked = HasVaWord && countParamRegSpills(Func, TRI.IntParamRegs) >= 1;
  return Marked;
}

namespace {
/// The constant \p Value holds, directly or through the register a COPY
/// loaded it into (`movabs $0x3000000008, %rax; mov %rax, (%rsp)`).
std::optional<uint64_t> storedConstant(VariadicScan &S, const MedVar &Value) {
  if (Value.isConst())
    return Value.ConstVal;
  const VariadicValueIndex &Values = S.values();
  const auto Key = VariadicValueIndex::keyOf(Value);
  const auto It = Values.Definitions.find(Key);
  if (It == Values.Definitions.end() ||
      Values.AmbiguousDefinitions.count(Key) ||
      It->second->Opcode != NdOp::COPY || It->second->NumInputs < 1 ||
      !It->second->Inputs[0].isConst())
    return std::nullopt;
  return It->second->Inputs[0].ConstVal;
}

/// The entry-SP displacements of the va_lists whose gp_offset and fp_offset
/// fields a store of \p VaStartWord fills: the word whole, or its two halves
/// side by side.
std::set<int64_t> x64VaStartWordStores(VariadicScan &S, uint64_t VaStartWord) {
  const uint64_t GpOffset = VaStartWord & 0xFFFFFFFFu;
  const uint64_t FpOffset = VaStartWord >> 32;
  std::set<int64_t> Words, GpFields, FpFields;
  for (const MedBlock &Block : S.Func.Blocks)
    for (const MedOp &Op : Block.Ops) {
      if (Op.Opcode != NdOp::STORE || Op.NumInputs < 2 ||
          Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
        continue;
      const MedVar &Value = Op.Inputs[1];
      if (Value.Size != sizeof(uint64_t) && Value.Size != sizeof(uint32_t))
        continue;
      const auto Word = storedConstant(S, Value);
      if (!Word)
        continue;
      const auto At = entrySpDelta(S, Op.Inputs[0], 0);
      if (!At)
        continue;
      if (Value.Size == sizeof(uint64_t) && *Word == VaStartWord)
        Words.insert(*At);
      else if (Value.Size == sizeof(uint32_t) && *Word == GpOffset)
        GpFields.insert(*At);
      else if (Value.Size == sizeof(uint32_t) && *Word == FpOffset)
        FpFields.insert(*At);
    }
  std::set<int64_t> VaLists = Words;
  for (int64_t At : GpFields)
    if (FpFields.count(At + static_cast<int64_t>(sizeof(uint32_t))))
      VaLists.insert(At);
  return VaLists;
}
} // namespace

std::optional<int> x64VaStartNamedGPRs(VariadicScan &S, uint64_t VaStartWord) {
  const uint64_t GpOffset = VaStartWord & 0xFFFFFFFFu;
  if (GpOffset % limits::kX64VaGpOffsetStep ||
      GpOffset > limits::kX64VaFpOffsetMin ||
      x64VaStartWordStores(S, VaStartWord).empty())
    return std::nullopt;
  return static_cast<int>(GpOffset / limits::kX64VaGpOffsetStep);
}

std::optional<X64VaStartGeometry> x64VaStartGeometry(VariadicScan &S,
                                                     uint64_t VaStartWord,
                                                     int64_t OverflowBase) {
  // The entry-SP address each pointer-wide store writes at each place; a
  // place written two different ones has none.
  std::map<int64_t, std::optional<int64_t>> Pointers;
  for (const MedBlock &Block : S.Func.Blocks)
    for (const MedOp &Op : Block.Ops) {
      if (Op.Opcode != NdOp::STORE || Op.NumInputs < 2 ||
          Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
        continue;
      const auto At = entrySpDelta(S, Op.Inputs[0], 0);
      if (!At)
        continue;
      const MedVar &Value = Op.Inputs[1];
      if (storedConstant(S, Value) || Value.Size != S.TRI.PointerSize)
        continue;
      const auto Address = entrySpDelta(S, Value, 0);
      auto [It, Inserted] = Pointers.emplace(*At, Address);
      if (!Inserted && It->second != Address)
        It->second.reset();
    }
  const std::set<int64_t> Candidates = x64VaStartWordStores(S, VaStartWord);
  std::optional<X64VaStartGeometry> Found;
  for (int64_t VaList : Candidates) {
    const auto Overflow = Pointers.find(VaList + 8);
    const auto SaveArea = Pointers.find(VaList + 16);
    if (Overflow == Pointers.end() || !Overflow->second ||
        *Overflow->second != OverflowBase || SaveArea == Pointers.end() ||
        !SaveArea->second)
      continue;
    if (Found)
      return std::nullopt;
    Found = X64VaStartGeometry{VaList, *SaveArea->second};
  }
  return Found;
}

/// The i386 stack va_list, homed and then reloaded or walked.
bool hasI386VariadicPrologue(VariadicScan &S) {
  const MedFunc &Func = S.Func;
  const TargetRegInfo &TRI = S.TRI;
  // i386 cdecl passes every argument (named and variadic) on the stack, so a
  // variadic prologue has no register save area.  va_start instead points a
  // va_list at the first unnamed argument -- entry_sp + PointerSize (return
  // address) + named-argument bytes, an entry-SP-positive pointer at least
  // two slots above entry SP -- and spills it to a home slot in the callee
  // frame; va_arg reloads that slot and walks it.  The tell-tale, robust
  // against
  // `&secondParam` (stored as an outgoing arg but never reloaded) and against
  // a plain incoming-stack-parameter read (loaded but never stored as a
  // pointer), is BOTH: an entry-SP-positive pointer (delta >= 2*PointerSize)
  // stored to a frame slot S, AND a reload from that same slot S.
  const int64_t MinPtrDelta = 2 * TRI.PointerSize;
  std::set<int64_t> HomeSlots;
  llvm::SmallVector<MedVar, 2> DirectSeeds;
  for (const auto &Blk : Func.Blocks)
    for (const auto &Op : Blk.Ops)
      if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 2 &&
          Op.MemoryAddressSpace == NdMemoryAddressSpace::Default)
        if (auto VD = entrySpDelta(S, Op.Inputs[1], 0))
          if (*VD >= MinPtrDelta && *VD <= limits::kVariadicOverflowBaseMax &&
              TRI.PointerSize > 0 && (*VD % TRI.PointerSize) == 0)
            if (auto AD = entrySpDelta(S, Op.Inputs[0], 0)) {
              HomeSlots.insert(*AD);
              DirectSeeds.push_back(Op.Inputs[1]);
            }
  // Both the reload and direct-walk proofs require a homed incoming pointer.
  if (HomeSlots.empty())
    return false;
  for (const auto &Blk : Func.Blocks)
    for (const auto &Op : Blk.Ops)
      if (Op.Opcode == NdOp::LOAD && Op.NumInputs >= 1 &&
          Op.MemoryAddressSpace == NdMemoryAddressSpace::Default)
        if (auto AD = entrySpDelta(S, Op.Inputs[0], 0))
          if (HomeSlots.count(*AD))
            return true;

  // At -O2 the va_list pointer can remain live in registers after its
  // mandatory home store, so va_arg walks it directly and never reloads the
  // home slot.  Follow that seed through PHIs, width-preserving views, and
  // constant pointer advances; a load through the resulting value proves the
  // stored entry-SP-positive pointer is an active va_arg walk.
  auto forwardsTransparent = [](const MedOp &Op, unsigned InputIdx) {
    if (InputIdx != 0)
      return false;
    switch (Op.Opcode) {
    case NdOp::COPY:
    case NdOp::INT_ZEXT:
    case NdOp::INT_SEXT:
      return Op.NumInputs >= 1;
    case NdOp::SUBBYTES:
      return Op.NumInputs >= 2 && Op.Inputs[1].isConst() &&
             Op.Inputs[1].ConstVal == 0;
    default:
      return false;
    }
  };
  auto forwardsConstantAdvance = [](const MedOp &Op, unsigned InputIdx) {
    if (Op.NumInputs < 2)
      return false;
    if (Op.Opcode == NdOp::INT_ADD)
      return (InputIdx == 0 && Op.Inputs[1].isConst()) ||
             (InputIdx == 1 && Op.Inputs[0].isConst());
    return Op.Opcode == NdOp::INT_SUB && InputIdx == 0 &&
           Op.Inputs[1].isConst();
  };

  auto DirectWalkPtrs = computeForwardValueClosure(
      Func, DirectSeeds, [&](const MedOp &Op, unsigned InputIdx) {
        return forwardsTransparent(Op, InputIdx) ||
               forwardsConstantAdvance(Op, InputIdx);
      });
  llvm::SmallVector<MedVar, 4> AdvancedSeeds;
  for (const MedBlock &Block : Func.Blocks)
    for (const MedOp &Op : Block.Ops)
      for (unsigned I = 0; I < Op.NumInputs; ++I)
        if (forwardsConstantAdvance(Op, I) &&
            containsValue(DirectWalkPtrs, Op.Inputs[I])) {
          AdvancedSeeds.push_back(Op.Output);
          break;
        }
  if (AdvancedSeeds.empty())
    return false;
  auto AdvancedWalkPtrs =
      computeForwardValueClosure(Func, AdvancedSeeds, forwardsTransparent);

  for (const auto &Blk : Func.Blocks)
    for (const auto &Op : Blk.Ops)
      if (Op.Opcode == NdOp::LOAD && Op.NumInputs >= 1 &&
          Op.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
          containsValue(AdvancedWalkPtrs, Op.Inputs[0]))
        return true;
  return false;
}

} // namespace med_variadic_detail

// A System V caller sets AL to the number of vector registers it passes a
// variadic function, which saves them to its register save area only when AL
// is not zero.  A function that takes every vector argument register as a
// parameter is passed all eight: AL is 8, and the area always holds them.
void bindX64VariadicVectorCount(MedFunc &Func, const TargetRegInfo &TRI,
                                uint8_t VectorRegisters) {
  for (MedBlock &Block : Func.Blocks)
    for (MedOp &Op : Block.Ops) {
      if (med_calling_conv_detail::isEntryLiveInCopy(Op, TRI.IntReturnReg))
        continue;
      for (uint8_t I = 0; I < Op.NumInputs; ++I) {
        MedVar &Input = Op.Inputs[I];
        if (Input.Kind == MedVar::Reg && Input.RegOff == TRI.IntReturnReg &&
            Input.Size == 1 && Input.SSAVer == 0)
          Input = MedVar::makeConst(VectorRegisters, 1);
      }
    }
}

} // namespace neverd
