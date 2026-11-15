//===- MedVariadicAArch64.cpp - AAPCS64 variadic prologue -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The AAPCS64 variadic prologue recognizer: the register save area that
/// spills most of both the GP and FP argument registers at entry, or only the
/// GP ones under the va_list that addresses them, and the proof that no read
/// observes its vector half.
///
//===----------------------------------------------------------------------===//

#include "../MedVariadicDetail.h"

#include "neverd/ir/Limits.h"
#include "neverd/ir/intrinsics/AArch64/AArch64AddressMemory.h"
#include "neverd/ir/med/IntrinsicShapes.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <vector>

namespace neverd {
namespace med_variadic_detail {
namespace {

// The AAPCS64 va_list (Procedure Call Standard for the Arm 64-bit
// Architecture, "The va_list type"): the __stack, __gr_top and __vr_top
// pointers, then the __gr_offs and __vr_offs words.
constexpr int64_t kVaListStack = 0;
constexpr int64_t kVaListGrTop = 8;
constexpr int64_t kVaListVrTop = 16;
constexpr int64_t kVaListGrOffs = 24;
constexpr int64_t kVaListVrOffs = 28;
constexpr uint16_t kVaListOffsBytes = 4;
/// va_start saves each vector argument register whole, in 16 bytes.
constexpr uint16_t kVectorSaveSlotBytes = 16;

bool overlaps(int64_t Begin, int64_t End, int64_t OtherBegin,
              int64_t OtherEnd) {
  return Begin < OtherEnd && OtherBegin < End;
}

/// Whether \p V is the stack pointer the function was entered with.
bool isEntryStackPointer(const VariadicScan &S, const MedVar &V) {
  return V.Kind == MedVar::Reg && V.RegOff == S.SpOff && V.SSAVer == 0;
}

/// Whether an operation's result is computed from its inputs alone, so a
/// frame address among them may survive in it.  Memory, calls, returns,
/// branches and intrinsics are not: the proof below asks what each of them
/// does with a frame address.
bool computesFromInputs(NdOp Opcode) {
  switch (Opcode) {
  case NdOp::LOAD:
  case NdOp::STORE:
  case NdOp::ATOMIC_XCHG:
  case NdOp::ATOMIC_ADD:
  case NdOp::ATOMIC_CMPXCHG:
  case NdOp::BRANCH:
  case NdOp::COND_BR:
  case NdOp::INDIR_BR:
  case NdOp::CALL:
  case NdOp::INDIR_CALL:
  case NdOp::RETURN:
  case NdOp::INTRINSIC:
  case NdOp::NOP:
    return false;
  default:
    return true;
  }
}

/// A constant the function's integer arithmetic computes.
struct ComputedConstant {
  uint64_t Value = 0;
  /// Whether it is a number alone.  An address of the image among its
  /// operands makes it one the image's placement decides.
  bool Scalar = true;
};

using ComputedConstants = std::map<VariadicValueIndex::Key, ComputedConstant>;

/// The constants the function's integer arithmetic computes, by value: what
/// va_start stores in __gr_offs and __vr_offs, which an AArch64 MOVK builds
/// from an AND and an OR of earlier constants, and -O0 stores as the low
/// bytes of a register, before propagation folds them.
ComputedConstants constantValues(VariadicScan &S) {
  const VariadicValueIndex &Index = S.values();
  ComputedConstants Known;
  const auto ValueOf = [&](const MedVar &V) -> std::optional<ComputedConstant> {
    if (V.isConst())
      return ComputedConstant{
          V.ConstVal, V.Provenance == ConstantAddressProvenance::Unknown ||
                          V.Provenance == ConstantAddressProvenance::Scalar};
    const auto It = Known.find(VariadicValueIndex::keyOf(V));
    if (It == Known.end())
      return std::nullopt;
    return It->second;
  };
  // Each pass that learns nothing ends the search, so it takes at most one
  // pass per constant the function computes.
  for (bool Grew = true; Grew;) {
    Grew = false;
    for (const MedBlock &Block : S.Func.Blocks)
      for (const MedOp &Op : Block.Ops) {
        if (Op.Output.isConst() || Op.Output.Size == 0 ||
            Op.Output.Size > sizeof(uint64_t) || Op.NumInputs == 0 ||
            (Op.Output.Kind != MedVar::Reg && Op.Output.Kind != MedVar::Temp))
          continue;
        const auto Key = VariadicValueIndex::keyOf(Op.Output);
        if (Known.count(Key) || Index.AmbiguousDefinitions.count(Key) ||
            Index.Phis.count(Key))
          continue;
        const auto A = ValueOf(Op.Inputs[0]);
        const auto B = Op.NumInputs > 1 ? ValueOf(Op.Inputs[1])
                                        : std::optional<ComputedConstant>();
        std::optional<uint64_t> Result;
        switch (Op.Opcode) {
        case NdOp::COPY:
          if (Op.NumInputs == 1 && A)
            Result = A->Value;
          break;
        case NdOp::INT_ZEXT:
          if (Op.NumInputs == 1 && A && Op.Inputs[0].Size > 0 &&
              Op.Inputs[0].Size < sizeof(uint64_t))
            Result = A->Value & ((uint64_t{1} << (8 * Op.Inputs[0].Size)) - 1);
          else if (Op.NumInputs == 1 && A)
            Result = A->Value;
          break;
        case NdOp::INT_SEXT:
          // The index a register-offset access extends from a W register.
          if (Op.NumInputs == 1 && A && Op.Inputs[0].Size > 0 &&
              Op.Inputs[0].Size < sizeof(uint64_t)) {
            const uint64_t Sign = uint64_t{1} << (8 * Op.Inputs[0].Size - 1);
            Result = ((A->Value & (2 * Sign - 1)) ^ Sign) - Sign;
          } else if (Op.NumInputs == 1 && A) {
            Result = A->Value;
          }
          break;
        case NdOp::INT_AND:
          if (A && B)
            Result = A->Value & B->Value;
          break;
        case NdOp::INT_OR:
          if (A && B)
            Result = A->Value | B->Value;
          break;
        case NdOp::INT_XOR:
          if (A && B)
            Result = A->Value ^ B->Value;
          break;
        case NdOp::INT_ADD:
          if (A && B)
            Result = A->Value + B->Value;
          break;
        case NdOp::INT_SUB:
          if (A && B)
            Result = A->Value - B->Value;
          break;
        case NdOp::SUBBYTES:
          // -O0 stores a W register, the low bytes of the X one MOV wrote.
          if (A && B && B->Value < sizeof(uint64_t))
            Result = A->Value >> (8 * B->Value);
          break;
        default:
          break;
        }
        if (!Result)
          continue;
        if (Op.Output.Size < sizeof(uint64_t))
          *Result &= (uint64_t{1} << (8 * Op.Output.Size)) - 1;
        Known.emplace(
            Key, ComputedConstant{*Result, A->Scalar && (!B || B->Scalar)});
        Grew = true;
      }
  }
  return Known;
}

/// The number \p V is, where it is a constant no address of the image is
/// part of.
std::optional<uint64_t> scalarConstant(const ComputedConstants &Constants,
                                       const MedVar &V) {
  if (V.isConst()) {
    if (V.Provenance != ConstantAddressProvenance::Unknown &&
        V.Provenance != ConstantAddressProvenance::Scalar)
      return std::nullopt;
    return V.ConstVal;
  }
  const auto It = Constants.find(VariadicValueIndex::keyOf(V));
  if (It == Constants.end() || !It->second.Scalar)
    return std::nullopt;
  return It->second.Value;
}

/// The offsets from the entry stack pointer a frame address may have.  Its
/// definitions fix one (entrySpDelta), or it is a join of addresses that
/// differ, which has each of its arms' offsets: the pointer an unrolled
/// va_arg steps up the incoming arguments, or the address of the next
/// argument in either area.  An addition may take its constant from a
/// register that holds one.  Nothing for a join in a loop, for more offsets
/// than kMedVariadicAArch64FrameOffsetsMaxChoices, or for any other
/// arithmetic.
class FrameOffsets {
public:
  using Offsets = llvm::SmallVector<int64_t, 2>;

  FrameOffsets(VariadicScan &S, const ComputedConstants &Constants)
      : S(S), Constants(Constants) {}

  std::optional<Offsets> of(const MedVar &V) { return evaluate(V, 0); }

private:
  using Key = VariadicValueIndex::Key;

  VariadicScan &S;
  const ComputedConstants &Constants;
  std::map<Key, std::optional<Offsets>> Known;
  std::set<Key> Active;

  std::optional<Offsets> evaluate(const MedVar &V, int Depth) {
    if (V.isConst() || (V.Kind != MedVar::Reg && V.Kind != MedVar::Temp))
      return std::nullopt;
    if (const auto Fixed = entrySpDelta(S, V, 0))
      return Offsets{*Fixed};
    const Key K = VariadicValueIndex::keyOf(V);
    if (const auto It = Known.find(K); It != Known.end())
      return It->second;
    const VariadicValueIndex &Index = S.values();
    if (Depth >=
            ::neverd::limits::ir::kMedVariadicAArch64FrameOffsetsDepthLimit ||
        Index.AmbiguousDefinitions.count(K) || Index.AmbiguousPhis.count(K) ||
        !Active.insert(K).second)
      return std::nullopt;
    std::optional<Offsets> Result = compute(V, K, Depth);
    Active.erase(K);
    Known.emplace(K, Result);
    return Result;
  }

  std::optional<Offsets> compute(const MedVar &V, const Key &K, int Depth) {
    // Only a whole pointer has an offset its joins and sums keep.
    if (V.Size != S.TRI.PointerSize)
      return std::nullopt;
    const VariadicValueIndex &Index = S.values();
    if (const auto Phi = Index.Phis.find(K); Phi != Index.Phis.end()) {
      Offsets Joined;
      for (const auto &[Pred, Arg] : Phi->second->Args) {
        (void)Pred;
        const auto Arm = evaluate(Arg, Depth + 1);
        if (!Arm)
          return std::nullopt;
        for (int64_t Offset : *Arm)
          if (!llvm::is_contained(Joined, Offset))
            Joined.push_back(Offset);
        if (Joined.size() >
            ::neverd::limits::ir::kMedVariadicAArch64FrameOffsetsMaxChoices)
          return std::nullopt;
      }
      if (Joined.empty())
        return std::nullopt;
      llvm::sort(Joined);
      return Joined;
    }
    const auto Definition = Index.Definitions.find(K);
    if (Definition == Index.Definitions.end())
      return std::nullopt;
    const MedOp &Op = *Definition->second;
    // A literal amount is the whole offset, as entrySpDelta reads it; a
    // register holds one only at the address's width.
    const auto Moved = [&](const MedVar &Address, const MedVar &Amount,
                           bool Subtract) -> std::optional<Offsets> {
      if (Address.Size != V.Size ||
          (!Amount.isConst() && Amount.Size != V.Size))
        return std::nullopt;
      const auto Bytes = scalarConstant(Constants, Amount);
      if (!Bytes)
        return std::nullopt;
      auto Base = evaluate(Address, Depth + 1);
      if (!Base)
        return std::nullopt;
      for (int64_t &Offset : *Base) {
        const auto From = static_cast<uint64_t>(Offset);
        Offset = static_cast<int64_t>(Subtract ? From - *Bytes : From + *Bytes);
      }
      return Base;
    };
    switch (Op.Opcode) {
    case NdOp::COPY:
      if (Op.NumInputs == 1 && Op.Inputs[0].Size == V.Size)
        return evaluate(Op.Inputs[0], Depth + 1);
      return std::nullopt;
    case NdOp::INT_ADD:
    case NdOp::INT_SUB:
      if (Op.NumInputs != 2)
        return std::nullopt;
      if (auto Result =
              Moved(Op.Inputs[0], Op.Inputs[1], Op.Opcode == NdOp::INT_SUB))
        return Result;
      if (Op.Opcode == NdOp::INT_ADD)
        return Moved(Op.Inputs[1], Op.Inputs[0], false);
      return std::nullopt;
    default:
      return std::nullopt;
    }
  }
};

/// The vector half of an AAPCS64 register save area: each of q(First)
/// through q7 spilled whole, once, with its entry value, to consecutive
/// 16-byte slots [Begin, End) from the entry stack pointer.
struct VectorSaveArea {
  int64_t Begin = 0;
  int64_t End = 0;
  size_t First = 0;
  /// The spills, as (block, operation) indices.
  std::vector<std::pair<size_t, size_t>> Spills;
  /// Just past each slot x7 is spilled to, where __gr_top points.
  std::set<int64_t> IntegerTops;
};

std::optional<VectorSaveArea> vectorSaveArea(VariadicScan &S) {
  const MedFunc &Func = S.Func;
  const llvm::ArrayRef<uint64_t> VectorRegs = S.TRI.FPParamRegs;
  const llvm::ArrayRef<uint64_t> IntegerRegs = S.TRI.IntParamRegs;
  const int64_t PointerSize = S.TRI.PointerSize;
  VectorSaveArea Area;
  std::map<size_t, std::vector<std::tuple<size_t, size_t, int64_t>>> Spills;
  for (size_t B = 0; B < Func.Blocks.size(); ++B)
    for (size_t I = 0; I < Func.Blocks[B].Ops.size(); ++I) {
      const MedOp &Op = Func.Blocks[B].Ops[I];
      if (Op.Opcode != NdOp::STORE || Op.NumInputs < 2)
        continue;
      const MedVar &Value = Op.Inputs[1];
      if (Value.Kind != MedVar::Reg || Value.SSAVer != 0)
        continue;
      if (Value.RegOff == IntegerRegs.back() && Value.Size == PointerSize &&
          Op.MemoryAddressSpace == NdMemoryAddressSpace::Default)
        if (const auto Slot = entrySpDelta(S, Op.Inputs[0], 0))
          Area.IntegerTops.insert(*Slot + PointerSize);
      const auto *Register = llvm::find(VectorRegs, Value.RegOff);
      if (Register == VectorRegs.end())
        continue;
      const auto Slot = entrySpDelta(S, Op.Inputs[0], 0);
      if (Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
          Value.Size != kVectorSaveSlotBytes || !Slot)
        return std::nullopt;
      Spills[static_cast<size_t>(Register - VectorRegs.begin())].emplace_back(
          B, I, *Slot);
    }
  if (Spills.empty() || Spills.rbegin()->first + 1 != VectorRegs.size())
    return std::nullopt;
  Area.First = Spills.begin()->first;
  if (Spills.size() != VectorRegs.size() - Area.First)
    return std::nullopt;
  Area.Begin = std::get<2>(Spills.begin()->second.front());
  Area.End =
      Area.Begin + static_cast<int64_t>((VectorRegs.size() - Area.First) *
                                        kVectorSaveSlotBytes);
  for (const auto &[Index, Stores] : Spills) {
    if (Stores.size() != 1 ||
        std::get<2>(Stores.front()) !=
            Area.Begin + static_cast<int64_t>((Index - Area.First) *
                                              kVectorSaveSlotBytes))
      return std::nullopt;
    Area.Spills.emplace_back(std::get<0>(Stores.front()),
                             std::get<1>(Stores.front()));
  }
  return Area;
}

/// Whether a frame address \p Value may be what va_start stores at
/// \p Field of the va_list at \p VaList: __stack points into the incoming
/// arguments, __gr_top past the integer half (or above the vector half
/// where there is none), __vr_top past the vector half.
bool isVaStartPointer(const VectorSaveArea &Area, int64_t VaList,
                      bool IntegerHalf, int64_t Field, int64_t Value) {
  if (Field == VaList + kVaListStack)
    return Value >= 0;
  if (Field == VaList + kVaListGrTop)
    return IntegerHalf ? Area.IntegerTops.count(Value) != 0 : Value >= Area.End;
  return Field == VaList + kVaListVrTop && Value == Area.End;
}

/// The va_list whose va_start points __vr_top just past \p Area: the one
/// that also points __stack into the incoming arguments and __gr_top past
/// the integer half, and sets both offsets.
std::optional<int64_t> vaListOver(VariadicScan &S, const VectorSaveArea &Area,
                                  int FirstVariadicRegister,
                                  const ComputedConstants &Constants) {
  const MedFunc &Func = S.Func;
  const int64_t PointerSize = S.TRI.PointerSize;
  std::map<int64_t, std::vector<const MedOp *>> StoresAt;
  std::optional<int64_t> VectorTop;
  for (const MedBlock &Block : Func.Blocks)
    for (const MedOp &Op : Block.Ops) {
      if (Op.Opcode != NdOp::STORE || Op.NumInputs < 2 ||
          Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
        continue;
      const auto At = entrySpDelta(S, Op.Inputs[0], 0);
      if (!At)
        continue;
      StoresAt[*At].push_back(&Op);
      if (Op.Inputs[1].isConst())
        continue;
      if (const auto Value = entrySpDelta(S, Op.Inputs[1], 0);
          Value && *Value == Area.End) {
        if (Op.Inputs[1].Size != PointerSize ||
            (VectorTop && *VectorTop != *At))
          return std::nullopt;
        VectorTop = *At;
      }
    }
  if (!VectorTop)
    return std::nullopt;
  const int64_t VaList = *VectorTop - kVaListVrTop;
  const bool IntegerHalf =
      static_cast<size_t>(FirstVariadicRegister) < S.TRI.IntParamRegs.size();
  const auto StoresPointer = [&](int64_t Field) {
    const auto It = StoresAt.find(Field);
    return It != StoresAt.end() &&
           llvm::any_of(It->second, [&](const MedOp *Op) {
             const auto Value = entrySpDelta(S, Op->Inputs[1], 0);
             return Op->Inputs[1].Size == PointerSize && Value &&
                    isVaStartPointer(Area, VaList, IntegerHalf, Field, *Value);
           });
  };
  if (!StoresPointer(VaList + kVaListStack) ||
      !StoresPointer(VaList + kVaListGrTop))
    return std::nullopt;
  const auto ConstantOf = [&](const MedVar &V) -> std::optional<uint64_t> {
    if (V.isConst())
      return V.ConstVal;
    const auto It = Constants.find(VariadicValueIndex::keyOf(V));
    if (It == Constants.end())
      return std::nullopt;
    return It->second.Value;
  };
  // Each offset is minus the bytes of its half's saved registers: one
  // 4-byte store, or one 8-byte store of both words.
  const auto StoresOffset = [&](int64_t Word, int64_t Bytes) {
    const auto Expected = static_cast<uint32_t>(-Bytes);
    const auto Matches = [&](int64_t At, uint16_t Size, unsigned Shift) {
      const auto It = StoresAt.find(At);
      return It != StoresAt.end() &&
             llvm::any_of(It->second, [&](const MedOp *Op) {
               const auto Value = ConstantOf(Op->Inputs[1]);
               return Op->Inputs[1].Size == Size && Value &&
                      static_cast<uint32_t>(*Value >> Shift) == Expected;
             });
    };
    const int64_t Pair = VaList + kVaListGrOffs;
    return Matches(Word, kVaListOffsBytes, 0) ||
           Matches(Pair, 2 * kVaListOffsBytes,
                   static_cast<unsigned>(8 * (Word - Pair)));
  };
  const size_t SavedIntegers =
      S.TRI.IntParamRegs.size() - static_cast<size_t>(FirstVariadicRegister);
  const size_t SavedVectors = S.TRI.FPParamRegs.size() - Area.First;
  if (!StoresOffset(VaList + kVaListGrOffs,
                    static_cast<int64_t>(SavedIntegers) * PointerSize) ||
      !StoresOffset(VaList + kVaListVrOffs,
                    static_cast<int64_t>(SavedVectors * kVectorSaveSlotBytes)))
    return std::nullopt;
  return VaList;
}

/// Whether no frame address can reach \p Area, __vr_top or __vr_offs other
/// than as va_start's own pointers: every value computed from the entry
/// stack pointer is used only as the address of a load that reads none of
/// them at any offset the address may have (FrameOffsets), as an address
/// stored to, as one of va_start's pointers, in arithmetic and in branches,
/// and is in no argument register a call or a return reads, nor in a vector
/// register.  A register-offset access keeps its instruction
/// (AArch64AddressMemory.h): it is such a load where its index is a
/// constant, and such a store where it stores no frame address.  A load ends
/// the chain: what va_arg reads from __stack and __gr_top it uses within the
/// overflow and integer areas, as the va_list convention keeps it.
bool frameStaysPrivate(VariadicScan &S, const VectorSaveArea &Area,
                       int64_t VaList, int FirstVariadicRegister,
                       const ComputedConstants &Constants) {
  using Key = VariadicValueIndex::Key;
  const MedFunc &Func = S.Func;
  const TargetRegInfo &TRI = S.TRI;
  const int64_t PointerSize = TRI.PointerSize;
  const bool IntegerHalf =
      static_cast<size_t>(FirstVariadicRegister) < TRI.IntParamRegs.size();
  struct Use {
    const MedOp *Op = nullptr;
    const PhiNode *Phi = nullptr;
  };
  std::map<Key, std::vector<Use>> Uses;
  std::set<Key> Frame;
  std::vector<Key> Work;
  const auto Reach = [&](const MedVar &V) {
    if (!V.isConst() && Frame.insert(VariadicValueIndex::keyOf(V)).second)
      Work.push_back(VariadicValueIndex::keyOf(V));
  };
  for (const MedBlock &Block : Func.Blocks) {
    for (const PhiNode &Phi : Block.Phis)
      for (const auto &[Pred, Arg] : Phi.Args) {
        (void)Pred;
        if (!Arg.isConst())
          Uses[VariadicValueIndex::keyOf(Arg)].push_back({nullptr, &Phi});
      }
    for (const MedOp &Op : Block.Ops)
      for (uint8_t I = 0; I < Op.NumInputs; ++I) {
        const MedVar &Input = Op.Inputs[I];
        if (Input.isConst())
          continue;
        Uses[VariadicValueIndex::keyOf(Input)].push_back({&Op, nullptr});
        if (isEntryStackPointer(S, Input))
          Reach(Input);
      }
  }
  while (!Work.empty()) {
    const Key Value = Work.back();
    Work.pop_back();
    const auto It = Uses.find(Value);
    if (It == Uses.end())
      continue;
    for (const Use &U : It->second) {
      if (U.Phi)
        Reach(U.Phi->Output);
      else if (computesFromInputs(U.Op->Opcode))
        Reach(U.Op->Output);
    }
  }
  const auto IsFrame = [&](const MedVar &V) {
    return !V.isConst() && Frame.count(VariadicValueIndex::keyOf(V)) != 0;
  };

  FrameOffsets Offsets(S, Constants);
  // Whether a read of \p Bytes at each of \p At reads none of the vector
  // state.
  const auto ReadsNoVectorState = [&](const FrameOffsets::Offsets &At,
                                      uint64_t Bytes) {
    return llvm::none_of(At, [&](int64_t Begin) {
      const auto End = static_cast<int64_t>(Begin + Bytes);
      return End <= Begin || overlaps(Begin, End, Area.Begin, Area.End) ||
             overlaps(Begin, End, VaList + kVaListVrTop,
                      VaList + kVaListVrTop + PointerSize) ||
             overlaps(Begin, End, VaList + kVaListVrOffs,
                      VaList + kVaListVrOffs + kVaListOffsBytes);
    });
  };
  for (const MedBlock &Block : Func.Blocks)
    for (const MedOp &Op : Block.Ops) {
      const bool Address = Op.NumInputs > 0 && IsFrame(Op.Inputs[0]);
      bool Operand = false;
      for (uint8_t I = 1; I < Op.NumInputs; ++I)
        Operand |= IsFrame(Op.Inputs[I]);
      if (!Address && !Operand)
        continue;
      switch (Op.Opcode) {
      case NdOp::LOAD: {
        const auto At = Offsets.of(Op.Inputs[0]);
        if (Operand || !At || Op.Output.Size == 0 ||
            Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
            !ReadsNoVectorState(*At, Op.Output.Size))
          return false;
        break;
      }
      case NdOp::STORE: {
        if (Op.NumInputs != 2 ||
            Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
          return false;
        if (!Operand)
          break;
        const auto At = entrySpDelta(S, Op.Inputs[0], 0);
        const auto Values = Offsets.of(Op.Inputs[1]);
        if (!At || !Values || Op.Inputs[1].Size != PointerSize ||
            !llvm::all_of(*Values, [&](int64_t Value) {
              return isVaStartPointer(Area, VaList, IntegerHalf, *At, Value);
            }))
          return false;
        break;
      }
      case NdOp::INTRINSIC: {
        if (!aarch64AddressMemoryMedShapeIsValid(Op))
          return false;
        const auto Instruction = *aarch64AddressMemoryInstruction(
            Op.Inputs[kAddressMemoryWordOperand + 1].ConstVal);
        if (!Instruction.isLoad()) {
          // It reads nothing, and may store any value but a frame address.
          if (IsFrame(Op.Inputs[kAddressMemoryDataOperand + 1]))
            return false;
          break;
        }
        // Its base and a constant index fix the bytes it reads, as a
        // LOAD's address does: va_arg of the next saved integer register,
        // at __gr_top plus the offset an unrolled va_arg left in a register.
        const auto Base = Offsets.of(Op.Inputs[kAddressMemoryBaseOperand + 1]);
        const auto Index = scalarConstant(
            Constants, Op.Inputs[kAddressMemoryIndexOperand + 1]);
        if (!Base || !Index)
          return false;
        FrameOffsets::Offsets At = *Base;
        for (int64_t &Begin : At)
          Begin = static_cast<int64_t>(static_cast<uint64_t>(Begin) +
                                       Instruction.addressOffset(*Index));
        if (!ReadsNoVectorState(At, Instruction.accessBytes()))
          return false;
        break;
      }
      case NdOp::BRANCH:
      case NdOp::COND_BR:
        // A branch on a frame address reads no memory.
        break;
      default:
        if (!computesFromInputs(Op.Opcode))
          return false;
        break;
      }
    }

  // A call also reads the argument registers and X8, and a return the
  // result registers among them, which the operation lists no input for.
  const uint64_t IndirectResult = TRI.indirectResultReg();
  std::set<uint64_t> Implicit(TRI.IntParamRegs.begin(), TRI.IntParamRegs.end());
  if (IndirectResult)
    Implicit.insert(IndirectResult);
  std::map<int, size_t> BlockIndex;
  for (size_t B = 0; B < Func.Blocks.size(); ++B)
    BlockIndex.emplace(Func.Blocks[B].Id, B);
  const auto ReachesReader = [&](size_t StartBlock, size_t StartOp,
                                 uint64_t Register) {
    const auto Writes = [&](const MedVar &V) {
      return V.Kind == MedVar::Reg && V.RegOff == Register &&
             V.Size >= PointerSize;
    };
    std::set<size_t> Seen;
    std::vector<std::pair<size_t, size_t>> Paths{{StartBlock, StartOp}};
    while (!Paths.empty()) {
      const auto [B, From] = Paths.back();
      Paths.pop_back();
      const MedBlock &Block = Func.Blocks[B];
      bool Killed = false;
      for (size_t I = From; I < Block.Ops.size() && !Killed; ++I) {
        const MedOp &Op = Block.Ops[I];
        if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
            Op.Opcode == NdOp::INDIR_BR ||
            (Op.Opcode == NdOp::RETURN && Register != IndirectResult))
          return true;
        Killed = Writes(Op.Output) || llvm::any_of(Op.IntrinsicOutputs, Writes);
      }
      if (Killed)
        continue;
      for (int Succ : Block.Succs) {
        const auto Next = BlockIndex.find(Succ);
        if (Next == BlockIndex.end())
          return true;
        // A PHI of the register starts another value, which is a frame
        // address and checked on its own where this one flows into it.
        if (llvm::any_of(Func.Blocks[Next->second].Phis,
                         [&](const PhiNode &Phi) {
                           return Phi.Output.Kind == MedVar::Reg &&
                                  Phi.Output.RegOff == Register;
                         }))
          continue;
        if (Seen.insert(Next->second).second)
          Paths.emplace_back(Next->second, 0);
      }
    }
    return false;
  };
  const auto Escapes = [&](const MedVar &V, size_t B, size_t From) {
    if (V.Kind != MedVar::Reg || !IsFrame(V))
      return false;
    if (TRI.isVectorReg(V.RegOff))
      return true;
    return Implicit.count(V.RegOff) != 0 && ReachesReader(B, From, V.RegOff);
  };
  for (size_t B = 0; B < Func.Blocks.size(); ++B) {
    for (const PhiNode &Phi : Func.Blocks[B].Phis)
      if (Escapes(Phi.Output, B, 0))
        return false;
    for (size_t I = 0; I < Func.Blocks[B].Ops.size(); ++I)
      if (Escapes(Func.Blocks[B].Ops[I].Output, B, I + 1))
        return false;
  }
  return true;
}

} // namespace

/// Most of both argument register files spilled at entry.
bool hasAAPCS64VariadicPrologue(VariadicScan &S) {
  const MedFunc &Func = S.Func;
  const TargetRegInfo &TRI = S.TRI;
  // AArch64 saves both the GP (x0-x7) and FP (q0-q7) argument registers to a
  // contiguous save area; spilling most of both register files at entry is
  // the variadic prologue's signature.  This is the AAPCS64 (Linux/ELF)
  // layout ONLY: Apple/Darwin arm64 passes EVERY variadic argument on the
  // stack and emits NO register save area (its va_start homes a single
  // overflow pointer, detected by MedVariadicDarwin.cpp).  On Mach-O this
  // save-area test would FALSE-POSITIVE on an ordinary -O0 function that merely
  // spills >=4 GP and
  // >=4 FP *named* parameters to its frame (e.g. a non-variadic
  // f(int,double,int,double,int,double,long,double)); the misclassification
  // skips detectXMMParams and silently drops every FP argument, so
  // detectVariadic uses this test only where variadic arguments travel in
  // registers; Darwin variadics use the home-slot test (MedVariadicDarwin.cpp).
  return countParamRegSpills(Func, TRI.IntParamRegs) >= kMinSaveAreaRegs &&
         countParamRegSpills(Func, TRI.FPParamRegs) >= kMinSaveAreaRegs;
}

/// The integer argument register the AAPCS64 register save area starts at:
/// va_start's prologue spills each argument register from the first unnamed
/// one through x7 to consecutive doublewords below __gr_top, wherever that is
/// in the frame.  A named parameter's own slot (-O0) lies elsewhere.
std::optional<int> aapcs64FirstVariadicRegister(VariadicScan &S) {
  return firstSavedArgumentRegister(S, std::nullopt);
}

std::optional<int> aapcs64IntegerVariadicRegister(VariadicScan &S) {
  const MedFunc &Func = S.Func;
  const llvm::ArrayRef<uint64_t> IntegerRegs = S.TRI.IntParamRegs;
  const int64_t PointerSize = S.TRI.PointerSize;
  if (IntegerRegs.empty() || PointerSize == 0 ||
      countParamRegSpills(Func, S.TRI.FPParamRegs) != 0)
    return std::nullopt;
  // Just past each slot x7 is spilled to, where __gr_top may point.
  std::set<int64_t> IntegerTops;
  std::map<int64_t, std::vector<const MedOp *>> StoresAt;
  for (const MedBlock &Block : Func.Blocks)
    for (const MedOp &Op : Block.Ops) {
      if (Op.Opcode != NdOp::STORE || Op.NumInputs < 2 ||
          Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
        continue;
      const auto At = entrySpDelta(S, Op.Inputs[0], 0);
      if (!At)
        continue;
      StoresAt[*At].push_back(&Op);
      const MedVar &Value = Op.Inputs[1];
      if (Value.Kind == MedVar::Reg && Value.SSAVer == 0 &&
          Value.RegOff == IntegerRegs.back() && Value.Size == PointerSize)
        IntegerTops.insert(*At + PointerSize);
    }
  const auto Constants = constantValues(S);
  const auto ConstantOf = [&](const MedVar &V) -> std::optional<uint64_t> {
    if (V.isConst())
      return V.ConstVal;
    const auto It = Constants.find(VariadicValueIndex::keyOf(V));
    if (It == Constants.end())
      return std::nullopt;
    return It->second.Value;
  };
  // __gr_offs is minus the bytes of the saved registers: one 4-byte store,
  // or the low word of one 8-byte store of both offsets.
  const auto StoresGrOffs = [&](int64_t VaList, int First) {
    const auto Expected = static_cast<uint32_t>(
        -static_cast<int64_t>(IntegerRegs.size() - First) * PointerSize);
    const auto It = StoresAt.find(VaList + kVaListGrOffs);
    return It != StoresAt.end() &&
           llvm::any_of(It->second, [&](const MedOp *Op) {
             const auto Value = ConstantOf(Op->Inputs[1]);
             return Value &&
                    (Op->Inputs[1].Size == kVaListOffsBytes ||
                     Op->Inputs[1].Size == 2 * kVaListOffsBytes) &&
                    static_cast<uint32_t>(*Value) == Expected;
           });
  };
  const auto StoresStack = [&](int64_t VaList) {
    const auto It = StoresAt.find(VaList + kVaListStack);
    return It != StoresAt.end() &&
           llvm::any_of(It->second, [&](const MedOp *Op) {
             const auto Value = entrySpDelta(S, Op->Inputs[1], 0);
             return Op->Inputs[1].Size == PointerSize && Value && *Value >= 0;
           });
  };
  // One va_list points __gr_top just past the spills, __stack into the
  // incoming arguments, and sets __gr_offs.
  for (const auto &[At, Stores] : StoresAt)
    for (const MedOp *Op : Stores) {
      const auto Value = entrySpDelta(S, Op->Inputs[1], 0);
      if (Op->Inputs[1].Size != PointerSize || !Value ||
          !IntegerTops.count(*Value))
        continue;
      const int64_t VaList = At - kVaListGrTop;
      if (!StoresStack(VaList))
        continue;
      // The contiguous spill chain must end at this va_list's __gr_top,
      // not at a different copy of x7 elsewhere in the frame.
      const int First = firstSavedArgumentRegister(S, *Value - PointerSize);
      if (First < static_cast<int>(IntegerRegs.size()) &&
          StoresGrOffs(VaList, First))
        return First;
    }
  return std::nullopt;
}

std::vector<std::pair<size_t, size_t>>
unreadAAPCS64VectorSpills(VariadicScan &S, int FirstVariadicRegister) {
  const TargetRegInfo &TRI = S.TRI;
  if (TRI.FPParamRegs.empty() || TRI.IntParamRegs.empty() ||
      TRI.PointerSize == 0 || FirstVariadicRegister < 0 ||
      static_cast<size_t>(FirstVariadicRegister) > TRI.IntParamRegs.size())
    return {};
  // The exception dispatcher enters a handler with values no edge here
  // shows; keep a function that has one.
  for (const MedBlock &Block : S.Func.Blocks)
    if (!Block.ExceptionalSuccs.empty() || !Block.ExceptionalPreds.empty())
      return {};
  const auto Area = vectorSaveArea(S);
  if (!Area)
    return {};
  const ComputedConstants Constants = constantValues(S);
  const auto VaList = vaListOver(S, *Area, FirstVariadicRegister, Constants);
  if (!VaList ||
      !frameStaysPrivate(S, *Area, *VaList, FirstVariadicRegister, Constants))
    return {};
  return Area->Spills;
}

} // namespace med_variadic_detail
} // namespace neverd
