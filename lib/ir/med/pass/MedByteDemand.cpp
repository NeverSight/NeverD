//===- MedByteDemand.cpp - Bytes a function reads of a value ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/MedByteDemand.h"

#include "neverd/ir/Limits.h"
#include "neverd/ir/TargetRegInfo.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/MathExtras.h"

namespace neverd {

MedByteMask medBytesOf(const MedVar &Value) {
  return !Value.Size || Value.Size >= kMedByteMaskBytes
             ? ~MedByteMask(0)
             : (MedByteMask(1) << Value.Size) - 1;
}

bool medReadsInputsWhole(const MedOp &Op) {
  switch (Op.Opcode) {
  case NdOp::LOAD:
  case NdOp::STORE:
  case NdOp::ATOMIC_XCHG:
  case NdOp::ATOMIC_ADD:
  case NdOp::ATOMIC_CMPXCHG:
  case NdOp::INT_DIV:
  case NdOp::INT_SDIV:
  case NdOp::INT_REM:
  case NdOp::INT_SREM:
  case NdOp::BRANCH:
  case NdOp::COND_BR:
  case NdOp::INDIR_BR:
  case NdOp::CALL:
  case NdOp::INDIR_CALL:
  case NdOp::RETURN:
  case NdOp::NOP:
  case NdOp::INTRINSIC:
    return true;
  default:
    break;
  }
  if (Op.MemoryOrdering != NdMemoryOrdering::None || !Op.Output.Size ||
      !Op.IntrinsicOutputs.empty())
    return true;
  return Op.Output.Kind != MedVar::Temp && Op.Output.Kind != MedVar::Reg;
}

bool medLeavesOtherRegisters(const MedFunc &Func) {
  if (!Func.MultiReturn.empty() || Func.FPReturnViaX87 ||
      Func.ExplicitX87ReturnValue || Func.ReturnsVectorRegister)
    return true;
  TypeRef Type = Func.ReturnType;
  if (Func.SourceTypeHint && Func.SourceTypeHint->ReturnType)
    Type = Func.SourceTypeHint->ReturnType;
  if (!Type ||
      (Type->Kind != NdTypeKind::Int && Type->Kind != NdTypeKind::Ptr &&
       Type->Kind != NdTypeKind::Void))
    return true;
  return llvm::any_of(Func.Blocks, [](const MedBlock &Block) {
    return !Block.ExceptionalSuccs.empty() || !Block.ExceptionalPreds.empty();
  });
}

namespace {

bool scalarLiteral(const MedVar &V) {
  return V.isConst() && !isAddressProvenance(V.Provenance);
}

/// \p Bytes and every byte below its highest: what a carry chain reads.
MedByteMask throughHighest(MedByteMask Bytes) {
  return Bytes ? llvm::maskTrailingOnes<MedByteMask>(llvm::Log2_64(Bytes) + 1)
               : 0;
}

/// The bytes of each input of \p Op that \p Bytes of its output depend on.
/// A copy, a view, a byte merge, a bitwise operation, a carry chain and a
/// shift pass on the bytes that can reach the observation. Other operations
/// read their inputs whole.
template <typename NeedFn, typename ZeroFn>
void inputDemand(const MedOp &Op, MedByteMask Bytes, NeedFn &&Need,
                 ZeroFn &&ZeroBytes, bool AllZeroFactsFit) {
  // A value the mask covers, and one a machine word holds.
  const auto Covered = [](const MedVar &Value) {
    return Value.Size && Value.Size <= kMedByteMaskBytes;
  };
  const auto Word = [](const MedVar &Value) {
    return Value.Size && Value.Size <= 8;
  };
  if (Covered(Op.Output))
    switch (Op.Opcode) {
    case NdOp::COPY:
    case NdOp::INT_NOT:
      if (Op.NumInputs == 1 && Op.Inputs[0].Size == Op.Output.Size) {
        Need(Op.Inputs[0], Bytes);
        return;
      }
      break;
    case NdOp::INT_ZEXT:
      if (Op.NumInputs == 1 && Covered(Op.Inputs[0])) {
        Need(Op.Inputs[0], Bytes & medBytesOf(Op.Inputs[0]));
        return;
      }
      break;
    case NdOp::INT_SEXT:
      if (Op.NumInputs == 1 && Covered(Op.Inputs[0])) {
        // The bytes above the source repeat its sign.
        const MedByteMask Low = medBytesOf(Op.Inputs[0]);
        Need(
            Op.Inputs[0],
            (Bytes & Low) |
                (Bytes & ~Low ? MedByteMask(1) << (Op.Inputs[0].Size - 1) : 0));
        return;
      }
      break;
    case NdOp::SUBBYTES:
      if (Op.NumInputs == 2 && scalarLiteral(Op.Inputs[1]) &&
          Covered(Op.Inputs[0]) && Op.Inputs[1].ConstVal < kMedByteMaskBytes) {
        Need(Op.Inputs[0], Bytes << Op.Inputs[1].ConstVal);
        return;
      }
      break;
    case NdOp::CONCAT:
      // The second operand is the low part.
      if (Op.NumInputs == 2 && Covered(Op.Inputs[0]) && Covered(Op.Inputs[1]) &&
          Op.Inputs[1].Size < kMedByteMaskBytes) {
        Need(Op.Inputs[1], Bytes & medBytesOf(Op.Inputs[1]));
        Need(Op.Inputs[0], Bytes >> Op.Inputs[1].Size);
        return;
      }
      break;
    case NdOp::INT_AND:
    case NdOp::INT_OR:
    case NdOp::INT_XOR:
      if (Op.NumInputs == 2 && Word(Op.Output)) {
        for (unsigned I = 0; I < 2; ++I) {
          // A literal has no SSA demand. Above the slice bound the order of
          // zero-byte queries can affect which earlier facts are available.
          if (AllZeroFactsFit && Op.Inputs[I].isConst())
            continue;
          const MedVar &Other = Op.Inputs[1 - I];
          MedByteMask Kept = Bytes;
          // A byte ANDed with zero or ORed with ones shows nothing of the
          // other operand: `and %eax, %ebx` reads only the low byte of an
          // EAX a `setcc` wrote where EBX holds a zero-extended byte.
          if (Op.Opcode == NdOp::INT_OR && scalarLiteral(Other)) {
            for (unsigned Byte = 0; Byte < 8; ++Byte)
              if (((Other.ConstVal >> (8 * Byte)) & 0xFF) == 0xFFu)
                Kept &= ~(MedByteMask(1) << Byte);
          } else if (Op.Opcode == NdOp::INT_AND) {
            Kept &= ~ZeroBytes(Other);
          }
          Need(Op.Inputs[I], Kept);
        }
        return;
      }
      break;
    case NdOp::INT_ADD:
    case NdOp::INT_SUB:
    case NdOp::INT_MULT:
      if (Op.NumInputs == 2 && Word(Op.Output)) {
        Need(Op.Inputs[0], throughHighest(Bytes));
        Need(Op.Inputs[1], throughHighest(Bytes));
        return;
      }
      break;
    case NdOp::INT_LEFT:
      // A left shift moves bits up only, whatever the amount.
      if (Op.NumInputs == 2 && Word(Op.Output)) {
        Need(Op.Inputs[0], throughHighest(Bytes));
        Need(Op.Inputs[1], medBytesOf(Op.Inputs[1]));
        return;
      }
      break;
    case NdOp::INT_RIGHT:
      if (Op.NumInputs == 2 && Word(Op.Output) &&
          Op.Inputs[0].Size == Op.Output.Size && scalarLiteral(Op.Inputs[1]) &&
          Op.Inputs[1].ConstVal < 8u * Op.Output.Size) {
        const unsigned Bits = static_cast<unsigned>(Op.Inputs[1].ConstVal);
        MedByteMask Read = Bytes << (Bits / 8);
        if (Bits % 8)
          Read |= Read << 1;
        Need(Op.Inputs[0], Read);
        return;
      }
      break;
    default:
      break;
    }
  for (uint8_t I = 0; I < Op.NumInputs; ++I)
    Need(Op.Inputs[I], medBytesOf(Op.Inputs[I]));
}
/// The zero bytes of the constant \p Value, among the bytes it has.
MedByteMask zeroBytesOf(const MedVar &Value) {
  if (isAddressProvenance(Value.Provenance))
    return 0;
  MedByteMask Zero = 0;
  for (unsigned Byte = 0; Byte < 8 && (!Value.Size || Byte < Value.Size);
       ++Byte)
    if (!((Value.ConstVal >> (8 * Byte)) & 0xFF))
      Zero |= MedByteMask(1) << Byte;
  return Zero;
}

/// Whether zeroBytesThrough knows what \p Op makes of zero bytes.
bool passesZeroBytes(const MedOp &Op) {
  if (!Op.Output.Size || Op.Output.Size > 8 ||
      Op.MemoryOrdering != NdMemoryOrdering::None)
    return false;
  const auto Narrow = [](const MedVar &Value) {
    return Value.Size && Value.Size <= 8;
  };
  switch (Op.Opcode) {
  case NdOp::COPY:
    return Op.NumInputs == 1 && Op.Inputs[0].Size == Op.Output.Size;
  case NdOp::INT_ZEXT:
    return Op.NumInputs == 1 && Narrow(Op.Inputs[0]);
  case NdOp::INT_AND:
  case NdOp::INT_OR:
  case NdOp::INT_XOR:
    return Op.NumInputs == 2;
  case NdOp::SUBBYTES:
    return Op.NumInputs == 2 && scalarLiteral(Op.Inputs[1]) &&
           Narrow(Op.Inputs[0]) && Op.Inputs[1].ConstVal < 8;
  case NdOp::CONCAT:
    return Op.NumInputs == 2 && Narrow(Op.Inputs[0]) && Narrow(Op.Inputs[1]) &&
           Op.Inputs[1].Size < 8;
  default:
    return false;
  }
}

/// The zero bytes of what \p Op computes, given its inputs' by \p Of.
template <typename OfFn>
MedByteMask zeroBytesThrough(const MedOp &Op, OfFn &&Of) {
  const MedByteMask Output = medBytesOf(Op.Output);
  switch (Op.Opcode) {
  case NdOp::COPY:
    return Of(Op.Inputs[0]) & Output;
  case NdOp::INT_ZEXT: {
    const MedByteMask Low = medBytesOf(Op.Inputs[0]);
    return ((Of(Op.Inputs[0]) & Low) | ~Low) & Output;
  }
  case NdOp::INT_AND:
    return (Of(Op.Inputs[0]) | Of(Op.Inputs[1])) & Output;
  case NdOp::INT_OR:
  case NdOp::INT_XOR:
    return Of(Op.Inputs[0]) & Of(Op.Inputs[1]) & Output;
  case NdOp::SUBBYTES:
    return (Of(Op.Inputs[0]) >> Op.Inputs[1].ConstVal) & Output;
  case NdOp::CONCAT:
    return ((Of(Op.Inputs[0]) << Op.Inputs[1].Size) |
            (Of(Op.Inputs[1]) & medBytesOf(Op.Inputs[1]))) &
           Output;
  default:
    return 0;
  }
}
} // namespace

void MedByteDemand::define(const PhiNode &Phi) {
  const unsigned Output = nodeOf(Phi.Output);
  Nodes[Output].Phi = &Phi;
  for (const auto &[Predecessor, Value] : Phi.Args) {
    (void)Predecessor;
    use(Value);
  }
  if (Phi.ExceptionalEntry)
    use(*Phi.ExceptionalEntry);
}

void MedByteDemand::define(const MedOp &Op) {
  if (Op.Output.Size && !Op.Output.isConst()) {
    const unsigned Output = nodeOf(Op.Output);
    if (!Nodes[Output].Op && !Nodes[Output].Phi)
      Nodes[Output].Op = &Op;
  }
  const bool Whole = medReadsInputsWhole(Op);
  for (uint8_t I = 0; I < Op.NumInputs; ++I) {
    use(Op.Inputs[I]);
    if (Whole)
      need(Op.Inputs[I], medBytesOf(Op.Inputs[I]));
  }
}

void MedByteDemand::use(const MedVar &Value) {
  if (!Value.isConst())
    Nodes[nodeOf(Value)].Used = true;
}

void MedByteDemand::need(const MedVar &Value, MedByteMask Bytes) {
  if (Value.isConst())
    return;
  Bytes = Value.Size > kMedByteMaskBytes && Bytes ? ~MedByteMask(0)
                                                  : Bytes & medBytesOf(Value);
  const unsigned Index = nodeOf(Value);
  Node &N = Nodes[Index];
  if ((N.Bytes | Bytes) == N.Bytes)
    return;
  N.Bytes |= Bytes;
  if (!N.Pending) {
    N.Pending = true;
    Pending.push_back(Index);
  }
}

void MedByteDemand::handBack(const MedVar &Value) {
  if (Value.isConst())
    return;
  use(Value);
  Returns.push_back(Value);
}

bool MedByteDemand::handsBack(const MedVar &Value) const {
  const auto Found = Index.find(medValueKey(Value));
  return Found != Index.end() && Nodes[Found->second].HandedBack;
}

void MedByteDemand::settle() {
  // What a return hands back: a call's result as it comes, through the PHIs
  // that merge it and the views of it a return reads; any other value is
  // one the function computes, read whole.
  while (!Returns.empty()) {
    const MedVar Value = Returns.back();
    Returns.pop_back();
    if (Value.isConst())
      continue;
    const unsigned At = nodeOf(Value);
    if (Nodes[At].HandedBack)
      continue;
    Nodes[At].HandedBack = true;
    if (const PhiNode *Phi = Nodes[At].Phi) {
      for (const auto &[Predecessor, Arm] : Phi->Args) {
        (void)Predecessor;
        Returns.push_back(Arm);
      }
      continue;
    }
    const MedOp *Op = Nodes[At].Op;
    if (!Op)
      continue;
    const bool View =
        ((Op->Opcode == NdOp::COPY || Op->Opcode == NdOp::INT_ZEXT ||
          Op->Opcode == NdOp::INT_SEXT) &&
         Op->NumInputs == 1) ||
        (Op->Opcode == NdOp::SUBBYTES && Op->NumInputs == 2 &&
         Op->Inputs[1].isConst() && Op->Inputs[1].ConstVal == 0);
    if (View && !medReadsInputsWhole(*Op))
      Returns.push_back(Op->Inputs[0]);
    else if (Op->Opcode != NdOp::CALL && Op->Opcode != NdOp::INDIR_CALL)
      need(Value, medBytesOf(Value));
  }
  const auto Seen = [&](const MedVar &Value) {
    return Value.Kind == MedVar::Temp ||
           (Value.Kind == MedVar::Reg &&
            (Value.RegOff == TRI.IntReturnReg || !OtherRegistersRead));
  };
  for (size_t I = 0; I < Nodes.size(); ++I) {
    if (Nodes[I].Used)
      continue;
    if (const MedOp *Op = Nodes[I].Op) {
      if (!medReadsInputsWhole(*Op) && !Seen(Op->Output))
        need(Op->Output, medBytesOf(Op->Output));
    } else if (const PhiNode *Phi = Nodes[I].Phi) {
      if (!Seen(Phi->Output))
        need(Phi->Output, medBytesOf(Phi->Output));
    }
  }
  const bool AllZeroFactsFit =
      Nodes.size() <= limits::ir::kMedByteDemandZeroByteValues;
  while (!Pending.empty()) {
    const unsigned Index = Pending.back();
    Pending.pop_back();
    Nodes[Index].Pending = false;
    const MedByteMask Bytes = Nodes[Index].Bytes;
    if (const PhiNode *Phi = Nodes[Index].Phi) {
      for (const auto &[Predecessor, Value] : Phi->Args) {
        (void)Predecessor;
        need(Value, Bytes);
      }
      if (Phi->ExceptionalEntry)
        need(*Phi->ExceptionalEntry, Bytes);
      continue;
    }
    const MedOp *Op = Nodes[Index].Op;
    if (Op && !medReadsInputsWhole(*Op))
      inputDemand(
          *Op, Bytes,
          [&](const MedVar &Input, MedByteMask Needed) { need(Input, Needed); },
          [&](const MedVar &Input) { return zeroBytes(Input); },
          AllZeroFactsFit);
  }
}

MedByteMask MedByteDemand::bytesRead(const MedVar &Value) const {
  const auto Found = Index.find(medValueKey(Value));
  return Found == Index.end() ? 0 : Nodes[Found->second].Bytes;
}

bool MedByteDemand::reads(const MedVar &Value) const {
  return bytesRead(Value) != 0;
}

bool MedByteDemand::used(const MedVar &Value) const {
  const auto Found = Index.find(medValueKey(Value));
  return Found != Index.end() && Nodes[Found->second].Used;
}

MedByteMask MedByteDemand::zeroBytes(const MedVar &Value) {
  if (Value.isConst())
    return zeroBytesOf(Value);
  const auto Root = Index.find(medValueKey(Value));
  if (Root == Index.end())
    return 0;
  if (Nodes[Root->second].ZeroVisited)
    return Nodes[Root->second].Zero;
  llvm::SmallVector<unsigned, 16> Slice, Work{Root->second};
  while (!Work.empty()) {
    const unsigned Each = Work.pop_back_val();
    Node &N = Nodes[Each];
    if (N.ZeroVisited)
      continue;
    N.ZeroVisited = true;
    if (!N.Phi && !(N.Op && passesZeroBytes(*N.Op)))
      continue;
    if (Slice.size() >= ::neverd::limits::ir::kMedByteDemandZeroByteValues) {
      // Past the bound nothing in the slice is known.
      for (unsigned Seen : Slice)
        Nodes[Seen].Zero = 0;
      return 0;
    }
    Slice.push_back(Each);
    N.Zero = medBytesOf(N.Phi ? N.Phi->Output : N.Op->Output);
    const auto Push = [&](const MedVar &Input) {
      if (Input.isConst())
        return;
      if (const auto Found = Index.find(medValueKey(Input));
          Found != Index.end())
        Work.push_back(Found->second);
    };
    if (N.Phi) {
      for (const auto &[Predecessor, Arm] : N.Phi->Args) {
        (void)Predecessor;
        Push(Arm);
      }
      if (N.Phi->ExceptionalEntry)
        Push(*N.Phi->ExceptionalEntry);
    } else {
      for (uint8_t I = 0; I < N.Op->NumInputs; ++I)
        Push(N.Op->Inputs[I]);
    }
  }
  const auto Of = [&](const MedVar &Input) -> MedByteMask {
    if (Input.isConst())
      return zeroBytesOf(Input);
    const auto Found = Index.find(medValueKey(Input));
    return Found == Index.end() ? 0 : Nodes[Found->second].Zero;
  };
  for (bool Changed = true; Changed;) {
    Changed = false;
    for (unsigned Each : Slice) {
      Node &N = Nodes[Each];
      MedByteMask Now;
      if (N.Phi) {
        Now = medBytesOf(N.Phi->Output);
        for (const auto &[Predecessor, Arm] : N.Phi->Args) {
          (void)Predecessor;
          Now &= Of(Arm);
        }
        if (N.Phi->ExceptionalEntry)
          Now &= Of(*N.Phi->ExceptionalEntry);
      } else {
        Now = zeroBytesThrough(*N.Op, Of);
      }
      Now &= N.Zero;
      Changed |= Now != N.Zero;
      N.Zero = Now;
    }
  }
  return Nodes[Root->second].Zero;
}

unsigned MedByteDemand::nodeOf(const MedVar &Value) {
  const auto [Found, New] = Index.try_emplace(
      medValueKey(Value), static_cast<unsigned>(Nodes.size()));
  if (New)
    Nodes.emplace_back();
  return Found->second;
}
} // namespace neverd
