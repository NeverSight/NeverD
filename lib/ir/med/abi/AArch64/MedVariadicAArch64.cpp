//===- MedVariadicAArch64.cpp - AAPCS64 variadic prologue -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The AAPCS64 variadic prologue recognizer: the register save area that
/// spills most of both the GP and FP argument registers at entry, and the
/// proof that no read observes its vector half.
///
//===----------------------------------------------------------------------===//

#include "../MedVariadicDetail.h"

#include "llvm/ADT/STLExtras.h"

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

/// The constants the function's integer arithmetic computes, by value: what
/// va_start stores in __gr_offs and __vr_offs, which an AArch64 MOVK builds
/// from an AND and an OR of earlier constants, and -O0 stores as the low
/// bytes of a register, before propagation folds them.
std::map<VariadicValueIndex::Key, uint64_t> constantValues(VariadicScan &S) {
  const VariadicValueIndex &Index = S.values();
  std::map<VariadicValueIndex::Key, uint64_t> Known;
  const auto ValueOf = [&](const MedVar &V) -> std::optional<uint64_t> {
    if (V.isConst())
      return V.ConstVal;
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
                                        : std::optional<uint64_t>();
        std::optional<uint64_t> Result;
        switch (Op.Opcode) {
        case NdOp::COPY:
          if (Op.NumInputs == 1)
            Result = A;
          break;
        case NdOp::INT_ZEXT:
          if (Op.NumInputs == 1 && A && Op.Inputs[0].Size > 0 &&
              Op.Inputs[0].Size < sizeof(uint64_t))
            Result = *A & ((uint64_t{1} << (8 * Op.Inputs[0].Size)) - 1);
          else if (Op.NumInputs == 1)
            Result = A;
          break;
        case NdOp::INT_AND:
          if (A && B)
            Result = *A & *B;
          break;
        case NdOp::INT_OR:
          if (A && B)
            Result = *A | *B;
          break;
        case NdOp::INT_XOR:
          if (A && B)
            Result = *A ^ *B;
          break;
        case NdOp::INT_ADD:
          if (A && B)
            Result = *A + *B;
          break;
        case NdOp::INT_SUB:
          if (A && B)
            Result = *A - *B;
          break;
        case NdOp::SUBBYTES:
          // -O0 stores a W register, the low bytes of the X one MOV wrote.
          if (A && B && *B < sizeof(uint64_t))
            Result = *A >> (8 * *B);
          break;
        default:
          break;
        }
        if (!Result)
          continue;
        if (Op.Output.Size < sizeof(uint64_t))
          *Result &= (uint64_t{1} << (8 * Op.Output.Size)) - 1;
        Known.emplace(Key, *Result);
        Grew = true;
      }
  }
  return Known;
}

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
                                  int FirstVariadicRegister) {
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
  const auto Constants = constantValues(S);
  const auto ConstantOf = [&](const MedVar &V) -> std::optional<uint64_t> {
    if (V.isConst())
      return V.ConstVal;
    const auto It = Constants.find(VariadicValueIndex::keyOf(V));
    if (It == Constants.end())
      return std::nullopt;
    return It->second;
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
/// stack pointer is used only as the fixed address of a load that reads
/// none of them, as an address stored to, as one of va_start's pointers, in
/// arithmetic and in branches, and is in no argument register a call or a
/// return reads, nor in a vector register.  A load ends the chain: what
/// va_arg reads from __stack and __gr_top it uses within the overflow and
/// integer areas, as the va_list convention keeps it.
bool frameStaysPrivate(VariadicScan &S, const VectorSaveArea &Area,
                       int64_t VaList, int FirstVariadicRegister) {
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

  const auto ReadsVectorState = [&](int64_t Begin, int64_t End) {
    return overlaps(Begin, End, Area.Begin, Area.End) ||
           overlaps(Begin, End, VaList + kVaListVrTop,
                    VaList + kVaListVrTop + PointerSize) ||
           overlaps(Begin, End, VaList + kVaListVrOffs,
                    VaList + kVaListVrOffs + kVaListOffsBytes);
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
        const auto At = entrySpDelta(S, Op.Inputs[0], 0);
        if (Operand || !At || Op.Output.Size == 0 ||
            Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
            ReadsVectorState(*At, *At + Op.Output.Size))
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
        const auto Value = entrySpDelta(S, Op.Inputs[1], 0);
        if (!At || !Value || Op.Inputs[1].Size != PointerSize ||
            !isVaStartPointer(Area, VaList, IntegerHalf, *At, *Value))
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
  const auto VaList = vaListOver(S, *Area, FirstVariadicRegister);
  if (!VaList || !frameStaysPrivate(S, *Area, *VaList, FirstVariadicRegister))
    return {};
  return Area->Spills;
}

} // namespace med_variadic_detail
} // namespace neverd
