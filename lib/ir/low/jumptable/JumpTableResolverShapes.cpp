//===- JumpTableResolverShapes.cpp - Composite table shapes --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Recognizers for composite and decoupled single-level jump-table layouts:
/// runtime-selected two-table dispatch, where one branch picks between two
/// adjacent code-pointer tables at run time, and constant-base absolute tables
/// whose load is decoupled from the branch by an -O0 spill/reload relay or a
/// shared multi-site computed-goto dispatch.  The two-level index-byte table
/// lives in JumpTableResolverTwoLevel.cpp.
///
/// Part of the CFGBuilder jump-table resolver; see JumpTableResolver.cpp for
/// top-level strategy dispatch and JumpTableResolverDetail.h for shared
/// backward-slicing helpers.
///
//===----------------------------------------------------------------------===//

#include "JumpTableResolverDetail.h"

#include "neverd/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/lift/AArch64Regs.h"

#include "llvm/Support/Debug.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#define DEBUG_TYPE "neverd-cfg-builder"

namespace neverd {

static uint64_t truncateToByteWidth(uint64_t Value, uint16_t Bytes) {
  if (Bytes == 0 || Bytes >= sizeof(Value))
    return Value;
  return Value & llvm::maskTrailingOnes<uint64_t>(Bytes * CHAR_BIT);
}

//===----------------------------------------------------------------------===//
// tryTwoTableSelect — runtime-selected table base (two adjacent tables)
//===----------------------------------------------------------------------===//

bool CFGBuilder::tryTwoTableSelect(const BinaryImage &Img,
                                   const InsnRecord &Rec, JumpTableInfo &Info,
                                   size_t *CandidateEvidenceBudget,
                                   bool *AnalysisComplete) {
  if (AnalysisComplete)
    *AnalysisComplete = false;
  bool Complete = true;
  struct CompletionPublisher {
    bool *Output;
    const bool &Complete;
    ~CompletionPublisher() {
      if (Output)
        *Output = Complete;
    }
  } PublishCompletion{AnalysisComplete, Complete};
  if (!CurrentImg || !CandidateEvidenceBudget) {
    Complete = false;
    return false;
  }
  auto consumeWork = [&](size_t Amount = 1) {
    if (Amount > *CandidateEvidenceBudget) {
      *CandidateEvidenceBudget = 0;
      Complete = false;
      return false;
    }
    *CandidateEvidenceBudget -= Amount;
    return true;
  };
  auto consumeProduct = [&](size_t Count, size_t Cost) {
    if (Count != 0 &&
        Cost > std::numeric_limits<size_t>::max() / Count) {
      *CandidateEvidenceBudget = 0;
      Complete = false;
      return false;
    }
    return consumeWork(Count * Cost);
  };
  auto consumeFactorProduct = [&](std::initializer_list<size_t> Factors) {
    size_t Product = 1;
    for (size_t Factor : Factors) {
      if (Factor != 0 &&
          Product > std::numeric_limits<size_t>::max() / Factor) {
        *CandidateEvidenceBudget = 0;
        Complete = false;
        return false;
      }
      Product *= Factor;
    }
    return consumeWork(Product);
  };
  auto orderedLookupWork = [](size_t Count) {
    size_t Work = 1;
    for (size_t N = Count; N > 1; N = N / 2 + N % 2)
      ++Work;
    return Work;
  };
  auto ensureAppendCapacity = [&](auto &Values, size_t Additional = 1) {
    const size_t Max = std::numeric_limits<size_t>::max();
    if (Additional > Max - Values.size()) {
      *CandidateEvidenceBudget = 0;
      Complete = false;
      return false;
    }
    const size_t Required = Values.size() + Additional;
    if (Required <= Values.capacity())
      return true;
    const size_t Doubled = Values.capacity() == 0
                               ? size_t{1}
                               : (Values.capacity() > Max / 2
                                      ? Max
                                      : Values.capacity() * 2);
    const size_t NewCapacity = std::max(Required, Doubled);
    if (NewCapacity == Max || !consumeProduct(NewCapacity, 2) ||
        !consumeWork(Values.size())) {
      if (NewCapacity == Max) {
        *CandidateEvidenceBudget = 0;
        Complete = false;
      }
      return false;
    }
    Values.reserve(NewCapacity);
    return true;
  };

  bool HasIndBranch = false;
  for (auto &Op : Rec.Ops) {
    if (!consumeWork())
      return false;
    if (Op.Opcode == NdOp::INDIR_BR && Op.NumInputs >= 1) {
      HasIndBranch = true;
      break;
    }
  }
  if (!HasIndBranch)
    return false;

  // Flatten the whole function prefix so a table base materialised in a
  // dominating block (the `lea`/`adrp`/`leal GOTOFF`) and a spill store of one
  // table base (i386 `cmov (%esp),...`) are both in scope.
  if (!consumeWork(2) || !consumeWork(orderedLookupWork(Insns.size())))
    return false;
  std::vector<LowOp> Ops;
  for (auto It = Insns.lower_bound(CurrentFuncEntry);
       It != Insns.end() && It->first <= Rec.Addr; ++It) {
    if (!consumeWork())
      return false;
    for (auto &Op : It->second.Ops) {
      if (!consumeWork() || !ensureAppendCapacity(Ops) || !consumeWork())
        return false;
      Ops.push_back(Op);
    }
  }

  auto budgetedReachingDefIdx = [&](int FromIdx, const NdVar &Value) {
    const int Last = std::min(FromIdx, static_cast<int>(Ops.size()) - 1);
    for (int I = Last; I >= 0; --I) {
      if (!consumeWork())
        return -1;
      const NdVar &Output = Ops[I].Output;
      if (Output.Space == Value.Space && Output.Offset == Value.Offset)
        return I;
    }
    return -1;
  };
  auto budgetedTraceIndexToRegister = [&](int FromIdx, NdVar Value) {
    for (int Depth = 0; Depth < limits::kMaxQuasiCopyDepth; ++Depth) {
      if (!consumeWork())
        return InvalidVA;
      if (Value.isReg())
        return Value.Offset;
      if (!Value.isTemp())
        return InvalidVA;
      const int Def = budgetedReachingDefIdx(FromIdx, Value);
      if (Def < 0)
        return InvalidVA;
      const LowOp &Op = Ops[Def];
      if ((Op.Opcode == NdOp::COPY || Op.Opcode == NdOp::INT_ZEXT ||
           Op.Opcode == NdOp::INT_SEXT) &&
          Op.NumInputs >= 1) {
        Value = Op.Inputs[0];
      } else if (Op.Opcode == NdOp::SUBBYTES && Op.NumInputs >= 2 &&
                 Op.Inputs[1].isConst() && Op.Inputs[1].Offset == 0) {
        Value = Op.Inputs[0];
      } else {
        return InvalidVA;
      }
      FromIdx = Def - 1;
    }
    Complete = false;
    return InvalidVA;
  };
  auto budgetedTraceToRegister = [&](int FromIdx, NdVar Value) {
    for (int Depth = 0; Depth < limits::kMaxQuasiCopyDepth; ++Depth) {
      if (!consumeWork())
        return InvalidVA;
      if (Value.isReg())
        return Value.Offset;
      if (!Value.isTemp())
        return InvalidVA;
      const int Def = budgetedReachingDefIdx(FromIdx, Value);
      if (Def < 0 || Ops[Def].Opcode != NdOp::COPY ||
          Ops[Def].NumInputs < 1)
        return InvalidVA;
      Value = Ops[Def].Inputs[0];
      FromIdx = Def - 1;
    }
    Complete = false;
    return InvalidVA;
  };
  auto budgetedScaledIndexReg = [&](int FromIdx, NdVar Value,
                                    NdVar *IndexValue, va_t *IndexUseAddr,
                                    int *IndexUseSeq) {
    for (int Depth = 0; Depth < limits::kMaxQuasiCopyDepth; ++Depth) {
      if (!consumeWork())
        return InvalidVA;
      if (!Value.isTemp() && !Value.isReg())
        return InvalidVA;
      const int Def = budgetedReachingDefIdx(FromIdx, Value);
      if (Def < 0)
        return InvalidVA;
      const LowOp &Op = Ops[Def];
      const bool Scaled =
          (Op.Opcode == NdOp::INT_MULT && Op.NumInputs >= 2 &&
           Op.Inputs[1].isConst() && Op.Inputs[1].Offset > 1) ||
          (Op.Opcode == NdOp::INT_LEFT && Op.NumInputs >= 2 &&
           Op.Inputs[1].isConst() && Op.Inputs[1].Offset >= 1);
      if (Scaled) {
        NdVar ExactIndex = Op.Inputs[0];
        va_t ExactUseAddr = Op.Addr;
        int ExactUseSeq = Op.Seq;
        const int WidenDef = budgetedReachingDefIdx(Def - 1, Op.Inputs[0]);
        if (WidenDef >= 0 && Ops[WidenDef].Opcode == NdOp::INT_ZEXT &&
            Ops[WidenDef].NumInputs >= 1 &&
            Ops[WidenDef].Inputs[0].Size != 0 &&
            Ops[WidenDef].Inputs[0].Size < Ops[WidenDef].Output.Size) {
          ExactIndex = Ops[WidenDef].Inputs[0];
          ExactUseAddr = Ops[WidenDef].Addr;
          ExactUseSeq = Ops[WidenDef].Seq;
        }
        if (IndexValue)
          *IndexValue = ExactIndex;
        if (IndexUseAddr)
          *IndexUseAddr = ExactUseAddr;
        if (IndexUseSeq)
          *IndexUseSeq = ExactUseSeq;
        return budgetedTraceIndexToRegister(Def - 1, Op.Inputs[0]);
      }
      if ((Op.Opcode != NdOp::COPY && Op.Opcode != NdOp::INT_ZEXT &&
           Op.Opcode != NdOp::INT_SEXT) ||
          Op.NumInputs < 1)
        return InvalidVA;
      Value = Op.Inputs[0];
      FromIdx = Def - 1;
    }
    Complete = false;
    return InvalidVA;
  };
  auto relocationSlotPresent = [&](va_t Address) -> std::optional<bool> {
    if (!consumeWork(orderedLookupWork(Img.CodePtrRelocSlots.size())))
      return std::nullopt;
    return Img.CodePtrRelocSlots.count(Address) != 0;
  };
  auto hasRelocationPrefix = [&](va_t Base, uint16_t Width,
                                 uint32_t Count) -> std::optional<bool> {
    if (Width == 0)
      return false;
    for (uint32_t Index = 0; Index < Count; ++Index) {
      if (!consumeWork())
        return std::nullopt;
      if (Index != 0 && Width > (InvalidVA - Base) / Index)
        return false;
      const std::optional<bool> Present =
          relocationSlotPresent(Base + static_cast<va_t>(Index) * Width);
      if (!Present)
        return std::nullopt;
      if (!*Present)
        return false;
    }
    return true;
  };

  const TargetRegInfo &TRI = getTargetRegInfo(CurrentImg->Arch);

  // Entry size of the table currently being matched; set per LOAD candidate in
  // the scan below.  foldArm uses it to prefer an arm fold that is an actual
  // code-pointer table over a stale non-table constant (see below).
  uint16_t TableEntW = 0;

  // i386 effective addresses are represented in the common internal VA width
  // after the guest-pointer arithmetic has completed.  That canonical
  // widening is not a frame epoch change, but the legacy frameSlotKey helper
  // intentionally does not traverse width changes because several older
  // authorization paths still consume it directly.  Peel the widening only
  // for this TwoTable spill candidate; the final point-sensitive load-role
  // certificate remains authoritative for publication.
  auto twoTableFrameSlotKey = [&](NdVar Address, int From, uint64_t &Base,
                                  int64_t &Offset) {
    auto signedFrameDelta = [](const NdVar &Value,
                               uint16_t ArithmeticSize)
        -> std::optional<int64_t> {
      if (!Value.isConst() || Value.Size == 0 ||
          Value.Size > sizeof(uint64_t) || ArithmeticSize == 0 ||
          ArithmeticSize > sizeof(uint64_t) ||
          Value.Provenance != ConstantAddressProvenance::Scalar)
        return std::nullopt;
      const unsigned SourceBits = static_cast<unsigned>(Value.Size) * CHAR_BIT;
      const unsigned ArithmeticBits =
          static_cast<unsigned>(ArithmeticSize) * CHAR_BIT;
      const uint64_t SourceMask =
          SourceBits == 64 ? std::numeric_limits<uint64_t>::max()
                           : (uint64_t{1} << SourceBits) - 1;
      const uint64_t ArithmeticMask =
          ArithmeticBits == 64 ? std::numeric_limits<uint64_t>::max()
                               : (uint64_t{1} << ArithmeticBits) - 1;
      const uint64_t Raw = (Value.Offset & SourceMask) & ArithmeticMask;
      const uint64_t Sign = uint64_t{1} << (ArithmeticBits - 1);
      if ((Raw & Sign) == 0)
        return static_cast<int64_t>(Raw);
      return -1 - static_cast<int64_t>((~Raw) & ArithmeticMask);
    };
    auto checkedFrameOffset = [](int64_t Current, int64_t Delta,
                                 bool Subtract) -> std::optional<int64_t> {
      constexpr int64_t Min = std::numeric_limits<int64_t>::min();
      constexpr int64_t Max = std::numeric_limits<int64_t>::max();
      if (!Subtract) {
        if ((Delta > 0 && Current > Max - Delta) ||
            (Delta < 0 && Current < Min - Delta))
          return std::nullopt;
        return Current + Delta;
      }
      if ((Delta > 0 && Current < Min + Delta) ||
          (Delta < 0 && Current > Max + Delta))
        return std::nullopt;
      return Current - Delta;
    };

    // Preserve the legacy two-phase contract: first peel the guest-address
    // COPY/ZEXT envelope, then resolve the frame expression itself.  Each phase
    // has its own bounded depth and charges the same candidate transaction.
    bool PeelStopped = false;
    for (int Depth = 0; Depth < limits::kMaxQuasiCopyDepth; ++Depth) {
      if (!consumeWork())
        return false;
      if (Address.isReg() && TRI.isFrameReg(Address.Offset)) {
        PeelStopped = true;
        break;
      }
      if (!Address.isReg() && !Address.isTemp()) {
        PeelStopped = true;
        break;
      }
      const int D = budgetedReachingDefIdx(From, Address);
      if (!Complete)
        return false;
      if (D < 0) {
        PeelStopped = true;
        break;
      }
      const LowOp &Def = Ops[D];
      if (Def.Opcode == NdOp::COPY && Def.NumInputs >= 1) {
        Address = Def.Inputs[0];
        From = D - 1;
        continue;
      }
      if (Def.Opcode == NdOp::INT_ZEXT && Def.NumInputs >= 1 &&
          Def.Inputs[0].Size == Img.getPointerSize() &&
          Def.Output.Size > Def.Inputs[0].Size) {
        Address = Def.Inputs[0];
        From = D - 1;
        continue;
      }
      PeelStopped = true;
      break;
    }
    if (!PeelStopped) {
      Complete = false;
      return false;
    }

    Offset = 0;
    for (int Depth = 0; Depth < limits::kMaxQuasiCopyDepth; ++Depth) {
      if (!consumeWork())
        return false;
      if (Address.isReg()) {
        if (!TRI.isFrameReg(Address.Offset))
          return false;
        Base = Address.Offset;
        return true;
      }
      if (!Address.isTemp())
        return false;
      const int D = budgetedReachingDefIdx(From, Address);
      if (!Complete)
        return false;
      if (D < 0)
        return false;
      const LowOp &Def = Ops[D];
      if (Def.Opcode == NdOp::COPY && Def.NumInputs >= 1) {
        Address = Def.Inputs[0];
        From = D - 1;
        continue;
      }
      if (Def.Opcode == NdOp::INT_ADD && Def.NumInputs >= 2) {
        const int ConstantSide = Def.Inputs[1].isConst()
                                     ? 1
                                     : (Def.Inputs[0].isConst() ? 0 : -1);
        if (ConstantSide < 0)
          return false;
        if (budgetedScaledIndexReg(D - 1, Def.Inputs[1 - ConstantSide],
                                   nullptr, nullptr, nullptr) != InvalidVA)
          return false;
        if (!Complete)
          return false;
        const std::optional<int64_t> Delta =
            signedFrameDelta(Def.Inputs[ConstantSide], Def.Output.Size);
        if (!Delta)
          return false;
        const std::optional<int64_t> Next =
            checkedFrameOffset(Offset, *Delta, false);
        if (!Next)
          return false;
        Offset = *Next;
        Address = Def.Inputs[1 - ConstantSide];
        From = D - 1;
        continue;
      }
      if (Def.Opcode == NdOp::INT_SUB && Def.NumInputs >= 2 &&
          Def.Inputs[1].isConst()) {
        const std::optional<int64_t> Delta =
            signedFrameDelta(Def.Inputs[1], Def.Output.Size);
        if (!Delta)
          return false;
        const std::optional<int64_t> Next =
            checkedFrameOffset(Offset, *Delta, true);
        if (!Next)
          return false;
        Offset = *Next;
        Address = Def.Inputs[0];
        From = D - 1;
        continue;
      }
      return false;
    }
    Complete = false;
    return false;
  };

  // Fold a select arm (one of the two candidate table-base sub-expressions) to
  // a constant table address.  A register arm is folded by emulating the prefix
  // up to the select (`Cutoff`), before the select overwrites it; a spilled arm
  // (i386 `cmov (%esp)`) is store-forwarded to the register that produced it.
  std::function<std::optional<uint64_t>(NdVar, int, va_t, int)> foldArm =
      [&](NdVar V, int From, va_t Cutoff,
          int Depth) -> std::optional<uint64_t> {
    if (!consumeWork())
      return std::nullopt;
    if (Depth > limits::kMaxSliceDepth) {
      Complete = false;
      return std::nullopt;
    }
    if (V.isConst())
      return V.Offset;
    NdVar Cur = V;
    int CurFrom = From;
    // A non-table constant a register folded to is kept only as a last resort:
    // the table-base register may have been REUSED earlier in the block as a
    // loop-carried value (e.g. `mov %rdx,%r8` overwriting an accumulator that
    // also lived in r8), so foldRegConstant can emulate the stale accumulator
    // value (which may happen to land in a mapped segment).  When that fold is
    // not a code-pointer table, keep following the def chain (the `mov` COPY to
    // the real base) and only fall back to it if the chain yields no table.
    std::optional<uint64_t> RegFallback;
    int G = 0;
    for (; G < limits::kMaxSliceDepth; ++G) {
      if (!consumeWork())
        return std::nullopt;
      if (Cur.isConst())
        return Cur.Offset;
      // A table base materialised in a dominator (`lea`/`adrp+add`/`leal
      // GOTOFF`/`pc+litpool`) folds via prefix emulation; a runtime copy of it
      // in the loop body does not (the emulator halts at the loop back-edge),
      // so try each register along the COPY chain and take the first that
      // folds.  Once the def chain moves before a register overwrite, emulate
      // only up to that earlier use point; using the select's cutoff throughout
      // would observe the newer register value and can mistake the other table
      // arm for this one.
      if (Cur.isReg()) {
        va_t FoldCutoff = Cutoff;
        if (CurFrom >= 0 && CurFrom < static_cast<int>(Ops.size()))
          FoldCutoff = std::min(FoldCutoff, Ops[CurFrom].Addr);
        auto F = foldRegConstant(Img, Rec, Cur.Offset, FoldCutoff, consumeWork);
        if (!Complete)
          return std::nullopt;
        if (F) {
          const std::optional<bool> HasRelocation =
              TableEntW == 0 ? std::optional<bool>(true)
                             : relocationSlotPresent(*F);
          if (!HasRelocation)
            return std::nullopt;
          if (*HasRelocation)
            return F;
          if (!RegFallback)
            RegFallback = F;
        }
      }
      int D = budgetedReachingDefIdx(CurFrom, Cur);
      if (!Complete)
        return std::nullopt;
      if (D < 0)
        break;
      const LowOp &O = Ops[D];
      if ((O.Opcode == NdOp::COPY || O.Opcode == NdOp::INT_ZEXT ||
           O.Opcode == NdOp::INT_SEXT) &&
          O.NumInputs >= 1) {
        Cur = O.Inputs[0];
        CurFrom = D - 1;
        continue;
      }
      if (O.Opcode == NdOp::SUBBYTES && O.NumInputs >= 2 &&
          O.Inputs[1].isConst() && O.Inputs[1].Offset == 0) {
        auto F = foldArm(O.Inputs[0], D - 1, Cutoff, Depth + 1);
        if (!Complete)
          return std::nullopt;
        if (!F)
          break;
        return truncateToByteWidth(*F, O.Output.Size);
      }
      if (O.Opcode == NdOp::INT_ADD && O.NumInputs >= 2) {
        auto A = foldArm(O.Inputs[0], D - 1, Cutoff, Depth + 1);
        if (!Complete)
          return std::nullopt;
        auto B = foldArm(O.Inputs[1], D - 1, Cutoff, Depth + 1);
        if (!Complete)
          return std::nullopt;
        if (A && B) {
          return truncateToByteWidth(*A + *B, O.Output.Size);
        }
        // i386 PIC materialises a table base as GOT_base + GOTOFF.  GOT_base
        // is legitimately zero in the relocatable image model, but
        // foldRegConstant deliberately rejects a zero fold, leaving just the
        // GOTOFF addend here.  Accept that addend only when the image proves it
        // starts a code-pointer relocation run; this keeps an unrelated
        // partially-folded add from being mistaken for a table address.
        if (!A && B && TableEntW != 0) {
          const std::optional<bool> HasRelocation = relocationSlotPresent(*B);
          if (!HasRelocation)
            return std::nullopt;
          if (*HasRelocation)
            return B;
        }
        if (A && !B && TableEntW != 0) {
          const std::optional<bool> HasRelocation = relocationSlotPresent(*A);
          if (!HasRelocation)
            return std::nullopt;
          if (*HasRelocation)
            return A;
        }
        break;
      }
      if (O.Opcode == NdOp::INT_SUB && O.NumInputs >= 2) {
        auto A = foldArm(O.Inputs[0], D - 1, Cutoff, Depth + 1);
        if (!Complete)
          return std::nullopt;
        auto B = foldArm(O.Inputs[1], D - 1, Cutoff, Depth + 1);
        if (!Complete)
          return std::nullopt;
        if (A && B) {
          return truncateToByteWidth(*A - *B, O.Output.Size);
        }
        break;
      }
      if (O.Opcode == NdOp::LOAD) {
        // Store-forward a spilled table base (i386 `cmov (%esp),...`).
        const NdVar &AddrV = (O.NumInputs >= 2) ? O.Inputs[1] : O.Inputs[0];
        uint64_t LBase = 0;
        int64_t LOff = 0;
        if (!twoTableFrameSlotKey(AddrV, D - 1, LBase, LOff))
          break;
        for (int K = D - 1; K >= 0; --K) {
          if (!consumeWork())
            return std::nullopt;
          const LowOp &S = Ops[K];
          if (S.Opcode != NdOp::STORE || S.NumInputs < 2)
            continue;
          const NdVar &SAddr = (S.NumInputs >= 3) ? S.Inputs[1] : S.Inputs[0];
          const NdVar &SVal = (S.NumInputs >= 3) ? S.Inputs[2] : S.Inputs[1];
          uint64_t SB = 0;
          int64_t SO = 0;
          if (twoTableFrameSlotKey(SAddr, K - 1, SB, SO) && SB == LBase &&
              SO == LOff)
            return foldArm(SVal, K - 1, Cutoff, Depth + 1);
          if (!Complete)
            return std::nullopt;
        }
        break;
      }
      break;
    }
    if (G == limits::kMaxSliceDepth)
      Complete = false;
    return RegFallback;
  };

  // Resolve a blend arm's mask through COPY chains.  A valid pointer select
  // uses an all-zero/all-ones mask produced by INT_NEG2(condition) and its
  // exact INT_NOT complement; arbitrary complementary bit masks could splice
  // the two addresses into a third pointer and are not a table-base select.
  struct MaskDefinition {
    int Kind = -1; // 0 = INT_NEG2 base mask, 1 = INT_NOT complement
    int OpIndex = -1;
  };
  auto maskDefinition = [&](NdVar M, int From) -> MaskDefinition {
    for (int G = 0; G < limits::kMaxQuasiCopyDepth; ++G) {
      if (!consumeWork())
        return {};
      int D = budgetedReachingDefIdx(From, M);
      if (!Complete)
        return {};
      if (D < 0)
        return {};
      if (Ops[D].Opcode == NdOp::INT_NOT)
        return {1, D};
      if (Ops[D].Opcode == NdOp::INT_NEG2)
        return {0, D};
      if (Ops[D].Opcode == NdOp::COPY && Ops[D].NumInputs >= 1) {
        M = Ops[D].Inputs[0];
        From = D - 1;
        continue;
      }
      return {};
    }
    Complete = false;
    return {};
  };

  for (int I = static_cast<int>(Ops.size()) - 1; I >= 0; --I) {
    if (!consumeWork())
      return false;
    const LowOp &L = Ops[I];
    if (L.Opcode != NdOp::LOAD || L.NumInputs < 1)
      continue;
    uint16_t W = L.Output.Size;
    if (W != 4 && W != 8)
      continue;
    TableEntW = W;
    const NdVar &AddrV = (L.NumInputs >= 2) ? L.Inputs[1] : L.Inputs[0];
    if (!AddrV.isReg() && !AddrV.isTemp())
      continue;
    int AddIdx = budgetedReachingDefIdx(I - 1, AddrV);
    if (!Complete)
      return false;
    int AddressCopyDepth = 0;
    for (; AddIdx >= 0 && AddressCopyDepth < limits::kMaxQuasiCopyDepth;
         ++AddressCopyDepth) {
      if (!consumeWork())
        return false;
      const LowOp &Forwarder = Ops[AddIdx];
      if (Forwarder.NumInputs < 1 ||
          (!Forwarder.Inputs[0].isReg() && !Forwarder.Inputs[0].isTemp()))
        break;
      const bool IsCopy = Forwarder.Opcode == NdOp::COPY;
      const bool IsCanonicalGuestAddressWiden =
          Forwarder.Opcode == NdOp::INT_ZEXT &&
          Forwarder.Inputs[0].Size == Img.getPointerSize() &&
          Forwarder.Output.Size >= Forwarder.Inputs[0].Size;
      if (!IsCopy && !IsCanonicalGuestAddressWiden)
        break;
      AddIdx = budgetedReachingDefIdx(AddIdx - 1, Forwarder.Inputs[0]);
      if (!Complete)
        return false;
    }
    if (AddressCopyDepth == limits::kMaxQuasiCopyDepth && AddIdx >= 0 &&
        Ops[AddIdx].NumInputs >= 1 &&
        (Ops[AddIdx].Inputs[0].isReg() || Ops[AddIdx].Inputs[0].isTemp()) &&
        (Ops[AddIdx].Opcode == NdOp::COPY ||
         (Ops[AddIdx].Opcode == NdOp::INT_ZEXT &&
          Ops[AddIdx].Inputs[0].Size == Img.getPointerSize() &&
          Ops[AddIdx].Output.Size >= Ops[AddIdx].Inputs[0].Size))) {
      Complete = false;
      return false;
    }
    if (AddIdx < 0 || Ops[AddIdx].Opcode != NdOp::INT_ADD ||
        Ops[AddIdx].NumInputs < 2)
      continue;

    // The load address is `base + index`; the base is the runtime-selected
    // table pointer.  Try each operand as the base.
    for (int BaseW = 0; BaseW < 2; ++BaseW) {
      if (!consumeWork())
        return false;
      NdVar BaseV = Ops[AddIdx].Inputs[BaseW];
      NdVar IdxV = Ops[AddIdx].Inputs[1 - BaseW];
      int BDef = budgetedReachingDefIdx(AddIdx - 1, BaseV);
      if (!Complete)
        return false;
      int BaseCopyDepth = 0;
      for (;
           BDef >= 0 && Ops[BDef].Opcode == NdOp::COPY &&
           Ops[BDef].NumInputs >= 1 &&
           (Ops[BDef].Inputs[0].isReg() || Ops[BDef].Inputs[0].isTemp()) &&
           BaseCopyDepth < limits::kMaxQuasiCopyDepth;
           ++BaseCopyDepth) {
        if (!consumeWork())
          return false;
        BDef = budgetedReachingDefIdx(BDef - 1, Ops[BDef].Inputs[0]);
        if (!Complete)
          return false;
      }
      if (BaseCopyDepth == limits::kMaxQuasiCopyDepth && BDef >= 0 &&
          Ops[BDef].Opcode == NdOp::COPY && Ops[BDef].NumInputs >= 1 &&
          (Ops[BDef].Inputs[0].isReg() || Ops[BDef].Inputs[0].isTemp())) {
        Complete = false;
        return false;
      }
      if (BDef < 0)
        continue;
      const LowOp &BD = Ops[BDef];
      va_t Cutoff = BD.Addr;

      // The positive arm is the SELECT true input / the blend operand ANDed
      // with the base mask M; the negative arm is the SELECT false input / the
      // operand ANDed with ~M.
      NdVar ArmPos, ArmNeg;
      bool Matched = false;
      bool MatchedSelect = false;
      bool MatchedBlend = false;
      int BlendAndIdx[2] = {-1, -1};
      int BlendArmSide[2] = {-1, -1};
      MaskDefinition BlendMaskDef[2];
      int PositiveBlendSide = -1;
      if (BD.Opcode == NdOp::SELECT && BD.NumInputs >= 3) {
        ArmPos = BD.Inputs[1];
        ArmNeg = BD.Inputs[2];
        Matched = true;
        MatchedSelect = true;
      } else if (BD.Opcode == NdOp::INT_OR && BD.NumInputs >= 2) {
        // Each OR input is INT_AND(table_arm, mask).  Split arm vs mask, then
        // classify by whether the mask is the negated one (~M).
        NdVar Arms[2], Masks[2];
        bool BlendOk = true;
        for (int Side = 0; Side < 2 && BlendOk; ++Side) {
          if (!consumeWork())
            return false;
          int AndD = budgetedReachingDefIdx(BDef - 1, BD.Inputs[Side]);
          if (!Complete)
            return false;
          int BlendCopyDepth = 0;
          for (;
               AndD >= 0 && Ops[AndD].Opcode == NdOp::COPY &&
               Ops[AndD].NumInputs >= 1 &&
               BlendCopyDepth < limits::kMaxQuasiCopyDepth;
               ++BlendCopyDepth) {
            if (!consumeWork())
              return false;
            AndD = budgetedReachingDefIdx(AndD - 1, Ops[AndD].Inputs[0]);
            if (!Complete)
              return false;
          }
          if (BlendCopyDepth == limits::kMaxQuasiCopyDepth && AndD >= 0 &&
              Ops[AndD].Opcode == NdOp::COPY && Ops[AndD].NumInputs >= 1 &&
              (Ops[AndD].Inputs[0].isReg() ||
               Ops[AndD].Inputs[0].isTemp())) {
            Complete = false;
            return false;
          }
          if (AndD < 0 || Ops[AndD].Opcode != NdOp::INT_AND ||
              Ops[AndD].NumInputs < 2) {
            BlendOk = false;
            break;
          }
          // The arm is the operand that folds to a code-pointer table; the
          // other is the select mask.
          int ArmWhich = -1;
          for (int W2 = 0; W2 < 2; ++W2) {
            if (!consumeWork())
              return false;
            auto Cand = foldArm(Ops[AndD].Inputs[W2], AndD - 1, Cutoff, 0);
            if (!Complete)
              return false;
            const std::optional<bool> HasRelocation =
                Cand ? relocationSlotPresent(*Cand) : std::optional<bool>(false);
            if (!HasRelocation)
              return false;
            if (*HasRelocation) {
              ArmWhich = W2;
              break;
            }
          }
          if (ArmWhich < 0) {
            BlendOk = false;
            break;
          }
          Arms[Side] = Ops[AndD].Inputs[ArmWhich];
          Masks[Side] = Ops[AndD].Inputs[1 - ArmWhich];
          BlendAndIdx[Side] = AndD;
          BlendArmSide[Side] = ArmWhich;
        }
        if (BlendOk) {
          BlendMaskDef[0] = maskDefinition(Masks[0], BDef - 1);
          if (!Complete)
            return false;
          BlendMaskDef[1] = maskDefinition(Masks[1], BDef - 1);
          if (!Complete)
            return false;
          if (BlendMaskDef[0].Kind >= 0 && BlendMaskDef[1].Kind >= 0 &&
              BlendMaskDef[0].Kind != BlendMaskDef[1].Kind) {
            // Positive arm uses M (the non-negated mask).
            const bool Neg0 = BlendMaskDef[0].Kind == 1;
            PositiveBlendSide = Neg0 ? 1 : 0;
            ArmPos = Neg0 ? Arms[1] : Arms[0];
            ArmNeg = Neg0 ? Arms[0] : Arms[1];
            Matched = true;
            MatchedBlend = true;
          }
        }
      }
      if (!Matched)
        continue;

      auto CposOpt = foldArm(ArmPos, BDef - 1, Cutoff, 0);
      if (!Complete)
        return false;
      auto CnegOpt = foldArm(ArmNeg, BDef - 1, Cutoff, 0);
      if (!Complete)
        return false;
      if (!CposOpt || !CnegOpt || *CposOpt == *CnegOpt)
        continue;
      uint64_t Cpos = *CposOpt, Cneg = *CnegOpt;
      uint64_t Lo = std::min(Cpos, Cneg), Hi = std::max(Cpos, Cneg);
      uint64_t Dbytes = Hi - Lo;
      if (Dbytes == 0 || Dbytes % W != 0)
        continue;

      const std::optional<bool> HasLoPrefix =
          hasRelocationPrefix(Lo, W, limits::kMinJumpTableEntries);
      const std::optional<bool> HasHiPrefix =
          hasRelocationPrefix(Hi, W, limits::kMinJumpTableEntries);
      if (!HasLoPrefix || !HasHiPrefix)
        return false;
      if (!*HasLoPrefix || !*HasHiPrefix)
        continue;

      // A SELECT/blend of two relocation-backed table bases is a
      // distinguishing composite shape.  From this point onward, failure of
      // the exact index-domain, base-merge, address, or target certificate is a
      // hard fail for this dispatch; generic resolvers must not publish one arm
      // as a single table.
      Info.CompositeShapeClaimed = true;

      NdVar ExactIndex;
      va_t IndexUseAddr = InvalidVA;
      int IndexUseSeq = -1;
      bool IndexIsPreScaled = false;
      uint64_t IdxReg =
          budgetedScaledIndexReg(AddIdx - 1, IdxV, &ExactIndex,
                                 &IndexUseAddr, &IndexUseSeq);
      if (!Complete)
        return false;
      if (IdxReg == InvalidVA) {
        // Size optimizers commonly fold `slot * W` into one bit mask, e.g.
        // `(x >> 5) & 0x38` for an eight-entry pointer table.  There is then no
        // MULT/LEFT for scaledIndexReg to find: the exact value consumed by the
        // address ADD is already the byte offset.  Keep that coordinate
        // explicit instead of pretending it is a logical slot index and
        // multiplying by W a second time in the address certificate.
        if ((!IdxV.isReg() && !IdxV.isTemp()) || IdxV.Size == 0)
          return false;
        ExactIndex = IdxV;
        IndexUseAddr = Ops[AddIdx].Addr;
        IndexUseSeq = Ops[AddIdx].Seq;
        IdxReg = budgetedTraceToRegister(AddIdx - 1, IdxV);
        if (!Complete)
          return false;
        IndexIsPreScaled = true;
      }
      if (IdxReg == InvalidVA || ExactIndex.Size == 0 ||
          IndexUseAddr == InvalidVA || IndexUseSeq < 0)
        return false;
      // The x86 LowIR address container is eight bytes even for an i386
      // guest.  The effective-address coordinate is nevertheless the low
      // guest-pointer lane; carry that exact lane into both the mask-domain
      // and LOAD-role proofs so neither side invents a synthetic high half.
      if ((ExactIndex.isReg() || ExactIndex.isTemp()) &&
          Img.getPointerSize() != 0 && ExactIndex.Size > Img.getPointerSize())
        ExactIndex.Size = static_cast<uint16_t>(Img.getPointerSize());

      // Relocation-run length proves that slots contain code pointers; it does
      // not constrain the runtime selector.  Prove the common per-arm domain
      // from the exact logical index occurrence.  A power-of-two AND is a
      // whole-bit-domain proof (including wrap/negative bit patterns), unlike
      // the old finite-prefix evaluator.  Other domains remain fail-closed
      // until represented by the shared exact BoundEvidence lattice.
      RequestedCompleteJumpTableProof = true;
      if (!JumpTableProofContextComplete) {
        Complete = false;
        return false;
      }
      constexpr size_t MaskGroupCount = 13;
      auto maskBound = [&](const LowOp &Mask) -> std::optional<uint32_t> {
        if (Mask.Opcode != NdOp::INT_AND || Mask.NumInputs < 2 ||
            (!Mask.Output.isReg() && !Mask.Output.isTemp()))
          return std::nullopt;
        const int ConstantSide = Mask.Inputs[0].isConst()
                                     ? 0
                                     : (Mask.Inputs[1].isConst() ? 1 : -1);
        if (ConstantSide < 0)
          return std::nullopt;
        const uint64_t M = Mask.Inputs[ConstantSide].Offset;
        uint64_t Bound = 0;
        if (IndexIsPreScaled) {
          // For an already-scaled byte coordinate the exact full domain is
          // `{0,W,...,(N-1)W}`.  An AND mask proves that domain precisely
          // when its low log2(W) bits are zero and the remaining quotient is
          // a contiguous power-of-two mask.
          if (W == 0 || M % W != 0 || M / W >= limits::kMaxJumpTableEntries)
            return std::nullopt;
          Bound = M / W + 1;
        } else {
          if (M >= limits::kMaxJumpTableEntries)
            return std::nullopt;
          Bound = M + 1;
        }
        if (!llvm::isPowerOf2_64(Bound) || Bound > limits::kMaxJumpTableEntries)
          return std::nullopt;
        return static_cast<uint32_t>(Bound);
      };

      // Power-of-two selector domains up to 4096 need only thirteen fixed
      // buckets.  Count first, prepay exact container lifetimes, then fill in a
      // second charged pass; no attacker-sized tree or reallocation is hidden.
      std::array<size_t, MaskGroupCount> MaskCounts{};
      for (const auto &[Addr, Insn] : Insns) {
        if (!consumeWork())
          return false;
        if (Insn.IsInstructionGuard)
          continue;
        for (const LowOp &Mask : Insn.Ops) {
          if (!consumeWork())
            return false;
          const std::optional<uint32_t> Bound = maskBound(Mask);
          if (!Bound)
            continue;
          const size_t Group = llvm::Log2_64(*Bound);
          if (Group >= MaskGroupCount || MaskCounts[Group] ==
                                             std::numeric_limits<size_t>::max()) {
            *CandidateEvidenceBudget = 0;
            Complete = false;
            return false;
          }
          ++MaskCounts[Group];
        }
      }
      std::array<std::vector<JumpTableValueOccurrence>, MaskGroupCount>
          MaskGroups;
      size_t NonEmptyGroups = 0;
      for (size_t Group = 0; Group < MaskGroupCount; ++Group) {
        const size_t Count = MaskCounts[Group];
        if (Count == 0)
          continue;
        if (!consumeProduct(Count, 3) || !consumeWork(2)) {
          *CandidateEvidenceBudget = 0;
          Complete = false;
          return false;
        }
        ++NonEmptyGroups;
        MaskGroups[Group].reserve(Count);
      }
      for (const auto &[Addr, Insn] : Insns) {
        if (!consumeWork())
          return false;
        if (Insn.IsInstructionGuard)
          continue;
        for (const LowOp &Mask : Insn.Ops) {
          if (!consumeWork())
            return false;
          const std::optional<uint32_t> Bound = maskBound(Mask);
          if (!Bound)
            continue;
          const size_t Group = llvm::Log2_64(*Bound);
          MaskGroups[Group].push_back({Mask.Output, Mask.Addr, Mask.Seq,
                                      /*DefinedAtPoint=*/true});
        }
      }

      if (NonEmptyGroups == 0)
        return false;
      // Queries and their bound mapping are retained until the single shared
      // matcher batch returns.  Preserve the original cumulative alternatives:
      // distinct reaching paths can use different mask bounds, so the query for
      // a larger bound must cover every preceding group as well.
      size_t RunningAlternatives = 0;
      size_t TotalAlternativeCopies = 0;
      for (size_t Group = 0; Group < MaskGroupCount; ++Group) {
        if (MaskCounts[Group] == 0)
          continue;
        if (MaskCounts[Group] >
                std::numeric_limits<size_t>::max() - RunningAlternatives ||
            (RunningAlternatives += MaskCounts[Group]) >
                std::numeric_limits<size_t>::max() - TotalAlternativeCopies) {
          *CandidateEvidenceBudget = 0;
          Complete = false;
          return false;
        }
        TotalAlternativeCopies += RunningAlternatives;
      }
      if (!consumeProduct(NonEmptyGroups, 14) || !consumeWork(4) ||
          !consumeProduct(TotalAlternativeCopies, 3))
        return false;
      std::vector<JumpTableValueQuery> Queries;
      std::vector<uint32_t> QueryBounds;
      Queries.reserve(NonEmptyGroups);
      QueryBounds.reserve(NonEmptyGroups);
      RunningAlternatives = 0;
      for (size_t Group = 0; Group < MaskGroupCount; ++Group) {
        if (MaskGroups[Group].empty())
          continue;
        RunningAlternatives += MaskGroups[Group].size();
        JumpTableValueQuery Query;
        Query.Candidate = ExactIndex;
        Query.UseAddr = IndexUseAddr;
        Query.UseSeq = IndexUseSeq;
        Query.Alternatives.reserve(RunningAlternatives);
        for (size_t PrefixGroup = 0; PrefixGroup <= Group; ++PrefixGroup)
          Query.Alternatives.insert(Query.Alternatives.end(),
                                    MaskGroups[PrefixGroup].begin(),
                                    MaskGroups[PrefixGroup].end());
        Query.AllowZeroExtension = true;
        Query.AllowSignExtension = false;
        Queries.push_back(std::move(Query));
        QueryBounds.push_back(uint32_t{1} << Group);
      }
      bool MatchComplete = false;
      const std::vector<bool> Matches = tableValuesMatchAtUses(
          Queries, &MatchComplete, nullptr, InvalidVA, nullptr,
          CandidateEvidenceBudget);
      if (!MatchComplete || Matches.size() != QueryBounds.size()) {
        Complete = false;
        return false;
      }
      uint32_t N = 0;
      for (size_t MatchIndex = 0; MatchIndex < Matches.size(); ++MatchIndex) {
        if (!consumeWork())
          return false;
        if (Matches[MatchIndex]) {
          N = QueryBounds[MatchIndex];
          break;
        }
      }
      if (N < limits::kMinJumpTableEntries)
        return false;
      if (N > limits::kMaxJumpTableEntries / 2) {
        Complete = false;
        return false;
      }
      const std::optional<bool> HasLoRun = hasRelocationPrefix(Lo, W, N);
      const std::optional<bool> HasHiRun = hasRelocationPrefix(Hi, W, N);
      if (!HasLoRun || !HasHiRun)
        return false;
      if (!*HasLoRun || !*HasHiRun)
        return false;

      // Read exactly the proven N slots from each physical run.  Extra
      // relocations adjacent to either table are foreign data, not selector
      // domain evidence.  The logical selector concatenates the lower and
      // higher runs, regardless of their physical separation.
      const size_t TargetCount = static_cast<size_t>(N) * 2;
      const size_t KnownEntryLookup =
          KnownFuncEntries ? orderedLookupWork(KnownFuncEntries->size()) : 0;
      const size_t RuntimeEntryLookup =
          orderedLookupWork(Img.RuntimeFunctionAddrs.size());
      const size_t VerifiedEntryLookup =
          orderedLookupWork(Img.VerifiedFunctionEntries.size());
      const size_t ImportStubLookup =
          orderedLookupWork(Img.ImportStubIndices.size());
      const size_t FragmentLookup =
          orderedLookupWork(Img.ExceptionMetadata.Functions.size());
      if (FragmentLookup >
          (std::numeric_limits<size_t>::max() - 3) / 2) {
        *CandidateEvidenceBudget = 0;
        Complete = false;
        return false;
      }
      const size_t FragmentWorkPerEntry = FragmentLookup * 2 + 3;
      // readVA, executable-owner/range validation, function-entry exclusion,
      // and explicit fragment ownership all traverse loader inventories.  Pay
      // their worst-case envelope before the first slot read so exhaustion
      // cannot leave a partially decoded target vector.
      if (!consumeFactorProduct({TargetCount, 16}) ||
          !consumeFactorProduct({TargetCount, Img.Segments.size(), 19}) ||
          !consumeFactorProduct({TargetCount, Img.Sections.size(), 12}) ||
          !consumeFactorProduct(
              {TargetCount, Img.ImportStubRanges.size(), 3}) ||
          !consumeFactorProduct({TargetCount, Img.Imports.size(), 2}) ||
          !consumeFactorProduct({TargetCount, Img.KnownCodeRanges.size(), 2}) ||
          !consumeFactorProduct({TargetCount, Img.Symbols.size(), 2}) ||
          !consumeFactorProduct({TargetCount, ImportStubLookup, 2}) ||
          !consumeFactorProduct({TargetCount, RuntimeEntryLookup, 2}) ||
          !consumeFactorProduct({TargetCount, VerifiedEntryLookup}) ||
          !consumeFactorProduct({TargetCount, KnownEntryLookup}) ||
          !consumeFactorProduct({TargetCount,
                                 Img.ExceptionMetadata.Functions.size(),
                                 FragmentWorkPerEntry}) ||
          !consumeProduct(TargetCount, 3) || !consumeWork(2))
        return false;

      auto readCodePtrRun = [&](va_t Base, uint32_t Count,
                                std::vector<va_t> &Out) -> bool {
        for (uint32_t I = 0; I < Count; ++I) {
          if (!consumeWork())
            return false;
          uint64_t Offset = 0;
          va_t Slot = 0;
          if (I != 0 && static_cast<uint64_t>(W) >
                            std::numeric_limits<uint64_t>::max() / I)
            return false;
          Offset = static_cast<uint64_t>(I) * W;
          if (Offset > std::numeric_limits<va_t>::max() - Base)
            return false;
          Slot = Base + Offset;
          const std::optional<bool> HasRelocation =
              relocationSlotPresent(Slot);
          if (!HasRelocation)
            return false;
          if (!*HasRelocation)
            return false;
          const uint8_t *P = Img.readVA(Slot, W);
          if (!P)
            return false;
          va_t RawTarget = 0;
          std::memcpy(&RawTarget, P,
                      W); // absolute code pointer (post-link VA)
          std::optional<va_t> Canonical =
              canonicalizeAbsoluteTableCodeTarget(Img, RawTarget);
          if (!Canonical)
            return false;
          const va_t Target = *Canonical;
          if (!isValidTarget(Img, Target, CurrentFuncEntry))
            return false;
          Out.push_back(Target);
        }
        return true;
      };
      std::vector<va_t> Union;
      Union.reserve(TargetCount);
      if (!readCodePtrRun(Lo, N, Union) || !readCodePtrRun(Hi, N, Union))
        return false;
      if (!Complete)
        return false;

      JumpTableValueOccurrence IndexOccurrence{ExactIndex, IndexUseAddr,
                                               IndexUseSeq,
                                               /*DefinedAtPoint=*/false};
      JumpTableValueOccurrence LoadOccurrence{L.Output, L.Addr, L.Seq,
                                              /*DefinedAtPoint=*/true};
      // Construct every retained vector only after its allocation, element, and
      // future cleanup work is reserved.  Result remains local until all
      // occurrence checks succeed, so no partial JumpTableInfo escapes.
      if (!consumeWork(8) || !consumeWork(3 * 2 + 2) ||
          !consumeWork(3 * 1 + 2) || !consumeWork(3 * 1 + 2) ||
          !consumeWork(3 * 1 + 2) || !consumeWork(3 * 1 + 2) ||
          !consumeWork(3 * 2 + 2))
        return false;
      JumpTableInfo Result;
      Result.CompositeShapeClaimed = true;
      JumpTableLoadRole Role;
      Role.Load = LoadOccurrence;
      Role.LoadWidth = W;
      Role.AllowedBases.reserve(2);
      Role.AllowedBases.push_back(Lo);
      Role.AllowedBases.push_back(Hi);
      Role.Indices.reserve(1);
      Role.Indices.push_back(IndexOccurrence);
      Role.AddressIndex = {IdxV, Ops[AddIdx].Addr, Ops[AddIdx].Seq,
                           /*DefinedAtPoint=*/false};
      Role.AddressScale = IndexIsPreScaled ? 1 : W;
      // scaledIndexReg deliberately records the logical selector at the input
      // of an address-width ZEXT.  The address-role proof must therefore be
      // allowed to follow that exact zero extension to the MULT/LEFT input;
      // the shared value matcher still rejects sign extension, truncation, or
      // any different reaching definition.
      Role.AllowZeroExtension =
          ExactIndex.Size != 0 && ExactIndex.Size < Img.getPointerSize();
      Role.SelectedBase = {BD.Output, BD.Addr, BD.Seq,
                           /*DefinedAtPoint=*/true};
      Role.TrueBase = Cpos;
      Role.FalseBase = Cneg;
      if (MatchedSelect) {
        Role.HasBaseSelect = true;
        Role.SelectCondition = {BD.Inputs[0], BD.Addr, BD.Seq,
                                /*DefinedAtPoint=*/false};
      } else if (MatchedBlend && PositiveBlendSide >= 0) {
        const int NegativeBlendSide = 1 - PositiveBlendSide;
        const LowOp &PositiveAnd = Ops[BlendAndIdx[PositiveBlendSide]];
        const LowOp &NegativeAnd = Ops[BlendAndIdx[NegativeBlendSide]];
        const LowOp &PositiveMask =
            Ops[BlendMaskDef[PositiveBlendSide].OpIndex];
        const LowOp &NegativeMask =
            Ops[BlendMaskDef[NegativeBlendSide].OpIndex];
        if (PositiveMask.Opcode != NdOp::INT_NEG2 ||
            PositiveMask.NumInputs < 1 ||
            NegativeMask.Opcode != NdOp::INT_NOT || NegativeMask.NumInputs < 1)
          return false;
        Role.HasBaseMaskBlend = true;
        Role.PositiveBlendArm = {PositiveAnd.Output, PositiveAnd.Addr,
                                 PositiveAnd.Seq,
                                 /*DefinedAtPoint=*/true};
        Role.NegativeBlendArm = {NegativeAnd.Output, NegativeAnd.Addr,
                                 NegativeAnd.Seq,
                                 /*DefinedAtPoint=*/true};
        Role.PositiveMask = {PositiveMask.Output, PositiveMask.Addr,
                             PositiveMask.Seq,
                             /*DefinedAtPoint=*/true};
        Role.NegativeMask = {NegativeMask.Output, NegativeMask.Addr,
                             NegativeMask.Seq,
                             /*DefinedAtPoint=*/true};
        Role.SelectCondition = {PositiveMask.Inputs[0], PositiveMask.Addr,
                                PositiveMask.Seq,
                                /*DefinedAtPoint=*/false};
        Role.PositiveBlendInputSide = static_cast<uint8_t>(PositiveBlendSide);
        Role.PositiveBaseInputSide =
            static_cast<uint8_t>(BlendArmSide[PositiveBlendSide]);
        Role.NegativeBaseInputSide =
            static_cast<uint8_t>(BlendArmSide[NegativeBlendSide]);
      } else {
        return false;
      }

      Result.setBaseAddr(Lo);
      Result.EntrySize = W;
      Result.MaxEntries = static_cast<uint32_t>(TargetCount);
      Result.PhysicalCapacity = static_cast<uint32_t>(TargetCount);
      Result.IndexDomainAuthenticated = true;
      Result.RelocAbsolute = true;
      Result.RelocBounded = true;
      Result.IsRelative = false;
      Result.IsSigned = false;
      Result.IndexReg = IdxReg;
      Result.IndexValueAtUse = ExactIndex;
      Result.IndexUseAddr = IndexUseAddr;
      Result.IndexUseSeq = IndexUseSeq;
      Result.IndexValueAlternatives.reserve(1);
      Result.IndexValueAlternatives.push_back(IndexOccurrence);
      Result.PreScaledIndex = true;
      Result.Stride = W;
      Result.TwoTableSelect = true;
      Result.TwoTableOffset = N * W; // concatenated (lo-first) coordinate
      Result.TwoTableHiPositive = (Cpos > Cneg);
      Result.ExplicitTargets = std::move(Union);
      Result.TargetLoads.reserve(1);
      Result.TargetLoads.push_back(LoadOccurrence);
      Result.LoadRoles.reserve(1);
      Result.LoadRoles.push_back(std::move(Role));
      Result.TableLoadAddr = L.Addr;
      Result.TableLoadSeq = L.Seq;
      Result.StorageRanges.reserve(2);
      Result.StorageRanges.push_back(JumpTableStorageRange{Lo, W, W, N});
      Result.StorageRanges.push_back(JumpTableStorageRange{Hi, W, W, N});
      Info = std::move(Result);
      LLVM_DEBUG(llvm::dbgs()
                 << "  two-table: exact-domain tables 0x" << llvm::utohexstr(Lo)
                 << " + 0x" << llvm::utohexstr(Hi) << " (" << (2u * N)
                 << " targets, N=" << N << ")\n");
      return true;
    }
  }
  return false;
}

//===----------------------------------------------------------------------===//
// tryConstBaseAbsoluteTable — constant-base absolute table, decoupled load
//===----------------------------------------------------------------------===//

bool CFGBuilder::tryConstBaseAbsoluteTable(
    const BinaryImage &Img, const InsnRecord &Rec, JumpTableInfo &Info,
    JumpTableExactConsumerGroup *ExactConsumerGroup, bool *ShapeClaimed,
    size_t *EvidenceBudget, bool *AnalysisComplete, bool ScanExactGroup,
    bool RequireCurrentBranchLoad, bool RequireExactGroupAnchor) const {
  if (AnalysisComplete)
    *AnalysisComplete = false;
  if (ShapeClaimed)
    *ShapeClaimed = false;
  if (ExactConsumerGroup) {
    ExactConsumerGroup->IndexOccurrences.clear();
    ExactConsumerGroup->BranchAddrs.clear();
    ExactConsumerGroup->MinimumPresentBranches = 2;
  }
  bool BudgetExhausted = false;
  auto orderedLookupWork = [](size_t Count) {
    size_t Work = 1;
    for (size_t N = Count; N > 1; N = N / 2 + N % 2)
      ++Work;
    return Work;
  };
  auto consume = [&](size_t Amount = 1) {
    if (!EvidenceBudget)
      return true;
    if (Amount > *EvidenceBudget) {
      *EvidenceBudget = 0;
      BudgetExhausted = true;
      return false;
    }
    *EvidenceBudget -= Amount;
    return true;
  };
  auto consumeProducts =
      [&](std::initializer_list<std::pair<size_t, size_t>> Products) {
        const size_t Max = std::numeric_limits<size_t>::max();
        size_t Total = 0;
        for (const auto &[Count, Cost] : Products) {
          if (Count != 0 && Cost > Max / Count)
            return consume(Max);
          const size_t Product = Count * Cost;
          if (Product > Max - Total)
            return consume(Max);
          Total += Product;
        }
        return consume(Total);
      };
  auto ensureAppendCapacity = [&](auto &Values, size_t Additional = 1) {
    const size_t Max = std::numeric_limits<size_t>::max();
    if (Additional > Max - Values.size())
      return consume(Max);
    const size_t Required = Values.size() + Additional;
    if (Required <= Values.capacity())
      return true;
    const size_t Doubled =
        Values.capacity() == 0
            ? size_t{1}
            : (Values.capacity() > Max / 2 ? Max : Values.capacity() * 2);
    const size_t NewCapacity = std::max(Required, Doubled);
    if (NewCapacity == Max) {
      consume(Max);
      return false;
    }
    if (!consumeProducts({{NewCapacity, 2}, {Values.size(), 1}}))
      return false;
    Values.reserve(NewCapacity);
    return true;
  };
  auto completed = [&](bool Result) {
    if (AnalysisComplete)
      *AnalysisComplete = true;
    return Result;
  };

  if (!CurrentImg)
    return completed(false);
  if (!consumeProducts({{Rec.Ops.size(), 2}}))
    return false;
  bool HasIndBranch = false;
  bool HasInstructionLocalPointerLoad = false;
  for (auto &Op : Rec.Ops) {
    if (Op.Opcode == NdOp::INDIR_BR && Op.NumInputs >= 1)
      HasIndBranch = true;
    if (Op.Opcode == NdOp::LOAD && (Op.Output.Size == 4 || Op.Output.Size == 8))
      HasInstructionLocalPointerLoad = true;
  }
  if (!HasIndBranch)
    return completed(false);

  // RISC targets commonly lower one memory dispatch as `LOAD; BR`, with both
  // instructions in the same basic block but in separate InsnRecords.  Trace
  // the branch input through value-preserving copies inside that block and
  // retain only the exact pointer LOAD that defines it.  This is deliberately
  // narrower than adopting an arbitrary earlier LOAD in the function.
  std::optional<va_t> BranchLocalPointerLoadAddr;
  bool HasBranchLocalFrameRelay = false;
  auto findBranchLocalPointerLoad = [&](va_t BranchAddr) {
    if (!consume(2) || !consume(orderedLookupWork(BlockStarts.size())))
      return false;
    auto Block = BlockStarts.upper_bound(BranchAddr);
    if (Block == BlockStarts.begin())
      return true;
    --Block;
    if (!consume(orderedLookupWork(Insns.size())))
      return false;
    std::vector<LowOp> BlockOps;
    for (auto It = Insns.lower_bound(*Block);
         It != Insns.end() && It->first <= BranchAddr; ++It) {
      if (!consume())
        return false;
      for (const LowOp &Op : It->second.Ops) {
        if (!ensureAppendCapacity(BlockOps) || !consume(2))
          return false;
        BlockOps.push_back(Op);
      }
    }
    if (!consume(BlockOps.size()))
      return false;
    int BranchIndex = -1;
    for (int I = static_cast<int>(BlockOps.size()) - 1; I >= 0; --I)
      if (BlockOps[I].Addr == BranchAddr &&
          BlockOps[I].Opcode == NdOp::INDIR_BR && BlockOps[I].NumInputs >= 1) {
        BranchIndex = I;
        break;
      }
    if (BranchIndex < 0)
      return true;
    NdVar Value = BlockOps[BranchIndex].Inputs[0];
    int From = BranchIndex - 1;
    for (int Depth = 0; Depth < limits::kMaxQuasiCopyDepth; ++Depth) {
      if (!consume(BlockOps.size()))
        return false;
      const int Def = reachingDefIdx(BlockOps, From, Value);
      if (Def < 0 || BlockOps[Def].Addr < *Block)
        return true;
      const LowOp &Producer = BlockOps[Def];
      if (Producer.Opcode == NdOp::LOAD &&
          Producer.Output.Size == Img.getPointerSize()) {
        const NdVar &Address = Producer.Inputs[Producer.NumInputs >= 2 ? 1 : 0];
        // A spill relay loads one pointer from a fixed frame slot.  On a
        // 32-bit guest the frame address is first widened to the common
        // internal VA width; a native-width guest uses frame+constant
        // directly.  Recognize only those exact value-preserving envelopes,
        // with at most the instruction-local COPY that materializes the frame
        // register.  A real table LOAD still carries a dynamic index and
        // cannot enter this path.
        if (Address.Size >= Img.getPointerSize()) {
          if (!consumeProducts({{BlockOps.size(), 3}}))
            return false;
          NdVar FrameAddress = Address;
          int BeforeAdd = Def - 1;
          if (Address.Size > Img.getPointerSize()) {
            const int WidenDef = reachingDefIdx(BlockOps, BeforeAdd, Address);
            if (WidenDef < 0 || BlockOps[WidenDef].Opcode != NdOp::INT_ZEXT ||
                BlockOps[WidenDef].NumInputs < 1 ||
                BlockOps[WidenDef].Inputs[0].Size != Img.getPointerSize()) {
              FrameAddress = {};
            } else {
              FrameAddress = BlockOps[WidenDef].Inputs[0];
              BeforeAdd = WidenDef - 1;
            }
          }
          uint64_t FrameRegister =
              FrameAddress.isReg() ? FrameAddress.Offset : InvalidVA;
          const int AddDef = FrameAddress.Size == Img.getPointerSize()
                                 ? reachingDefIdx(BlockOps, BeforeAdd,
                                                  FrameAddress)
                                 : -1;
          if (AddDef >= 0 && BlockOps[AddDef].Opcode == NdOp::COPY &&
              BlockOps[AddDef].NumInputs >= 1 &&
              BlockOps[AddDef].Inputs[0].isReg())
            FrameRegister = BlockOps[AddDef].Inputs[0].Offset;
          if (FrameRegister != InvalidVA &&
              getTargetRegInfo(Img.Arch).isFrameReg(FrameRegister)) {
            HasBranchLocalFrameRelay = true;
            return true;
          }
          if (AddDef >= 0 && BlockOps[AddDef].Opcode == NdOp::INT_ADD &&
              BlockOps[AddDef].NumInputs >= 2) {
            const LowOp &Add = BlockOps[AddDef];
            const int ConstantSide = Add.Inputs[1].isConst()
                                         ? 1
                                         : (Add.Inputs[0].isConst() ? 0 : -1);
            if (ConstantSide >= 0) {
              const NdVar Base = Add.Inputs[1 - ConstantSide];
              uint64_t BaseRegister = Base.isReg() ? Base.Offset : InvalidVA;
              if (Base.isTemp()) {
                const int BaseDef = reachingDefIdx(BlockOps, AddDef - 1, Base);
                if (BaseDef >= 0 && BlockOps[BaseDef].Opcode == NdOp::COPY &&
                    BlockOps[BaseDef].NumInputs >= 1 &&
                    BlockOps[BaseDef].Inputs[0].isReg())
                  BaseRegister = BlockOps[BaseDef].Inputs[0].Offset;
              }
              if (BaseRegister != InvalidVA &&
                  getTargetRegInfo(Img.Arch).isFrameReg(BaseRegister)) {
                HasBranchLocalFrameRelay = true;
                return true;
              }
            }
          }
        }
        BranchLocalPointerLoadAddr = Producer.Addr;
        return true;
      }
      if (Producer.Opcode != NdOp::COPY || Producer.NumInputs < 1)
        return true;
      Value = Producer.Inputs[0];
      From = Def - 1;
    }
    // A chain that continues beyond the bounded trace is not a completed
    // semantic rejection; preserve the branch opaquely.
    return false;
  };
  if ((ScanExactGroup || RequireCurrentBranchLoad) &&
      !findBranchLocalPointerLoad(Rec.Addr))
    return false;

  std::vector<va_t> DecoupledPredecessorLoadAddrs;
  bool HasDecoupledPredecessorGroup = false;
  bool HasSinglePredecessorRelay = false;
  if (ScanExactGroup && CurrentFuncRange &&
      CurrentFuncRange->first == CurrentFuncEntry) {
    // A shared -O0 local computed-goto dispatch reloads its target from a
    // frame slot in the branch block.  Its scaled table LOADs live in two or
    // more direct predecessors.  Inventory only pointer LOADs in those direct
    // predecessor blocks; a normal single-site spill/reload deliberately does
    // not enter the multi-consumer group path.
    if (!consume(4) || !consume(orderedLookupWork(BlockStarts.size())))
      return false;
    auto BranchBlock = BlockStarts.upper_bound(Rec.Addr);
    if (BranchBlock != BlockStarts.begin()) {
      --BranchBlock;
      std::vector<va_t> PredecessorBlocks;
      for (const auto &[Addr, Candidate] : Insns) {
        if (!consume())
          return false;
        if (!Candidate.IsBranch || Candidate.IsCall)
          continue;
        const bool DirectTarget = Candidate.BranchTarget == *BranchBlock;
        const bool FallthroughTarget =
            Candidate.IsCond && Candidate.Size <= InvalidVA - Addr &&
            Addr + Candidate.Size == *BranchBlock;
        if (!DirectTarget && !FallthroughTarget)
          continue;
        if (!consume(orderedLookupWork(BlockStarts.size())))
          return false;
        auto PredBlock = BlockStarts.upper_bound(Addr);
        if (PredBlock == BlockStarts.begin())
          continue;
        --PredBlock;
        if (!consume(PredecessorBlocks.size()))
          return false;
        if (llvm::is_contained(PredecessorBlocks, *PredBlock))
          continue;
        if (!ensureAppendCapacity(PredecessorBlocks) || !consume())
          return false;
        PredecessorBlocks.push_back(*PredBlock);
      }

      size_t LoadBearingPredecessors = 0;
      for (va_t PredStart : PredecessorBlocks) {
        if (!consume() ||
            !consume(orderedLookupWork(BlockStarts.size())) ||
            !consume(orderedLookupWork(Insns.size())))
          return false;
        const auto PredEndIt = BlockStarts.upper_bound(PredStart);
        const va_t PredEnd =
            PredEndIt == BlockStarts.end() ? InvalidVA : *PredEndIt;
        bool HasPointerLoad = false;
        for (auto It = Insns.lower_bound(PredStart);
             It != Insns.end() && It->first < PredEnd; ++It) {
          if (!consume() || !consume(It->second.Ops.size()))
            return false;
          const bool InstructionHasPointerLoad = std::any_of(
              It->second.Ops.begin(), It->second.Ops.end(),
              [](const LowOp &Op) {
                return Op.Opcode == NdOp::LOAD &&
                       (Op.Output.Size == 4 || Op.Output.Size == 8);
              });
          if (!InstructionHasPointerLoad)
            continue;
          HasPointerLoad = true;
          if (!ensureAppendCapacity(DecoupledPredecessorLoadAddrs) ||
              !consume())
            return false;
          DecoupledPredecessorLoadAddrs.push_back(It->first);
        }
        LoadBearingPredecessors += HasPointerLoad ? 1 : 0;
      }
      HasDecoupledPredecessorGroup =
          !BranchLocalPointerLoadAddr && LoadBearingPredecessors >= 2;
      HasSinglePredecessorRelay =
          (BranchLocalPointerLoadAddr || HasBranchLocalFrameRelay) &&
          LoadBearingPredecessors == 1;
      if (!HasDecoupledPredecessorGroup && !HasSinglePredecessorRelay)
        DecoupledPredecessorLoadAddrs.clear();
    }
  }

  bool WantsExactGroupInventory =
      ScanExactGroup &&
      (BranchLocalPointerLoadAddr || HasBranchLocalFrameRelay ||
       HasDecoupledPredecessorGroup) &&
      CurrentFuncRange && CurrentFuncRange->first == CurrentFuncEntry;
  if (WantsExactGroupInventory && HasBranchLocalFrameRelay &&
      ExactConsumerGroup)
    ExactConsumerGroup->MinimumPresentBranches = 1;
  if (RequireExactGroupAnchor && !WantsExactGroupInventory)
    return completed(false);
  if (WantsExactGroupInventory) {
    // Establish the current branch's exact local absolute-table model before
    // copying/scanning the rest of an attacker-sized function.  The recursive
    // local phase retains lexical definitions through Rec but considers only
    // the exact same-block LOAD feeding Rec's branch, and disables group
    // recursion; all of its work debits the same candidate account.  Once it
    // has authenticated the base/width/
    // relocation run and exact load/index occurrence, later group inventory
    // exhaustion is evidence-incomplete for a claimed table shape, never
    // ordinary callback evidence.
    JumpTableInfo LocalInfo;
    bool LocalShapeClaimed = false;
    bool LocalAnalysisComplete = false;
    const bool LocalRecovered = tryConstBaseAbsoluteTable(
        Img, Rec, LocalInfo, /*ExactConsumerGroup=*/nullptr, &LocalShapeClaimed,
        EvidenceBudget, &LocalAnalysisComplete,
        /*ScanExactGroup=*/false, /*RequireCurrentBranchLoad=*/true);
    if ((LocalRecovered || LocalShapeClaimed) && ShapeClaimed)
      *ShapeClaimed = true;
    if (!LocalAnalysisComplete)
      return false;
    // Exact-group authority is anchored in the current branch.  A generic
    // memory callback in the same function must never borrow a later sibling's
    // absolute relocation model.
    if (!LocalRecovered && !HasDecoupledPredecessorGroup) {
      if (!HasSinglePredecessorRelay)
        return completed(false);
      // A single computed-goto site may load its table target in the sole
      // predecessor, spill it, and reload it immediately before the branch.
      // Keep this as a single-consumer candidate, restricted below to pointer
      // LOADs in that exact predecessor.  An arbitrary callback LOAD therefore
      // cannot borrow a table model from an unrelated sibling block.
      WantsExactGroupInventory = false;
    }
  }

  // This occurrence-backed strategy handles both an in-instruction memory
  // jump (`jmp *tab(,idx,W)`) and a decoupled spill/reload relay.  The unified
  // load-address and load-output certificates below prevent an unrelated
  // prefix LOAD from being adopted merely because it has the same shape.

  // A decoupled relay still needs the whole lexical prefix because its table
  // loads live in predecessor goto-sites.  An in-instruction memory branch is
  // different: optimized classifiers can tail-duplicate several independently
  // bounded consumers of the same label table.  When an authoritative function
  // range exists, retain every such direct consumer so the bounds resolver can
  // prove their union as one atomic candidate group.  Loads from ordinary
  // instructions remain excluded from that group.
  const bool ScanWholeFunction =
      WantsExactGroupInventory;
  std::vector<std::pair<va_t, va_t>> InstructionLocalConsumers;
  std::vector<LowOp> Ops;
  auto appendOps = [&](const std::vector<LowOp> &Source) {
    for (const LowOp &Op : Source) {
      // Capacity growth pays buffer/move work; each retained LowOp still owns
      // one source visit plus its copy construction/future destruction.
      if (!ensureAppendCapacity(Ops) || !consume(2))
        return false;
      Ops.push_back(Op);
    }
    return true;
  };
  if (RequireCurrentBranchLoad) {
    // Retain the lexical definitions needed to fold the current dispatch's
    // table base, but do not let an earlier sibling LOAD become its model.
    // LoadScanOrder below is restricted to the exact same-block producer in
    // this mode.
    if (!consume(orderedLookupWork(Insns.size())))
      return false;
    for (auto It = Insns.lower_bound(CurrentFuncEntry);
         It != Insns.end() && It->first <= Rec.Addr; ++It) {
      if (!consume() || !appendOps(It->second.Ops))
        return false;
    }
  } else {
    if (!consume(orderedLookupWork(Insns.size())))
      return false;
    for (auto It = Insns.lower_bound(CurrentFuncEntry); It != Insns.end();
         ++It) {
      if ((ScanWholeFunction && It->first >= CurrentFuncRange->second) ||
          (!ScanWholeFunction && It->first > Rec.Addr))
        break;
      if (!consume())
        return false;
      if (!appendOps(It->second.Ops))
        return false;
    }
  }

  auto reachingDef = [&](int From, const NdVar &Value) -> std::optional<int> {
    const size_t Scan =
        From < 0 ? 0 : std::min(Ops.size(), static_cast<size_t>(From) + 1);
    if (!consume(Scan))
      return std::nullopt;
    return reachingDefIdx(Ops, From, Value);
  };
  // Discover a literal arm from its use-local expression without invoking the
  // function-prefix register folder.  This is only a candidate hint: the
  // address-role solver below must still replay the exact LOAD use across all
  // feasible paths before the occurrence can survive publication.
  std::function<std::optional<uint64_t>(NdVar, int, int)>
      foldPointLocalConstant =
          [&](NdVar Value, int From, int Depth) -> std::optional<uint64_t> {
    if (!consume() || Depth >= limits::kMaxSliceDepth)
      return std::nullopt;
    if (Value.isConst())
      return truncateToByteWidth(Value.Offset, Value.Size);
    if (!Value.isReg() && !Value.isTemp())
      return std::nullopt;
    const std::optional<int> Def = reachingDef(From, Value);
    if (!Def || *Def < 0)
      return std::nullopt;
    const LowOp &Producer = Ops[*Def];
    if ((Producer.Opcode == NdOp::COPY || Producer.Opcode == NdOp::INT_ZEXT) &&
        Producer.NumInputs >= 1) {
      const std::optional<uint64_t> Input =
          foldPointLocalConstant(Producer.Inputs[0], *Def - 1, Depth + 1);
      if (!Input)
        return std::nullopt;
      return truncateToByteWidth(*Input, Producer.Output.Size);
    }
    if ((Producer.Opcode == NdOp::INT_ADD ||
         Producer.Opcode == NdOp::INT_SUB) &&
        Producer.NumInputs >= 2) {
      const std::optional<uint64_t> Left =
          foldPointLocalConstant(Producer.Inputs[0], *Def - 1, Depth + 1);
      const std::optional<uint64_t> Right =
          foldPointLocalConstant(Producer.Inputs[1], *Def - 1, Depth + 1);
      if (!Left || !Right)
        return std::nullopt;
      const uint64_t Result =
          Producer.Opcode == NdOp::INT_ADD ? *Left + *Right : *Left - *Right;
      return truncateToByteWidth(Result, Producer.Output.Size);
    }
    return std::nullopt;
  };
  if (ScanWholeFunction) {
    if (!consume(2))
      return false;
    for (int I = 0; I < static_cast<int>(Ops.size()); ++I) {
      const LowOp &Branch = Ops[I];
      if (!consume())
        return false;
      if (Branch.Opcode != NdOp::INDIR_BR || Branch.NumInputs < 1)
        continue;
      if (!consume(orderedLookupWork(BlockStarts.size())))
        return false;
      auto Block = BlockStarts.upper_bound(Branch.Addr);
      if (Block == BlockStarts.begin())
        continue;
      --Block;
      NdVar Value = Branch.Inputs[0];
      int From = I - 1;
      bool TraceCompleted = false;
      for (int Depth = 0; Depth < limits::kMaxQuasiCopyDepth; ++Depth) {
        const std::optional<int> Def = reachingDef(From, Value);
        if (!Def)
          return false;
        if (*Def < 0 || Ops[*Def].Addr < *Block) {
          TraceCompleted = true;
          break;
        }
        const LowOp &Producer = Ops[*Def];
        if (Producer.Opcode == NdOp::LOAD &&
            Producer.Output.Size == Img.getPointerSize()) {
          if (!ensureAppendCapacity(InstructionLocalConsumers) || !consume(2))
            return false;
          InstructionLocalConsumers.emplace_back(Producer.Addr, Branch.Addr);
          TraceCompleted = true;
          break;
        }
        if (Producer.Opcode != NdOp::COPY || Producer.NumInputs < 1) {
          TraceCompleted = true;
          break;
        }
        Value = Producer.Inputs[0];
        From = *Def - 1;
      }
      if (!TraceCompleted)
        return false;
    }
  }
  auto chargeScaledIndexTrace = [&]() {
    // scaledIndexReg can walk one quasi-copy chain, inspect one widening
    // definition, then walk a second chain to the source register.
    return consumeProducts(
        {{Ops.size(), size_t{2} * limits::kMaxQuasiCopyDepth + 1}});
  };
  auto chargeRegisterTrace = [&]() {
    return consumeProducts({{Ops.size(), limits::kMaxQuasiCopyDepth}});
  };
  auto chargeExactI386GOTOFFInput = [&]() {
    const size_t Max = std::numeric_limits<size_t>::max();
    const size_t Occurrences = RelocatedInstructionAddressOccurrences.size();
    size_t OwnerQueryWork = 1;
    auto addProduct = [&](size_t Count, size_t Cost) {
      if (Count != 0 && Cost > Max / Count)
        return false;
      const size_t Product = Count * Cost;
      if (Product > Max - OwnerQueryWork)
        return false;
      OwnerQueryWork += Product;
      return true;
    };
    // isExactI386GOTOFFInput authenticates the relocation field, its decoded
    // LowIR input occurrence, and the field's data-object owner.  Pay the
    // complete positive-path inventory before entering that helper; a numeric
    // displacement without this exact provenance is never a table base.
    if (!addProduct(7, Img.Segments.size()) ||
        !addProduct(6, Img.Sections.size()) ||
        !addProduct(2, orderedLookupWork(Img.ImportStubIndices.size())) ||
        !addProduct(2, Img.ImportStubRanges.size()) ||
        !addProduct(2, Img.Imports.size()) ||
        !addProduct(3, orderedLookupWork(Img.RuntimeFunctionAddrs.size())) ||
        !addProduct(1,
                    orderedLookupWork(Img.VerifiedFunctionEntries.size())) ||
        !addProduct(1, Img.KnownCodeRanges.size()) ||
        !addProduct(1, Img.Symbols.size()) ||
        !addProduct(1, orderedLookupWork(Img.RodataAnchorSeg.size())))
      return consume(Max);
    return consumeProducts(
        {{size_t{1}, orderedLookupWork(Insns.size())},
         {size_t{1}, orderedLookupWork(Img.DataAddressRelocOperands.size())},
         {Img.DataAddressRelocOperands.size(), 1},
         {Occurrences, 1},
         {Occurrences,
          orderedLookupWork(Img.DataAddressRelocOperands.size())},
         {Occurrences, OwnerQueryWork}});
  };

  // Recover the concrete scale of a scaled-index operand (INT_MULT const /
  // INT_LEFT shift), traced through value-preserving reshapes.  A null result
  // is evidence exhaustion; zero is a completed non-scaled result.
  auto scaleOf = [&](NdVar V, int From) -> std::optional<uint32_t> {
    for (int G = 0; G < limits::kMaxQuasiCopyDepth; ++G) {
      if (!V.isReg() && !V.isTemp())
        return uint32_t{0};
      const std::optional<int> Def = reachingDef(From, V);
      if (!Def)
        return std::nullopt;
      const int D = *Def;
      if (D < 0)
        return uint32_t{0};
      const LowOp &O = Ops[D];
      if (O.Opcode == NdOp::INT_MULT && O.NumInputs >= 2 &&
          O.Inputs[1].isConst())
        return static_cast<uint32_t>(O.Inputs[1].Offset);
      if (O.Opcode == NdOp::INT_LEFT && O.NumInputs >= 2 &&
          O.Inputs[1].isConst() && O.Inputs[1].Offset < 6)
        return 1u << O.Inputs[1].Offset;
      if ((O.Opcode == NdOp::COPY || O.Opcode == NdOp::INT_ZEXT ||
           O.Opcode == NdOp::INT_SEXT) &&
          O.NumInputs >= 1) {
        V = O.Inputs[0];
        From = D - 1;
        continue;
      }
      return uint32_t{0};
    }
    return uint32_t{0};
  };

  // Scan backward for the table load: a LOAD of pointer width whose address is
  // `const_base + idx*W` (W == the load width) and whose base carries a run of
  // absolute code-pointer relocations (the verifiable label-table signature).
  // The nearest such load to the dispatch wins; a shared multi-site dispatch
  // reads one common base, so any site's load recovers the same table.
  bool FoundModel = false;
  va_t ModelBase = 0;
  va_t ModelOwnerVA = InvalidVA;
  uint16_t ModelWidth = 0;
  uint32_t ModelRun = 0;
  std::optional<JumpTableValueOccurrence> ModelZeroOccurrence;
  bool AllowI386GOTOFFRelay = false;
  if (!HasInstructionLocalPointerLoad && Img.Arch == Arch::X86 &&
      Img.isELF() && Img.getPointerSize() == 4 &&
      !Img.I386GOTPCFields.empty() &&
      !RelocatedInstructionScalarModelOccurrences.empty()) {
    if (!consume(Img.DataAddressRelocOperands.size()))
      return false;
    size_t PositiveGOTOFFFields = 0;
    for (const auto &[FieldVA, Field] : Img.DataAddressRelocOperands) {
      if (FieldVA < CurrentFuncEntry || FieldVA > Rec.Addr ||
          Field.Kind != RelocatedAddressFieldKind::I386ELFGOTOFF ||
          Field.TargetOwnerVA == InvalidVA)
        continue;
      if (++PositiveGOTOFFFields >= 2)
        break;
    }
    if (PositiveGOTOFFFields >= 2) {
      // This producer-first pass is specific to an -O0 relay: the indirect
      // branch must reload its target through a value-preserving chain from a
      // prior pointer-sized LOAD.  A direct callback later in the same
      // function must not inherit an earlier table merely because several
      // GOTOFF fields precede it lexically.
      if (!consume(Ops.size()))
        return false;
      int BranchIndex = -1;
      for (int I = static_cast<int>(Ops.size()) - 1; I >= 0; --I)
        if (Ops[I].Addr == Rec.Addr && Ops[I].Opcode == NdOp::INDIR_BR &&
            Ops[I].NumInputs >= 1) {
          BranchIndex = I;
          break;
        }
      if (BranchIndex >= 0) {
        NdVar Value = Ops[BranchIndex].Inputs[0];
        int From = BranchIndex - 1;
        for (int Depth = 0; Depth < limits::kMaxQuasiCopyDepth; ++Depth) {
          const std::optional<int> Def = reachingDef(From, Value);
          if (!Def)
            return false;
          if (*Def < 0)
            break;
          const LowOp &Producer = Ops[*Def];
          if (Producer.Opcode == NdOp::LOAD) {
            AllowI386GOTOFFRelay =
                Producer.Output.Size == Img.getPointerSize();
            break;
          }
          if ((Producer.Opcode != NdOp::COPY &&
               Producer.Opcode != NdOp::INT_ZEXT &&
               Producer.Opcode != NdOp::INT_SEXT) ||
              Producer.NumInputs < 1)
            break;
          Value = Producer.Inputs[0];
          From = *Def - 1;
        }
      }
    }
  }
  std::vector<int> LoadScanOrder;
  if (!consumeProducts(
          {{Ops.size(), ScanWholeFunction ? (AllowI386GOTOFFRelay ? 4 : 2)
                                          : (AllowI386GOTOFFRelay ? 2 : 1)},
           {Ops.size(),
            ScanWholeFunction ? InstructionLocalConsumers.size() : 0},
           {Ops.size(),
            ScanWholeFunction ? InstructionLocalConsumers.size() : 0},
           {Ops.size(), DecoupledPredecessorLoadAddrs.empty() ? 0 : 1},
           {Ops.size(), DecoupledPredecessorLoadAddrs.size()}}))
    return false;
  if (ScanWholeFunction) {
    // A grouped i386 relay still needs its earliest call/POP-backed GOTOFF
    // occurrence to establish the model before later spill/reload arms can be
    // authenticated.  This first pass is positive-only; once it finds the
    // model, the loop below skips directly to the exact predecessor inventory
    // instead of granting unrelated lexical LOADs group membership.
    if (AllowI386GOTOFFRelay)
      for (int I = 0; I < static_cast<int>(Ops.size()); ++I) {
        if (!ensureAppendCapacity(LoadScanOrder) || !consume())
          return false;
        LoadScanOrder.push_back(I);
      }
    // Anchor the model in the current branch before considering siblings.  A
    // later, unrelated absolute table in the same function must not become the
    // current branch's model merely because its address sorts last.
    for (int I = static_cast<int>(Ops.size()) - 1; I >= 0; --I)
      if (BranchLocalPointerLoadAddr &&
          Ops[I].Addr == *BranchLocalPointerLoadAddr) {
        if (!ensureAppendCapacity(LoadScanOrder) || !consume())
          return false;
        LoadScanOrder.push_back(I);
      }
    for (int I = static_cast<int>(Ops.size()) - 1; I >= 0; --I)
      if ((!BranchLocalPointerLoadAddr ||
           Ops[I].Addr != *BranchLocalPointerLoadAddr) &&
          std::any_of(InstructionLocalConsumers.begin(),
                      InstructionLocalConsumers.end(), [&](const auto &Entry) {
                        return Entry.first == Ops[I].Addr &&
                               Entry.second != Rec.Addr;
                      })) {
        if (!ensureAppendCapacity(LoadScanOrder) || !consume())
          return false;
        LoadScanOrder.push_back(I);
      }
    for (int I = static_cast<int>(Ops.size()) - 1; I >= 0; --I)
      if ((!BranchLocalPointerLoadAddr ||
           Ops[I].Addr != *BranchLocalPointerLoadAddr) &&
          std::none_of(
              InstructionLocalConsumers.begin(),
              InstructionLocalConsumers.end(),
              [&](const auto &Entry) { return Entry.first == Ops[I].Addr; }) &&
          llvm::is_contained(DecoupledPredecessorLoadAddrs, Ops[I].Addr)) {
        if (!ensureAppendCapacity(LoadScanOrder) || !consume())
          return false;
        LoadScanOrder.push_back(I);
      }
  } else if (RequireCurrentBranchLoad) {
    for (int I = static_cast<int>(Ops.size()) - 1; I >= 0; --I)
      if (BranchLocalPointerLoadAddr &&
          Ops[I].Addr == *BranchLocalPointerLoadAddr) {
        if (!ensureAppendCapacity(LoadScanOrder) || !consume())
          return false;
        LoadScanOrder.push_back(I);
      }
  } else if (HasSinglePredecessorRelay) {
    for (int I = static_cast<int>(Ops.size()) - 1; I >= 0; --I)
      if (llvm::is_contained(DecoupledPredecessorLoadAddrs, Ops[I].Addr)) {
        if (!ensureAppendCapacity(LoadScanOrder) || !consume())
          return false;
        LoadScanOrder.push_back(I);
      }
  } else if (AllowI386GOTOFFRelay) {
    // An i386 call/POP model is produced before any frame spill/reload of the
    // GOT base.  Visit the lexical producer first so its exact candidate-local
    // graph proof can authenticate the table once; later loads of that same
    // exact GOTOFF table are still required to pass the complete address-role
    // proof below.  The ordinary reverse pass remains available when this
    // positive-only pass finds no model.
    for (int I = 0; I < static_cast<int>(Ops.size()); ++I) {
      if (!ensureAppendCapacity(LoadScanOrder) || !consume())
        return false;
      LoadScanOrder.push_back(I);
    }
    for (int I = static_cast<int>(Ops.size()) - 1; I >= 0; --I) {
      if (!ensureAppendCapacity(LoadScanOrder) || !consume())
        return false;
      LoadScanOrder.push_back(I);
    }
  } else {
    for (int I = static_cast<int>(Ops.size()) - 1; I >= 0; --I) {
      if (!ensureAppendCapacity(LoadScanOrder) || !consume())
        return false;
      LoadScanOrder.push_back(I);
    }
  }
  std::optional<std::set<va_t>> DecodedAnchors;
  auto ensureDecodedAnchors = [&]() {
    if (DecodedAnchors)
      return true;
    const size_t AnchorOccurrenceCount =
        RelocatedInstructionAddressOccurrences.size();
    if (!consumeProducts({{AnchorOccurrenceCount, 4},
                          {AnchorOccurrenceCount,
                           orderedLookupWork(PublishedReachableInsns.size())},
                          {AnchorOccurrenceCount,
                           orderedLookupWork(Img.RelCodeRelocSlots.size())},
                          {AnchorOccurrenceCount,
                           orderedLookupWork(Img.CodePtrRelocSlots.size())},
                          {AnchorOccurrenceCount,
                           orderedLookupWork(AnchorOccurrenceCount)}}) ||
        !consume(2))
      return false;
    DecodedAnchors.emplace(currentRelocatedInstructionTableAnchors(Img));
    return true;
  };
  auto chargeRelocationRun = [&]() {
    return consumeProducts({{size_t{limits::kMaxJumpTableEntries} + 1,
                             orderedLookupWork(Img.CodePtrRelocSlots.size())}});
  };
  auto chargeNextAnchorBound = [&]() {
    const size_t Max = std::numeric_limits<size_t>::max();
    const size_t RelAnchorCount = Img.RelCodeTableAnchors.size();
    const size_t DecodedAnchorCount = DecodedAnchors->size();
    const size_t DataFieldCount = Img.DataAddressRelocOperands.size();
    if (RelAnchorCount > Max - DecodedAnchorCount ||
        RelAnchorCount + DecodedAnchorCount > Max - DataFieldCount)
      return consume(Max);
    const size_t AnchorUpper =
        RelAnchorCount + DecodedAnchorCount + DataFieldCount;

    auto accumulateProduct = [&](size_t &Total, size_t Count, size_t Cost) {
      if (Count != 0 && Cost > Max / Count)
        return false;
      const size_t Product = Count * Cost;
      if (Product > Max - Total)
        return false;
      Total += Product;
      return true;
    };

    // hasExecutableCodeOwnerAt first resolves the segment/section owner, then
    // checks both import spellings and every function-identity inventory.  On
    // Mach-O, each failed import lookup may also scan Imports and repeat the
    // code-address query.  Prepay that platform-wide worst case for every data
    // relocation field before entering the helper.
    size_t OwnerQueryWork = 1;
    if (!accumulateProduct(OwnerQueryWork, 7, Img.Segments.size()) ||
        !accumulateProduct(OwnerQueryWork, 6, Img.Sections.size()) ||
        !accumulateProduct(OwnerQueryWork, 2,
                           orderedLookupWork(Img.ImportStubIndices.size())) ||
        !accumulateProduct(OwnerQueryWork, 2, Img.ImportStubRanges.size()) ||
        !accumulateProduct(OwnerQueryWork, 2, Img.Imports.size()) ||
        !accumulateProduct(
            OwnerQueryWork, 3,
            orderedLookupWork(Img.RuntimeFunctionAddrs.size())) ||
        !accumulateProduct(
            OwnerQueryWork, 1,
            orderedLookupWork(Img.VerifiedFunctionEntries.size())) ||
        !accumulateProduct(OwnerQueryWork, 1, Img.KnownCodeRanges.size()) ||
        !accumulateProduct(OwnerQueryWork, 1, Img.Symbols.size()))
      return consume(Max);

    const size_t AnchorLookup = orderedLookupWork(AnchorUpper);
    const size_t CodePtrLookup =
        orderedLookupWork(Img.CodePtrRelocSlots.size());
    // The helper copies both source sets into a new ordered set, probes the
    // authenticated-source map and executable owner for every loader field,
    // may retain each resulting anchor node, then performs upper_bound and a
    // complete suffix walk with one code-pointer lookup per anchor.
    return consumeProducts({{RelAnchorCount, AnchorLookup + 3},
                            {DecodedAnchorCount, AnchorLookup + 3},
                            {DataFieldCount, OwnerQueryWork},
                            {DataFieldCount, AnchorLookup},
                            {DataFieldCount, 4},
                            {size_t{1}, AnchorLookup},
                            {AnchorUpper, CodePtrLookup + 1}});
  };
  const size_t I386GOTOFFPriorityCount =
      AllowI386GOTOFFRelay ? Ops.size() : 0;
  for (size_t OrderIndex = 0; OrderIndex < LoadScanOrder.size(); ++OrderIndex) {
    if (ScanWholeFunction && OrderIndex < I386GOTOFFPriorityCount &&
        FoundModel) {
      OrderIndex = I386GOTOFFPriorityCount - 1;
      continue;
    }
    if (OrderIndex == I386GOTOFFPriorityCount && FoundModel &&
        !ScanWholeFunction)
      break;
    const bool ExactI386GOTOFFOnly =
        OrderIndex < I386GOTOFFPriorityCount;
    const int I = LoadScanOrder[OrderIndex];
    if (!consume())
      return false;
    const LowOp &L = Ops[I];
    if (L.Opcode != NdOp::LOAD || L.NumInputs < 1)
      continue;
    uint16_t W = L.Output.Size;
    if (W != 4 && W != 8)
      continue;
    auto appendOccurrence = [&](va_t ResolvedBase, const NdVar &ExactIndex,
                                va_t IndexUseAddr, int IndexUseSeq,
                                va_t RoleBase, const NdVar &RoleIndex,
                                va_t RoleIndexUseAddr, int RoleIndexUseSeq,
                                uint64_t RoleAddressScale,
                                std::optional<uint32_t> LiteralCoordinate,
                                JumpTableFrameStorageRole FrameStorage = {}) {
      // A frame relay has several predecessor table loads but one exact
      // branch consumer.  Keep every selector occurrence in the atomic group
      // while projecting presence onto that authenticated branch; the direct
      // multi-consumer path below retains its two-distinct-branch threshold.
      va_t ConsumerBranchAddr =
          HasBranchLocalFrameRelay ? Rec.Addr : L.Addr;
      if (ScanWholeFunction && ExactConsumerGroup && !LiteralCoordinate &&
          !AllowI386GOTOFFRelay) {
        if (!consume(InstructionLocalConsumers.size()))
          return false;
        for (const auto &[LoadAddr, BranchAddr] : InstructionLocalConsumers)
          if (LoadAddr == L.Addr) {
            ConsumerBranchAddr = BranchAddr;
            break;
          }
      }
      JumpTableValueOccurrence IndexOccurrence{ExactIndex, IndexUseAddr,
                                               IndexUseSeq,
                                               /*DefinedAtPoint=*/false};
      JumpTableValueOccurrence RoleIndexOccurrence{
          RoleIndex, RoleIndexUseAddr, RoleIndexUseSeq,
          /*DefinedAtPoint=*/false};
      JumpTableValueOccurrence LoadOccurrence{L.Output, L.Addr, L.Seq,
                                              /*DefinedAtPoint=*/true};
      if (ShapeClaimed)
        *ShapeClaimed = true;
      if ((!LiteralCoordinate &&
           !ensureAppendCapacity(Info.IndexValueAlternatives)) ||
          !ensureAppendCapacity(Info.TargetLoads) ||
          !ensureAppendCapacity(Info.LoadRoles) ||
          (ScanWholeFunction && ExactConsumerGroup && !LiteralCoordinate &&
           !AllowI386GOTOFFRelay &&
           (!ensureAppendCapacity(ExactConsumerGroup->IndexOccurrences) ||
            !ensureAppendCapacity(ExactConsumerGroup->BranchAddrs))) ||
          !consume(8))
        return false;
      if (!LiteralCoordinate)
        Info.IndexValueAlternatives.push_back(IndexOccurrence);
      Info.TargetLoads.push_back(LoadOccurrence);
      JumpTableLoadRole Role;
      Role.Load = LoadOccurrence;
      Role.LoadWidth = W;
      Role.FrameStorage = std::move(FrameStorage);
      if (!ensureAppendCapacity(Role.AllowedBases) ||
          !ensureAppendCapacity(Role.Indices) || !consume(4))
        return false;
      Role.AllowedBases.push_back(RoleBase);
      Role.Indices.push_back(RoleIndexOccurrence);
      Role.AddressScale = RoleAddressScale;
      Role.IsLiteralCoordinate = LiteralCoordinate.has_value();
      Role.LiteralCoordinate = LiteralCoordinate.value_or(0);
      Info.LoadRoles.push_back(std::move(Role));
      if (ScanWholeFunction && ExactConsumerGroup && !LiteralCoordinate &&
          !AllowI386GOTOFFRelay) {
        ExactConsumerGroup->IndexOccurrences.push_back(IndexOccurrence);
        ExactConsumerGroup->BranchAddrs.push_back(ConsumerBranchAddr);
      }
      return true;
    };
    const NdVar &AddrV = (L.NumInputs >= 2) ? L.Inputs[1] : L.Inputs[0];
    if (!AddrV.isReg() && !AddrV.isTemp())
      continue;
    if (FoundModel) {
      const std::optional<uint64_t> LiteralAddress =
          foldPointLocalConstant(AddrV, I - 1, 0);
      const uint64_t ModelSpan = uint64_t(ModelRun) * ModelWidth;
      if (LiteralAddress && W == ModelWidth && *LiteralAddress >= ModelBase &&
          *LiteralAddress - ModelBase < ModelSpan &&
          (*LiteralAddress - ModelBase) % ModelWidth == 0) {
        const uint64_t ByteOffset = *LiteralAddress - ModelBase;
        const uint64_t Slot = ByteOffset / ModelWidth;
        if (!appendOccurrence(ModelBase, NdVar::scalar(Slot, W), InvalidVA, -1,
                              ModelBase, NdVar::scalar(ByteOffset, AddrV.Size),
                              InvalidVA, -1, 1, static_cast<uint32_t>(Slot),
                              {}))
          return false;
        continue;
      }
    }
    const std::optional<int> InitialAdd = reachingDef(I - 1, AddrV);
    if (!InitialAdd)
      return false;
    int AddIdx = *InitialAdd;
    for (int G = 0;
         AddIdx >= 0 && Ops[AddIdx].NumInputs >= 1 &&
         (Ops[AddIdx].Opcode == NdOp::COPY ||
          (Ops[AddIdx].Opcode == NdOp::INT_ZEXT &&
           Ops[AddIdx].Output.Size == sizeof(va_t) &&
           Ops[AddIdx].Inputs[0].Size == Img.getPointerSize())) &&
         (Ops[AddIdx].Inputs[0].isReg() || Ops[AddIdx].Inputs[0].isTemp()) &&
         G < limits::kMaxQuasiCopyDepth;
         ++G) {
      const std::optional<int> Next =
          reachingDef(AddIdx - 1, Ops[AddIdx].Inputs[0]);
      if (!Next)
        return false;
      AddIdx = *Next;
    }
    if (AddIdx < 0 || Ops[AddIdx].Opcode != NdOp::INT_ADD ||
        Ops[AddIdx].NumInputs < 2)
      continue;

    // i386 PIC folds an exact R_386_GOTOFF displacement into a second ADD:
    //   address = (authenticated_GOT_zero + index * W) + GOTOFF(table)
    // Peel only that exact relocation occurrence.  The inner base still has to
    // reach the matching call/POP + GOTPC model below, so neither a numeric
    // constant nor an unrelated absolute relocation can acquire this shape.
    int AddressAddIdx = AddIdx;
    std::optional<va_t> ExactI386GOTOFFBase;
    std::optional<va_t> ExactI386GOTOFFOwner;
    bool ExactI386GOTOFFHasScaledInner = false;
    for (int ConstantSide = 0;
         AllowI386GOTOFFRelay && ConstantSide < 2; ++ConstantSide) {
      const NdVar &Constant = Ops[AddIdx].Inputs[ConstantSide];
      if (!Constant.isConst() || Constant.Size != 4 ||
          Constant.Provenance != ConstantAddressProvenance::DataAddress ||
          Constant.AddressOwnerVA == InvalidVA)
        continue;
      if (!chargeExactI386GOTOFFInput())
        return false;
      if (!isExactI386GOTOFFInput(Ops[AddIdx], ConstantSide))
        continue;
      ExactI386GOTOFFBase = static_cast<va_t>(Constant.Offset);
      ExactI386GOTOFFOwner = Constant.AddressOwnerVA;
      const std::optional<int> Inner =
          reachingDef(AddIdx - 1, Ops[AddIdx].Inputs[1 - ConstantSide]);
      if (!Inner)
        return false;
      if (*Inner >= 0 && Ops[*Inner].Opcode == NdOp::INT_ADD &&
          Ops[*Inner].NumInputs >= 2) {
        AddressAddIdx = *Inner;
        ExactI386GOTOFFHasScaledInner = true;
      }
      break;
    }
    if (ExactI386GOTOFFOnly && !ExactI386GOTOFFBase)
      continue;

    // A literal computed-goto arm such as `goto *table[2]` has no scaled
    // index in LowIR: clang emits `GOT_zero + GOTOFF(table + 2*W)`.  Once a
    // variable arm has authenticated the physical table, retain this exact
    // relocation occurrence as the corresponding scalar index.  The field
    // must remain inside the same complete, sized relocation object.  The
    // ordinary address-role proof below still has to authenticate this LOAD's
    // runtime address, so a numeric displacement or another object cannot
    // become a table arm.
    if (ExactI386GOTOFFBase && !ExactI386GOTOFFHasScaledInner && FoundModel &&
        ModelZeroOccurrence) {
      const va_t SlotAddress = *ExactI386GOTOFFBase;
      const uint64_t Span = uint64_t(ModelRun) * ModelWidth;
      if (W == ModelWidth && SlotAddress >= ModelBase &&
          ExactI386GOTOFFOwner &&
          *ExactI386GOTOFFOwner == ModelOwnerVA &&
          SlotAddress - ModelBase < Span &&
          (SlotAddress - ModelBase) % ModelWidth == 0) {
        const uint64_t Slot = (SlotAddress - ModelBase) / ModelWidth;
        if (!appendOccurrence(
                ModelBase, NdVar::scalar(Slot, W), InvalidVA, -1, SlotAddress,
                ModelZeroOccurrence->Value, ModelZeroOccurrence->Addr,
                ModelZeroOccurrence->Seq, 1,
                static_cast<uint32_t>(Slot), {}))
          return false;
      }
      continue;
    }

    // A shared absolute-table dispatch may also contain compile-time arms
    // such as `goto *table[2]`.  Those LOADs have no scaled selector, but they
    // are still necessary target producers once the corresponding label block
    // becomes reachable.  Retain one only after an earlier dynamic occurrence
    // has authenticated this exact table and the complete LOAD address folds
    // to one aligned slot inside that same relocation run.  The address-role
    // proof later revalidates the static slot at the exact LOAD use; this does
    // not add the literal to the dynamic selector group or grant authority to
    // the rest of the physical table.
    bool HasScaledAddressInput = false;
    if (!ExactI386GOTOFFBase && FoundModel) {
      for (int Side = 0; Side < 2; ++Side) {
        if (!chargeScaledIndexTrace())
          return false;
        if (scaledIndexReg(Ops, AddressAddIdx - 1,
                           Ops[AddressAddIdx].Inputs[Side]) != InvalidVA) {
          HasScaledAddressInput = true;
          break;
        }
      }
    }
    if (!ExactI386GOTOFFBase && FoundModel && !HasScaledAddressInput) {
      std::optional<va_t> LiteralAddress;
      auto foldRegisterAt = [&](uint64_t Register,
                                va_t At) -> std::optional<va_t> {
        if (!consumeProducts(
                {{Img.Segments.size(), 4}, {Img.Sections.size(), 2}}))
          return std::nullopt;
        return foldRegConstant(Img, Rec, Register, At, [&](size_t Amount) {
          return consume(Amount);
        });
      };

      if (Ops[AddressAddIdx].Output.isReg())
        LiteralAddress =
            foldRegisterAt(Ops[AddressAddIdx].Output.Offset, L.Addr);

      if (!LiteralAddress && Ops[AddressAddIdx].Opcode == NdOp::INT_ADD &&
          Ops[AddressAddIdx].NumInputs >= 2) {
        if (!consume(2))
          return false;
        for (int ConstantSide = 0; ConstantSide < 2 && !LiteralAddress;
             ++ConstantSide) {
          const NdVar &Delta = Ops[AddressAddIdx].Inputs[ConstantSide];
          if (!Delta.isConst() ||
              (Delta.Provenance != ConstantAddressProvenance::Unknown &&
               Delta.Provenance != ConstantAddressProvenance::Scalar))
            continue;
          if (!chargeRegisterTrace())
            return false;
          const uint64_t BaseRegister = traceToRegister(
              Ops, AddressAddIdx - 1,
              Ops[AddressAddIdx].Inputs[1 - ConstantSide]);
          if (BaseRegister == InvalidVA)
            continue;
          const std::optional<va_t> FoldedBase =
              foldRegisterAt(BaseRegister, Ops[AddressAddIdx].Addr);
          if (!FoldedBase || *FoldedBase > InvalidVA - Delta.Offset)
            continue;
          LiteralAddress = *FoldedBase + Delta.Offset;
        }
      }
      if (BudgetExhausted)
        return false;

      const uint64_t ModelSpan = uint64_t(ModelRun) * ModelWidth;
      if (LiteralAddress && *LiteralAddress >= ModelBase &&
          *LiteralAddress - ModelBase < ModelSpan &&
          (*LiteralAddress - ModelBase) % ModelWidth == 0) {
        const uint64_t ByteOffset = *LiteralAddress - ModelBase;
        const uint64_t Slot = ByteOffset / ModelWidth;
        if (!appendOccurrence(
                ModelBase, NdVar::scalar(Slot, W), InvalidVA, -1, ModelBase,
                NdVar::scalar(ByteOffset, AddrV.Size), InvalidVA, -1, 1,
                static_cast<uint32_t>(Slot), {}))
          return false;
        continue;
      }
    }

    for (int Side = 0; Side < 2; ++Side) {
      if (!consume() || !chargeScaledIndexTrace())
        return false;
      NdVar ExactIndex;
      va_t IndexUseAddr = InvalidVA;
      int IndexUseSeq = -1;
      uint64_t Idx =
          scaledIndexReg(Ops, AddressAddIdx - 1,
                         Ops[AddressAddIdx].Inputs[Side], &ExactIndex,
                         &IndexUseAddr, &IndexUseSeq);
      if (Idx == InvalidVA)
        continue;
      // AArch64 permits XZR as the register-offset operand of a memory access.
      // The lifter retains the architectural register identity in this address
      // expression, but the selector-domain proof must use its exact numeric
      // meaning.  Canonicalize only this occurrence-local table index; ordinary
      // registers still require their complete reaching definition.
      if (Img.Arch == Arch::AArch64 && ExactIndex.isReg() &&
          ExactIndex.Offset == a64reg::XZR)
        ExactIndex = NdVar::scalar(0, ExactIndex.Size);
      const std::optional<uint32_t> Scale =
          scaleOf(Ops[AddressAddIdx].Inputs[Side], AddressAddIdx - 1);
      if (!Scale)
        return false;
      if (*Scale != W)
        continue; // the scale must be the entry width for an absolute table

      // The other operand is the table base: a constant data VA, either a
      // literal (`disp(,idx,W)`) or a register folded to one (`lea tab,%rN`).
      const NdVar &BaseV = Ops[AddressAddIdx].Inputs[1 - Side];
      std::optional<va_t> Base;
      JumpTableFrameStorageRole FrameStorage;
      std::vector<JumpTableValueOccurrence> StackStorageConsumers;
      bool StackTableMutated = false;
      if (ExactI386GOTOFFBase) {
        // The first occurrence establishes the candidate-local GOT-zero
        // model.  A later occurrence may reuse only that same exact relocation
        // target; tableLoadAddressesMatchRole subsequently proves every
        // retained runtime address, so a frame reload or unrelated base cannot
        // borrow the model merely because the displacement bytes match.
        if (FoundModel &&
            (*ExactI386GOTOFFBase != ModelBase ||
             !ExactI386GOTOFFOwner ||
             *ExactI386GOTOFFOwner != ModelOwnerVA))
          continue;
        const bool ReusesAuthenticatedTableBase = FoundModel;
        const bool ModelZero =
            ReusesAuthenticatedTableBase ||
            exactI386ModelZeroReaches(Ops[AddressAddIdx], 1 - Side,
                                      *ExactI386GOTOFFBase);
        if (!ModelZero)
          continue;
        Base = *ExactI386GOTOFFBase;
      } else if (BaseV.isConst())
        Base = static_cast<va_t>(BaseV.Offset);
      else if (BaseV.isReg() || BaseV.isTemp()) {
        if (!chargeRegisterTrace())
          return false;
        uint64_t BReg = traceToRegister(Ops, AddressAddIdx - 1, BaseV);
        if (BReg != InvalidVA) {
          if (!consumeProducts(
                  {{Img.Segments.size(), 4}, {Img.Sections.size(), 2}}))
            return false;
          auto F = foldRegConstant(Img, Rec, BReg,
                                   Ops[AddressAddIdx].Addr,
                                   [&](size_t Amount) {
                                     return consume(Amount);
                                   });
          if (BudgetExhausted)
            return false;
          if (F && Img.getSegmentFor(*F))
            Base = static_cast<va_t>(*F);
          if (!Base && !DecoupledPredecessorLoadAddrs.empty()) {
            if (!consume(DecoupledPredecessorLoadAddrs.size()))
              return false;
            if (!llvm::is_contained(DecoupledPredecessorLoadAddrs, L.Addr))
              continue;
            std::vector<JumpTableFrameInitializerChunk> Initializers;
            const va_t StackSource = resolveStackMaterializedTableSource(
                Img, Rec, Ops, I, BReg, W, /*TableDisp=*/0,
                &StackTableMutated, &Initializers, &StackStorageConsumers);
            if (StackTableEvidenceIncompleteBranches.count(Rec.Addr))
              return false;
            if (StackSource != InvalidVA) {
              FrameStorage.RuntimeBase = {
                  {BaseV, Ops[AddressAddIdx].Addr, Ops[AddressAddIdx].Seq,
                   /*DefinedAtPoint=*/false},
                  /*ByteAddend=*/0};
              FrameStorage.CompleteAddress = {
                  Ops[AddressAddIdx].Output, Ops[AddressAddIdx].Addr,
                  Ops[AddressAddIdx].Seq, /*DefinedAtPoint=*/true};
              FrameStorage.Initializers = std::move(Initializers);
              if (FrameStorage.RuntimeBase.Use.Value.Size == 0 ||
                  FrameStorage.RuntimeBase.Use.Addr == InvalidVA ||
                  FrameStorage.RuntimeBase.Use.Seq < 0 ||
                  FrameStorage.RuntimeBase.Use.DefinedAtPoint ||
                  FrameStorage.CompleteAddress.Value.Size == 0 ||
                  !FrameStorage.CompleteAddress.DefinedAtPoint ||
                  FrameStorage.Initializers.empty())
                continue;
              Base = StackSource;
            }
          }
        }
      }
      if (!Base)
        continue;
      const va_t ResolvedBase = *Base;
      // The exact in-instruction base/scale plus the minimum contiguous run of
      // loader-authenticated code-pointer relocations is already specific to
      // an absolute jump table.  Record that branch-local shape before the
      // full physical-run and whole-image next-anchor audits: either audit may
      // exhaust on unrelated loader inventory, but that must not turn this
      // table-shaped jump into a callback-shaped tail call.
      const size_t PrefixLookup =
          orderedLookupWork(Img.CodePtrRelocSlots.size());
      if (!consumeProducts({{limits::kMinJumpTableEntries, PrefixLookup + 2}}))
        return false;
      bool HasMinimumRelocationPrefix = true;
      for (size_t Slot = 0; Slot < limits::kMinJumpTableEntries; ++Slot) {
        if (Slot > (std::numeric_limits<va_t>::max() - ResolvedBase) / W ||
            !Img.CodePtrRelocSlots.count(ResolvedBase + Slot * W)) {
          HasMinimumRelocationPrefix = false;
          break;
        }
      }
      if (!HasMinimumRelocationPrefix)
        continue;
      if (ShapeClaimed)
        *ShapeClaimed = true;
      if (!consumeProducts(
              {{Img.Segments.size(), 2}, {Img.Sections.size(), 1}}))
        return false;
      const auto *Seg = Img.getSegmentFor(ResolvedBase);
      if (!Seg || Seg->Data.empty())
        continue;
      if (!chargeRelocationRun())
        return false;
      uint32_t Run = countCodePtrRelocRun(Img, ResolvedBase, W);
      bool HasExactSizedObject = false;
      const bool ExactBaseOwnerAuthenticated =
          !ExactI386GOTOFFBase ||
          (ExactI386GOTOFFOwner && *ExactI386GOTOFFOwner == ResolvedBase);
      if (ExactBaseOwnerAuthenticated) {
        // An exact data symbol owns the whole table even when another
        // relocation names an interior constant arm.  For i386 GOTOFF, first
        // require that the relocation owner names this exact base.  Validate
        // the complete object against its mapped owner and the already
        // authenticated relocation run before ignoring an interior consumer
        // anchor.
        if (!consumeProducts({{Img.Symbols.size(), 2},
                              {Img.Segments.size(), 2},
                              {Img.Sections.size(), 2}}))
          return false;
        const std::optional<va_t> OwnerEnd =
            Img.mappedObjectOwnerEnd(ResolvedBase);
        const uint64_t ObjectSize = Img.dataObjectSizeAt(ResolvedBase);
        if (ObjectSize >= W && OwnerEnd &&
            ObjectSize <= InvalidVA - ResolvedBase &&
            ResolvedBase + ObjectSize <= *OwnerEnd &&
            ObjectSize % W == 0) {
          const uint64_t ExactSlots = ObjectSize / W;
          if (ExactSlots >= limits::kMinJumpTableEntries &&
              ExactSlots <= limits::kMaxJumpTableEntries &&
              ExactSlots <= Run) {
            Run = static_cast<uint32_t>(ExactSlots);
            HasExactSizedObject = true;
          }
        }
      }
      if (!HasExactSizedObject) {
        if (!ensureDecodedAnchors())
          return false;
        if (Run != 0 && !chargeNextAnchorBound())
          return false;
        Run = boundCodePtrRunByNextAnchor(Img, ResolvedBase, W, Run,
                                          *DecodedAnchors);
      }
      if (Run < limits::kMinJumpTableEntries)
        continue;

      // A shared -O0 dispatch may have one table LOAD in every predecessor,
      // with all targets spilled to the same frame slot and merged at one
      // INDIR_BR.  Collect every occurrence of one decode-identical table;
      // the publication proof then requires every feasible branch-target arm
      // to originate in this set.  A different-base load is not silently
      // adopted into the model and therefore makes an actual mixed merge fail
      // closed at the branch certificate.
      if (FoundModel &&
          (ResolvedBase != ModelBase || W != ModelWidth || Run != ModelRun))
        continue;

      if (!chargeRegisterTrace())
        return false;
      uint64_t IdxSrc = traceRegSource(Ops, AddressAddIdx - 1, Idx);
      if (!FoundModel) {
        FoundModel = true;
        ModelBase = ResolvedBase;
        ModelOwnerVA = ExactI386GOTOFFOwner.value_or(InvalidVA);
        ModelWidth = W;
        ModelRun = Run;
        if (ExactI386GOTOFFBase)
          ModelZeroOccurrence = JumpTableValueOccurrence{
              Ops[AddressAddIdx].Inputs[1 - Side], Ops[AddressAddIdx].Addr,
              Ops[AddressAddIdx].Seq, /*DefinedAtPoint=*/false};
        Info.setBaseAddr(ResolvedBase);
        Info.EntrySize = W;
        Info.IsRelative = false;
        Info.IsSigned = false;
        Info.IndexReg = (IdxSrc != InvalidVA) ? IdxSrc : Idx;
        Info.IndexValueAtUse = ExactIndex;
        Info.IndexUseAddr = IndexUseAddr;
        Info.IndexUseSeq = IndexUseSeq;
        Info.TableLoadAddr = L.Addr;
        Info.TableLoadSeq = L.Seq;
        // The relocation run authenticates storage and entry decoding only.
        // The exact runtime selector domain is established later by a guard,
        // mask, or complete modulo proof.
        Info.PhysicalCapacity = Run;
        Info.RelocAbsolute = true;
        if (!FrameStorage.Initializers.empty()) {
          size_t StaticSourceCount = 0;
          for (const JumpTableFrameInitializerChunk &Initializer :
               FrameStorage.Initializers) {
            if (Initializer.StaticSources.size() >
                std::numeric_limits<size_t>::max() - StaticSourceCount)
              return false;
            StaticSourceCount += Initializer.StaticSources.size();
          }
          if (!consumeProducts(
                  {{FrameStorage.Initializers.size(), 5},
                   {StaticSourceCount, 3},
                   {StackStorageConsumers.size(), 3}}))
            return false;
          Info.AuthenticatedFrameStorage = FrameStorage;
          Info.AuthenticatedStorageConsumers = StackStorageConsumers;
          Info.MutatedUnsafe = StackTableMutated;
        }
      }
      if (!appendOccurrence(ResolvedBase, ExactIndex, IndexUseAddr,
                            IndexUseSeq, ResolvedBase, ExactIndex,
                            IndexUseAddr, IndexUseSeq, W, std::nullopt,
                            std::move(FrameStorage)))
        return false;
      LLVM_DEBUG(llvm::dbgs() << "  const-base-abs: decoupled absolute table 0x"
                              << llvm::utohexstr(ResolvedBase) << " (W=" << W
                              << ", " << Run << " entries)\n");
      break;
    }
  }
  return completed(FoundModel);
}

} // namespace neverd
