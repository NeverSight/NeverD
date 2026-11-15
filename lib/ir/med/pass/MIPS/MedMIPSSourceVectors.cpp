//===- MedMIPSSourceVectors.cpp - MIPS FP source contracts --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Track defined scalar FPR bytes and locally initialized MSA vectors. Complete
/// fixed scalar call descriptors own argument reads, clobbers and results.
/// Incoming MSA vectors and saved FPR restoration still need separate proofs.
//===----------------------------------------------------------------------===//

#include "neverd/ArchSupport.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/med/LowToMedError.h"
#include "neverd/ir/med/MIPS/MIPSIntrinsicShapes.h"
#include "neverd/ir/med/MIPS/MIPSSourceVectors.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/libc/LibCFloatingArguments.h"
#include "neverd/lift/MIPS/MIPSRegs.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/DenseMap.h"

#include <bitset>
#include <iterator>
#include <optional>
#include <set>
#include <tuple>
#include <vector>

namespace neverd {

const SourceCallTypeHint *mipsFloatingCallSite(
    const LowOp &Op, Arch Target,
    const std::map<SourceCallOccurrenceKey, SourceCallTypeHint> *Sites) {
  if (Sites && !Sites->empty())
    if (const auto Key = sourceCallOccurrenceKey(Op, Target))
      if (const auto It = Sites->find(*Key); It != Sites->end()) {
        const auto &H = It->second;
        std::string Diagnostic;
        if (H.CallKind != SourceCallTypeHint::Kind::Analyst ||
            H.AnalystSite != Key || H.Signature.Architecture != Target ||
            !H.Signature.MIPSScalars ||
            H.AnalystFixedCount !=
                (H.Signature.MIPSScalars->FixedCount
                     ? int(*H.Signature.MIPSScalars->FixedCount)
                     : -1) ||
            !validateSourceABI(H.Signature, Diagnostic))
          throw LowToMedConversionError("invalid MIPS call-site ABI");
        return &H;
      }
  return nullptr;
}

static bool hasScalarFP64ABI(const BinaryImage &Image, Arch Target) {
  return Image.MIPS && Image.MIPS->hasScalarFP64ABI(isMIPS64(Target));
}

void requireMIPSSourceVectors(
    const LowFunc &Function, Arch Target, const BinaryImage *Image,
    const std::map<va_t, FloatingCallArguments> *Calls,
    const std::map<SourceCallOccurrenceKey, SourceCallTypeHint> *Sites) {
  const auto IsVector = [](const NdVar &V) {
    return V.isReg() && mipsreg::isVector(V.Offset);
  };
  const auto SignatureOf =
      [&](const LowOp &Op) -> const FloatingCallArguments * {
    if (const auto *Site = mipsFloatingCallSite(Op, Target, Sites))
      return Site->Signature.MIPSScalars.get();
    if (Calls && (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) &&
        Op.NumInputs && Op.Inputs[0].isConst())
      if (const auto Call = Calls->find(Op.Inputs[0].Offset);
          Call != Calls->end())
        return &Call->second;
    return nullptr;
  };
  bool HasVectors = false;
  for (const auto &Block : Function.Blocks)
    for (const auto &Op : Block.Ops) {
      HasVectors |= IsVector(Op.Output) || SignatureOf(Op);
      for (unsigned I = 0; I != Op.NumInputs; ++I)
        HasVectors |= IsVector(Op.Inputs[I]);
    }
  if (!HasVectors)
    return;
  if (!isMIPS(Target))
    throw LowToMedConversionError(
        "MIPS FP/vector source requires a MIPS target");
  const bool FP64 = Image && hasScalarFP64ABI(*Image, Target);
  const bool FP32 =
      Image && Image->MIPS && Image->MIPS->hasScalarFP32ABI(isMIPS64(Target));
  const bool Scalar = FP64 || FP32;
  if (Image && !Scalar)
    throw LowToMedConversionError(
        "MIPS FP/vector source requires an explicit scalar FP profile");
  using Mask = std::bitset<mipsreg::VectorCount * mipsreg::VectorBytes>;
  struct Transfer {
    Mask Needs, Defines, Kills;
    std::optional<Mask> Incoming;
  };
  const auto Index = [](const NdVar &V) {
    return (V.Offset - mipsreg::VectorBase) / mipsreg::VectorBytes;
  };
  const auto Bytes = [&](const NdVar &V) {
    Mask M;
    const auto First = Index(V) * mipsreg::VectorBytes;
    for (unsigned I = 0; I < V.Size; ++I)
      M.set(First + I);
    return M;
  };
  const auto ABIBytes = [&](uint64_t Register, unsigned Size) {
    if (FP32 && Size == 8)
      return Bytes(NdVar::reg(Register, 4)) |
             Bytes(NdVar::reg(Register + mipsreg::VectorBytes, 4));
    return Bytes(NdVar::reg(Register, Size));
  };
  std::vector<Transfer> Effects(Function.Blocks.size());
  llvm::DenseMap<int, size_t> Indices;
  for (size_t I = 0; I != Function.Blocks.size(); ++I) {
    const auto &Block = Function.Blocks[I];
    auto &T = Effects[I];
    Indices.try_emplace(Block.Id, I);
    if (!Block.ExceptionalSuccs.empty() || !Block.ExceptionalPreds.empty())
      throw LowToMedConversionError(
          "MSA source requires ordinary control flow");
    for (const auto &Op : Block.Ops) {
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
        const FloatingCallArguments *Signature = SignatureOf(Op);
        if (!Scalar || !Signature ||
            !floatingCallTypes(Target, BinaryFormat::ELF, *Signature))
          throw LowToMedConversionError(
              "MIPS floating call ABI is not qualified");
        for (const auto &P : Signature->Scalars) {
          if (P.Kind != SourceABICarrierKind::FloatingRegister)
            continue;
          const auto Read = ABIBytes(P.Register, P.Bytes);
          if ((T.Kills & Read).any())
            throw LowToMedConversionError("MIPS call reads undefined FP bytes");
          T.Needs |= Read & ~T.Defines;
        }
        const auto &TRI = getTargetRegInfo(Target);
        Mask Clobbered;
        Clobbered.set();
        for (unsigned R = 0; R < mipsreg::VectorCount; ++R)
          if ((FP32 && R >= 20) || TRI.isCallPreserved(mipsreg::w(R), 8))
            Clobbered &= ~Bytes(NdVar::reg(mipsreg::w(R), FP32 ? 4 : 8));
        T.Defines &= ~Clobbered;
        T.Kills |= Clobbered;
        if (Signature->ReturnKind == NdTypeKind::Float) {
          const auto Result = ABIBytes(TRI.FPReturnReg, Signature->ReturnBytes);
          T.Defines |= Result;
          T.Kills &= ~Result;
        }
      }
      for (unsigned J = 0; J != Op.NumInputs; ++J) {
        if (!IsVector(Op.Inputs[J]))
          continue;
        const auto Read = Bytes(Op.Inputs[J]);
        if ((T.Kills & Read).any())
          throw LowToMedConversionError(
              "MSA source observes a partial vector definition");
        T.Needs |= Read & ~T.Defines;
      }
      if (!IsVector(Op.Output))
        continue;
      const auto R = Index(Op.Output);
      // n64 preserves F24–F31. o32 FP64 preserves F20,F22,...,F30.
      if ((!isMIPS64(Target) && !FP64 && R >= 20) ||
          getTargetRegInfo(Target).isCallPreserved(mipsreg::w(R), 8))
        throw LowToMedConversionError(
            "MSA source saved FPR restoration is not qualified");
      const auto Whole =
          Bytes(NdVar::reg(Op.Output.Offset, mipsreg::VectorBytes));
      const auto Written = Bytes(Op.Output);
      T.Defines = (T.Defines & ~Whole) | Written;
      T.Kills = (T.Kills & ~Whole) | (Whole & ~Written);
    }
  }

  // A join may use a vector only when every incoming execution defines it.
  // Revisiting the entry through a back edge cannot initialize a caller input.
  std::vector<size_t> Work{0};
  Mask Entry;
  if (Scalar)
    for (uint64_t Register : getTargetRegInfo(Target).FPParamRegs)
      Entry |= ABIBytes(Register, 8);
  Effects.front().Incoming = Entry;
  while (!Work.empty()) {
    const auto I = Work.back();
    Work.pop_back();
    auto &T = Effects[I];
    if ((T.Needs & ~*T.Incoming).any())
      throw LowToMedConversionError(
          "MSA incoming vector source ABI is not qualified");
    const auto Out = (*T.Incoming & ~T.Kills) | T.Defines;
    for (int Id : Function.Blocks[I].Succs) {
      const auto It = Indices.find(Id);
      if (It == Indices.end())
        throw LowToMedConversionError("MSA source has an unresolved successor");
      auto &In = Effects[It->second].Incoming;
      const Mask Next = In ? *In & Out : Out;
      if (!In || *In != Next) {
        In = Next;
        Work.push_back(It->second);
      }
    }
  }
  for (const auto &T : Effects)
    if (!T.Incoming && T.Needs.any())
      throw LowToMedConversionError(
          "MSA unreachable block has unbound vector inputs");
}

void projectMIPSFP32Registers(MedFunc &Function,
                              llvm::function_ref<MedVar(uint16_t)> Temporary,
                              llvm::function_ref<MedVar(unsigned)> Pair) {
  std::bitset<mipsreg::VectorCount / 2> Used;
  const auto WordIndex = [](const MedVar &V) -> std::optional<unsigned> {
    if (V.Kind == MedVar::Reg && V.Size == 4 && mipsreg::isVector(V.RegOff))
      return (V.RegOff - mipsreg::VectorBase) / mipsreg::VectorBytes;
    return std::nullopt;
  };
  // Projection preserves the unwritten partner on each word store. Those
  // synthetic reads must not turn a pair first defined by this function into
  // an incoming parameter. Compute physical entry demand before adding them.
  const auto EntryMask = [&](const MedVar &V) -> unsigned {
    if (V.Kind != MedVar::Reg || !mipsreg::isVector(V.RegOff))
      return 0;
    const unsigned R = (V.RegOff - mipsreg::VectorBase) / mipsreg::VectorBytes;
    if (R < 12 || R >= 16)
      return 0;
    return (V.Size == 8 ? 3u : 1u) << (R - 12);
  };
  struct Demand {
    unsigned Reads = 0, Writes = 0, Incoming = 0;
  };
  std::vector<Demand> Demands(Function.Blocks.size());
  llvm::DenseMap<int, size_t> DemandIndex;
  for (size_t I = 0; I < Function.Blocks.size(); ++I) {
    DemandIndex[Function.Blocks[I].Id] = I;
    for (const auto &Op : Function.Blocks[I].Ops) {
      auto &D = Demands[I];
      for (unsigned K = 0; K < Op.NumInputs; ++K)
        D.Reads |= EntryMask(Op.Inputs[K]) & ~D.Writes;
      D.Writes |= EntryMask(Op.Output);
    }
  }
  bool Changed;
  do {
    Changed = false;
    for (size_t I = Function.Blocks.size(); I-- > 0;) {
      auto &D = Demands[I];
      unsigned Live = D.Reads;
      for (int Successor : Function.Blocks[I].Succs)
        Live |=
            Demands[DemandIndex.find(Successor)->second].Incoming & ~D.Writes;
      Changed |= Live != D.Incoming;
      D.Incoming = Live;
    }
  } while (Changed);
  for (auto &Block : Function.Blocks) {
    std::vector<MedOp> Projected;
    for (auto &Op : Block.Ops) {
      // A LowIR double read joins the odd/even physical words. Keep that
      // whole ABI carrier visible to parameter-width inference.
      if (Op.Opcode == NdOp::CONCAT && Op.NumInputs == 2) {
        const auto High = WordIndex(Op.Inputs[0]);
        const auto Low = WordIndex(Op.Inputs[1]);
        if (High && Low && !(*Low & 1) && *High == *Low + 1) {
          Used.set(*Low / 2);
          Op.Opcode = NdOp::COPY;
          Op.NumInputs = 1;
          Op.Inputs[0] = Pair(*Low);
        }
      }
      const auto Extract = [&](unsigned Register, bool Opposite) {
        Used.set(Register / 2);
        MedOp Read;
        Read.Opcode = NdOp::SUBBYTES;
        Read.Addr = Op.Addr;
        Read.Output = Temporary(4);
        Read.addInput(Pair(Register & ~1u));
        Read.addInput(MedVar::makeConst(((Register & 1) ^ Opposite) * 4, 4));
        const auto Value = Read.Output;
        Projected.push_back(std::move(Read));
        return Value;
      };
      for (unsigned I = 0; I < Op.NumInputs; ++I)
        if (auto Index = WordIndex(Op.Inputs[I]))
          Op.Inputs[I] = Extract(*Index, false);
      const auto Destination = WordIndex(Op.Output);
      if (!Destination) {
        Projected.push_back(std::move(Op));
        continue;
      }
      const auto Other = Extract(*Destination, true);
      Op.Output = Temporary(4);
      const auto Value = Op.Output;
      MedOp Write;
      Write.Opcode = NdOp::CONCAT;
      Write.Addr = Op.Addr;
      Write.Output = Pair(*Destination & ~1u);
      Write.addInput(*Destination & 1 ? Value : Other);
      Write.addInput(*Destination & 1 ? Other : Value);
      Projected.push_back(std::move(Op));
      Projected.push_back(std::move(Write));
    }
    Block.Ops = std::move(Projected);
  }
  if (Used.none())
    return;
  // The demanded o32 entry carriers are f12/f13 and f14/f15. Every other
  // used pair starts unbound. Retain those bits explicitly instead of inventing
  // source parameters; the ordinary byte-use proof must establish they are
  // dead.
  std::vector<MedOp> Entry;
  for (unsigned I = 0; I < Used.size(); ++I) {
    if (!Used[I] || ((I == 6 || I == 7) &&
                     (Demands.front().Incoming & (3u << (2 * (I - 6))))))
      continue;
    MedVar Words[2];
    for (auto &Word : Words) {
      MedOp Unknown;
      Unknown.Opcode = NdOp::INTRINSIC;
      Unknown.Addr = Function.Entry;
      Word = Unknown.Output = Temporary(4);
      Unknown.addInput(MedVar::makeConst(unsigned(Intrinsic::MIPSUnpredictable),
                                         sizeof(Intrinsic)));
      Entry.push_back(std::move(Unknown));
    }
    MedOp Join;
    Join.Opcode = NdOp::CONCAT;
    Join.Addr = Function.Entry;
    Join.Output = Pair(I * 2);
    Join.addInput(Words[1]);
    Join.addInput(Words[0]);
    Entry.push_back(std::move(Join));
  }
  auto &First = Function.Blocks.front().Ops;
  Entry.insert(Entry.end(), std::make_move_iterator(First.begin()),
               std::make_move_iterator(First.end()));
  First = std::move(Entry);
}

void discardUnobservedMIPSFPUpperBits(
    MedFunc &Function, const BinaryImage &Image,
    const std::map<va_t, FloatingCallArguments> &EntrySignatures,
    const DebugContext *Debug) {
  const Arch Target = Image.Arch;
  if (!isMIPS(Target) || Function.SkippedSSA ||
      !(Image.MIPS && Image.MIPS->hasScalarFPABI(isMIPS64(Target))))
    return;
  // A selected leaf need not occur in the outgoing call inventory. Resolve
  // its exact declaration even when it has no caller in this pipeline run.
  const auto Known = EntrySignatures.find(Function.Entry);
  std::optional<FloatingCallArguments> Local;
  if (Known == EntrySignatures.end() && Debug)
    if (const auto Symbol = Debug->resolveFunction(Function.Entry);
        Symbol && Symbol->Addr == Function.Entry)
      Local =
          libc::floatingCallArguments(*Symbol, Target, BinaryFormat::ELF,
                                      Image.instructionModeAt(Function.Entry)
                                          .value_or(InstructionMode::Default));
  const auto *Signature = Known != EntrySignatures.end() ? &Known->second
                          : Local                        ? &*Local
                                                         : nullptr;
  bool Declared = false;
  if (Signature && !Signature->ReturnElements)
    if (auto Types = floatingCallTypes(Target, BinaryFormat::ELF, *Signature)) {
      Declared = true;
      Function.ReturnType = Types->Result;
      Function.HasDeclaredMIPSIntegerResult =
          Types->Result->Kind == NdTypeKind::Int;
      Function.MultiReturn.clear();
      Function.ReturnsVectorRegister = false;
      // A MIPS16 body and its typed direct callers must share the logical
      // parameter types. Retain exact register identities and widths: a
      // split double or an omitted input needs a separate entry adapter.
      if (Signature->MIPS16Parameters && !Signature->FixedCount &&
          Signature->Scalars.size() == Function.Params.size()) {
        bool Exact = true;
        for (size_t I = 0; I < Function.Params.size(); ++I) {
          const auto &P = Function.Params[I];
          const auto &S = Signature->Scalars[I];
          Exact &= S.Kind == SourceABICarrierKind::IntegerRegister &&
                   P.RegOff == S.Register && P.Size == S.Bytes;
        }
        if (Exact) {
          Function.TypedParams.clear();
          for (size_t I = 0; I < Function.Params.size(); ++I)
            Function.TypedParams.push_back(
                {Function.Params[I].display(), Types->Parameters[I]});
        }
      }
    }
  if (!Function.ReturnType || !Function.MultiReturn.empty())
    return;
  const bool Floating = Function.ReturnType->Kind == NdTypeKind::Float;
  if (!Declared && !Floating &&
      !(Function.ReturnType->Kind == NdTypeKind::Int &&
        !Function.ReturnsVectorRegister))
    return;
  std::vector<MedOp *> Candidates;
  for (auto &Block : Function.Blocks)
    for (auto &Op : Block.Ops)
      if (mipsScalarMedShapeIsValid(Op, Target) &&
          static_cast<Intrinsic>(Op.Inputs[0].ConstVal) ==
              Intrinsic::MIPSUnpredictable)
        Candidates.push_back(&Op);
  if (Candidates.empty())
    return;

  using Key = std::tuple<MedVar::VarKind, int, int>;
  const auto KeyOf = [](const MedVar &V) {
    return Key{V.Kind, V.Id, V.SSAVer};
  };
  struct Use {
    const MedOp *Op = nullptr;
    const PhiNode *Phi = nullptr;
    unsigned Input = 0;
    uint16_t Bytes = 0;
  };
  std::map<Key, std::vector<Use>> Uses;
  bool CompleteCalls = true;
  for (const auto &Block : Function.Blocks) {
    for (const auto &Phi : Block.Phis) {
      for (const auto &[Pred, Arg] : Phi.Args)
        Uses[KeyOf(Arg)].push_back({nullptr, &Phi, 0, Arg.Size});
      if (Phi.ExceptionalEntry)
        Uses[KeyOf(*Phi.ExceptionalEntry)].push_back(
            {nullptr, &Phi, 0, Phi.ExceptionalEntry->Size});
    }
    for (const auto &Op : Block.Ops) {
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL)
        CompleteCalls &= bool(Op.ScalarCallSignature);
      for (unsigned I = 0; I < Op.NumInputs; ++I)
        Uses[KeyOf(Op.Inputs[I])].push_back(
            {&Op, nullptr, I, Op.Inputs[I].Size});
    }
  }
  for (const auto &Call : Function.CallInfos)
    for (const auto &Arg : Call.Args)
      Uses[KeyOf(Arg)].push_back({nullptr, nullptr, 0, Arg.Size});
  if (!CompleteCalls)
    return;
  // Partial-preservation records have implicit uses outside Ops. MIPS
  // currently refuses saved FPR writes at source admission; retain the
  // conservative result if such a record nevertheless reaches this pass.
  for (const auto &Clobber : Function.CallClobbers)
    if (Clobber.PreservedPrefixSize)
      return;

  // A physical return register's earlier SSA definitions cannot reach the
  // return if a full write replaces them in the same block. Explicit uses
  // (including calls and stores) still participate in the byte-use proof.
  std::set<Key> Overwritten;
  for (const auto &Block : Function.Blocks) {
    std::map<uint64_t, uint16_t> LaterWrites;
    for (auto It = Block.Ops.rbegin(); It != Block.Ops.rend(); ++It) {
      if (It->Opcode == NdOp::RETURN)
        LaterWrites.clear();
      const auto &Value = It->Output;
      if (Value.Kind != MedVar::Reg)
        continue;
      auto &Bytes = LaterWrites[Value.RegOff];
      if (Bytes >= Value.Size)
        Overwritten.insert(KeyOf(Value));
      Bytes = std::max(Bytes, Value.Size);
    }
    for (const auto &Phi : Block.Phis)
      if (Phi.Output.Kind == MedVar::Reg &&
          LaterWrites[Phi.Output.RegOff] >= Phi.Output.Size)
        Overwritten.insert(KeyOf(Phi.Output));
  }

  const auto &TRI = getTargetRegInfo(Target);
  const auto Observed = [&](MedVar Root) {
    struct Span {
      MedVar Value;
      unsigned First, End;
    };
    std::vector<Span> Work{{Root, 0, Root.Size}};
    std::set<std::tuple<Key, unsigned, unsigned>> Seen;
    while (!Work.empty()) {
      const auto [Value, First, End] = Work.back();
      Work.pop_back();
      if (!Seen.emplace(KeyOf(Value), First, End).second)
        continue;
      // Keep every potentially returned FPR definition conservative unless
      // a later full write proves it cannot supply the implicit ABI result.
      if (Floating && Value.Kind == MedVar::Reg &&
          Value.RegOff == TRI.FPReturnReg &&
          First < Function.ReturnType->Size &&
          !Overwritten.contains(KeyOf(Value)))
        return true;
      // RETURN may still carry only its branch target here. Integer
      // result registers are observable even without an explicit operand.
      if (!Floating && Value.Kind == MedVar::Reg &&
          !Overwritten.contains(KeyOf(Value)) &&
          ((Value.RegOff == TRI.IntReturnReg &&
            First < Function.ReturnType->Size) ||
           (Value.RegOff == TRI.IntReturnReg2 &&
            Function.ReturnType->Size > TRI.FullRegWidth &&
            First < Function.ReturnType->Size - TRI.FullRegWidth)))
        return true;
      if (Value.Kind == MedVar::Reg &&
          TRI.isCallPreserved(Value.RegOff, Value.Size))
        return true;
      for (const auto &Use : Uses[KeyOf(Value)]) {
        const unsigned Last = std::min<unsigned>(End, Use.Bytes);
        if (First >= Last)
          continue;
        if (Use.Phi) {
          Work.push_back({Use.Phi->Output, First, Last});
          continue;
        }
        if (!Use.Op)
          return true;
        const auto &Op = *Use.Op;
        if (Op.MemoryOrdering != NdMemoryOrdering::None ||
            Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
          return true;
        if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
            Op.Output.Size == Op.Inputs[0].Size) {
          Work.push_back({Op.Output, First, Last});
        } else if ((Op.Opcode == NdOp::INT_AND || Op.Opcode == NdOp::INT_OR ||
                    Op.Opcode == NdOp::INT_XOR) &&
                   Op.NumInputs == 2 && Op.Output.Size == Op.Inputs[0].Size &&
                   Op.Output.Size == Op.Inputs[1].Size) {
          // Bitwise transport cannot move an unknown high bit into a low
          // returned byte. Conditional word writes use such masks to keep
          // an untaken destination's upper bytes intact.
          Work.push_back({Op.Output, First, Last});
        } else if (Op.Opcode == NdOp::CONCAT && Op.NumInputs == 2 &&
                   Op.Output.Size == Op.Inputs[0].Size + Op.Inputs[1].Size) {
          const unsigned Bias = Use.Input == 0 ? Op.Inputs[1].Size : 0;
          Work.push_back({Op.Output, First + Bias, Last + Bias});
        } else if (Op.Opcode == NdOp::SUBBYTES && Op.NumInputs == 2 &&
                   Use.Input == 0 && Op.Inputs[1].isConst() &&
                   Op.Inputs[1].ConstVal <= Op.Inputs[0].Size &&
                   Op.Output.Size <=
                       Op.Inputs[0].Size - Op.Inputs[1].ConstVal) {
          const unsigned Begin = Op.Inputs[1].ConstVal;
          const unsigned L = std::max(First, Begin);
          const unsigned H = std::min(Last, Begin + Op.Output.Size);
          if (L < H)
            Work.push_back({Op.Output, L - Begin, H - Begin});
        } else {
          return true;
        }
      }
    }
    return false;
  };
  for (auto *Op : Candidates)
    if (!Observed(Op->Output)) {
      // This canonical fill is outside all observable source byte ranges.
      // It grants no value, CSE, or motion contract to a live occurrence.
      Op->Opcode = NdOp::COPY;
      Op->Inputs[0] = MedVar::makeConst(0, Op->Output.Size);
    }
}

} // namespace neverd
