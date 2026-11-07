//===- LowScalarConstant.cpp - Bounded integer constant evaluation -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shares width-correct scalar constant evaluation across LowIR analyses.

#include "neverd/ir/low/LowScalarConstant.h"

#include "neverd/ir/Limits.h"
#include "neverd/ir/low/BlockRegisterConstant.h"
#include "neverd/ir/low/CallRegisterBody.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/ArrayRef.h"

#include <algorithm>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace neverd {

std::optional<uint64_t> lowScalarConstantResult(
    const LowOp &Op,
    llvm::function_ref<std::optional<uint64_t>(const NdVar &)> ValueOf) {
  if (!Op.Output.Size || Op.Output.Size > 8 ||
      (!Op.Output.isReg() && !Op.Output.isTemp()) ||
      Op.NumInputs > std::size(Op.Inputs))
    return std::nullopt;
  const auto Mask = [](uint16_t Width) {
    return UINT64_MAX >> ((8 - Width) * 8);
  };
  // AArch64 immediates can retain their encoded scalar width. Coerce only
  // literal operands here; callback lookups must still see their exact lane.
  if (Op.NumInputs == 2 && Op.Output.Size > 1 &&
      (Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB ||
       Op.Opcode == NdOp::INT_MULT || Op.Opcode == NdOp::INT_AND ||
       Op.Opcode == NdOp::INT_OR || Op.Opcode == NdOp::INT_XOR ||
       Op.Opcode == NdOp::INT_LEFT || Op.Opcode == NdOp::INT_RIGHT ||
       Op.Opcode == NdOp::INT_ASHR)) {
    LowOp Coerced = Op;
    bool Changed = false;
    for (unsigned I = 0; I != 2; ++I)
      if (Coerced.Inputs[I].isConst() && Coerced.Inputs[I].Size &&
          Coerced.Inputs[I].Size < Op.Output.Size) {
        Coerced.Inputs[I].Offset &= Mask(Coerced.Inputs[I].Size);
        Coerced.Inputs[I].Size = Op.Output.Size;
        Changed = true;
      }
    if (Changed)
      return lowScalarConstantResult(Coerced, ValueOf);
  }
  std::optional<uint64_t> Result;
  if (Op.NumInputs == 1 &&
      (Op.Opcode == NdOp::COPY || Op.Opcode == NdOp::INT_ZEXT ||
       Op.Opcode == NdOp::INT_SEXT || Op.Opcode == NdOp::INT_NOT ||
       Op.Opcode == NdOp::INT_NEGATE || Op.Opcode == NdOp::INT_NEG2 ||
       Op.Opcode == NdOp::BOOL_NOT)) {
    const auto Width = Op.Inputs[0].Size;
    const bool Extension =
        Op.Opcode == NdOp::INT_ZEXT || Op.Opcode == NdOp::INT_SEXT;
    if (!Width || Width > Op.Output.Size ||
        (Op.Opcode == NdOp::BOOL_NOT && (Width != 1 || Op.Output.Size != 1)) ||
        (!Extension && Width != Op.Output.Size))
      return std::nullopt;
    if (auto Value = ValueOf(Op.Inputs[0])) {
      const uint64_t Bits = *Value & Mask(Width);
      if (Op.Opcode == NdOp::INT_SEXT)
        Result = llvm::APInt(Width * 8, Bits)
                     .sextOrTrunc(Op.Output.Size * 8)
                     .getZExtValue();
      else if (Op.Opcode == NdOp::INT_NOT || Op.Opcode == NdOp::INT_NEGATE)
        Result = ~Bits;
      else if (Op.Opcode == NdOp::INT_NEG2)
        Result = uint64_t{0} - Bits;
      else
        Result = Op.Opcode == NdOp::BOOL_NOT ? !Bits : Bits;
    }
  } else if (Op.NumInputs == 2 && Op.Output.Size == 1 && Op.Inputs[0].Size &&
             Op.Inputs[0].Size <= 8 && Op.Inputs[0].Size == Op.Inputs[1].Size &&
             (Op.Opcode == NdOp::INT_EQUAL || Op.Opcode == NdOp::INT_NOTEQUAL ||
              Op.Opcode == NdOp::INT_LESS || Op.Opcode == NdOp::INT_LESSEQUAL ||
              Op.Opcode == NdOp::INT_SLESS ||
              Op.Opcode == NdOp::INT_SLESSEQUAL)) {
    const auto Left = ValueOf(Op.Inputs[0]);
    const auto Right = ValueOf(Op.Inputs[1]);
    if (Left && Right) {
      const auto Width = Op.Inputs[0].Size;
      uint64_t L = *Left & Mask(Width), R = *Right & Mask(Width);
      if (Op.Opcode == NdOp::INT_SLESS || Op.Opcode == NdOp::INT_SLESSEQUAL) {
        // Bias the sign bit instead of converting an out-of-range unsigned
        // value to a host signed integer.
        const uint64_t Sign = uint64_t{1} << (Width * 8 - 1);
        L ^= Sign;
        R ^= Sign;
      }
      Result = Op.Opcode == NdOp::INT_EQUAL      ? L == R
               : Op.Opcode == NdOp::INT_NOTEQUAL ? L != R
               : Op.Opcode == NdOp::INT_LESS || Op.Opcode == NdOp::INT_SLESS
                   ? L < R
                   : L <= R;
    }
  } else if (Op.NumInputs == 2 &&
             (Op.Opcode == NdOp::INT_LEFT || Op.Opcode == NdOp::INT_RIGHT ||
              Op.Opcode == NdOp::INT_ASHR) &&
             Op.Inputs[0].Size == Op.Output.Size &&
             Op.Inputs[1].Size == Op.Output.Size) {
    const auto Left = ValueOf(Op.Inputs[0]);
    const auto Right = ValueOf(Op.Inputs[1]);
    if (Left && Right) {
      const unsigned Bits = Op.Output.Size * 8;
      const llvm::APInt Value(Bits, *Left);
      const unsigned Count =
          unsigned(std::min<uint64_t>(*Right & Mask(Op.Output.Size), Bits));
      // ISA count masking belongs to the lifter. LowIR shifts saturate at
      // their declared width; never perform an out-of-range host shift.
      Result = (Op.Opcode == NdOp::INT_LEFT ? Value.shl(Count)
                : Op.Opcode == NdOp::INT_RIGHT
                    ? Value.lshr(Count)
                    : Value.ashr(std::min(Count, Bits - 1)))
                   .getZExtValue();
    }
  } else if (Op.NumInputs == 2 &&
             (Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB ||
              Op.Opcode == NdOp::INT_MULT || Op.Opcode == NdOp::INT_XOR ||
              Op.Opcode == NdOp::INT_AND || Op.Opcode == NdOp::INT_OR ||
              Op.Opcode == NdOp::BOOL_AND || Op.Opcode == NdOp::BOOL_OR ||
              Op.Opcode == NdOp::BOOL_XOR) &&
             Op.Inputs[0].Size == Op.Output.Size &&
             Op.Inputs[1].Size == Op.Output.Size) {
    if ((Op.Opcode == NdOp::INT_XOR || Op.Opcode == NdOp::INT_SUB) &&
        Op.Inputs[0] == Op.Inputs[1] &&
        (Op.Inputs[0].isReg() || Op.Inputs[0].isTemp()))
      Result = 0;
    else {
      const auto Left = ValueOf(Op.Inputs[0]);
      const auto Right = ValueOf(Op.Inputs[1]);
      const uint64_t WidthMask = Mask(Op.Output.Size);
      if (Op.Opcode == NdOp::INT_AND && ((Left && (*Left & WidthMask) == 0) ||
                                         (Right && (*Right & WidthMask) == 0)))
        Result = 0;
      else if (Op.Opcode == NdOp::INT_OR &&
               ((Left && (*Left & WidthMask) == WidthMask) ||
                (Right && (*Right & WidthMask) == WidthMask)))
        Result = WidthMask;
      else if (Left && Right) {
        if (Op.Opcode == NdOp::BOOL_AND || Op.Opcode == NdOp::BOOL_OR ||
            Op.Opcode == NdOp::BOOL_XOR) {
          if (Op.Output.Size != 1)
            return std::nullopt;
          const bool L = (*Left & UINT8_MAX) != 0;
          const bool R = (*Right & UINT8_MAX) != 0;
          Result = Op.Opcode == NdOp::BOOL_AND  ? L && R
                   : Op.Opcode == NdOp::BOOL_OR ? L || R
                                                : L != R;
        } else
          Result = Op.Opcode == NdOp::INT_ADD    ? *Left + *Right
                   : Op.Opcode == NdOp::INT_SUB  ? *Left - *Right
                   : Op.Opcode == NdOp::INT_MULT ? *Left * *Right
                   : Op.Opcode == NdOp::INT_AND  ? *Left & *Right
                   : Op.Opcode == NdOp::INT_OR   ? *Left | *Right
                                                 : *Left ^ *Right;
      }
    }
  }
  return Result ? std::optional<uint64_t>{*Result & Mask(Op.Output.Size)}
                : std::nullopt;
}

namespace {
using ValueLookup = llvm::function_ref<std::optional<uint64_t>(const NdVar &)>;

// Pack storage into words without promoting the proof to word granularity.
// A byte mask retains partial-register writes, including unaligned ranges.
// Replacing an aligned constant updates one existing node instead of freeing
// and allocating eight byte nodes on every machine operation.
class RegisterConstants {
  static constexpr unsigned WordBytes = sizeof(uint64_t);
  struct Word {
    uint64_t Bits = 0;
    uint8_t Known = 0;
  };
  std::map<uint64_t, Word> Words;

  static uint64_t base(uint64_t Offset) {
    return Offset & ~(uint64_t{WordBytes} - 1);
  }
  static uint64_t bitsMask(unsigned Size) {
    return UINT64_MAX >> ((WordBytes - Size) * 8);
  }
  static uint8_t byteMask(unsigned Begin, unsigned Size) {
    return static_cast<uint8_t>(((1U << Size) - 1) << Begin);
  }

public:
  void clear() { Words.clear(); }

  std::optional<uint64_t> read(uint64_t Offset, unsigned Size) const {
    uint64_t Bits = 0;
    for (unsigned I = 0; I < Size;) {
      const auto Address = Offset + I;
      const auto Begin = static_cast<unsigned>(Address - base(Address));
      const unsigned Count = std::min(Size - I, WordBytes - Begin);
      const auto Found = Words.find(base(Address));
      const auto Mask = byteMask(Begin, Count);
      if (Found == Words.end() || (Found->second.Known & Mask) != Mask)
        return {};
      Bits |= ((Found->second.Bits >> (Begin * 8)) & bitsMask(Count))
              << (I * 8);
      I += Count;
    }
    return Bits;
  }

  void write(uint64_t Offset, unsigned Size, uint64_t Bits) {
    for (unsigned I = 0; I < Size;) {
      const auto Address = Offset + I;
      const auto Begin = static_cast<unsigned>(Address - base(Address));
      const unsigned Count = std::min(Size - I, WordBytes - Begin);
      auto &Value = Words[base(Address)];
      const uint64_t Mask = bitsMask(Count) << (Begin * 8);
      Value.Bits =
          (Value.Bits & ~Mask) | ((Bits >> (I * 8)) << (Begin * 8) & Mask);
      Value.Known |= byteMask(Begin, Count);
      I += Count;
    }
  }

  void forget(uint64_t Offset, unsigned Size) {
    const uint64_t End = Offset + Size;
    // Visit only materialized words, even for a very wide unknown output.
    for (auto It = Words.lower_bound(base(Offset));
         It != Words.end() && It->first < End;) {
      const auto Begin =
          static_cast<unsigned>(Offset > It->first ? Offset - It->first : 0);
      const auto Count = static_cast<unsigned>(
          std::min<uint64_t>(WordBytes, End - It->first) - Begin);
      It->second.Known &= ~byteMask(Begin, Count);
      if (!It->second.Known)
        It = Words.erase(It);
      else
        ++It;
    }
  }
};

bool visitScalarBlockValues(
    llvm::ArrayRef<LowBlock> Blocks,
    llvm::function_ref<bool(const LowOp &)> Select,
    llvm::function_ref<void(const LowOp &, ValueLookup)> Observe,
    size_t &Remaining, bool NumericOnly = false) {
  for (const auto &Block : Blocks) {
    if (Block.Ops.size() > Remaining) {
      Remaining = 0;
      return false;
    }
    Remaining -= Block.Ops.size();
    if (std::none_of(Block.Ops.begin(), Block.Ops.end(), Select))
      continue;
    RegisterConstants Registers;
    std::map<uint64_t, std::pair<uint16_t, uint64_t>> Temps;
    va_t Instruction = InvalidVA;
    auto ValueOf = [&](const NdVar &Value) -> std::optional<uint64_t> {
      if (!Value.Size || Value.Size > sizeof(uint64_t))
        return {};
      if (Value.isConst() &&
          (!NumericOnly || !isAddressProvenance(Value.Provenance)))
        return Value.Offset & (UINT64_MAX >> ((8 - Value.Size) * 8));
      if (Value.isTemp()) {
        const auto It = Temps.find(Value.Offset);
        if (It != Temps.end() && It->second.first == Value.Size)
          return It->second.second;
        return {};
      }
      if (!Value.isReg() || Value.Offset > UINT64_MAX - Value.Size)
        return {};
      return Registers.read(Value.Offset, Value.Size);
    };
    for (const auto &Op : Block.Ops) {
      if (Op.Addr != Instruction) {
        Temps.clear();
        Instruction = Op.Addr;
      }
      if (Select(Op))
        Observe(Op, ValueOf);
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
          Op.Opcode == NdOp::INTRINSIC) {
        Registers.clear();
        Temps.clear();
        continue;
      }
      const auto &Output = Op.Output;
      if (!Output.Size || (!Output.isReg() && !Output.isTemp()))
        continue;
      const auto Result =
          Op.MemoryOrdering == NdMemoryOrdering::None &&
                  Op.MemoryAddressSpace == NdMemoryAddressSpace::Default
              ? lowScalarConstantResult(Op, ValueOf)
              : std::nullopt;
      if (Output.isTemp()) {
        if (Output.Offset > UINT64_MAX - Output.Size) {
          Temps.clear();
          continue;
        }
        // Known temporaries hold at most one scalar word. Visit just the
        // overlapping range, including a prior word's partial tail.
        const uint64_t Back = sizeof(uint64_t) - 1;
        for (auto It = Temps.lower_bound(
                 Output.Offset > Back ? Output.Offset - Back : 0);
             It != Temps.end() && It->first < Output.Offset + Output.Size;) {
          if (Output.Offset < It->first + It->second.first)
            It = Temps.erase(It);
          else
            ++It;
        }
        if (Result)
          Temps[Output.Offset] = {Output.Size, *Result};
      } else if (Output.Offset <= UINT64_MAX - Output.Size) {
        // Erase overlapping byte facts even for a wide unknown result. Never
        // interpret an architectural subregister write as a whole GPR write.
        if (Result)
          Registers.write(Output.Offset, Output.Size, *Result);
        else
          Registers.forget(Output.Offset, Output.Size);
      } else
        Registers.clear();
    }
  }
  return true;
}
} // namespace

std::map<SourceCallOccurrenceKey, va_t>
constantRegisterCallTargets(const LowFunc &Function) {
  return constantRegisterCallTargets(CallRegisterBodyView(Function));
}

std::map<SourceCallOccurrenceKey, va_t>
constantRegisterCallTargets(const CallRegisterBodyView &Function) {
  std::map<SourceCallOccurrenceKey, va_t> Targets;
  std::set<SourceCallOccurrenceKey> Seen, Repeated;
  size_t Remaining = limits::ir::kConstantRegisterCallOperations;
  const auto Select = [](const LowOp &Op) {
    return Op.Opcode == NdOp::INDIR_CALL && Op.NumInputs == 1 &&
           !Op.Inputs[0].isConst();
  };
  const bool Complete = visitScalarBlockValues(
      Function.Blocks, Select,
      [&](const LowOp &Op, ValueLookup ValueOf) {
        if (const auto Site = sourceCallOccurrenceKey(Op)) {
          if (!Seen.insert(*Site).second) {
            Targets.erase(*Site);
            Repeated.insert(*Site);
          } else if (const auto Target = ValueOf(Op.Inputs[0]))
            Targets.emplace(*Site, *Target);
        }
      },
      Remaining);
  if (!Complete)
    return {};
  // Local proofs need no predecessor state. Only unresolved occurrences pay
  // for the bounded graph walk, and incomplete/multi-entry bodies inherit none.
  if (Targets.size() + Repeated.size() == Seen.size() || !Function.Complete ||
      Function.HasIndependentRoots || Function.UnsafeIndirectBranches)
    return Targets;
  const auto Spend = [&](size_t Count) {
    if (Count > Remaining) {
      Remaining = 0;
      return false;
    }
    Remaining -= Count;
    return true;
  };
  if (!Spend(Function.Blocks.size()))
    return {};
  std::map<int, const LowBlock *> Blocks;
  for (const auto &Block : Function.Blocks)
    if (!Blocks.emplace(Block.Id, &Block).second)
      return Targets;
  for (const auto &Block : Function.Blocks) {
    std::set<SourceCallOccurrenceKey> Pending;
    for (const auto &Op : Block.Ops)
      if (Select(Op))
        if (const auto Site = sourceCallOccurrenceKey(Op);
            Site && !Targets.count(*Site) && !Repeated.count(*Site))
          Pending.insert(*Site);
    if (Pending.empty())
      continue;
    std::vector<const LowBlock *> Path{&Block};
    std::set<int> Visited{Block.Id};
    bool Cyclic = false;
    while (Path.back() != &Function.Blocks.front()) {
      const auto &Current = *Path.back();
      if (!Spend(1))
        return {};
      if (Current.Preds.size() != 1 || !Current.ExceptionalPreds.empty())
        break;
      const auto Found = Blocks.find(Current.Preds.front());
      if (Found == Blocks.end())
        break;
      const auto &Pred = *Found->second;
      if (Pred.Succs.size() != 1 || Pred.Succs.front() != Current.Id ||
          !Pred.ExceptionalSuccs.empty() || !Pred.ExceptionalPreds.empty() ||
          Pred.InstructionBoundaries.empty())
        break;
      const auto &Edge = Pred.InstructionBoundaries.back();
      if (!Edge.Size || Edge.Address < Pred.StartAddr ||
          Edge.Address >= Pred.EndAddr ||
          Edge.Size != Pred.EndAddr - Edge.Address ||
          Edge.FirstOp > Pred.Ops.size() ||
          Edge.OpCount != Pred.Ops.size() - Edge.FirstOp)
        break;
      const bool Direct =
          Edge.Control == LowInstructionControl::Branch && Edge.Immediate &&
          *Edge.Immediate == Current.StartAddr &&
          !hasLowInstructionControlFlag(Edge.ControlFlags,
                                        LowInstructionControlFlag::Indirect) &&
          !hasLowInstructionControlFlag(Edge.ControlFlags,
                                        LowInstructionControlFlag::Conditional);
      const bool Fallthrough = Edge.Control == LowInstructionControl::None &&
                               Pred.EndAddr == Current.StartAddr;
      if (!Direct && !Fallthrough)
        break;
      if (!Visited.insert(Pred.Id).second) {
        Cyclic = true;
        break;
      }
      Path.push_back(&Pred);
    }
    if (Cyclic || Path.size() == 1)
      continue;
    LowBlock Prefix;
    for (auto It = Path.rbegin(); It != Path.rend(); ++It) {
      // Copying and evaluating the prefix both consume the same work cap.
      if (!Spend((*It)->Ops.size()))
        return {};
      Prefix.Ops.insert(Prefix.Ops.end(), (*It)->Ops.begin(), (*It)->Ops.end());
    }
    if (!visitScalarBlockValues(
            llvm::ArrayRef(&Prefix, 1),
            [&](const LowOp &Op) {
              const auto Site = sourceCallOccurrenceKey(Op);
              return Site && Pending.count(*Site);
            },
            [&](const LowOp &Op, ValueLookup ValueOf) {
              if (const auto Target = ValueOf(Op.Inputs[0]))
                Targets.emplace(*sourceCallOccurrenceKey(Op), *Target);
            },
            Remaining))
      return {};
  }
  return Targets;
}

LowCallRegisterConstant
constantRegisterValueAtCall(const LowFunc &Function,
                            const SourceCallOccurrenceKey &Site,
                            const NdVar &Register) {
  LowCallRegisterConstant Result;
  if (!Register.isReg() || !Register.Size || Register.Size > 8)
    return Result;
  size_t Remaining = limits::ir::kConstantRegisterCallOperations;
  const bool Complete = visitScalarBlockValues(
      Function.Blocks,
      [&](const LowOp &Op) { return sourceCallOccurrenceKey(Op) == Site; },
      [&](const LowOp &, ValueLookup ValueOf) {
        if (Result.OccurrenceFound)
          Result.Value.reset();
        else {
          Result.OccurrenceFound = true;
          Result.Value = ValueOf(Register);
        }
      },
      Remaining);
  if (!Complete) {
    Result = {};
    Result.BudgetExceeded = true;
  }
  return Result;
}

std::set<const LowOp *>
lowBlockAbsorbingConstants(llvm::ArrayRef<LowBlock> Blocks, size_t &Remaining) {
  std::set<const LowOp *> Result;
  const bool Complete = visitScalarBlockValues(
      Blocks,
      [](const LowOp &Op) {
        return (Op.Opcode == NdOp::INT_AND || Op.Opcode == NdOp::INT_OR) &&
               Op.NumInputs == 2;
      },
      [&](const LowOp &Op, ValueLookup ValueOf) {
        LowOp Folded = Op;
        for (unsigned I = 0; I != 2; ++I)
          if (const auto Value = ValueOf(Op.Inputs[I]))
            Folded.Inputs[I] = NdVar::scalar(*Value, Op.Inputs[I].Size);
        if (lowAbsorbingConstant(Folded))
          Result.insert(&Op);
      },
      Remaining, true);
  return Complete ? Result : std::set<const LowOp *>{};
}

} // namespace neverd
