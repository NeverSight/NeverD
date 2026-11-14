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
  if (Image && !FP64)
    throw LowToMedConversionError(
        "MIPS FP/vector source requires explicit FP64 profile");
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
        if (!FP64 || !Signature ||
            !floatingCallTypes(Target, BinaryFormat::ELF, *Signature))
          throw LowToMedConversionError(
              "MIPS floating call ABI is not qualified");
        for (const auto &P : Signature->Scalars) {
          if (P.Kind != SourceABICarrierKind::FloatingRegister)
            continue;
          const auto Read = Bytes(NdVar::reg(P.Register, P.Bytes));
          if ((T.Kills & Read).any())
            throw LowToMedConversionError("MIPS call reads undefined FP bytes");
          T.Needs |= Read & ~T.Defines;
        }
        const auto &TRI = getTargetRegInfo(Target);
        Mask Clobbered;
        Clobbered.set();
        for (unsigned R = 0; R < mipsreg::VectorCount; ++R)
          if (TRI.isCallPreserved(mipsreg::w(R), 8))
            Clobbered &= ~Bytes(NdVar::reg(mipsreg::w(R), 8));
        T.Defines &= ~Clobbered;
        T.Kills |= Clobbered;
        if (Signature->ReturnKind == NdTypeKind::Float) {
          const auto Result =
              Bytes(NdVar::reg(TRI.FPReturnReg, Signature->ReturnBytes));
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
  if (FP64)
    for (uint64_t Register : getTargetRegInfo(Target).FPParamRegs)
      Entry |= Bytes(NdVar::reg(Register, 8));
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

void discardUnobservedMIPSFPUpperBits(
    MedFunc &Function, const BinaryImage &Image,
    const std::map<va_t, FloatingCallArguments> &EntrySignatures,
    const DebugContext *Debug) {
  const Arch Target = Image.Arch;
  if (!isMIPS(Target) || Function.SkippedSSA ||
      !hasScalarFP64ABI(Image, Target))
    return;
  // A selected leaf need not occur in the outgoing call inventory. Resolve
  // its exact declaration even when it has no caller in this pipeline run.
  const auto Known = EntrySignatures.find(Function.Entry);
  std::optional<FloatingCallArguments> Local;
  if (Known == EntrySignatures.end() && Debug)
    if (const auto Symbol = Debug->resolveFunction(Function.Entry);
        Symbol && Symbol->Addr == Function.Entry)
      Local = libc::floatingCallArguments(*Symbol, Target, BinaryFormat::ELF);
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
    }
  if (!Function.ReturnType || !Function.MultiReturn.empty())
    return;
  const bool Floating = Function.ReturnType->Kind == NdTypeKind::Float;
  if (!Declared && !(Floating && Function.ReturnType->Size == 4) &&
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
      // Keep every potentially returned FPR definition conservative; the
      // ABI reads its low word even when RETURN still carries the GPR seed.
      if (Floating && Value.Kind == MedVar::Reg &&
          Value.RegOff == TRI.FPReturnReg && First < Function.ReturnType->Size)
        return true;
      // RETURN may still carry only its branch target here. Integer
      // result registers are observable even without an explicit operand.
      if (!Floating && Value.Kind == MedVar::Reg &&
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
